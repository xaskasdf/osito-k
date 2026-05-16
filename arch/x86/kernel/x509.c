/*
 * x509.c — minimal X.509 v3 parser, just enough to extract the EC
 * P-256 SubjectPublicKey from a leaf certificate.
 *
 * Scope is intentionally narrow:
 *   - Parse only what's needed for ECDSA SKE verification: walk the
 *     TBSCertificate to SubjectPublicKeyInfo and pull the
 *     uncompressed EC point bytes (32 + 32 = 64 octets).
 *   - No name validation, no validity checks, no extensions parsing,
 *     no algorithm-id verification beyond confirming the SPKI is
 *     well-formed enough for our purposes.
 *   - No chain walking, no signature checks on the cert itself
 *     (that would close A12 fully but is a separate ASN.1+RSA/ECDSA
 *     workitem). Cert-pinning (commit 9aaaf84) gives us trust in
 *     the leaf; this file just extracts the public material.
 *
 * Trust model assumes the caller has already pin-checked the cert
 * before extracting key material from it.
 */

#include "../include/types.h"
#include "x509.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

/* Byte-compare returning 0 on equal — kernel has no libc memcmp. */
static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (a[i] != b[i]) return 1;
    return 0;
}

/* ── DER primitives ────────────────────────────────────────── */

/* Read a DER length at *p (≤ end). Advances *p past the length
 * field. Sets *out to the value. Returns 0 on success, -1 on
 * malformed/overflow. */
static int der_read_len(const uint8_t **p, const uint8_t *end, uint32_t *out) {
    if (*p >= end) return -1;
    uint8_t b = *(*p)++;
    if ((b & 0x80) == 0) { *out = b; return 0; }
    uint8_t nb = b & 0x7F;
    if (nb == 0 || nb > 4 || *p + nb > end) return -1;
    uint32_t v = 0;
    for (uint32_t i = 0; i < nb; i++) v = (v << 8) | *(*p)++;
    *out = v;
    return 0;
}

/* Skip a single TLV (Tag-Length-Value) starting at *p; advance *p
 * past the entire TLV. Returns 0 on success. */
