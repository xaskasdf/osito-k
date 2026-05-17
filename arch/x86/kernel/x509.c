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
extern void sha384(const uint8_t *data, uint64_t len, uint8_t out[48]);
extern int  rsa_pkcs1_v15_sha256_verify(const uint8_t *sig, uint32_t sig_len,
                                         const uint8_t *n,   uint32_t n_len,
                                         const uint8_t *e,   uint32_t e_len,
                                         const uint8_t hash[32]);
extern int  rsa_pkcs1_v15_sha384_verify(const uint8_t *sig, uint32_t sig_len,
                                         const uint8_t *n,   uint32_t n_len,
                                         const uint8_t *e,   uint32_t e_len,
                                         const uint8_t hash[48]);
extern int  ecdsa_p256_verify(const uint8_t pub_x[32], const uint8_t pub_y[32],
                              const uint8_t hash[32],
                              const uint8_t *sig, uint32_t sig_len);
extern int  ecdsa_p384_verify(const uint8_t pub_x[48], const uint8_t pub_y[48],
                              const uint8_t hash[48],
                              const uint8_t *sig, uint32_t sig_len);

/* OIDs we recognize for signature algorithms. */
static const uint8_t OID_SHA256_WITH_RSA[] = {
    0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
};
static const uint8_t OID_SHA384_WITH_RSA[] = {
    0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0C
};
static const uint8_t OID_ECDSA_WITH_SHA256[] = {
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
};
static const uint8_t OID_ECDSA_WITH_SHA384[] = {
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x03
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
    SIG_RSA_SHA384,
    SIG_ECDSA_P256_SHA256,
    SIG_ECDSA_P256_SHA384,   /* P-256 key + SHA-384 hash (truncated) */
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
    if (avail >= sizeof OID_SHA384_WITH_RSA &&
        !bytes_eq(p, OID_SHA384_WITH_RSA, sizeof OID_SHA384_WITH_RSA))
        return SIG_RSA_SHA384;
    if (avail >= sizeof OID_ECDSA_WITH_SHA256 &&
        !bytes_eq(p, OID_ECDSA_WITH_SHA256, sizeof OID_ECDSA_WITH_SHA256))
        return SIG_ECDSA_P256_SHA256;
    if (avail >= sizeof OID_ECDSA_WITH_SHA384 &&
        !bytes_eq(p, OID_ECDSA_WITH_SHA384, sizeof OID_ECDSA_WITH_SHA384))
        return SIG_ECDSA_P256_SHA384;
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

    /* Compute the hash matching the sig algorithm.  For ECDSA-P256
     * with SHA-384, the 48-byte digest is truncated to the leftmost
     * 32 bytes (curve order width) per FIPS 186-4 §6.4. */
    uint8_t hash32[32];
    uint8_t hash48[48];
    bool use_sha384 = (sa == SIG_RSA_SHA384 || sa == SIG_ECDSA_P256_SHA384);
    if (use_sha384)
        sha384(cp.tbs, cp.tbs_len, hash48);
    else
        sha256(cp.tbs, cp.tbs_len, hash32);

    if (sa == SIG_RSA_SHA256 || sa == SIG_RSA_SHA384) {
        const uint8_t *n, *e;
        uint32_t n_len, e_len;
        if (extract_rsa_pubkey(issuer_cert, issuer_len,
                               &n, &n_len, &e, &e_len) < 0) {
            serial_puts("[X509] chain: issuer RSA pubkey extract failed\n");
            return -1;
        }
        int rc = (sa == SIG_RSA_SHA256)
            ? rsa_pkcs1_v15_sha256_verify(cp.sig, cp.sig_len, n, n_len, e, e_len, hash32)
            : rsa_pkcs1_v15_sha384_verify(cp.sig, cp.sig_len, n, n_len, e, e_len, hash48);
        if (rc < 0) {
            serial_puts(sa == SIG_RSA_SHA256
                ? "[X509] chain: RSA-SHA256 verify FAILED\n"
                : "[X509] chain: RSA-SHA384 verify FAILED\n");
            return -1;
        }
        serial_puts(sa == SIG_RSA_SHA256
            ? "[X509] chain: RSA-SHA256 link verified\n"
            : "[X509] chain: RSA-SHA384 link verified\n");
        return 0;
    }

    if (sa == SIG_ECDSA_P256_SHA256 || sa == SIG_ECDSA_P256_SHA384) {
        /* ECDSA verification — the *curve* is determined by the
         * issuer's pubkey, NOT by the sigalg OID.  Probe both
         * extractors and dispatch to the matching primitive.  P-256
         * first since it's the common case. */
        uint8_t p256x[32], p256y[32];
        if (x509_extract_ec_pubkey(issuer_cert, issuer_len, p256x, p256y) == 0) {
            /* For SHA-384 → P-256, FIPS 186-4 §6.4 leftmost-truncates
             * the 48-byte digest to 32 bytes. */
            const uint8_t *hh = (sa == SIG_ECDSA_P256_SHA256) ? hash32 : hash48;
            if (ecdsa_p256_verify(p256x, p256y, hh, cp.sig, cp.sig_len) != 0) {
                serial_puts(sa == SIG_ECDSA_P256_SHA256
                    ? "[X509] chain: ECDSA-P256-SHA256 verify FAILED\n"
                    : "[X509] chain: ECDSA-P256-SHA384 verify FAILED\n");
                return -1;
            }
            serial_puts(sa == SIG_ECDSA_P256_SHA256
                ? "[X509] chain: ECDSA-P256-SHA256 link verified\n"
                : "[X509] chain: ECDSA-P256-SHA384 link verified\n");
            return 0;
        }
        uint8_t p384x[48], p384y[48];
        if (x509_extract_ec_pubkey_p384(issuer_cert, issuer_len,
                                         p384x, p384y) == 0) {
            /* SHA-256 against P-384 is not used in practice, but
             * support it for completeness: zero-pad the 32-byte
             * hash to 48 (right-align — leftmost bits zero).  The
             * canonical case is SHA-384 → P-384, no truncation. */
            uint8_t hh48[48];
            if (sa == SIG_ECDSA_P256_SHA384) {
                for (int i = 0; i < 48; i++) hh48[i] = hash48[i];
            } else {
                for (int i = 0; i < 16; i++) hh48[i] = 0;
                for (int i = 0; i < 32; i++) hh48[16 + i] = hash32[i];
            }
            if (ecdsa_p384_verify(p384x, p384y, hh48, cp.sig, cp.sig_len) != 0) {
                serial_puts("[X509] chain: ECDSA-P384 verify FAILED\n");
                return -1;
            }
            serial_puts("[X509] chain: ECDSA-P384 link verified\n");
            return 0;
        }
        serial_puts("[X509] chain: issuer EC pubkey extract failed (neither P-256 nor P-384)\n");
        return -1;
    }

    return -1;
}

