/*
 * revocation.c -- is the server's certificate revoked? (CRLs)
 *
 * Once tls_verify_chain has accepted a chain, tls_revocation_note keeps
 * what the check needs: the leaf's serial, issuer and CRL (its CRL
 * Distribution Points), and the issuer's key. When the handshake is done,
 * before any data, tls_revocation_check reads that CRL over plain HTTP
 * (http_open_plain), checks it carries the issuer's signature and name,
 * and looks for the serial in it: after the handshake, so that a slow CRL
 * doesn't run into the server's handshake timeout. Only a listed serial
 * stops the connection. A CRL that can't be fetched or checked doesn't,
 * and is logged: the soft fail browsers use too. CRLs stay in memory until
 * their nextUpdate, a day at most, four at a time.
 *
 * No OCSP: Let's Encrypt's certificates and Google Trust Services' name
 * only CRLs, and the CAs that run OCSP publish CRLs as well. The issuer
 * is the chain's second certificate (BearSSL checked the order), its key
 * read here from the DER; tls_verify_signature checks the signature.
 */

#include "../include/hal.h"
#include "../include/types.h"
#include "http.h"
#include "tls_verify.h"

extern uint32_t ntp_get_utc(void);

#define CRL_MAX     (32U << 20)         /* the biggest CRL taken */
#define CRL_CACHE   4

/* ── DER ─────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p, *end;
} der_t;

/* The next element: its tag, its contents in *in, where it starts in
 * *raw; -1 at the end or if it's malformed */
static int der_get(der_t *d, uint8_t *tag, der_t *in, const uint8_t **raw)
{
    if (d->end - d->p < 2)
        return -1;
    const uint8_t *start = d->p;
    uint8_t t = *d->p++;
    uint32_t len = *d->p++;
    if (len & 0x80) {
        uint32_t k = len & 0x7F;
        if (k == 0 || k > 4 || (uint32_t)(d->end - d->p) < k)
            return -1;
        for (len = 0; k; k--)
            len = (len << 8) | *d->p++;
    }
    if (len > (uint32_t)(d->end - d->p))
        return -1;
    if (tag)
        *tag = t;
    if (in) {
        in->p = d->p;
        in->end = d->p + len;
    }
    if (raw)
        *raw = start;
    d->p += len;
    return 0;
}

static int der_peek(const der_t *d)
{
    return d->p < d->end ? *d->p : -1;
}

static int der_expect(der_t *d, uint8_t tag, der_t *in)
{
    uint8_t t;
    return der_peek(d) == tag && der_get(d, &t, in, NULL) == 0 ? 0 : -1;
}

static int der_is(der_t v, const uint8_t *oid, uint32_t len)
{
    return (uint32_t)(v.end - v.p) == len && memcmp(v.p, oid, len) == 0;
}

/* UTCTime or GeneralizedTime, YYMMDDHHMMSSZ or YYYYMMDDHHMMSSZ, as Unix
 * time; 0 if it isn't one */
static uint32_t der_time(uint8_t tag, der_t v)
{
    uint32_t n = (uint32_t)(v.end - v.p), digits = tag == 0x17 ? 12 : 14, f[7] = {0};
    if ((tag != 0x17 && tag != 0x18) || n != digits + 1 || v.p[digits] != 'Z')
        return 0;
    for (uint32_t i = 0; i < digits; i++)
        if (v.p[i] < '0' || v.p[i] > '9')
            return 0;
    const uint8_t *p = v.p;
    int year = tag == 0x17 ? (p[0] - '0') * 10 + (p[1] - '0') : 0;
    if (tag == 0x17) {
        year += year < 50 ? 2000 : 1900;
        p += 2;
    } else {
        year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
        p += 4;
    }
    for (int i = 0; i < 5; i++, p += 2)
        f[i] = (uint32_t)((p[0] - '0') * 10 + (p[1] - '0'));
    /* Days since 1970 of year/month/day (Howard Hinnant's days_from_civil) */
    int y = year - (f[0] <= 2), m = (int)f[0], d = (int)f[1];
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    if (days < 0)
        return 0;
    return (uint32_t)days * 86400 + f[2] * 3600 + f[3] * 60 + f[4];
}