static int der_skip_tlv(const uint8_t **p, const uint8_t *end) {
    if (*p >= end) return -1;
    (*p)++;                     /* skip tag */
    uint32_t len;
    if (der_read_len(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    *p += len;
    return 0;
}

/* Enter a constructed type (SEQUENCE/SET) at *p. Verifies the tag
 * matches `expected_tag`, reads the length, sets *inner_end to the
 * end of the contents, and advances *p to the first inner byte.
 * Returns 0 on success. */
static int der_enter(const uint8_t **p, const uint8_t *end,
                     uint8_t expected_tag, const uint8_t **inner_end) {
    if (*p >= end || **p != expected_tag) return -1;
    (*p)++;
    uint32_t len;
    if (der_read_len(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    *inner_end = *p + len;
    return 0;
}

/* ── X.509 leaf-cert EC pubkey extraction ──────────────────── */

int x509_extract_ec_pubkey(const uint8_t *cert, uint32_t cert_len,
                           uint8_t pub_x[32], uint8_t pub_y[32]) {
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;

    /* Outer Certificate SEQUENCE */
    const uint8_t *outer_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) {
        serial_puts("[X509] no outer SEQUENCE\n"); return -1;
    }

    /* TBSCertificate SEQUENCE */
    const uint8_t *tbs_end;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) {
        serial_puts("[X509] no TBSCertificate\n"); return -1;
    }

    /* Optional [0] EXPLICIT Version (context-specific constructed = 0xA0).
     * If absent, version is v1 and serialNumber follows directly. */
    if (p < tbs_end && p[0] == 0xA0) {
        if (der_skip_tlv(&p, tbs_end) < 0) {
            serial_puts("[X509] bad version tag\n"); return -1;
        }
    }

    /* Skip serialNumber, signature AlgorithmIdentifier, issuer Name,
     * validity SEQUENCE, subject Name. Each is one TLV at this level. */
    for (int i = 0; i < 5; i++) {
        if (der_skip_tlv(&p, tbs_end) < 0) {
            serial_puts("[X509] truncated tbs prefix\n"); return -1;
        }
    }

    /* SubjectPublicKeyInfo SEQUENCE */
    const uint8_t *spki_end;
    if (der_enter(&p, tbs_end, 0x30, &spki_end) < 0) {
        serial_puts("[X509] no SubjectPublicKeyInfo\n"); return -1;
    }

    /* AlgorithmIdentifier ::= SEQUENCE {
     *     algorithm   OBJECT IDENTIFIER,
     *     parameters  ANY DEFINED BY algorithm OPTIONAL  }
     *
     * For an EC P-256 key both fields are fixed OIDs:
     *   algorithm  = 1.2.840.10045.2.1   (id-ecPublicKey)
     *   parameters = 1.2.840.10045.3.1.7 (prime256v1 / secp256r1)
     *
     * Validating both is cheap and catches an entire class of
     * cert-confusion attacks (e.g. an attacker presenting a
     * pin-matching cert whose key is actually RSA, which we'd then
     * misuse as if it were a P-256 point). */
    static const uint8_t OID_ID_EC_PUBLIC_KEY[] = {
        0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
    };
    static const uint8_t OID_PRIME256V1[] = {
        0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
    };
    const uint8_t *alg_end;
    if (der_enter(&p, spki_end, 0x30, &alg_end) < 0) {
        serial_puts("[X509] no AlgorithmIdentifier\n"); return -1;
    }
    if ((uint32_t)(alg_end - p) < sizeof OID_ID_EC_PUBLIC_KEY ||
        bytes_eq(p, OID_ID_EC_PUBLIC_KEY, sizeof OID_ID_EC_PUBLIC_KEY) != 0) {
        serial_puts("[X509] algorithm is not id-ecPublicKey\n"); return -1;
    }
    p += sizeof OID_ID_EC_PUBLIC_KEY;
    if (p >= alg_end) {
        serial_puts("[X509] missing curve parameters\n"); return -1;
    }
    if ((uint32_t)(alg_end - p) < sizeof OID_PRIME256V1 ||
        bytes_eq(p, OID_PRIME256V1, sizeof OID_PRIME256V1) != 0) {
        serial_puts("[X509] curve is not prime256v1 (P-256)\n"); return -1;
    }
    p = alg_end;     /* advance past AlgorithmIdentifier */

    /* subjectPublicKey BIT STRING. Body layout for an uncompressed
     * EC point on a 256-bit curve:
     *   1 byte  unused-bits   (must be 0)
     *   1 byte  0x04          (uncompressed point marker)
     *   32 byte X coordinate
     *   32 byte Y coordinate                                 */
    if (p >= spki_end || *p != 0x03) {
        serial_puts("[X509] subjectPublicKey not BIT STRING\n"); return -1;
    }
    p++;
    uint32_t bs_len;
    if (der_read_len(&p, spki_end, &bs_len) < 0) {
        serial_puts("[X509] bad BIT STRING length\n"); return -1;
    }
    if (p + bs_len > spki_end || bs_len < 66) {
        serial_puts("[X509] BIT STRING too short for P-256 point\n"); return -1;
    }
    if (p[0] != 0) {
        serial_puts("[X509] unsupported unused-bits\n"); return -1;
    }
    if (p[1] != 0x04) {
        serial_puts("[X509] non-uncompressed EC point\n"); return -1;
    }
    for (int i = 0; i < 32; i++) pub_x[i] = p[2 + i];
    for (int i = 0; i < 32; i++) pub_y[i] = p[34 + i];
    return 0;
}

/* Walk the TLS Certificate-handshake message body to find the LEAF
 * cert and extract its EC pubkey. The body layout is:
 *   uint24 total_list_len
 *   { uint24 cert_len ; opaque cert[cert_len] } *  */
int x509_extract_ec_pubkey_from_msg(const uint8_t *cert_msg, uint32_t msg_len,
                                     uint8_t pub_x[32], uint8_t pub_y[32]) {
    if (msg_len < 6) return -1;
    uint32_t total = ((uint32_t)cert_msg[0] << 16)
                   | ((uint32_t)cert_msg[1] <<  8)
                   |  (uint32_t)cert_msg[2];
    if (total + 3 > msg_len || total < 3) return -1;
    uint32_t leaf_len = ((uint32_t)cert_msg[3] << 16)
                      | ((uint32_t)cert_msg[4] <<  8)
                      |  (uint32_t)cert_msg[5];
    if (leaf_len + 6 > msg_len) return -1;
    return x509_extract_ec_pubkey(cert_msg + 6, leaf_len, pub_x, pub_y);
}

/* ── Chain link validation primitives ─────────────────────────── */

extern void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);
extern int  rsa_pkcs1_v15_sha256_verify(const uint8_t *sig, uint32_t sig_len,
                                         const uint8_t *n,   uint32_t n_len,
                                         const uint8_t *e,   uint32_t e_len,
                                         const uint8_t hash[32]);
extern int  ecdsa_p256_verify(const uint8_t pub_x[32], const uint8_t pub_y[32],
                              const uint8_t hash[32],
                              const uint8_t *sig, uint32_t sig_len);

/* OIDs we recognize for signature algorithms. */
static const uint8_t OID_SHA256_WITH_RSA[] = {
    0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
};
static const uint8_t OID_ECDSA_WITH_SHA256[] = {
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
};
static const uint8_t OID_RSA_ENCRYPTION[] = {
    0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01
};

/* Walk the top of a Certificate and locate its three signed pieces:
 *
 *   Certificate ::= SEQUENCE {
 *       tbsCertificate       TBSCertificate,
 *       signatureAlgorithm   AlgorithmIdentifier,
 *       signature            BIT STRING
 *   }
 *
 * We need (a) the raw TBSCertificate bytes (to hash), (b) the
 * signature algorithm OID body (to dispatch RSA vs ECDSA), and
 * (c) the signature value (without the BIT STRING's leading
 * unused-bits byte). */
typedef struct {
    const uint8_t *tbs;       uint32_t tbs_len;     /* bytes to hash */
    const uint8_t *sigalg;    uint32_t sigalg_len;  /* AlgorithmIdentifier body */
    const uint8_t *sig;       uint32_t sig_len;     /* bare signature bytes */
} cert_parts_t;

static int cert_split(const uint8_t *cert, uint32_t cert_len, cert_parts_t *out)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;

    const uint8_t *outer_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;

    /* TBSCertificate: SEQUENCE — we need to record where the
     * SEQUENCE tag starts and where it ends so we can hash the
     * whole tag+len+body. */
    const uint8_t *tbs_start = p;
    const uint8_t *tbs_end_marker;
    if (der_enter(&p, outer_end, 0x30, &tbs_end_marker) < 0) return -1;
    p = tbs_end_marker;   /* skip tbs body */
    out->tbs     = tbs_start;
    out->tbs_len = (uint32_t)(p - tbs_start);

    /* signatureAlgorithm: SEQUENCE — for verify we just need the
     * inner OID body (the first OID inside the SEQUENCE). */
    const uint8_t *alg_start = p;
    const uint8_t *alg_end;
    if (der_enter(&p, outer_end, 0x30, &alg_end) < 0) return -1;
    out->sigalg     = alg_start;
    out->sigalg_len = (uint32_t)(alg_end - alg_start);
    p = alg_end;

    /* signature: BIT STRING (tag 0x03).  The first content byte is
     * the unused-bits count, which is 0 for X.509 sigs.  Skip it. */
    if (p >= outer_end || *p != 0x03) return -1;
    p++;
    uint32_t bit_len;
    if (der_read_len(&p, outer_end, &bit_len) < 0) return -1;
    if (p + bit_len > outer_end || bit_len < 1) return -1;
    if (*p != 0x00) return -1;            /* unused-bits should be 0 */
    out->sig     = p + 1;
    out->sig_len = bit_len - 1;
    return 0;
}