/* ── Validity-window check (A12.6) ──────────────────────────── */

/* Convert a calendar date (UTC, Gregorian) to Unix seconds. */
static uint32_t civil_to_unix(int year, int month, int day,
                              int hour, int minute, int second)
{
    /* Howard Hinnant's date algorithm — works for years > 1583. */
    int y = year - (month <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
                   + (unsigned)day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int days_since_epoch = era * 146097 + (int)doe - 719468;
    int64_t s = (int64_t)days_since_epoch * 86400
              + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
    if (s < 0) return 0;
    if (s > (int64_t)0xFFFFFFFF) return 0xFFFFFFFF;
    return (uint32_t)s;
}

/* Parse ASCII digit at p, return value or -1. */
static int digit(uint8_t c) {
    return (c >= '0' && c <= '9') ? (int)(c - '0') : -1;
}
/* Read N decimal digits at *p, advance *p, return value or -1. */
static int read_ndigits(const uint8_t **p, const uint8_t *end, int n) {
    if (*p + n > end) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int d = digit((*p)[i]);
        if (d < 0) return -1;
        v = v * 10 + d;
    }
    *p += n;
    return v;
}

/* Parse a UTCTime ("YYMMDDHHMMSSZ", 13 bytes) or GeneralizedTime
 * ("YYYYMMDDHHMMSSZ", 15 bytes) at the current TLV.  Advances *p
 * past the TLV.  Returns 0 + fills *out_unix; -1 on failure. */
static int parse_time_tlv(const uint8_t **p, const uint8_t *end,
                          uint32_t *out_unix)
{
    if (*p >= end) return -1;
    uint8_t tag = *(*p)++;
    uint32_t len;
    if (der_read_len(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    const uint8_t *q = *p;
    const uint8_t *q_end = *p + len;
    *p = q_end;          /* always advance past TLV */

    int year, month, day, hour, minute, second;
    if (tag == 0x17 && len >= 13) {
        /* UTCTime: YYMMDDHHMMSSZ.  RFC 5280 §4.1.2.5.1: YY ∈ 00..49
         * → 2000..2049; YY ∈ 50..99 → 1950..1999. */
        int yy = read_ndigits(&q, q_end, 2);
        if (yy < 0) return -1;
        year = (yy < 50) ? (2000 + yy) : (1900 + yy);
    } else if (tag == 0x18 && len >= 15) {
        /* GeneralizedTime: YYYYMMDDHHMMSSZ. */
        year = read_ndigits(&q, q_end, 4);
        if (year < 0) return -1;
    } else {
        return -1;
    }
    month  = read_ndigits(&q, q_end, 2);
    day    = read_ndigits(&q, q_end, 2);
    hour   = read_ndigits(&q, q_end, 2);
    minute = read_ndigits(&q, q_end, 2);
    second = read_ndigits(&q, q_end, 2);
    if (month < 1 || month > 12 || day < 1 || day > 31) return -1;
    if (hour < 0 || hour > 23) return -1;
    if (minute < 0 || minute > 59 || second < 0 || second > 60) return -1;
    if (q >= q_end || (*q != 'Z' && *q != '+' && *q != '-')) return -1;
    /* We don't handle timezone offsets — RFC 5280 mandates 'Z'. */

    *out_unix = civil_to_unix(year, month, day, hour, minute, second);
    return 0;
}

int x509_check_validity(const uint8_t *cert, uint32_t cert_len, uint32_t now_utc)
{
    if (now_utc == 0) {
        /* No real clock — can't make a judgement.  Caller (pin)
         * still applies; we don't fail closed without a clock. */
        return 0;
    }
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) {
        serial_puts("[X509] validity: outer SEQ failed\n");
        return -1;
    }
    const uint8_t *tbs_end;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) {
        serial_puts("[X509] validity: TBS SEQ failed\n");
        return -1;
    }

    /* Optional [0] EXPLICIT Version */
    if (p < tbs_end && p[0] == 0xA0) {
        if (der_skip_tlv(&p, tbs_end) < 0) {
            serial_puts("[X509] validity: skip version failed\n");
            return -1;
        }
    }
    /* serial, sigAlg, issuer (3 TLVs) — then Validity SEQUENCE. */
    for (int i = 0; i < 3; i++) {
        if (der_skip_tlv(&p, tbs_end) < 0) {
            serial_puts("[X509] validity: skip pre-Validity TLV ");
            serial_putdec((uint64_t)i);
            serial_puts(" failed\n");
            return -1;
        }
    }

    /* Validity ::= SEQUENCE { notBefore Time, notAfter Time }. */
    const uint8_t *val_end;
    if (der_enter(&p, tbs_end, 0x30, &val_end) < 0) {
        serial_puts("[X509] validity: enter Validity SEQ failed (cur=0x");
        if (p < tbs_end) {
            serial_putdec((uint64_t)p[0]);
        }
        serial_puts(")\n");
        return -1;
    }

    uint32_t not_before, not_after;
    if (parse_time_tlv(&p, val_end, &not_before) < 0) {
        serial_puts("[X509] validity: bad notBefore\n");
        return -1;
    }
    if (parse_time_tlv(&p, val_end, &not_after) < 0) {
        serial_puts("[X509] validity: bad notAfter\n");
        return -1;
    }
    if (now_utc < not_before) {
        serial_puts("[X509] validity: not yet valid (now=");
        serial_putdec((uint64_t)now_utc);
        serial_puts(" notBefore=");
        serial_putdec((uint64_t)not_before);
        serial_puts(")\n");
        return -1;
    }
    if (now_utc > not_after) {
        serial_puts("[X509] validity: expired (now=");
        serial_putdec((uint64_t)now_utc);
        serial_puts(" notAfter=");
        serial_putdec((uint64_t)not_after);
        serial_puts(")\n");
        return -1;
    }
    return 0;
}