/* ── Certificates ────────────────────────────────────────────── */

typedef struct {
    der_t serial;               /* the INTEGER's contents */
    const uint8_t *issuer, *subject;
    uint32_t issuer_len, subject_len;
    der_t spki;
    char crl[256];              /* the first http:// CRL distribution point */
} cert_t;

/* The first [6] (a URI) under v that starts with http:// */
static void find_http_uri(der_t v, char *out, uint32_t cap, int depth)
{
    uint8_t tag;
    der_t in;
    while (!out[0] && depth < 6 && der_get(&v, &tag, &in, NULL) == 0) {
        uint32_t n = (uint32_t)(in.end - in.p);
        if (tag == 0x86 && n > 7 && n < cap && memcmp(in.p, "http://", 7) == 0) {
            memcpy(out, in.p, n);
            out[n] = 0;
        } else if (tag & 0x20) {
            find_http_uri(in, out, cap, depth + 1);
        }
    }
}

static int parse_cert(const uint8_t *c, uint32_t len, cert_t *out)
{
    static const uint8_t crl_dp[] = { 0x55, 0x1D, 0x1F };      /* 2.5.29.31 */
    der_t d = { c, c + len }, cert, tbs, v, ext;
    uint8_t tag;
    const uint8_t *raw;
    memset(out, 0, sizeof(*out));
    if (der_expect(&d, 0x30, &cert) || der_expect(&cert, 0x30, &tbs))
        return -1;
    if (der_peek(&tbs) == 0xA0 && der_get(&tbs, &tag, NULL, NULL))
        return -1;
    if (der_expect(&tbs, 0x02, &out->serial) || der_expect(&tbs, 0x30, NULL))
        return -1;
    if (der_peek(&tbs) != 0x30 || der_get(&tbs, &tag, &v, &raw))
        return -1;
    out->issuer = raw;
    out->issuer_len = (uint32_t)(v.end - raw);
    if (der_expect(&tbs, 0x30, NULL))                           /* validity */
        return -1;
    if (der_peek(&tbs) != 0x30 || der_get(&tbs, &tag, &v, &raw))
        return -1;
    out->subject = raw;
    out->subject_len = (uint32_t)(v.end - raw);
    if (der_expect(&tbs, 0x30, &out->spki))
        return -1;
    while (der_get(&tbs, &tag, &v, NULL) == 0) {
        der_t exts;
        if (tag != 0xA3 || der_expect(&v, 0x30, &exts))         /* extensions */
            continue;
        while (der_expect(&exts, 0x30, &ext) == 0) {
            der_t oid, value;
            if (der_expect(&ext, 0x06, &oid))
                break;
            if (der_peek(&ext) == 0x01)
                der_get(&ext, &tag, NULL, NULL);                /* critical */
            if (der_expect(&ext, 0x04, &value))
                break;
            if (der_is(oid, crl_dp, sizeof(crl_dp)))
                find_http_uri(value, out->crl, sizeof(out->crl), 0);
        }
    }
    return 0;
}

