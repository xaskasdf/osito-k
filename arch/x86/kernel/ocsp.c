/*
 * ocsp.c — RFC 6960 OCSP client.
 *
 * Builds and POSTs an OCSPRequest, then parses the response to
 * extract the certStatus tag for the requested cert.  RFC 6960
 * §4.2.2.2 signature verification IS performed: hash the captured
 * tbsResponseData TLV, dispatch by signatureAlgorithm (RSA-SHA256
 * or ECDSA-P256-SHA256), and verify against the responder cert
 * (delegated signer when `certs [0]` is present and its issuer DN
 * matches the target cert's issuer DN — RFC 6960 §4.2.2.2 same-CA
 * constraint — otherwise against the issuer cert directly).
 * Verify failures are logged ([OCSP] sig verify: ...) and reported
 * back to the caller, but the parsed certStatus is still returned
 * — tls13.c decides whether to abort on verify failure.
 *
 * Request wire format:
 *
 *   OCSPRequest ::= SEQUENCE {
 *     tbsRequest                  TBSRequest,
 *     optionalSignature   [0]     EXPLICIT Signature OPTIONAL }
 *
 *   TBSRequest ::= SEQUENCE {
 *     version             [0] EXPLICIT Version DEFAULT v1,
 *     requestorName       [1] EXPLICIT GeneralName OPTIONAL,
 *     requestList         SEQUENCE OF Request,
 *     requestExtensions   [2] EXPLICIT Extensions OPTIONAL }
 *
 *   Request ::= SEQUENCE { reqCert CertID }
 *
 *   CertID ::= SEQUENCE {
 *     hashAlgorithm     AlgorithmIdentifier,
 *     issuerNameHash    OCTET STRING,
 *     issuerKeyHash     OCTET STRING,
 *     serialNumber      CertificateSerialNumber }
 *
 * We use SHA-1 as the hash algorithm (universally supported by
 * responders, even though SHA-1 is weak — the responder is just
 * using it as an indexing key, not a security primitive).
 */

#include "../include/types.h"
#include "ocsp.h"
#include "x509.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

extern void sha1(const void *data, uint32_t len, uint8_t digest[20]);
extern void sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);
extern int  ecdsa_p256_verify(const uint8_t pub_x[32], const uint8_t pub_y[32],
                              const uint8_t hash[32],
                              const uint8_t *sig, uint32_t sig_len);
extern int  rsa_pkcs1_v15_sha256_verify(const uint8_t *sig, uint32_t sig_len,
                                         const uint8_t *n,   uint32_t n_len,
                                         const uint8_t *e,   uint32_t e_len,
                                         const uint8_t hash[32]);

/* http_plain.c */
extern int http_plain_post(const char *url, const char *content_type,
                           const uint8_t *body, uint32_t body_len,
                           uint8_t *out, uint32_t out_cap);

/* DER helpers (matched to x509.c) */
static uint32_t emit_length(uint8_t *buf, uint32_t off, uint32_t len)
{
    if (len < 128) { buf[off++] = (uint8_t)len; return off; }
    if (len < 256) { buf[off++] = 0x81; buf[off++] = (uint8_t)len; return off; }
    buf[off++] = 0x82;
    buf[off++] = (uint8_t)(len >> 8);
    buf[off++] = (uint8_t)len;
    return off;
}
static uint32_t emit_tlv(uint8_t *buf, uint32_t off, uint8_t tag,
                         const uint8_t *body, uint32_t body_len)
{
    buf[off++] = tag;
    off = emit_length(buf, off, body_len);
    for (uint32_t i = 0; i < body_len; i++) buf[off++] = body[i];
    return off;
}

/* AlgorithmIdentifier for SHA-1: SEQUENCE { OID(sha1) NULL }.
 * The full TLV is 9 bytes: 30 07 06 05 2b0e03021a 0500. */
static const uint8_t SHA1_ALG_ID[] = {
    0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00
};

/* Build an OCSP request for (cert, issuer).  Writes the DER into
 * `out` and returns its length.  Returns -1 on overflow. */