/* Determine which signature algorithm the cert was signed with by
 * matching its sigalg AlgorithmIdentifier against our known OIDs. */
typedef enum {
    SIG_UNKNOWN = 0,
    SIG_RSA_SHA256,
    SIG_ECDSA_P256_SHA256,
} sig_alg_t;

static sig_alg_t sigalg_recognize(const cert_parts_t *cp)
{
    /* sigalg points at the OUTER SEQUENCE tag.  Skip into its body
     * to find the OID. */
    const uint8_t *p   = cp->sigalg;
    const uint8_t *end = cp->sigalg + cp->sigalg_len;
    const uint8_t *body_end;
    if (der_enter(&p, end, 0x30, &body_end) < 0) return SIG_UNKNOWN;
    uint32_t avail = (uint32_t)(body_end - p);
    if (avail >= sizeof OID_SHA256_WITH_RSA &&
        !bytes_eq(p, OID_SHA256_WITH_RSA, sizeof OID_SHA256_WITH_RSA))
        return SIG_RSA_SHA256;
    if (avail >= sizeof OID_ECDSA_WITH_SHA256 &&
        !bytes_eq(p, OID_ECDSA_WITH_SHA256, sizeof OID_ECDSA_WITH_SHA256))
        return SIG_ECDSA_P256_SHA256;
    return SIG_UNKNOWN;
}

/* Extract the RSA modulus + exponent from an issuer cert's SPKI.
 * Returns 0 with `n_out` and `e_out` pointing into `cert`. */