/* The key in a SubjectPublicKeyInfo: RSA, or EC on P-256/384/521 */
static int spki_key(der_t spki, tls_pubkey_t *k)
{
    static const uint8_t rsa[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01 };
    static const uint8_t ec[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01 };
    static const uint8_t p256[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07 };
    static const uint8_t p384[] = { 0x2B, 0x81, 0x04, 0x00, 0x22 };
    static const uint8_t p521[] = { 0x2B, 0x81, 0x04, 0x00, 0x23 };
    der_t alg, oid, bits;
    memset(k, 0, sizeof(*k));
    if (der_expect(&spki, 0x30, &alg) || der_expect(&alg, 0x06, &oid) ||
        der_expect(&spki, 0x03, &bits) || bits.end - bits.p < 2 || bits.p[0] != 0)
        return -1;
    bits.p++;
    if (der_is(oid, rsa, sizeof(rsa))) {
        der_t seq, n, e;
        if (der_expect(&bits, 0x30, &seq) || der_expect(&seq, 0x02, &n) ||
            der_expect(&seq, 0x02, &e))
            return -1;
        while (n.end - n.p > 1 && !n.p[0])
            n.p++;
        while (e.end - e.p > 1 && !e.p[0])
            e.p++;
        k->nlen = (uint32_t)(n.end - n.p);
        k->elen = (uint32_t)(e.end - e.p);
        if (k->nlen > sizeof(k->n) || k->elen > sizeof(k->e))
            return -1;
        memcpy(k->n, n.p, k->nlen);
        memcpy(k->e, e.p, k->elen);
        k->type = 1;
        return 0;
    }
    der_t curve;
    if (!der_is(oid, ec, sizeof(ec)) || der_expect(&alg, 0x06, &curve))
        return -1;
    k->curve = der_is(curve, p256, sizeof(p256)) ? 23 : der_is(curve, p384, sizeof(p384)) ? 24 :
               der_is(curve, p521, sizeof(p521)) ? 25 : 0;
    k->qlen = (uint32_t)(bits.end - bits.p);
    if (!k->curve || k->qlen > sizeof(k->q))
        return -1;
    memcpy(k->q, bits.p, k->qlen);
    k->type = 2;
    return 0;
}

/* An AlgorithmIdentifier's signature as a TLS scheme tls_verify_signature
 * takes: RSA PKCS#1 v1.5 or ECDSA with SHA-256/384/512; 0 otherwise */
static uint16_t sig_scheme(der_t alg)
{
    static const uint8_t rsa[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01 };
    static const uint8_t ecdsa[] = { 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03 };
    der_t oid;
    if (der_expect(&alg, 0x06, &oid))
        return 0;
    uint32_t n = (uint32_t)(oid.end - oid.p);
    if (n == 9 && !memcmp(oid.p, rsa, 8) && oid.p[8] >= 11 && oid.p[8] <= 13)
        return (uint16_t)(0x0401 + 0x100 * (oid.p[8] - 11));
    if (n == 8 && !memcmp(oid.p, ecdsa, 7) && oid.p[7] >= 2 && oid.p[7] <= 4)
        return (uint16_t)(0x0403 + 0x100 * (oid.p[7] - 2));
    return 0;
}

/* ── CRLs ────────────────────────────────────────────────────── */

typedef struct {
    der_t tbs_raw, revoked;
    uint32_t next_update;       /* 0: none given */
    const uint8_t *issuer;
    uint32_t issuer_len;
    uint16_t scheme;
    der_t sig;
} crl_t;

/* CertificateList (RFC 5280 5.1): what the checks need, or -1 */
static int parse_crl(const uint8_t *data, uint32_t len, crl_t *c)
{
    der_t d = { data, data + len }, list, tbs, alg, v;
    const uint8_t *raw;
    uint8_t tag;
    memset(c, 0, sizeof(*c));
    if (der_expect(&d, 0x30, &list) || der_peek(&list) != 0x30 ||
        der_get(&list, &tag, &tbs, &raw))
        return -1;
    c->tbs_raw.p = raw;
    c->tbs_raw.end = tbs.end;
    if (der_expect(&list, 0x30, &alg) || der_expect(&list, 0x03, &c->sig) ||
        c->sig.end - c->sig.p < 2 || c->sig.p[0] != 0)
        return -1;
    c->sig.p++;
    c->scheme = sig_scheme(alg);
    if (der_peek(&tbs) == 0x02)
        der_get(&tbs, &tag, NULL, NULL);                        /* version */
    if (der_expect(&tbs, 0x30, NULL) || der_peek(&tbs) != 0x30 ||
        der_get(&tbs, &tag, &v, &raw))
        return -1;
    c->issuer = raw;
    c->issuer_len = (uint32_t)(v.end - raw);
    if (der_get(&tbs, &tag, &v, NULL) || !der_time(tag, v))      /* thisUpdate */
        return -1;
    if (der_peek(&tbs) == 0x17 || der_peek(&tbs) == 0x18) {
        der_get(&tbs, &tag, &v, NULL);
        c->next_update = der_time(tag, v);
    }
    if (der_peek(&tbs) == 0x30)
        der_get(&tbs, &tag, &c->revoked, NULL);
    return 0;
}

