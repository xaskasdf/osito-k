/*
 * crl.c — RFC 5280 §5 Certificate Revocation List parser + checker.
 *
 * Workflow per crl_check_revoked():
 *   1. x509_get_serial_number()  → our cert's serial (INTEGER body).
 *   2. x509_get_crldp_url()      → URL of the CA's CRL.
 *   3. Try disk cache (tls/crl-<hash8>.der); fall back to
 *      http_plain_get and persist on success.
 *   4. Parse CertificateList → tbsCertList → revokedCertificates,
 *      linear-scan for a matching serial.
 *
 * DER shape (RFC 5280 §5.1):
 *
 *   CertificateList ::= SEQUENCE {
 *     tbsCertList         TBSCertList,
 *     signatureAlgorithm  AlgorithmIdentifier,
 *     signatureValue      BIT STRING  }
 *
 *   TBSCertList ::= SEQUENCE {
 *     version              Version OPTIONAL,
 *     signature            AlgorithmIdentifier,
 *     issuer               Name,
 *     thisUpdate           Time,
 *     nextUpdate           Time OPTIONAL,
 *     revokedCertificates  SEQUENCE OF SEQUENCE {
 *       userCertificate    CertificateSerialNumber,
 *       revocationDate     Time,
 *       crlEntryExtensions Extensions OPTIONAL } OPTIONAL,
 *     crlExtensions    [0] EXPLICIT Extensions OPTIONAL }
 *
 * The signature on tbsCertList is NOT verified here (informative
 * mode — same posture ocsp.c shipped with).  TODO: thread the
 * issuer cert through and verify with x509_verify_chain_link's
 * primitives.
 */

#include "../include/types.h"
#include "crl.h"
#include "x509.h"

extern void serial_puts(const char *s);
extern void serial_putdec(uint64_t v);

extern void *kmalloc(uint64_t size);
extern void  kfree(void *ptr);

/* http_plain.c */
extern int http_plain_get(const char *url, uint8_t *out, uint32_t out_cap);

/* sha256 — used to derive the cache filename from the URL. */
extern void sha256(const void *data, uint32_t len, uint8_t digest[32]);

/* VFS / osfs2 — match the stub layout used by cert_pin.c. */
typedef struct {
    uint32_t fs_version;
    uint32_t ino;
    void    *data;
    uint64_t size;
} vfs_stub_t;
extern bool  vfs_find(const char *path, int mode, void *out);
extern int   vfs_read(void *node, uint64_t offset, void *buf, uint64_t len);
extern bool  osfs2_is_mounted(void);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);

/* ── DER helpers (mirrored from x509.c, kept private here) ─── */

static int der_read_len(const uint8_t **p, const uint8_t *end, uint32_t *out)
{
    if (*p >= end) return -1;
    uint8_t b = *(*p)++;
    if (b < 0x80) { *out = b; return 0; }
    uint8_t n = b & 0x7F;
    if (n == 0 || n > 4) return -1;
    if (*p + n > end) return -1;
    uint32_t v = 0;
    for (uint8_t i = 0; i < n; i++) v = (v << 8) | *(*p)++;
    *out = v;
    return 0;
}