static int extract_rsa_pubkey(const uint8_t *cert, uint32_t cert_len,
                              const uint8_t **n_out, uint32_t *n_len_out,
                              const uint8_t **e_out, uint32_t *e_len_out)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    const uint8_t *tbs_end;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    /* Optional version */
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* serial, sigAlg, issuer, validity, subject (5 TLVs) */
    for (int i = 0; i < 5; i++)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* SPKI SEQUENCE */
    const uint8_t *spki_end;
    if (der_enter(&p, tbs_end, 0x30, &spki_end) < 0) return -1;
    /* AlgorithmIdentifier — must be rsaEncryption */
    const uint8_t *alg_end;
    if (der_enter(&p, spki_end, 0x30, &alg_end) < 0) return -1;
    if ((uint32_t)(alg_end - p) < sizeof OID_RSA_ENCRYPTION ||
        bytes_eq(p, OID_RSA_ENCRYPTION, sizeof OID_RSA_ENCRYPTION))
        return -1;
    p = alg_end;
    /* subjectPublicKey BIT STRING */
    if (p >= spki_end || *p != 0x03) return -1;
    p++;
    uint32_t bs_len;
    if (der_read_len(&p, spki_end, &bs_len) < 0) return -1;
    if (p + bs_len > spki_end || bs_len < 1) return -1;
    if (*p != 0x00) return -1;
    p++; bs_len--;
    /* Body is RSAPublicKey ::= SEQUENCE { modulus INTEGER, exponent INTEGER } */
    const uint8_t *rpk_end;
    if (der_enter(&p, p + bs_len, 0x30, &rpk_end) < 0) return -1;
    /* modulus INTEGER */
    if (p >= rpk_end || *p != 0x02) return -1;
    p++;
    uint32_t mlen;
    if (der_read_len(&p, rpk_end, &mlen) < 0) return -1;
    if (p + mlen > rpk_end) return -1;
    /* DER INTEGER may have a leading 0x00 sign byte for positive
     * values whose MSB would otherwise be set.  Strip it. */
    const uint8_t *mp = p;
    if (mlen > 1 && mp[0] == 0x00) { mp++; mlen--; }
    *n_out = mp;
    *n_len_out = mlen;
    p += mlen;
    if (mlen != 256) { /* Not RSA-2048 — bail. */ return -1; }
    /* publicExponent INTEGER */
    if (p >= rpk_end || *p != 0x02) return -1;
    p++;
    uint32_t elen;
    if (der_read_len(&p, rpk_end, &elen) < 0) return -1;
    if (p + elen > rpk_end || elen == 0 || elen > 8) return -1;
    *e_out = p;
    *e_len_out = elen;
    return 0;
}

int x509_verify_chain_link(const uint8_t *child_cert, uint32_t child_len,
                           const uint8_t *issuer_cert, uint32_t issuer_len)
{
    cert_parts_t cp;
    if (cert_split(child_cert, child_len, &cp) < 0) {
        serial_puts("[X509] chain: cert split failed\n");
        return -1;
    }
    sig_alg_t sa = sigalg_recognize(&cp);
    if (sa == SIG_UNKNOWN) {
        serial_puts("[X509] chain: unrecognized signature algorithm\n");
        return -1;
    }

    uint8_t hash[32];
    sha256(cp.tbs, cp.tbs_len, hash);

    if (sa == SIG_RSA_SHA256) {
        const uint8_t *n, *e;
        uint32_t n_len, e_len;
        if (extract_rsa_pubkey(issuer_cert, issuer_len,
                               &n, &n_len, &e, &e_len) < 0) {
            serial_puts("[X509] chain: issuer RSA pubkey extract failed\n");
            return -1;
        }
        if (rsa_pkcs1_v15_sha256_verify(cp.sig, cp.sig_len,
                                        n, n_len, e, e_len, hash) < 0) {
            serial_puts("[X509] chain: RSA-SHA256 verify FAILED\n");
            return -1;
        }
        serial_puts("[X509] chain: RSA-SHA256 link verified\n");
        return 0;
    }

    if (sa == SIG_ECDSA_P256_SHA256) {
        uint8_t pub_x[32], pub_y[32];
        if (x509_extract_ec_pubkey(issuer_cert, issuer_len, pub_x, pub_y) < 0) {
            serial_puts("[X509] chain: issuer EC pubkey extract failed\n");
            return -1;
        }
        if (ecdsa_p256_verify(pub_x, pub_y, hash, cp.sig, cp.sig_len) != 0) {
            serial_puts("[X509] chain: ECDSA-P256 verify FAILED\n");
            return -1;
        }
        serial_puts("[X509] chain: ECDSA-P256 link verified\n");
        return 0;
    }

    return -1;
}