/* 1 if serial is in the list, 0 if not, -1 if the list is malformed */
static int crl_lists(der_t revoked, der_t serial, uint32_t *entries)
{
    der_t entry, s;
    uint32_t sn = (uint32_t)(serial.end - serial.p);
    *entries = 0;
    while (revoked.p < revoked.end) {
        if (der_expect(&revoked, 0x30, &entry) || der_expect(&entry, 0x02, &s))
            return -1;
        (*entries)++;
        if ((uint32_t)(s.end - s.p) == sn && memcmp(s.p, serial.p, sn) == 0)
            return 1;
    }
    return 0;
}

static struct {
    char url[256];
    uint8_t *data;
    uint32_t len, pages, until;
} cache[CRL_CACHE];
static uint32_t cache_next;

/* The CRL at url, from the cache or fetched; 0, or -1 with why logged */
static int crl_get(const char *url, uint32_t now, const uint8_t **data, uint32_t *len)
{
    for (int i = 0; i < CRL_CACHE; i++) {
        if (!cache[i].data || strcmp(cache[i].url, url) != 0)
            continue;
        if (now < cache[i].until) {
            *data = cache[i].data;
            *len = cache[i].len;
            return 0;
        }
        mem_free_pages(cache[i].data, cache[i].pages);
        cache[i].data = NULL;
    }

    /* http://host[:port]/path */
    static char host[128];
    const char *p = url + 7, *path = p;
    uint32_t hl = 0, port = 80;
    while (*path && *path != '/' && *path != ':')
        path++;
    hl = (uint32_t)(path - p);
    if (*path == ':')
        for (port = 0, path++; *path >= '0' && *path <= '9'; path++)
            port = port * 10 + (uint32_t)(*path - '0');
    if (!hl || hl >= sizeof(host) || *path != '/' || !port || port > 65535)
        return -1;
    memcpy(host, p, hl);
    host[hl] = 0;

    static uint8_t *scratch;
    if (!scratch && !(scratch = mem_alloc_aligned_high(CRL_MAX, 4096))) {
        serial_puts("[CRL] no memory\n");
        return -1;
    }
    static http_session_t s;
    static http_response_t resp;
    if (http_open_plain(&s, host, (uint16_t)port) < 0)
        return -1;
    int n = -1;
    if (http_request(&s, "GET", path, host, NULL, NULL, 0, &resp) == 0) {
        if (resp.status_code == 200)
            n = http_read_body_full(&s, &resp, scratch, CRL_MAX);
        else
            serial_puts("[CRL] the server didn't answer 200\n");
    }
    http_close(&s);
    if (n <= 0 || (uint32_t)n >= CRL_MAX ||
        (resp.content_length && (uint32_t)n != resp.content_length)) {
        serial_puts("[CRL] couldn't read it\n");
        return -1;
    }

    /* Kept until its nextUpdate, a day at most */
    crl_t c;
    uint32_t pages = ((uint32_t)n + 4095) / 4096;
    uint8_t *copy = parse_crl(scratch, (uint32_t)n, &c) == 0 ? mem_alloc_aligned_high((uint64_t)pages * 4096, 4096) : NULL;
    if (!copy)
        return -1;
    memcpy(copy, scratch, (uint32_t)n);
    uint32_t i = cache_next++ % CRL_CACHE;
    if (cache[i].data)
        mem_free_pages(cache[i].data, cache[i].pages);
    strcpy(cache[i].url, url);
    cache[i].data = copy;
    cache[i].len = (uint32_t)n;
    cache[i].pages = pages;
    cache[i].until = now + 86400;
    if (c.next_update && c.next_update < cache[i].until)
        cache[i].until = c.next_update;
    *data = copy;
    *len = (uint32_t)n;
    serial_puts("[CRL] fetched ");
    serial_puts(url);
    serial_puts(", ");
    serial_putdec((uint32_t)n);
    serial_puts(" bytes\n");
    return 0;
}