/* ── P-384 EC pubkey extraction (A12.7) ──────────────────────── */

int x509_extract_ec_pubkey_p384(const uint8_t *cert, uint32_t cert_len,
                                uint8_t pub_x[48], uint8_t pub_y[48])
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    const uint8_t *tbs_end;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    for (int i = 0; i < 5; i++)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;

    const uint8_t *spki_end;
    if (der_enter(&p, tbs_end, 0x30, &spki_end) < 0) return -1;

    /* AlgorithmIdentifier — id-ecPublicKey ‖ secp384r1 */
    static const uint8_t OID_ID_EC_PK[] = {
        0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
    };
    static const uint8_t OID_SECP384R1[] = {
        0x06, 0x05, 0x2B, 0x81, 0x04, 0x00, 0x22
    };
    const uint8_t *alg_end;
    if (der_enter(&p, spki_end, 0x30, &alg_end) < 0) return -1;
    if ((uint32_t)(alg_end - p) < sizeof OID_ID_EC_PK ||
        bytes_eq(p, OID_ID_EC_PK, sizeof OID_ID_EC_PK) != 0) return -1;
    p += sizeof OID_ID_EC_PK;
    if ((uint32_t)(alg_end - p) < sizeof OID_SECP384R1 ||
        bytes_eq(p, OID_SECP384R1, sizeof OID_SECP384R1) != 0) return -1;
    p = alg_end;

    /* subjectPublicKey BIT STRING: unused-bits ‖ 0x04 ‖ X(48) ‖ Y(48) */
    if (p >= spki_end || *p != 0x03) return -1;
    p++;
    uint32_t bs_len;
    if (der_read_len(&p, spki_end, &bs_len) < 0) return -1;
    if (p + bs_len > spki_end || bs_len < 98) return -1;
    if (p[0] != 0x00 || p[1] != 0x04) return -1;
    for (int i = 0; i < 48; i++) pub_x[i] = p[2 + i];
    for (int i = 0; i < 48; i++) pub_y[i] = p[50 + i];
    return 0;
}

/* ── SAN / hostname matching (A12.8) ──────────────────────────── */

/* Case-insensitive single-byte tolower for ASCII. */
static inline char to_lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* RFC 6125 §6.4.3 dNSName match.  `pat` is a label string from a
 * SAN dNSName; `host` is the user-supplied hostname.  Both are
 * matched case-insensitively.  Wildcards (`*`) may appear in the
 * leftmost label of `pat` only, and consume exactly one label of
 * `host` — no empty match, no multi-label match. */