static int der_skip_tlv(const uint8_t **p, const uint8_t *end)
{
    if (*p >= end) return -1;
    (*p)++;                 /* tag */
    uint32_t len;
    if (der_read_len(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    *p += len;
    return 0;
}

static int der_enter(const uint8_t **p, const uint8_t *end,
                     uint8_t want_tag, const uint8_t **inner_end)
{
    if (*p >= end || **p != want_tag) return -1;
    (*p)++;
    uint32_t len;
    if (der_read_len(p, end, &len) < 0) return -1;
    if (*p + len > end) return -1;
    *inner_end = *p + len;
    return 0;
}

/* Serial comparison.  CertificateSerialNumber is an INTEGER, which
 * DER encodes with a leading 0x00 byte if the high bit of the
 * magnitude is set (to keep it positive).  Normalize by stripping
 * leading zero bytes on both sides before comparing — this makes
 * the comparison representation-invariant. */
static int serial_equal(const uint8_t *a, uint32_t alen,
                        const uint8_t *b, uint32_t blen)
{
    while (alen > 1 && a[0] == 0x00) { a++; alen--; }
    while (blen > 1 && b[0] == 0x00) { b++; blen--; }
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* ── CRL parser ──────────────────────────────────────────────── */

int crl_parse_and_check(const uint8_t *crl, uint32_t crl_len,
                        const uint8_t *serial, uint32_t serial_len)
{
    const uint8_t *p = crl;
    const uint8_t *end = crl + crl_len;

    /* CertificateList ::= SEQUENCE { ... } */
    const uint8_t *list_end;
    if (der_enter(&p, end, 0x30, &list_end) < 0) {
        serial_puts("[CRL] bad outer SEQUENCE\n");
        return CRL_ERROR;
    }
    /* tbsCertList SEQUENCE */
    const uint8_t *tbs_end;
    if (der_enter(&p, list_end, 0x30, &tbs_end) < 0) {
        serial_puts("[CRL] bad TBSCertList\n");
        return CRL_ERROR;
    }
    /* Optional version INTEGER (v2 = 1).  If present, the first
     * tag inside TBSCertList is 0x02. */
    if (p < tbs_end && p[0] == 0x02) {
        if (der_skip_tlv(&p, tbs_end) < 0) return CRL_ERROR;
    }
    /* signature AlgorithmIdentifier, issuer Name, thisUpdate.
     * (3 mandatory TLVs to skip.) */
    for (int i = 0; i < 3; i++) {
        if (der_skip_tlv(&p, tbs_end) < 0) {
            serial_puts("[CRL] truncated header\n");
            return CRL_ERROR;
        }
    }
    /* Optional nextUpdate.  It's a Time = UTCTime (0x17) or
     * GeneralizedTime (0x18). */
    if (p < tbs_end && (p[0] == 0x17 || p[0] == 0x18)) {
        if (der_skip_tlv(&p, tbs_end) < 0) return CRL_ERROR;
    }
    /* revokedCertificates is OPTIONAL.  If present it's a SEQUENCE
     * (tag 0x30); if absent we either hit the crlExtensions [0]
     * tag (0xA0) or end-of-tbs.  A CRL with no revoked entries =
     * vacuously CRL_GOOD. */
    if (p >= tbs_end || p[0] != 0x30) {
        return CRL_GOOD;
    }
    const uint8_t *rev_end;
    if (der_enter(&p, tbs_end, 0x30, &rev_end) < 0) {
        serial_puts("[CRL] bad revokedCertificates\n");
        return CRL_ERROR;
    }
    uint32_t scanned = 0;
    while (p < rev_end) {
        const uint8_t *entry_end;
        if (der_enter(&p, rev_end, 0x30, &entry_end) < 0) {
            serial_puts("[CRL] bad entry SEQUENCE\n");
            return CRL_ERROR;
        }
        /* userCertificate INTEGER. */
        if (p >= entry_end || *p != 0x02) {
            p = entry_end; continue;
        }
        p++;
        uint32_t s_len;
        if (der_read_len(&p, entry_end, &s_len) < 0) {
            serial_puts("[CRL] bad serial INTEGER\n");
            return CRL_ERROR;
        }
        if (p + s_len > entry_end) return CRL_ERROR;
        const uint8_t *s_body = p;
        if (serial_equal(s_body, s_len, serial, serial_len)) {
            serial_puts("[CRL] REVOKED match after ");
            serial_putdec((uint64_t)scanned);
            serial_puts(" entries\n");
            return CRL_REVOKED;
        }
        scanned++;
        p = entry_end;
    }
    serial_puts("[CRL] not revoked (scanned ");
    serial_putdec((uint64_t)scanned);
    serial_puts(" entries)\n");
    return CRL_GOOD;
}

/* ── Cache filename derivation ───────────────────────────────── */

static void hex8_of_url(const char *url, char out[9])
{
    /* Strlen without depending on stringlib. */
    uint32_t n = 0;
    while (url[n]) n++;
    uint8_t digest[32];
    sha256(url, n, digest);
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[i * 2]     = H[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[digest[i] & 0xF];
    }
    out[8] = 0;
}

/* Build "tls/crl-XXXXXXXX.der" into `out` (cap >= 22 needed). */
static void make_cache_path(const char *url, char *out, uint32_t cap)
{
    static const char prefix[] = "tls/crl-";
    uint32_t i = 0;
    while (prefix[i] && i + 1 < cap) { out[i] = prefix[i]; i++; }
    char h[9];
    hex8_of_url(url, h);
    for (int j = 0; j < 8 && i + 1 < cap; j++, i++) out[i] = h[j];
    static const char suffix[] = ".der";
    for (int j = 0; suffix[j] && i + 1 < cap; j++, i++) out[i] = suffix[j];
    if (i < cap) out[i] = 0;
}

/* ── Disk cache helpers ──────────────────────────────────────── */

/* Read full CRL file from osfs2 into a fresh kmalloc'd buffer.
 * Returns buffer pointer (caller kfree's) or NULL.  *out_len set. */
static uint8_t *load_cached_crl(const char *path, uint32_t *out_len)
{
    if (!osfs2_is_mounted()) return NULL;
    vfs_stub_t node;
    if (!vfs_find(path, 0, &node)) return NULL;
    if (node.size == 0 || node.size > (8u << 20)) return NULL;
    uint8_t *buf = (uint8_t *)kmalloc(node.size);
    if (!buf) return NULL;
    int n = vfs_read(&node, 0, buf, node.size);
    if (n <= 0) { kfree(buf); return NULL; }
    *out_len = (uint32_t)n;
    return buf;
}

static void persist_crl(const char *path,
                        const uint8_t *crl, uint32_t crl_len)
{
    if (!osfs2_is_mounted()) return;
    void *f = osfs2_create(path, (uint64_t)crl_len);
    if (!f) {
        serial_puts("[CRL] cache create fail: "); serial_puts(path); serial_puts("\n");
        return;
    }
    if (osfs2_write(f, 0, crl, (uint64_t)crl_len) < 0) {
        serial_puts("[CRL] cache write fail\n");
        return;
    }
    serial_puts("[CRL] cached "); serial_putdec((uint64_t)crl_len);
    serial_puts(" bytes at "); serial_puts(path); serial_puts("\n");
}

/* ── Top-level entrypoint ────────────────────────────────────── */

/* Big enough for typical CDP-served CRLs.  GTS/Let's-Encrypt CRLs
 * are usually under 1-2 MB; pick 4 MB as a generous ceiling so a
 * grown list doesn't silently truncate.  kmalloc returns from the
 * kernel heap which auto-grows. */
#define CRL_MAX_BYTES   (4u * 1024u * 1024u)

int crl_check_revoked(const uint8_t *cert_der, uint32_t cert_len)
{
    const uint8_t *serial_ptr;
    uint32_t serial_len;
    if (x509_get_serial_number(cert_der, cert_len,
                               &serial_ptr, &serial_len) < 0) {
        serial_puts("[CRL] no serial in cert\n");
        return CRL_ERROR;
    }
    char url[512];
    int url_n = x509_get_crldp_url(cert_der, cert_len, url, sizeof url);
    if (url_n < 0) {
        serial_puts("[CRL] no CRLDistributionPoints in cert\n");
        return CRL_ERROR;
    }
    serial_puts("[CRL] URL: "); serial_puts(url); serial_puts("\n");

    char cache_path[64];
    make_cache_path(url, cache_path, sizeof cache_path);

    /* Snapshot serial into a local stack buffer so the long
     * allocation/IO window below can't dangle the pointer if the
     * cert_der memory is freed by the caller mid-flight. */
    uint8_t serial_copy[64];
    uint32_t scopy_len = serial_len > sizeof serial_copy
                         ? sizeof serial_copy : serial_len;
    for (uint32_t i = 0; i < scopy_len; i++) serial_copy[i] = serial_ptr[i];

    /* Try cache first. */
    uint32_t crl_len = 0;
    uint8_t *crl_buf = load_cached_crl(cache_path, &crl_len);
    bool from_cache = (crl_buf != NULL);
    if (from_cache) {
        serial_puts("[CRL] using cached copy ("); serial_putdec((uint64_t)crl_len);
        serial_puts(" bytes)\n");
    } else {
        crl_buf = (uint8_t *)kmalloc(CRL_MAX_BYTES);
        if (!crl_buf) {
            serial_puts("[CRL] kmalloc fail\n");
            return CRL_ERROR;
        }
        int got = http_plain_get(url, crl_buf, CRL_MAX_BYTES);
        if (got <= 0) {
            serial_puts("[CRL] http_plain_get fail\n");
            kfree(crl_buf);
            return CRL_ERROR;
        }
        crl_len = (uint32_t)got;
        serial_puts("[CRL] downloaded "); serial_putdec((uint64_t)crl_len);
        serial_puts(" bytes\n");
        persist_crl(cache_path, crl_buf, crl_len);
    }

    int rc = crl_parse_and_check(crl_buf, crl_len,
                                 serial_copy, scopy_len);
    kfree(crl_buf);

    /* Informative-mode reminder: sig over tbsCertList not verified. */
    if (rc == CRL_REVOKED) {
        serial_puts("[CRL] WARNING informative mode — "
                    "tbsCertList signature unverified\n");
    }
    return rc;
}
