/*
 * tls_verify.c -- checking the server's certificates and signature
 *
 * The TLS clients (tls13.c, tls.c) authenticate the server here, with
 * BearSSL 0.6's X.509 engine and signature checks (lib/bearssl): the chain
 * up to one of Mozilla's roots (ca_roots.c), the host name, the dates, and
 * the handshake signature by the leaf's key. BearSSL 0.6 has no RSA-PSS,
 * which TLS 1.3 requires of RSA servers, so EMSA-PSS-VERIFY (RFC 8017
 * 9.1.2) is here, over BearSSL's RSA public operation and hashes.
 *
 * One caller at a time (http.c's session owner serializes the TLS
 * clients): the contexts are static. The same as arch/arm/kernel's.
 */

#include "../lib/bearssl/inc/bearssl.h"     /* types.h, via lib/bearssl/port */
#include "tls_verify.h"

extern void     serial_puts(const char *s);
extern void     serial_putdec(uint64_t val);
extern uint32_t ntp_get_utc(void);          /* NTP, else the CMOS clock */

extern const br_x509_trust_anchor ca_roots[];
extern const size_t ca_roots_count;

/* ── The chain ───────────────────────────────────────────────── */

static const char *x509_error(unsigned err)
{
    switch (err) {
    case BR_ERR_X509_EXPIRED:           return "a certificate is expired or not yet valid";
    case BR_ERR_X509_NOT_TRUSTED:       return "the chain doesn't end at a known root";
    case BR_ERR_X509_BAD_SERVER_NAME:   return "the certificate isn't for this host name";
    case BR_ERR_X509_BAD_SIGNATURE:     return "a signature in the chain is wrong";
    case BR_ERR_X509_DN_MISMATCH:       return "the chain is out of order (issuer/subject mismatch)";
    case BR_ERR_X509_NOT_CA:            return "an issuer isn't a CA";
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: return "a key is used for what it may not";
    case BR_ERR_X509_WEAK_PUBLIC_KEY:   return "a key is too weak";
    case BR_ERR_X509_UNSUPPORTED:       return "an algorithm isn't supported (SHA-1/MD5 signatures are refused)";
    case BR_ERR_X509_CRITICAL_EXTENSION: return "an unknown critical extension";
    case BR_ERR_X509_TIME_UNKNOWN:      return "the time of day is unknown (date sync)";
    case BR_ERR_X509_EMPTY_CHAIN:       return "no certificate";
    case BR_ERR_X509_LIMIT_EXCEEDED:    return "the chain is too long";
    default:                            return "a malformed certificate";
    }
}

int tls_verify_chain(const uint8_t *const *certs, const uint32_t *lens, int n,
                     const char *host, tls_pubkey_t *leaf)
{
    static br_x509_minimal_context xc;
    uint32_t now = ntp_get_utc();
    if (!now) {
        serial_puts("[TLS] certificate: the time of day is unknown (`date sync`), refusing\n");
        return -1;
    }
    br_x509_minimal_init(&xc, &br_sha256_vtable, ca_roots, ca_roots_count);
    br_x509_minimal_set_rsa(&xc, br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(&xc, &br_ec_prime_i31, br_ecdsa_i31_vrfy_asn1);
    /* SHA-2 only: no SHA-1 or MD5 signature in a chain is accepted */
    br_x509_minimal_set_hash(&xc, br_sha224_ID, &br_sha224_vtable);
    br_x509_minimal_set_hash(&xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&xc, br_sha512_ID, &br_sha512_vtable);
    /* BearSSL counts days from January 1st of year 0 */
    br_x509_minimal_set_time(&xc, 719528 + now / 86400, now % 86400);

    const br_x509_class **vt = &xc.vtable;
    (*vt)->start_chain(vt, host);
    for (int i = 0; i < n; i++) {
        (*vt)->start_cert(vt, lens[i]);
        (*vt)->append(vt, certs[i], lens[i]);
        (*vt)->end_cert(vt);
    }
    unsigned err = (*vt)->end_chain(vt);
    if (err) {
        serial_puts("[TLS] certificate refused: ");
        serial_puts(x509_error(err));
        serial_puts(" (BearSSL error ");
        serial_putdec(err);
        serial_puts(")\n");
        return -1;
    }
    unsigned usages = 0;
    const br_x509_pkey *pk = (*vt)->get_pkey(vt, &usages);
    if (!pk || !(usages & BR_KEYTYPE_SIGN)) {
        serial_puts("[TLS] certificate refused: its key may not sign\n");
        return -1;
    }
    memset(leaf, 0, sizeof(*leaf));
    leaf->type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        if (pk->key.rsa.nlen > sizeof(leaf->n) || pk->key.rsa.elen > sizeof(leaf->e))
            return -1;
        memcpy(leaf->n, pk->key.rsa.n, pk->key.rsa.nlen);
        memcpy(leaf->e, pk->key.rsa.e, pk->key.rsa.elen);
        leaf->nlen = (uint32_t)pk->key.rsa.nlen;
        leaf->elen = (uint32_t)pk->key.rsa.elen;
    } else {
        if (pk->key.ec.qlen > sizeof(leaf->q))
            return -1;
        leaf->curve = pk->key.ec.curve;
        memcpy(leaf->q, pk->key.ec.q, pk->key.ec.qlen);
        leaf->qlen = (uint32_t)pk->key.ec.qlen;
    }
    serial_puts("[TLS] certificate chain verified for ");
    serial_puts(host);
    serial_puts("\n");
    return 0;
}