static int san_dns_match(const char *pat, uint32_t pat_len,
                         const char *host, uint32_t host_len)
{
    if (pat_len == 0 || host_len == 0) return 0;

    /* Find first '.' in pat to identify the leftmost label. */
    uint32_t first_dot = 0;
    while (first_dot < pat_len && pat[first_dot] != '.') first_dot++;

    bool has_wild = (first_dot >= 1 && pat[0] == '*' && first_dot == 1);
    if (!has_wild) {
        /* Plain equality, case-insensitive. */
        if (pat_len != host_len) return 0;
        for (uint32_t i = 0; i < pat_len; i++)
            if (to_lower_ascii(pat[i]) != to_lower_ascii(host[i])) return 0;
        return 1;
    }

    /* Wildcard: pat = "*" || rest_of_pat (starting at first_dot).
     * Match host's leftmost label, then require rest_of_host ==
     * rest_of_pat (case-insensitive).  Empty leftmost label is not
     * allowed (RFC 6125: "presented name MUST NOT contain a
     * wildcard character (e.g., '*') that matches a public suffix"
     * — we don't enforce public-suffix list, just the empty-label
     * guard). */
    uint32_t host_dot = 0;
    while (host_dot < host_len && host[host_dot] != '.') host_dot++;
    if (host_dot == 0) return 0;                 /* empty label */
    if (host_dot == host_len) return 0;          /* host has no dot */
    uint32_t pat_rest_len = pat_len - first_dot;
    uint32_t host_rest_len = host_len - host_dot;
    if (pat_rest_len != host_rest_len) return 0;
    for (uint32_t i = 0; i < pat_rest_len; i++)
        if (to_lower_ascii(pat[first_dot + i]) !=
            to_lower_ascii(host[host_dot + i])) return 0;
    return 1;
}

/* Find the extensions block ([3] EXPLICIT tag inside TBSCertificate)
 * and return pointers to its content (the inner Extensions SEQUENCE).
 * Returns 0 on success, -1 if the cert has no extensions block. */
static int find_extensions(const uint8_t *cert, uint32_t cert_len,
                           const uint8_t **ext_seq_start,
                           const uint8_t **ext_seq_end)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end, *tbs_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* serial, sigAlg, issuer, validity, subject, SPKI = 6 TLVs */
    for (int i = 0; i < 6; i++)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* Optional issuerUniqueID [1] and subjectUniqueID [2] before
     * the extensions [3] block. */
    while (p < tbs_end) {
        uint8_t tag = *p;
        if (tag == 0xA3) {
            /* Found extensions: enter the [3] explicit wrapper. */
            const uint8_t *exp_end;
            if (der_enter(&p, tbs_end, 0xA3, &exp_end) < 0) return -1;
            /* Inside [3]: a single SEQUENCE OF Extension. */
            if (der_enter(&p, exp_end, 0x30, ext_seq_end) < 0) return -1;
            *ext_seq_start = p;
            return 0;
        }
        if (tag == 0x81 || tag == 0x82) {
            if (der_skip_tlv(&p, tbs_end) < 0) return -1;
            continue;
        }
        /* Unknown tag in this position — bail. */
        break;
    }
    return -1;
}

/* Find the Subject CN value (case insensitive match in matcher).
 * The CN OID is 2.5.4.3 = 06 03 55 04 03. */
static int find_subject_cn(const uint8_t *cert, uint32_t cert_len,
                           const uint8_t **cn_str, uint32_t *cn_len)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end, *tbs_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* serial, sigAlg, issuer, validity (4 TLVs) → subject SEQUENCE */
    for (int i = 0; i < 4; i++)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    const uint8_t *subj_end;
    if (der_enter(&p, tbs_end, 0x30, &subj_end) < 0) return -1;

    /* Subject is a SEQUENCE of RDN (SET OF AttributeTypeAndValue). */
    static const uint8_t OID_CN[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };
    while (p < subj_end) {
        const uint8_t *rdn_end;
        if (der_enter(&p, subj_end, 0x31, &rdn_end) < 0) return -1;
        while (p < rdn_end) {
            const uint8_t *atv_end;
            if (der_enter(&p, rdn_end, 0x30, &atv_end) < 0) return -1;
            /* OID */
            if ((uint32_t)(atv_end - p) >= sizeof OID_CN &&
                bytes_eq(p, OID_CN, sizeof OID_CN) == 0) {
                p += sizeof OID_CN;
                /* Value is a tag-prefixed string — PrintableString
                 * (0x13), UTF8String (0x0c), or others.  Just read
                 * the body bytes. */
                if (p >= atv_end) return -1;
                p++;                                    /* skip tag */
                uint32_t v_len;
                if (der_read_len(&p, atv_end, &v_len) < 0) return -1;
                if (p + v_len > atv_end) return -1;
                *cn_str = p;
                *cn_len = v_len;
                return 0;
            }
            p = atv_end;
        }
    }
    return -1;
}