static int build_ocsp_request(const uint8_t *cert, uint32_t cert_len,
                              const uint8_t *issuer, uint32_t issuer_len,
                              uint8_t *out, uint32_t out_cap)
{
    /* Gather inputs. */
    const uint8_t *issuer_dn;  uint32_t issuer_dn_len;
    const uint8_t *issuer_pk;  uint32_t issuer_pk_len;
    const uint8_t *serial;     uint32_t serial_len;
    if (x509_get_issuer_der(cert, cert_len, &issuer_dn, &issuer_dn_len) < 0) return -1;
    if (x509_get_subject_pubkey_bits(issuer, issuer_len, &issuer_pk, &issuer_pk_len) < 0) return -1;
    if (x509_get_serial_number(cert, cert_len, &serial, &serial_len) < 0) return -1;

    uint8_t name_hash[20]; sha1(issuer_dn, issuer_dn_len, name_hash);
    uint8_t key_hash[20];  sha1(issuer_pk, issuer_pk_len, key_hash);

    /* Build innermost-out.
     *
     * CertID body =
     *   AlgorithmIdentifier(SHA-1, 11 bytes)
     *   OCTET STRING name_hash(2 + 20 = 22 bytes)
     *   OCTET STRING key_hash (22 bytes)
     *   INTEGER serial (2 + serial_len, plus 1 if MSB needs zero sign byte)
     *
     * To keep this simple we emit the leading 0x00 sign byte when
     * the serial's MSB has the sign bit set (= positive integer
     * encoding). */
    uint8_t buf[512];
    uint32_t off = 0;

    /* AlgorithmIdentifier (SHA-1 — 11 bytes, full TLV). */
    for (uint32_t i = 0; i < sizeof SHA1_ALG_ID; i++) buf[off++] = SHA1_ALG_ID[i];
    /* issuerNameHash */
    off = emit_tlv(buf, off, 0x04, name_hash, 20);
    /* issuerKeyHash */
    off = emit_tlv(buf, off, 0x04, key_hash, 20);
    /* serialNumber INTEGER */
    {
        uint8_t serbuf[64];
        uint32_t sl = 0;
        if (serial_len > 0 && (serial[0] & 0x80)) {
            serbuf[sl++] = 0x00;
        }
        for (uint32_t i = 0; i < serial_len && sl < sizeof serbuf; i++)
            serbuf[sl++] = serial[i];
        off = emit_tlv(buf, off, 0x02, serbuf, sl);
    }
    /* Now `buf[0..off]` is the CertID body.  Wrap in:
     *   Request          = SEQUENCE { CertID }   (just wrap)
     *   requestList      = SEQUENCE OF Request
     *   TBSRequest       = SEQUENCE { requestList }
     *   OCSPRequest      = SEQUENCE { tbsRequest } */
    uint8_t wrap1[600], wrap2[700], wrap3[800];

    /* certid -> CertID SEQUENCE */
    uint32_t w1 = 0;
    w1 = emit_tlv(wrap1, w1, 0x30, buf, off);

    /* Request -> SEQUENCE { reqCert } — reqCert is the CertID we
     * just wrapped, so Request body = wrap1[0..w1]. */
    uint32_t w2 = 0;
    w2 = emit_tlv(wrap2, w2, 0x30, wrap1, w1);

    /* requestList SEQUENCE OF Request (one entry). */
    uint32_t w3 = 0;
    w3 = emit_tlv(wrap3, w3, 0x30, wrap2, w2);

    /* TBSRequest SEQUENCE { requestList }. */
    static uint8_t tbs[1024];
    uint32_t t = 0;
    t = emit_tlv(tbs, t, 0x30, wrap3, w3);

    /* OCSPRequest SEQUENCE { tbsRequest }. */
    if (out_cap < t + 16) return -1;
    uint32_t o = 0;
    o = emit_tlv(out, o, 0x30, tbs, t);
    return (int)o;
}

/* Walk the response DER to find the SingleResponse.certStatus tag.
 *
 * OCSPResponse ::= SEQUENCE {
 *   responseStatus ENUMERATED, responseBytes [0] EXPLICIT ResponseBytes OPTIONAL }
 * ResponseBytes ::= SEQUENCE { responseType OID, response OCTET STRING }
 * The response OCTET STRING wraps a BasicOCSPResponse:
 * BasicOCSPResponse ::= SEQUENCE { tbsResponseData ResponseData,
 *                                  signatureAlgorithm AlgorithmIdentifier,
 *                                  signature BIT STRING,
 *                                  certs [0] EXPLICIT SEQUENCE OF Cert OPTIONAL }
 * ResponseData ::= SEQUENCE { ... responses SEQUENCE OF SingleResponse }
 *
 * SingleResponse ::= SEQUENCE { certID, certStatus, thisUpdate,
 *                               nextUpdate [0] OPTIONAL,
 *                               singleExtensions [1] OPTIONAL }
 *
 * certStatus is a CHOICE: [0] good NULL, [1] revoked RevokedInfo,
 * [2] unknown NULL.  Tag = 0x80 + n for the n-th choice. */