/* ── Signatures ──────────────────────────────────────────────── */

/* br_digest_size is BearSSL's internal; the descriptor word is public */
static uint32_t digest_size(const br_hash_class *hc)
{
    return (hc->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK;
}

static const br_hash_class *scheme_hash(uint16_t scheme)
{
    switch (scheme & 0xFF) {
    case 0x03: case 0x01:               /* ECDSA, PKCS#1: hash in the high byte */
        return (scheme >> 8) == 4 ? &br_sha256_vtable :
               (scheme >> 8) == 5 ? &br_sha384_vtable :
               (scheme >> 8) == 6 ? &br_sha512_vtable : 0;
    case 0x04: return scheme == 0x0804 ? &br_sha256_vtable : 0;
    case 0x05: return scheme == 0x0805 ? &br_sha384_vtable : 0;
    case 0x06: return scheme == 0x0806 ? &br_sha512_vtable : 0;
    }
    return 0;
}

static uint32_t hash_of(const br_hash_class *hc, const void *data, uint32_t len,
                        uint8_t out[64])
{
    br_hash_compat_context ctx;
    hc->init(&ctx.vtable);
    hc->update(&ctx.vtable, data, len);
    hc->out(&ctx.vtable, out);
    return (uint32_t)digest_size(hc);
}

/* EMSA-PSS-VERIFY with MGF1 over the same hash and a salt as long as the
 * hash, as TLS 1.3 requires (RFC 8446 4.2.3) */
static int pss_verify(const tls_pubkey_t *key, const br_hash_class *hc,
                      const uint8_t mhash[64], const uint8_t *sig, uint32_t siglen)
{
    static uint8_t em[512], db[512];
    uint32_t hlen = (uint32_t)digest_size(hc);
    if (siglen != key->nlen || siglen > sizeof(em))
        return -1;
    br_rsa_public_key pk = { (unsigned char *)key->n, key->nlen,
                             (unsigned char *)key->e, key->elen };
    memcpy(em, sig, siglen);
    if (!br_rsa_i31_public(em, siglen, &pk))
        return -1;
    /* emBits = modBits - 1; a modulus starting on a byte boundary leaves a
     * zero byte in front of EM */
    uint32_t top = 8;
    while (top && !(key->n[0] & (1U << (top - 1))))
        top--;
    uint32_t mod_bits = (key->nlen - 1) * 8 + top;
    uint32_t em_bits = mod_bits - 1;
    uint32_t em_len = (em_bits + 7) / 8;
    const uint8_t *e = em + (siglen - em_len);
    if (siglen != em_len && em[0])
        return -1;
    if (em_len < 2 * hlen + 2 || e[em_len - 1] != 0xBC)
        return -1;
    uint32_t db_len = em_len - hlen - 1;
    const uint8_t *h = e + db_len;
    uint8_t zero_bits = (uint8_t)(0xFF << (8 - (8 * em_len - em_bits)));
    if ((8 * em_len - em_bits) && (e[0] & zero_bits))
        return -1;
    /* DB = maskedDB xor MGF1(H) */
    for (uint32_t done = 0, counter = 0; done < db_len; counter++) {
        br_hash_compat_context ctx;
        uint8_t c[4] = { (uint8_t)(counter >> 24), (uint8_t)(counter >> 16),
                         (uint8_t)(counter >> 8), (uint8_t)counter };
        uint8_t mask[64];
        hc->init(&ctx.vtable);
        hc->update(&ctx.vtable, h, hlen);
        hc->update(&ctx.vtable, c, 4);
        hc->out(&ctx.vtable, mask);
        for (uint32_t i = 0; i < hlen && done < db_len; i++, done++)
            db[done] = e[done] ^ mask[i];
    }
    if (8 * em_len - em_bits)
        db[0] &= (uint8_t)~zero_bits;
    /* DB = zeros || 0x01 || salt, the salt hlen long */
    uint32_t ps = db_len - hlen - 1;
    for (uint32_t i = 0; i < ps; i++)
        if (db[i])
            return -1;
    if (db[ps] != 0x01)
        return -1;
    /* H' = Hash(8 zeros || mHash || salt) */
    br_hash_compat_context ctx;
    static const uint8_t zeros[8];
    uint8_t h2[64];
    hc->init(&ctx.vtable);
    hc->update(&ctx.vtable, zeros, 8);
    hc->update(&ctx.vtable, mhash, hlen);
    hc->update(&ctx.vtable, db + ps + 1, hlen);
    hc->out(&ctx.vtable, h2);
    uint8_t diff = 0;
    for (uint32_t i = 0; i < hlen; i++)
        diff |= h2[i] ^ h[i];
    return diff ? -1 : 0;
}

int tls_verify_signature(const tls_pubkey_t *key, uint16_t scheme,
                         const uint8_t *msg, uint32_t len,
                         const uint8_t *sig, uint32_t siglen)
{
    const br_hash_class *hc = scheme_hash(scheme);
    uint8_t mhash[64];
    if (!hc)
        return -1;
    uint32_t hlen = hash_of(hc, msg, len, mhash);
    uint8_t kind = scheme & 0xFF;

    if (kind == 0x03) {                         /* ECDSA */
        if (key->type != BR_KEYTYPE_EC)
            return -1;
        br_ec_public_key pk = { key->curve, (unsigned char *)key->q, key->qlen };
        return br_ecdsa_i31_vrfy_asn1(&br_ec_prime_i31, mhash, hlen, &pk, sig, siglen) ? 0 : -1;
    }
    if (key->type != BR_KEYTYPE_RSA)
        return -1;
    if (kind == 0x01) {                         /* RSA PKCS#1 v1.5 (TLS 1.2) */
        const unsigned char *oid = hlen == 32 ? BR_HASH_OID_SHA256 :
                                   hlen == 48 ? BR_HASH_OID_SHA384 : BR_HASH_OID_SHA512;
        br_rsa_public_key pk = { (unsigned char *)key->n, key->nlen,
                                 (unsigned char *)key->e, key->elen };
        uint8_t out[64];
        if (!br_rsa_i31_pkcs1_vrfy(sig, siglen, oid, hlen, &pk, out))
            return -1;
        uint8_t diff = 0;
        for (uint32_t i = 0; i < hlen; i++)
            diff |= out[i] ^ mhash[i];
        return diff ? -1 : 0;
    }
    return pss_verify(key, hc, mhash, sig, siglen);     /* RSA-PSS */
}

const char *tls_scheme_name(uint16_t scheme)
{
    switch (scheme) {
    case 0x0403: return "ECDSA/SHA-256";      /* the curve is bound in 1.3 only */
    case 0x0503: return "ECDSA/SHA-384";
    case 0x0603: return "ECDSA/SHA-512";
    case 0x0804: return "RSA-PSS/SHA-256";
    case 0x0805: return "RSA-PSS/SHA-384";
    case 0x0806: return "RSA-PSS/SHA-512";
    case 0x0401: return "RSA PKCS#1/SHA-256";
    case 0x0501: return "RSA PKCS#1/SHA-384";
    case 0x0601: return "RSA PKCS#1/SHA-512";
    }
    return "?";
}

uint32_t tls_signature_schemes(uint8_t *out, int tls13)
{
    static const uint16_t v13[] = { 0x0403, 0x0503, 0x0603, 0x0804, 0x0805, 0x0806 };
    static const uint16_t v12[] = { 0x0401, 0x0501, 0x0601 };
    uint32_t n = 0;
    for (uint32_t i = 0; i < sizeof(v13) / sizeof(v13[0]); i++) {
        out[n++] = (uint8_t)(v13[i] >> 8);
        out[n++] = (uint8_t)v13[i];
    }
    if (!tls13) {
        for (uint32_t i = 0; i < sizeof(v12) / sizeof(v12[0]); i++) {
            out[n++] = (uint8_t)(v12[i] >> 8);
            out[n++] = (uint8_t)v12[i];
        }
    }
    return n;
}