int x509_match_hostname(const uint8_t *cert, uint32_t cert_len,
                        const char *hostname)
{
    if (!hostname || hostname[0] == 0) return -1;
    uint32_t host_len = 0;
    while (hostname[host_len]) host_len++;

    const uint8_t *ext_p, *ext_end;
    int have_san = 0;
    int got_match = 0;
    if (find_extensions(cert, cert_len, &ext_p, &ext_end) == 0) {
        static const uint8_t OID_SAN[] = { 0x06, 0x03, 0x55, 0x1d, 0x11 };
        while (ext_p < ext_end) {
            const uint8_t *ex_end;
            if (der_enter(&ext_p, ext_end, 0x30, &ex_end) < 0) break;
            const uint8_t *probe = ext_p;
            const uint8_t *probe_end = ex_end;
            /* AlgorithmIdentifier-like: OID, optional BOOLEAN
             * (critical), OCTET STRING (value). */
            if ((uint32_t)(probe_end - probe) < sizeof OID_SAN ||
                bytes_eq(probe, OID_SAN, sizeof OID_SAN) != 0) {
                ext_p = ex_end;
                continue;
            }
            probe += sizeof OID_SAN;
            if (probe < probe_end && *probe == 0x01) {
                /* critical BOOLEAN — skip */
                if (der_skip_tlv(&probe, probe_end) < 0) break;
            }
            /* OCTET STRING wrapping the GeneralNames SEQUENCE. */
            if (probe >= probe_end || *probe != 0x04) { ext_p = ex_end; continue; }
            probe++;
            uint32_t os_len;
            if (der_read_len(&probe, probe_end, &os_len) < 0) { ext_p = ex_end; continue; }
            if (probe + os_len > probe_end) { ext_p = ex_end; continue; }
            const uint8_t *gn_start = probe;
            const uint8_t *gn_end_outer = probe + os_len;

            /* GeneralNames SEQUENCE. */
            const uint8_t *gn_seq_end;
            const uint8_t *gp = gn_start;
            if (der_enter(&gp, gn_end_outer, 0x30, &gn_seq_end) < 0) {
                ext_p = ex_end; continue;
            }
            have_san = 1;
            while (gp < gn_seq_end) {
                uint8_t tag = *gp;
                gp++;
                uint32_t glen;
                if (der_read_len(&gp, gn_seq_end, &glen) < 0) break;
                if (gp + glen > gn_seq_end) break;
                if (tag == 0x82) {
                    /* [2] dNSName IA5String */
                    if (san_dns_match((const char *)gp, glen,
                                       hostname, host_len)) {
                        got_match = 1;
                        break;
                    }
                }
                gp += glen;
            }
            if (got_match) break;
            ext_p = ex_end;
        }
    }

    if (got_match) return 0;
    if (have_san)  return -1;       /* SAN present → don't fall to CN */

    /* No SAN — fall back to CN matching. */
    const uint8_t *cn; uint32_t cn_len;
    if (find_subject_cn(cert, cert_len, &cn, &cn_len) == 0) {
        if (san_dns_match((const char *)cn, cn_len, hostname, host_len))
            return 0;
    }
    return -1;
}

/* ── v3 extension parsing (A12.10) ──────────────────────────── */

static const uint8_t OID_BASIC_CONSTRAINTS[] = {
    0x06, 0x03, 0x55, 0x1d, 0x13
};
static const uint8_t OID_KEY_USAGE[] = {
    0x06, 0x03, 0x55, 0x1d, 0x0f
};

/* Walk a single Extension TLV body to identify which of the v3
 * extensions we care about it is, and populate `out` accordingly.
 * `ext_body` points at the contents of the Extension SEQUENCE
 * (i.e. just past the outer 0x30/length tag-and-length). */
static void parse_one_extension(const uint8_t *ext_body, uint32_t ext_len,
                                x509_v3_t *out)
{
    const uint8_t *p = ext_body;
    const uint8_t *end = ext_body + ext_len;

    /* Extension OID. */
    if (p >= end || *p != 0x06) return;
    const uint8_t *oid_p = p;
    uint32_t oid_full_len;
    /* tag(1) + length-octets(1+) + content */
    if (p + 1 >= end) return;
    p++;
    if (der_read_len(&p, end, &oid_full_len) < 0) return;
    if (p + oid_full_len > end) return;
    /* Reconstruct {tag, length, body} comparator block for OID matching. */
    /* Easier: just look at oid_p[0..1+sizeof len+body] vs known prefixes. */
    /* For simplicity, advance past the OID and compare a few known
     * 5-byte OIDs against the full TLV (`oid_p[0..5)` since our two
     * extensions are short). */
    p += oid_full_len;

    bool is_bc = (uint32_t)(p - oid_p) >= sizeof OID_BASIC_CONSTRAINTS
              && bytes_eq(oid_p, OID_BASIC_CONSTRAINTS,
                           sizeof OID_BASIC_CONSTRAINTS) == 0;
    bool is_ku = (uint32_t)(p - oid_p) >= sizeof OID_KEY_USAGE
              && bytes_eq(oid_p, OID_KEY_USAGE, sizeof OID_KEY_USAGE) == 0;
    if (!is_bc && !is_ku) return;

    /* Optional critical BOOLEAN — skip if present. */
    if (p < end && *p == 0x01) {
        if (der_skip_tlv(&p, end) < 0) return;
    }

    /* OCTET STRING wrapping the actual extension value. */
    if (p >= end || *p != 0x04) return;
    p++;
    uint32_t os_len;
    if (der_read_len(&p, end, &os_len) < 0) return;
    if (p + os_len > end) return;
    const uint8_t *val = p;
    const uint8_t *val_end = p + os_len;

    if (is_bc) {
        /* BasicConstraints ::= SEQUENCE {
         *   cA  BOOLEAN DEFAULT FALSE,
         *   pathLenConstraint INTEGER (0..MAX) OPTIONAL
         * } */
        out->has_bc = true;
        const uint8_t *bc_p = val;
        const uint8_t *bc_end;
        if (der_enter(&bc_p, val_end, 0x30, &bc_end) < 0) return;
        if (bc_p < bc_end && *bc_p == 0x01) {
            /* cA BOOLEAN */
            bc_p++;
            uint32_t bl;
            if (der_read_len(&bc_p, bc_end, &bl) < 0) return;
            if (bl >= 1 && bc_p[0] != 0x00) out->is_ca = true;
            bc_p += bl;
        }
        if (bc_p < bc_end && *bc_p == 0x02) {
            /* pathLenConstraint INTEGER */
            bc_p++;
            uint32_t il;
            if (der_read_len(&bc_p, bc_end, &il) < 0) return;
            if (il > 0 && il <= 4) {
                int v = 0;
                for (uint32_t i = 0; i < il; i++) v = (v << 8) | bc_p[i];
                out->path_len = v;
            }
        }
    } else if (is_ku) {
        /* KeyUsage ::= BIT STRING.  Tag 0x03, then unused-bits + bits. */
        out->has_ku = true;
        if (val + 2 >= val_end || *val != 0x03) return;
        uint32_t bs_len;
        const uint8_t *kp = val + 1;
        if (der_read_len(&kp, val_end, &bs_len) < 0) return;
        if (kp + bs_len > val_end || bs_len < 2) return;
        uint8_t unused = kp[0];
        const uint8_t *bits = kp + 1;
        uint32_t bit_count = (bs_len - 1) * 8 - unused;
        uint32_t flags = 0;
        for (uint32_t i = 0; i < bit_count && i < 9; i++) {
            uint8_t byte = bits[i / 8];
            uint8_t mask = (uint8_t)(0x80 >> (i % 8));
            if (byte & mask) flags |= ((uint32_t)1 << i);
        }
        out->key_usage_flags = flags;
    }
}