/* What the check needs, taken from the chain while its handshake message
 * is at hand. One handshake at a time (callers hold the network). */
static struct {
    int ready;
    uint8_t serial[64], issuer[1024];
    uint32_t serial_len, issuer_len;
    char crl[256];
    tls_pubkey_t key;
} noted;

void tls_revocation_note(const uint8_t *const *certs, const uint32_t *lens, int n)
{
    static cert_t leaf, issuer;
    noted.ready = 0;
    if (n < 2 || parse_cert(certs[0], lens[0], &leaf) < 0 || !leaf.crl[0]) {
        serial_puts("[CRL] the certificate names no CRL: not checked\n");
        return;
    }
    uint32_t sl = (uint32_t)(leaf.serial.end - leaf.serial.p);
    if (parse_cert(certs[1], lens[1], &issuer) < 0 || issuer.subject_len != leaf.issuer_len ||
        memcmp(issuer.subject, leaf.issuer, leaf.issuer_len) != 0 ||
        sl > sizeof(noted.serial) || leaf.issuer_len > sizeof(noted.issuer) ||
        spki_key(issuer.spki, &noted.key) < 0) {
        serial_puts("[CRL] no usable issuer key in the chain: not checked\n");
        return;
    }
    memcpy(noted.serial, leaf.serial.p, sl);
    noted.serial_len = sl;
    memcpy(noted.issuer, leaf.issuer, leaf.issuer_len);
    noted.issuer_len = leaf.issuer_len;
    strcpy(noted.crl, leaf.crl);
    noted.ready = 1;
}

int tls_revocation_check(void)
{
    if (!noted.ready)
        return 0;
    noted.ready = 0;
    const uint8_t *data;
    uint32_t len, entries, now = ntp_get_utc();
    crl_t c;
    if (crl_get(noted.crl, now, &data, &len) < 0 || parse_crl(data, len, &c) < 0) {
        serial_puts("[CRL] not checked\n");
        return 0;
    }
    if (c.issuer_len != noted.issuer_len || memcmp(c.issuer, noted.issuer, noted.issuer_len) != 0 ||
        !c.scheme || tls_verify_signature(&noted.key, c.scheme, c.tbs_raw.p,
                                          (uint32_t)(c.tbs_raw.end - c.tbs_raw.p), c.sig.p,
                                          (uint32_t)(c.sig.end - c.sig.p)) < 0) {
        serial_puts("[CRL] not the issuer's signature or name: not checked\n");
        return 0;
    }
    der_t serial = { noted.serial, noted.serial + noted.serial_len };
    int listed = crl_lists(c.revoked, serial, &entries);
    if (listed == 1) {
        serial_puts("[TLS] CRL: the certificate is revoked\n");
        return -1;
    }
    serial_puts(listed == 0 ? "[CRL] not revoked (" : "[CRL] malformed list (");
    serial_putdec(entries);
    serial_puts(c.next_update && now > c.next_update ? " entries, past its nextUpdate)\n"
                                                     : " entries)\n");
    return 0;
}