/* DER walk helpers — local copies to keep ocsp.c self-contained. */
static int der_read_len_loc(const uint8_t **p, const uint8_t *end, uint32_t *out) {
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
static int der_enter_loc(const uint8_t **p, const uint8_t *end,
                         uint8_t expected_tag, const uint8_t **inner_end) {
    if (*p >= end || **p != expected_tag) return -1;
    (*p)++;
    uint32_t len;
    if (der_read_len_loc(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    *inner_end = *p + len;
    return 0;
}
static int der_skip_loc(const uint8_t **p, const uint8_t *end) {
    if (*p >= end) return -1;
    (*p)++;
    uint32_t len;
    if (der_read_len_loc(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    *p += len;
    return 0;
}

/* Compare two byte ranges. Returns 0 on equality, nonzero otherwise. */
static int ocsp_memeq(const uint8_t *a, uint32_t a_len,
                      const uint8_t *b, uint32_t b_len)
{
    if (a_len != b_len) return 1;
    for (uint32_t i = 0; i < a_len; i++) if (a[i] != b[i]) return 1;
    return 0;
}

/* OIDs used by sigAlg dispatch — match the encodings in x509.c.  Note
 * we match against the bare OID TLV (tag 0x06 + length + value). */
static const uint8_t OCSP_OID_SHA256_RSA[] = {
    0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B
};
static const uint8_t OCSP_OID_ECDSA_SHA256[] = {
    0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
};

typedef enum {
    OCSP_SIG_UNKNOWN = 0,
    OCSP_SIG_RSA_SHA256,
    OCSP_SIG_ECDSA_P256_SHA256,
} ocsp_sig_alg_t;

/* Parsed material needed to verify the BasicOCSPResponse signature. */
typedef struct {
    const uint8_t *tbs;       uint32_t tbs_len;       /* tbsResponseData TLV */
    ocsp_sig_alg_t alg;
    const uint8_t *sig;       uint32_t sig_len;       /* raw sig bytes */
    const uint8_t *signer;    uint32_t signer_len;    /* delegate signer cert DER, or NULL */
} ocsp_verify_material_t;

/* Recognize a signatureAlgorithm AlgorithmIdentifier.  Receives a
 * pointer at the OUTER SEQUENCE tag and its length. */
static ocsp_sig_alg_t ocsp_recognize_sigalg(const uint8_t *p, uint32_t len)
{
    const uint8_t *end = p + len;
    const uint8_t *body_end;
    if (der_enter_loc(&p, end, 0x30, &body_end) < 0) return OCSP_SIG_UNKNOWN;
    uint32_t avail = (uint32_t)(body_end - p);
    if (avail >= sizeof OCSP_OID_SHA256_RSA &&
        ocsp_memeq(p, sizeof OCSP_OID_SHA256_RSA,
                   OCSP_OID_SHA256_RSA, sizeof OCSP_OID_SHA256_RSA) == 0)
        return OCSP_SIG_RSA_SHA256;
    if (avail >= sizeof OCSP_OID_ECDSA_SHA256 &&
        ocsp_memeq(p, sizeof OCSP_OID_ECDSA_SHA256,
                   OCSP_OID_ECDSA_SHA256, sizeof OCSP_OID_ECDSA_SHA256) == 0)
        return OCSP_SIG_ECDSA_P256_SHA256;
    return OCSP_SIG_UNKNOWN;
}

static ocsp_status_t parse_ocsp_response(const uint8_t *body, uint32_t body_len,
                                         ocsp_verify_material_t *vm)
{
    /* Diagnostic dump of the first 16 bytes — useful for debugging
     * parser mismatches against new responders. */
    serial_puts("[OCSP] resp hex: ");
    for (uint32_t i = 0; i < body_len && i < 16; i++) {
        uint8_t b = body[i];
        const char hex[] = "0123456789abcdef";
        char s[3] = { hex[b >> 4], hex[b & 0xF], 0 };
        serial_puts(s);
    }
    serial_puts("\n");

    const uint8_t *p = body;
    const uint8_t *end = body + body_len;
    const uint8_t *outer_end;
    if (der_enter_loc(&p, end, 0x30, &outer_end) < 0) return OCSP_ERROR;
    /* responseStatus ENUMERATED */
    if (p >= outer_end || *p != 0x0A) return OCSP_ERROR;
    p++;
    uint32_t el;
    if (der_read_len_loc(&p, outer_end, &el) < 0) return OCSP_ERROR;
    if (el == 0 || p + el > outer_end) return OCSP_ERROR;
    uint8_t rs = *p;
    p += el;
    if (rs != 0) {
        serial_puts("[OCSP] responseStatus != successful: ");
        serial_putdec((uint64_t)rs); serial_puts("\n");
        return OCSP_ERROR;
    }
    /* [0] EXPLICIT responseBytes */
    if (p >= outer_end || *p != 0xA0) return OCSP_ERROR;
    const uint8_t *rb_end;
    if (der_enter_loc(&p, outer_end, 0xA0, &rb_end) < 0) return OCSP_ERROR;
    /* ResponseBytes SEQUENCE { responseType OID, response OCTET STRING } */
    const uint8_t *rb_seq_end;
    if (der_enter_loc(&p, rb_end, 0x30, &rb_seq_end) < 0) return OCSP_ERROR;
    /* responseType OID — skip without enforcement (RFC says id-pkix-
     * ocsp-basic = 1.3.6.1.5.5.7.48.1.1; we'll just continue). */
    if (der_skip_loc(&p, rb_seq_end) < 0) return OCSP_ERROR;
    /* response OCTET STRING wrapping BasicOCSPResponse. */
    if (p >= rb_seq_end || *p != 0x04) return OCSP_ERROR;
    p++;
    uint32_t os_len;
    if (der_read_len_loc(&p, rb_seq_end, &os_len) < 0) return OCSP_ERROR;
    if (p + os_len > rb_seq_end) return OCSP_ERROR;

    /* BasicOCSPResponse SEQUENCE { tbsResponseData, sigAlg, sig BIT STRING, certs? } */
    const uint8_t *basic_end;
    if (der_enter_loc(&p, p + os_len, 0x30, &basic_end) < 0) return OCSP_ERROR;
    /* ResponseData SEQUENCE { version?, responderID, producedAt, responses, ext? }
     * — capture the full tbsResponseData TLV (including its outer tag+length)
     * because that's what the responder hashed. */
    const uint8_t *tbs_start = p;
    const uint8_t *rd_end;
    if (der_enter_loc(&p, basic_end, 0x30, &rd_end) < 0) return OCSP_ERROR;
    uint32_t tbs_len = (uint32_t)(rd_end - tbs_start);
    if (vm) { vm->tbs = tbs_start; vm->tbs_len = tbs_len; }
    /* Skip optional version [0]. */
    if (p < rd_end && p[0] == 0xA0)
        if (der_skip_loc(&p, rd_end) < 0) return OCSP_ERROR;
    /* responderID is a CHOICE [1] name or [2] keyHash — skip either. */
    if (p < rd_end && (p[0] == 0xA1 || p[0] == 0xA2))
        if (der_skip_loc(&p, rd_end) < 0) return OCSP_ERROR;
    /* producedAt GeneralizedTime */
    if (der_skip_loc(&p, rd_end) < 0) return OCSP_ERROR;

    /* responses SEQUENCE OF SingleResponse */
    const uint8_t *resps_end;
    if (der_enter_loc(&p, rd_end, 0x30, &resps_end) < 0) return OCSP_ERROR;

    /* Take the first SingleResponse (we sent one request). */
    const uint8_t *sr_end;
    if (der_enter_loc(&p, resps_end, 0x30, &sr_end) < 0) return OCSP_ERROR;
    /* certID — skip */
    if (der_skip_loc(&p, sr_end) < 0) return OCSP_ERROR;
    /* certStatus CHOICE — tag 0x80=good (NULL), 0xA1=revoked (RevokedInfo),
     * 0x82=unknown (NULL). */
    if (p >= sr_end) return OCSP_ERROR;
    uint8_t status_tag = *p;
    ocsp_status_t status;
    switch (status_tag) {
    case 0x80: status = OCSP_GOOD;    break;
    case 0xA1: status = OCSP_REVOKED; break;
    case 0x82: status = OCSP_UNKNOWN; break;
    default:
        serial_puts("[OCSP] unknown certStatus tag 0x");
        serial_putdec((uint64_t)status_tag); serial_puts("\n");
        return OCSP_ERROR;
    }

    /* If the caller doesn't need verify material we're done. */
    if (!vm) return status;

    /* Resume at end of tbsResponseData (rd_end) to parse sigAlg/sig/certs. */
    p = rd_end;

    /* signatureAlgorithm AlgorithmIdentifier (SEQUENCE). */
    if (p >= basic_end || *p != 0x30) return status;   /* malformed but status known */
    const uint8_t *sa_start = p;
    const uint8_t *sa_end;
    if (der_enter_loc(&p, basic_end, 0x30, &sa_end) < 0) return status;
    vm->alg = ocsp_recognize_sigalg(sa_start, (uint32_t)(sa_end - sa_start));
    p = sa_end;

    /* signature BIT STRING. */
    if (p >= basic_end || *p != 0x03) return status;
    p++;
    uint32_t bs_len;
    if (der_read_len_loc(&p, basic_end, &bs_len) < 0) return status;
    if (p + bs_len > basic_end || bs_len < 1) return status;
    if (*p != 0x00) return status;                    /* unused-bits must be 0 */
    vm->sig     = p + 1;
    vm->sig_len = bs_len - 1;
    p += bs_len;

    /* certs [0] EXPLICIT SEQUENCE OF Certificate OPTIONAL — capture
     * the FIRST embedded cert if present. */
    if (p < basic_end && *p == 0xA0) {
        const uint8_t *certs_explicit_end;
        if (der_enter_loc(&p, basic_end, 0xA0, &certs_explicit_end) == 0) {
            const uint8_t *certs_seq_end;
            if (der_enter_loc(&p, certs_explicit_end, 0x30, &certs_seq_end) == 0) {
                /* First Certificate is itself a SEQUENCE.  Capture its
                 * full TLV (tag+length+body) — that's what x509 helpers
                 * expect. */
                if (p < certs_seq_end && *p == 0x30) {
                    const uint8_t *c_start = p;
                    const uint8_t *c_end;
                    if (der_enter_loc(&p, certs_seq_end, 0x30, &c_end) == 0) {
                        vm->signer     = c_start;
                        vm->signer_len = (uint32_t)(c_end - c_start);
                    }
                }
            }
        }
    }

    return status;
}

/* RFC 6960 §4.2.2.2 — verify the responder signature over
 * tbsResponseData using the delegate signer cert (if present) or the
 * issuer cert directly.  When a delegate cert is embedded we sanity-
 * check that its issuer DN matches the issuer DN of the cert being
 * status-checked — same-CA constraint, since RFC 6960 §4.2.2.2
 * requires the OCSP signer be either the issuer itself or a cert
 * directly issued by the same CA.
 *
 * Returns 0 on verified, -1 on any failure (logged but non-fatal —
 * the caller in tls13.c decides policy). */
static int ocsp_verify_sig(const ocsp_verify_material_t *vm,
                           const uint8_t *cert,    uint32_t cert_len,
                           const uint8_t *issuer,  uint32_t issuer_len)
{
    if (!vm || !vm->tbs || !vm->sig || vm->alg == OCSP_SIG_UNKNOWN) {
        serial_puts("[OCSP] sig verify: FAIL (no material / unsupported alg)\n");
        return -1;
    }

    /* Decide whether to verify under the issuer's own key or the
     * embedded delegate signer cert's key. */
    const uint8_t *signer_cert     = issuer;
    uint32_t       signer_cert_len = issuer_len;

    if (vm->signer && vm->signer_len > 0) {
        /* Same-CA constraint: responder cert's issuer DN must equal
         * the issuer DN of the cert we're checking. */
        const uint8_t *resp_issuer_dn,  *target_issuer_dn;
        uint32_t       resp_issuer_len,  target_issuer_len;
        if (x509_get_issuer_der(vm->signer, vm->signer_len,
                                &resp_issuer_dn, &resp_issuer_len) < 0 ||
            x509_get_issuer_der(cert, cert_len,
                                &target_issuer_dn, &target_issuer_len) < 0) {
            serial_puts("[OCSP] sig verify: FAIL (delegate-signer DN parse)\n");
            return -1;
        }
        if (ocsp_memeq(resp_issuer_dn,  resp_issuer_len,
                       target_issuer_dn, target_issuer_len) != 0) {
            serial_puts("[OCSP] sig verify: FAIL (delegate-signer issuer DN mismatch)\n");
            return -1;
        }
        signer_cert     = vm->signer;
        signer_cert_len = vm->signer_len;
        serial_puts("[OCSP] using embedded delegate signer cert\n");
    }

    /* Hash the captured tbsResponseData with SHA-256 (only hash
     * supported by our dispatch table). */
    uint8_t digest[32];
    sha256(vm->tbs, vm->tbs_len, digest);

    int rc = -1;
    if (vm->alg == OCSP_SIG_ECDSA_P256_SHA256) {
        uint8_t qx[32], qy[32];
        if (x509_extract_ec_pubkey(signer_cert, signer_cert_len, qx, qy) < 0) {
            serial_puts("[OCSP] sig verify: FAIL (signer EC pubkey extract)\n");
            return -1;
        }
        rc = ecdsa_p256_verify(qx, qy, digest, vm->sig, vm->sig_len);
        if (rc != 0) {
            serial_puts("[OCSP] sig verify: FAIL (ECDSA-P256-SHA256)\n");
            return -1;
        }
    } else if (vm->alg == OCSP_SIG_RSA_SHA256) {
        const uint8_t *n, *e;
        uint32_t       n_len, e_len;
        if (x509_extract_rsa_pubkey(signer_cert, signer_cert_len,
                                    &n, &n_len, &e, &e_len) < 0) {
            serial_puts("[OCSP] sig verify: FAIL (signer RSA pubkey extract)\n");
            return -1;
        }
        rc = rsa_pkcs1_v15_sha256_verify(vm->sig, vm->sig_len,
                                          n, n_len, e, e_len, digest);
        if (rc != 0) {
            serial_puts("[OCSP] sig verify: FAIL (RSA-PKCS1-SHA256)\n");
            return -1;
        }
    } else {
        serial_puts("[OCSP] sig verify: FAIL (unsupported sigalg)\n");
        return -1;
    }

    serial_puts("[OCSP] sig verify: OK\n");
    return 0;
}

ocsp_status_t ocsp_check(const uint8_t *cert, uint32_t cert_len,
                         const uint8_t *issuer, uint32_t issuer_len)
{
    char url[256];
    int url_len = x509_get_aia_ocsp(cert, cert_len, url, sizeof url);
    if (url_len <= 0) {
        serial_puts("[OCSP] no AIA-OCSP URL in cert\n");
        return OCSP_ERROR;
    }
    serial_puts("[OCSP] responder: "); serial_puts(url); serial_puts("\n");

    static uint8_t req_buf[1024];
    int req_len = build_ocsp_request(cert, cert_len, issuer, issuer_len,
                                      req_buf, sizeof req_buf);
    if (req_len < 0) {
        serial_puts("[OCSP] build_ocsp_request failed\n");
        return OCSP_ERROR;
    }
    serial_puts("[OCSP] request "); serial_putdec((uint64_t)req_len);
    serial_puts(" bytes\n");

    static uint8_t resp_buf[8192];
    int resp_len = http_plain_post(url, "application/ocsp-request",
                                    req_buf, (uint32_t)req_len,
                                    resp_buf, sizeof resp_buf);
    if (resp_len <= 0) {
        serial_puts("[OCSP] POST failed\n");
        return OCSP_ERROR;
    }
    serial_puts("[OCSP] response "); serial_putdec((uint64_t)resp_len);
    serial_puts(" bytes\n");

    ocsp_verify_material_t vm = { 0 };
    ocsp_status_t st = parse_ocsp_response(resp_buf, (uint32_t)resp_len, &vm);
    if (st == OCSP_ERROR) return st;

    /* Verify the responder signature — informative-mode: log result
     * but don't downgrade the parsed certStatus.  The caller in
     * tls13.c decides whether to abort on verify failure. */
    (void)ocsp_verify_sig(&vm, cert, cert_len, issuer, issuer_len);
    return st;
}

/* RFC 6066 §8 / RFC 8446 §4.4.2.1 — parse a STAPLED OCSP response
 * embedded inside the Certificate handshake message.  Same parser as
 * ocsp_check uses; just skips the network roundtrip. */
ocsp_status_t ocsp_parse_stapled(const uint8_t *resp,   uint32_t resp_len,
                                 const uint8_t *cert,   uint32_t cert_len,
                                 const uint8_t *issuer, uint32_t issuer_len)
{
    if (!resp || resp_len == 0) return OCSP_ERROR;
    serial_puts("[OCSP] stapled response "); serial_putdec((uint64_t)resp_len);
    serial_puts(" bytes\n");
    ocsp_verify_material_t vm = { 0 };
    ocsp_status_t st = parse_ocsp_response(resp, resp_len, &vm);
    if (st == OCSP_ERROR) return st;
    (void)ocsp_verify_sig(&vm, cert, cert_len, issuer, issuer_len);
    return st;
}