int x509_parse_v3(const uint8_t *cert, uint32_t cert_len, x509_v3_t *out)
{
    out->has_bc = false;
    out->is_ca = false;
    out->path_len = -1;
    out->has_ku = false;
    out->key_usage_flags = 0;

    const uint8_t *ext_seq_start, *ext_seq_end;
    if (find_extensions(cert, cert_len, &ext_seq_start, &ext_seq_end) < 0)
        return 0;   /* no extensions block — leave defaults */

    const uint8_t *p = ext_seq_start;
    while (p < ext_seq_end) {
        const uint8_t *ex_end;
        if (der_enter(&p, ext_seq_end, 0x30, &ex_end) < 0) break;
        parse_one_extension(p, (uint32_t)(ex_end - p), out);
        p = ex_end;
    }
    return 0;
}

int x509_check_chain_constraints(const uint8_t **chain,
                                 const uint32_t *chain_lens,
                                 uint32_t        count)
{
    if (count == 0) return -1;
    /* Walk intermediates: chain[1..count-1].  The leaf chain[0]
     * generally doesn't have CA=TRUE; the root chain[count-1]
     * may or may not be in the chain (we still check if present). */
    for (uint32_t i = 1; i < count; i++) {
        x509_v3_t v3;
        x509_parse_v3(chain[i], chain_lens[i], &v3);

        if (!v3.has_bc || !v3.is_ca) {
            serial_puts("[X509] chain constraint: cert#");
            serial_putdec((uint64_t)i);
            serial_puts(" lacks BasicConstraints.cA=TRUE\n");
            return -1;
        }
        /* If KeyUsage is present (most intermediates), it MUST
         * include keyCertSign — the right to sign other certs. */
        if (v3.has_ku && !(v3.key_usage_flags & X509_KU_KEY_CERT_SIGN)) {
            serial_puts("[X509] chain constraint: cert#");
            serial_putdec((uint64_t)i);
            serial_puts(" KeyUsage lacks keyCertSign\n");
            return -1;
        }
        /* pathLenConstraint check: the number of NON-self-issued
         * intermediates between this cert and the leaf must be
         * ≤ path_len.  Count of intermediates below this one =
         * (i - 1).  RFC 5280 §4.2.1.9. */
        if (v3.path_len >= 0 && (int)(i - 1) > v3.path_len) {
            serial_puts("[X509] chain constraint: cert#");
            serial_putdec((uint64_t)i);
            serial_puts(" pathLenConstraint violated\n");
            return -1;
        }
    }
    return 0;
}

/* ── AIA extension parsing (A12.11) ─────────────────────────── */

static const uint8_t OID_AIA[] = {
    0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x01, 0x01
};
static const uint8_t OID_AD_CA_ISSUERS[] = {
    0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x30, 0x02
};
static const uint8_t OID_AD_OCSP[] = {
    0x06, 0x08, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x07, 0x30, 0x01
};

/* Walk the AIA extension's AccessDescription list and look for the
 * given access-method OID.  On match, copy the URI (tag 0x86 [6]
 * IMPLICIT IA5String for the uniformResourceIdentifier choice) into
 * `out`.  Returns URL length on match, -1 otherwise. */
static int aia_get_url(const uint8_t *cert, uint32_t cert_len,
                       const uint8_t *want_oid, uint32_t want_oid_len,
                       char *out, uint32_t cap)
{
    const uint8_t *ext_seq_start, *ext_seq_end;
    if (find_extensions(cert, cert_len, &ext_seq_start, &ext_seq_end) < 0)
        return -1;
    const uint8_t *p = ext_seq_start;
    while (p < ext_seq_end) {
        const uint8_t *ex_end;
        if (der_enter(&p, ext_seq_end, 0x30, &ex_end) < 0) break;
        const uint8_t *probe = p;
        /* Extension OID must match AIA. */
        if ((uint32_t)(ex_end - probe) < sizeof OID_AIA ||
            bytes_eq(probe, OID_AIA, sizeof OID_AIA) != 0) {
            p = ex_end; continue;
        }
        probe += sizeof OID_AIA;
        /* Optional critical BOOLEAN. */
        if (probe < ex_end && *probe == 0x01)
            if (der_skip_tlv(&probe, ex_end) < 0) { p = ex_end; continue; }
        /* OCTET STRING wrapping the AIA SEQUENCE. */
        if (probe >= ex_end || *probe != 0x04) { p = ex_end; continue; }
        probe++;
        uint32_t os_len;
        if (der_read_len(&probe, ex_end, &os_len) < 0) { p = ex_end; continue; }
        if (probe + os_len > ex_end) { p = ex_end; continue; }
        const uint8_t *aia_p = probe;
        const uint8_t *aia_outer_end = probe + os_len;

        /* AIA ::= SEQUENCE OF AccessDescription */
        const uint8_t *aia_seq_end;
        if (der_enter(&aia_p, aia_outer_end, 0x30, &aia_seq_end) < 0) {
            p = ex_end; continue;
        }
        while (aia_p < aia_seq_end) {
            const uint8_t *ad_end;
            if (der_enter(&aia_p, aia_seq_end, 0x30, &ad_end) < 0) break;
            /* AccessDescription ::= SEQUENCE {
             *   accessMethod   OBJECT IDENTIFIER,
             *   accessLocation GeneralName  }
             *
             * accessLocation has tag 0x86 ([6] IMPLICIT IA5String)
             * when the choice is uniformResourceIdentifier. */
            if ((uint32_t)(ad_end - aia_p) < want_oid_len ||
                bytes_eq(aia_p, want_oid, want_oid_len) != 0) {
                aia_p = ad_end; continue;
            }
            aia_p += want_oid_len;
            if (aia_p >= ad_end || *aia_p != 0x86) { aia_p = ad_end; continue; }
            aia_p++;
            uint32_t url_len;
            if (der_read_len(&aia_p, ad_end, &url_len) < 0) return -1;
            if (aia_p + url_len > ad_end) return -1;
            /* Copy with NUL terminator. */
            uint32_t take = (url_len + 1 > cap) ? (cap - 1) : url_len;
            for (uint32_t i = 0; i < take; i++) out[i] = (char)aia_p[i];
            if (cap > 0) out[take] = 0;
            return (int)url_len;
        }
        return -1;
    }
    return -1;
}

int x509_get_aia_caissuers(const uint8_t *cert, uint32_t cert_len,
                          char *out, uint32_t cap)
{
    return aia_get_url(cert, cert_len,
                       OID_AD_CA_ISSUERS, sizeof OID_AD_CA_ISSUERS,
                       out, cap);
}
int x509_get_aia_ocsp(const uint8_t *cert, uint32_t cert_len,
                     char *out, uint32_t cap)
{
    return aia_get_url(cert, cert_len,
                       OID_AD_OCSP, sizeof OID_AD_OCSP,
                       out, cap);
}

/* ── CRL Distribution Points (RFC 5280 §4.2.1.13) ─────────────── */

/* OID 2.5.29.31 = 06 03 55 1D 1F */
static const uint8_t OID_CRLDP[] = {
    0x06, 0x03, 0x55, 0x1D, 0x1F
};

/* Walk a GeneralNames SEQUENCE looking for the first [6] IMPLICIT
 * IA5String entry (uniformResourceIdentifier).  Copies into `out`
 * (NUL-terminated, truncated to cap) and returns URL length.  -1
 * if no URI entry found. */
static int gn_extract_uri(const uint8_t *p, const uint8_t *end,
                          char *out, uint32_t cap)
{
    while (p < end) {
        uint8_t tag = *p;
        if (tag == 0x86) {
            p++;
            uint32_t url_len;
            if (der_read_len(&p, end, &url_len) < 0) return -1;
            if (p + url_len > end) return -1;
            uint32_t take = (url_len + 1 > cap) ? (cap - 1) : url_len;
            for (uint32_t i = 0; i < take; i++) out[i] = (char)p[i];
            if (cap > 0) out[take] = 0;
            return (int)url_len;
        }
        if (der_skip_tlv(&p, end) < 0) return -1;
    }
    return -1;
}

int x509_get_crldp_url(const uint8_t *cert, uint32_t cert_len,
                       char *out, uint32_t cap)
{
    const uint8_t *ext_seq_start, *ext_seq_end;
    if (find_extensions(cert, cert_len, &ext_seq_start, &ext_seq_end) < 0)
        return -1;
    const uint8_t *p = ext_seq_start;
    while (p < ext_seq_end) {
        const uint8_t *ex_end;
        if (der_enter(&p, ext_seq_end, 0x30, &ex_end) < 0) break;
        const uint8_t *probe = p;
        if ((uint32_t)(ex_end - probe) < sizeof OID_CRLDP ||
            bytes_eq(probe, OID_CRLDP, sizeof OID_CRLDP) != 0) {
            p = ex_end; continue;
        }
        probe += sizeof OID_CRLDP;
        /* Optional critical BOOLEAN. */
        if (probe < ex_end && *probe == 0x01)
            if (der_skip_tlv(&probe, ex_end) < 0) { p = ex_end; continue; }
        /* OCTET STRING wrapping the CRLDP SEQUENCE. */
        if (probe >= ex_end || *probe != 0x04) { p = ex_end; continue; }
        probe++;
        uint32_t os_len;
        if (der_read_len(&probe, ex_end, &os_len) < 0) { p = ex_end; continue; }
        if (probe + os_len > ex_end) { p = ex_end; continue; }
        const uint8_t *cdp_p = probe;
        const uint8_t *cdp_outer_end = probe + os_len;
        /* SEQUENCE OF DistributionPoint */
        const uint8_t *cdp_seq_end;
        if (der_enter(&cdp_p, cdp_outer_end, 0x30, &cdp_seq_end) < 0) {
            p = ex_end; continue;
        }
        while (cdp_p < cdp_seq_end) {
            const uint8_t *dp_end;
            if (der_enter(&cdp_p, cdp_seq_end, 0x30, &dp_end) < 0) break;
            /* DistributionPoint fields are all [n] EXPLICIT.  We want
             * [0] distributionPoint → [0] fullName → GeneralNames. */
            if (cdp_p >= dp_end || *cdp_p != 0xA0) { cdp_p = dp_end; continue; }
            const uint8_t *dpname_end;
            if (der_enter(&cdp_p, dp_end, 0xA0, &dpname_end) < 0) {
                cdp_p = dp_end; continue;
            }
            /* Inside [0] DistributionPointName CHOICE — fullName is
             * [0] IMPLICIT GeneralNames. */
            if (cdp_p >= dpname_end || *cdp_p != 0xA0) {
                cdp_p = dp_end; continue;
            }
            const uint8_t *fn_end;
            if (der_enter(&cdp_p, dpname_end, 0xA0, &fn_end) < 0) {
                cdp_p = dp_end; continue;
            }
            int r = gn_extract_uri(cdp_p, fn_end, out, cap);
            if (r > 0) return r;
            cdp_p = dp_end;
        }
        return -1;
    }
    return -1;
}

/* Issuer DN extraction: return raw DER bytes of the entire Issuer
 * SEQUENCE TLV (including the SEQUENCE tag + length).  Used by
 * OCSP CertID building (issuer name hash = SHA-1 of these bytes). */
int x509_get_issuer_der(const uint8_t *cert, uint32_t cert_len,
                        const uint8_t **out_ptr, uint32_t *out_len)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end, *tbs_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    /* Skip optional version + serial + sigAlg. */
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    for (int i = 0; i < 2; i++)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* Issuer Name = SEQUENCE OF RDN.  Capture the whole TLV. */
    if (p >= tbs_end || *p != 0x30) return -1;
    const uint8_t *iss_start = p;
    p++;
    uint32_t il;
    if (der_read_len(&p, tbs_end, &il) < 0) return -1;
    if (p + il > tbs_end) return -1;
    *out_ptr = iss_start;
    *out_len = (uint32_t)(p + il - iss_start);
    return 0;
}

/* Subject Public Key BIT STRING content (without the tag/length and
 * the leading unused-bits byte).  This is what OCSP CertID hashes
 * for the issuer-key-hash field. */
int x509_get_subject_pubkey_bits(const uint8_t *cert, uint32_t cert_len,
                                 const uint8_t **out_ptr, uint32_t *out_len)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end, *tbs_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* serial, sigAlg, issuer, validity, subject = 5 */
    for (int i = 0; i < 5; i++)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* SubjectPublicKeyInfo */
    const uint8_t *spki_end;
    if (der_enter(&p, tbs_end, 0x30, &spki_end) < 0) return -1;
    /* Skip AlgorithmIdentifier. */
    if (der_skip_tlv(&p, spki_end) < 0) return -1;
    /* subjectPublicKey BIT STRING. */
    if (p >= spki_end || *p != 0x03) return -1;
    p++;
    uint32_t bs_len;
    if (der_read_len(&p, spki_end, &bs_len) < 0) return -1;
    if (p + bs_len > spki_end || bs_len < 1) return -1;
    /* Skip the unused-bits byte (should be 0). */
    *out_ptr = p + 1;
    *out_len = bs_len - 1;
    return 0;
}

int x509_get_serial_number(const uint8_t *cert, uint32_t cert_len,
                           const uint8_t **out_ptr, uint32_t *out_len)
{
    const uint8_t *p = cert;
    const uint8_t *end = cert + cert_len;
    const uint8_t *outer_end, *tbs_end;
    if (der_enter(&p, end, 0x30, &outer_end) < 0) return -1;
    if (der_enter(&p, outer_end, 0x30, &tbs_end) < 0) return -1;
    if (p < tbs_end && p[0] == 0xA0)
        if (der_skip_tlv(&p, tbs_end) < 0) return -1;
    /* serial INTEGER */
    if (p >= tbs_end || *p != 0x02) return -1;
    const uint8_t *s_tag = p;
    p++;
    uint32_t sl;
    if (der_read_len(&p, tbs_end, &sl) < 0) return -1;
    if (p + sl > tbs_end) return -1;
    /* Return value bytes (without the INTEGER tag/length).  Strip
     * leading 0x00 sign byte if present. */
    const uint8_t *vp = p;
    if (sl > 1 && vp[0] == 0x00) { vp++; sl--; }
    *out_ptr = vp;
    *out_len = sl;
    (void)s_tag;
    return 0;
}
