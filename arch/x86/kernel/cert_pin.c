/*
 * cert_pin.c — leaf-cert SHA-256 pinning for TLS handshakes.
 *
 * MVP pin table; entries are added as we verify them. To onboard a
 * new endpoint:
 *
 *   1. Run the kernel against the host with mode=WARN. The handshake
 *      will print:
 *        [PIN] leaf SHA-256 = <64 hex chars>
 *   2. Verify the fingerprint out-of-band:
 *        echo | openssl s_client -connect HOST:443 -servername HOST \
 *          | openssl x509 -outform der | sha256sum
 *      Both should match.
 *   3. Append the digest to the `pin_table[]` below, rebuild.
 *   4. Flip cert_pin_set_mode(CERT_PIN_STRICT) once all live endpoints
 *      are pinned.
 *
 * Cert rotation: leaf certs typically rotate every 60–90 days for
 * Let's Encrypt / Cloudflare. When a cert rotates, the kernel will
 * reject (STRICT) or warn (WARN); operator re-pins.
 */

#include "cert_pin.h"

extern void  serial_puts(const char *s);
extern void  serial_putdec(uint64_t v);

/* sha256 single-shot helper from kernel/crypto.c. */
extern void  sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);

/* osfs2 — used to persist the dynamic pin table across boots.
 * Stub matches vfs_node_t layout (32 bytes) from fs/vfs.h; replicated
 * here so cert_pin.c stays a leaf module with no fs/vfs dependencies. */
typedef struct {
    uint32_t fs_version;
    uint32_t ino;
    void    *data;
    uint64_t size;
} vfs_stub_t;
extern bool  vfs_find(const char *path, int mode, void *out);
extern int   vfs_read(void *node, uint64_t offset, void *buf, uint64_t len);
extern bool  osfs2_is_mounted(void);
extern int   osfs2_delete(const char *name);
extern void *osfs2_create(const char *name, uint64_t size);
extern int   osfs2_write(void *file, uint64_t offset, const void *buf, uint64_t len);

/* osfs2 path for the persisted dynamic-pin table.  Lives at the
 * root because cert_pin.c has no concept of multi-host namespacing
 * yet (the pin table is global, not per-hostname). */
#define DYN_PIN_PATH       "tls/pins.bin"
#define DYN_PIN_MAGIC      0x504e504bU  /* "KPNP" — Kernel PiN Persistence */

/* ── Pin table ─────────────────────────────────────────────── */

typedef struct {
    const char *label;          /* free-form: hostname, comment */
    uint8_t     digest[32];     /* SHA-256 of DER-encoded leaf cert */
} cert_pin_t;

static const cert_pin_t pin_table[] = {
    /* inferconnect.naranjositos.tech — Cloudflare-fronted leaf cert.
     * Verified 2026-05-10 on macOS host:
     *   echo | openssl s_client -connect inferconnect.naranjositos.tech:443 \
     *     -servername inferconnect.naranjositos.tech \
     *     | openssl x509 -outform der | shasum -a 256
     *   → b0db909ebee1ecc1717bee9a8a302bd78383ce2c1a5ef2c914e7ad49e5163621
     * Cross-checked against the kernel's own [PIN] log on the same date,
     * matched byte-for-byte.
     *
     * Cert rotates every ~90 days; on rotation the kernel will reject
     * (STRICT) or warn (WARN). Re-pin via the workflow at the top of
     * this file. */
    { .label = "inferconnect.naranjositos.tech (leaf)",
      .digest = {
          0xb0,0xdb,0x90,0x9e,0xbe,0xe1,0xec,0xc1,
          0x71,0x7b,0xee,0x9a,0x8a,0x30,0x2b,0xd7,
          0x83,0x83,0xce,0x2c,0x1a,0x5e,0xf2,0xc9,
          0x14,0xe7,0xad,0x49,0xe5,0x16,0x36,0x21
      } },
    /* Google Trust Services WE1 — issuing intermediate.  Pinning the
     * intermediate gives leaf-rotation tolerance: GTS rotates leaf
     * certs every ~90 days but the WE1 intermediate rotates every
     * few years.  As long as either the leaf OR the intermediate
     * matches, the chain is accepted (cert_pin_check walks each
     * entry and accepts on first match).  Verified 2026-05-10:
     *   openssl s_client ... -showcerts \
     *     | <extract Cert 2> | openssl x509 -outform der | shasum -a 256 */
    { .label = "GTS WE1 intermediate",
      .digest = {
          0x1d,0xfc,0x16,0x05,0xfb,0xad,0x35,0x8d,
          0x8b,0xc8,0x44,0xf7,0x6d,0x15,0x20,0x3f,
          0xac,0x9c,0xa5,0xc1,0xa7,0x9f,0xd4,0x85,
          0x7f,0xfa,0xf2,0x86,0x4f,0xbe,0xbf,0x96
      } },
    /* GTS Root R4 — root CA (P-384 key, cross-signed by GlobalSign
     * Root CA).  Pinning the root anchors the chain past any future
     * intermediate rotation; even if Google retires WE1 entirely,
     * the new intermediate will still chain through GTS Root R4 and
     * the pin will hold.  Verified 2026-05-16:
     *   openssl x509 -in root.pem -outform der | shasum -a 256
     *   → 76b27b80a58027dc3cf1da68dac17010ed93997d0b603e2fadbe85012493b5a7
     *
     * We can't *cryptographically* verify the WE1 → GTS Root R4
     * link yet (it's ECDSA-P256-SHA384 signed against a P-384 key
     * and we don't have a P-384 verify primitive), but pinning the
     * root by SHA-256 doesn't need it. */
    { .label = "GTS Root R4 (P-384)",
      .digest = {
          0x76,0xb2,0x7b,0x80,0xa5,0x80,0x27,0xdc,
          0x3c,0xf1,0xda,0x68,0xda,0xc1,0x70,0x10,
          0xed,0x93,0x99,0x7d,0x0b,0x60,0x3e,0x2f,
          0xad,0xbe,0x85,0x01,0x24,0x93,0xb5,0xa7
      } },
    { 0, {0} }   /* sentinel — keep last */
};

#define PIN_TABLE_LEN  ((sizeof pin_table / sizeof pin_table[0]) - 1)

/* ── Dynamic pin table (runtime + persisted) ───────────────────
 *
 * Captured automatically on a successful chain match when the
 * leaf isn't already known (static or dynamic).  Persisted to
 * osfs2 so that subsequent boots remember rotated leaves even if
 * the static intermediate eventually rotates too.  FIFO eviction
 * when full. */
#define CERT_PIN_DYN_MAX  32
static uint8_t  dyn_digests[CERT_PIN_DYN_MAX][32];
static uint8_t  dyn_count = 0;     /* 0..CERT_PIN_DYN_MAX */
static uint8_t  dyn_head  = 0;     /* FIFO insertion index when full */
static bool     dyn_dirty = false; /* unflushed changes pending */

static int dyn_table_has(const uint8_t *digest)
{
    for (uint8_t i = 0; i < dyn_count; i++) {
        const uint8_t *a = dyn_digests[i];
        uint8_t diff = 0;
        for (uint32_t j = 0; j < 32; j++) diff |= (uint8_t)(a[j] ^ digest[j]);
        if (diff == 0) return 1;
    }
    return 0;
}

static void dyn_table_add(const uint8_t *digest)
{
    uint8_t slot;
    if (dyn_count < CERT_PIN_DYN_MAX) {
        slot = dyn_count++;
    } else {
        /* Full — evict the oldest (FIFO via dyn_head). */
        slot = dyn_head;
        dyn_head = (uint8_t)((dyn_head + 1) % CERT_PIN_DYN_MAX);
    }
    for (uint32_t j = 0; j < 32; j++) dyn_digests[slot][j] = digest[j];
    dyn_dirty = true;
}

/* Persist dyn_digests[0..dyn_count) to osfs2.  Format:
 *   uint32 LE magic
 *   uint32 LE count
 *   N * 32 bytes digest
 * Called from cert_pin_check_leaf after a new pin is captured. */
static void dyn_table_save(void)
{
    if (!dyn_dirty) return;
    if (!osfs2_is_mounted()) {
        /* No FS to persist into yet — keep the in-memory copy and
         * try again next time something changes. */
        return;
    }

    uint8_t buf[8 + CERT_PIN_DYN_MAX * 32];
    buf[0] = (uint8_t)(DYN_PIN_MAGIC      ); buf[1] = (uint8_t)(DYN_PIN_MAGIC >> 8);
    buf[2] = (uint8_t)(DYN_PIN_MAGIC >> 16); buf[3] = (uint8_t)(DYN_PIN_MAGIC >> 24);
    buf[4] = dyn_count;
    buf[5] = buf[6] = buf[7] = 0;

    /* Write entries in insertion order, oldest first.  When the
     * ring has wrapped (dyn_count == CERT_PIN_DYN_MAX), the
     * "oldest" entry is at dyn_head; otherwise indices 0..count-1
     * are already in order. */
    uint32_t off = 8;
    if (dyn_count == CERT_PIN_DYN_MAX) {
        for (uint8_t i = 0; i < CERT_PIN_DYN_MAX; i++) {
            uint8_t idx = (uint8_t)((dyn_head + i) % CERT_PIN_DYN_MAX);
            for (uint32_t j = 0; j < 32; j++) buf[off++] = dyn_digests[idx][j];
        }
    } else {
        for (uint8_t i = 0; i < dyn_count; i++)
            for (uint32_t j = 0; j < 32; j++) buf[off++] = dyn_digests[i][j];
    }
    uint32_t total = off;

    osfs2_delete(DYN_PIN_PATH);   /* ignore missing-file errors */
    void *f = osfs2_create(DYN_PIN_PATH, (uint64_t)total);
    if (!f) {
        serial_puts("[PIN] dyn save: osfs2_create failed\n");
        return;
    }
    int wr = osfs2_write(f, 0, buf, (uint64_t)total);
    if (wr < 0) {
        serial_puts("[PIN] dyn save: osfs2_write failed\n");
        return;
    }
    dyn_dirty = false;
    serial_puts("[PIN] dyn table saved (");
    serial_putdec((uint64_t)dyn_count);
    serial_puts(" entries)\n");
}

int cert_pin_load_dynamic(void)
{
    if (!osfs2_is_mounted()) return -1;

    vfs_stub_t node;
    if (!vfs_find(DYN_PIN_PATH, 0, &node)) return -1;

    uint8_t buf[8 + CERT_PIN_DYN_MAX * 32];
    int n = vfs_read(&node, 0, buf, sizeof buf);
    if (n < 8) return -1;

    uint32_t magic = (uint32_t)buf[0]
                   | ((uint32_t)buf[1] << 8)
                   | ((uint32_t)buf[2] << 16)
                   | ((uint32_t)buf[3] << 24);
    if (magic != DYN_PIN_MAGIC) {
        serial_puts("[PIN] dyn load: bad magic, ignoring\n");
        return -1;
    }
    uint8_t count = buf[4];
    if (count > CERT_PIN_DYN_MAX) count = CERT_PIN_DYN_MAX;
    if (n < (int)(8 + (uint32_t)count * 32)) return -1;

    dyn_count = count;
    dyn_head  = 0;
    for (uint8_t i = 0; i < count; i++)
        for (uint32_t j = 0; j < 32; j++)
            dyn_digests[i][j] = buf[8 + i * 32 + j];
    dyn_dirty = false;

    serial_puts("[PIN] dyn table loaded (");
    serial_putdec((uint64_t)dyn_count);
    serial_puts(" entries)\n");
    return 0;
}

/* ── Mode (default WARN until table is populated) ──────────── */

static cert_pin_mode_t g_mode = CERT_PIN_WARN;

void cert_pin_set_mode(cert_pin_mode_t m) { g_mode = m; }
cert_pin_mode_t cert_pin_get_mode(void)   { return g_mode; }

/* ── Helpers ───────────────────────────────────────────────── */

static void puthex(uint8_t b)
{
    static const char hx[] = "0123456789abcdef";
    char out[3] = { hx[(b >> 4) & 0xF], hx[b & 0xF], 0 };
    serial_puts(out);
}

static int digests_equal(const uint8_t *a, const uint8_t *b)
{
    /* Constant-time over 32 bytes; not security-critical here but
     * good hygiene since this is a comparison of cryptographic
     * digests. */
    uint8_t diff = 0;
    for (uint32_t i = 0; i < 32; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0 ? 1 : 0;
}

/* ── Main entry ────────────────────────────────────────────── */

/* Walk every cert in the TLS Certificate handshake message, hash it,
 * log the SHA-256 so the operator can copy it into pin_table[], and
 * succeed as soon as ANY cert in the chain matches a pin. This means
 * the operator can pin either the leaf (rotates ~90d) or the
 * intermediate (rotates ~years) — pinning the intermediate is more
 * stable since leaf certs rotate frequently but their issuer doesn't. */
int cert_pin_check_leaf(const uint8_t *cert_msg, uint32_t cert_msg_len)
{
    if (g_mode == CERT_PIN_OFF) return 0;

    if (cert_msg_len < 6) {
        serial_puts("[PIN] cert msg too short — rejecting\n");
        return (g_mode == CERT_PIN_STRICT) ? -1 : 0;
    }
    uint32_t total = ((uint32_t)cert_msg[0] << 16)
                   | ((uint32_t)cert_msg[1] <<  8)
                   |  (uint32_t)cert_msg[2];
    if (total + 3 > cert_msg_len || total < 3) {
        serial_puts("[PIN] malformed cert list — rejecting\n");
        return (g_mode == CERT_PIN_STRICT) ? -1 : 0;
    }

    /* Walk the chain. Each entry is uint24 cert_len + cert bytes. */
    uint32_t off = 3;
    uint32_t end = 3 + total;
    int      cert_idx = 0;
    int      any_match = 0;
    int      dyn_match = 0;
    const cert_pin_t *match_entry = 0;
    uint8_t  leaf_digest[32];
    bool     have_leaf = false;
    /* Capture cert pointers as we walk so we can run RFC 5280 chain
     * signature verification (A12.5) after the pin check.  Up to 4
     * cap; CF chains we see today are 2-3 entries.  Pointers into
     * cert_msg are stable for the duration of this call. */
    const uint8_t *chain_ptr[4];
    uint32_t       chain_len_arr[4];
    int            chain_count = 0;
    while (off + 3 <= end) {
        uint32_t cert_len = ((uint32_t)cert_msg[off]     << 16)
                          | ((uint32_t)cert_msg[off + 1] <<  8)
                          |  (uint32_t)cert_msg[off + 2];
        off += 3;
        if (off + cert_len > end) {
            serial_puts("[PIN] malformed chain — rejecting\n");
            return (g_mode == CERT_PIN_STRICT) ? -1 : 0;
        }
        uint8_t digest[32];
        sha256(cert_msg + off, cert_len, digest);

        if (cert_idx == 0) {
            for (uint32_t j = 0; j < 32; j++) leaf_digest[j] = digest[j];
            have_leaf = true;
        }

        /* Always log every chain cert so the operator can pick which
         * one to pin (leaf is most specific; intermediate is most
         * stable; root is most coarse). */
        serial_puts("[PIN] cert#");
        serial_putdec((uint64_t)cert_idx);
        serial_puts(" SHA-256 = ");
        for (uint32_t i = 0; i < 32; i++) puthex(digest[i]);
        serial_puts(" (");
        serial_putdec(cert_len);
        serial_puts(" B)\n");

        for (uint32_t i = 0; i < PIN_TABLE_LEN && !any_match; i++) {
            if (digests_equal(pin_table[i].digest, digest)) {
                any_match = 1;
                match_entry = &pin_table[i];
            }
        }
        if (!any_match && dyn_table_has(digest)) {
            any_match = 1;
            dyn_match = 1;
        }
        if (chain_count < 4) {
            chain_ptr[chain_count]     = cert_msg + off;
            chain_len_arr[chain_count] = cert_len;
            chain_count++;
        }
        off += cert_len;
        cert_idx++;
    }

    /* A12.6: validity-window check.  Reject any cert in the chain
     * whose notBefore/notAfter doesn't bracket the current clock.
     * If NTP hasn't synced (ntp_get_utc returns 0), skip cleanly —
     * the pin is still authoritative.  Informative mode: log but
     * don't abort. */
    extern uint32_t ntp_get_utc(void);
    extern int x509_check_validity(const uint8_t *cert, uint32_t len,
                                   uint32_t now_utc);
    uint32_t now_utc = ntp_get_utc();
    if (now_utc != 0) {
        for (int i = 0; i < chain_count; i++) {
            int v = x509_check_validity(chain_ptr[i], chain_len_arr[i], now_utc);
            serial_puts("[PIN] cert#");
            serial_putdec((uint64_t)i);
            serial_puts(" validity: ");
            serial_puts(v == 0 ? "OK\n" : "OUT OF WINDOW\n");
        }
    } else {
        serial_puts("[PIN] validity skipped (NTP not synced)\n");
    }

    /* A12.5: cryptographic chain link verification.  For each pair
     * (cert[i], cert[i+1]), check that cert[i]'s signature was made
     * by cert[i+1]'s public key.  Pinning above already gives us
     * trust in the leaf/intermediate; this check adds defense-in-
     * depth — if a static pin matched a stale intermediate but the
     * leaf was rotated through a different chain, the link verify
     * would catch it.  Logs result; does NOT abort the handshake on
     * failure (yet — this is informative-mode rollout). */
    if (chain_count >= 2) {
        extern int x509_verify_chain_link(const uint8_t *child,  uint32_t cl,
                                          const uint8_t *issuer, uint32_t il);
        for (int i = 0; i + 1 < chain_count; i++) {
            int r = x509_verify_chain_link(chain_ptr[i],     chain_len_arr[i],
                                           chain_ptr[i + 1], chain_len_arr[i + 1]);
            serial_puts("[PIN] chain link ");
            serial_putdec((uint64_t)i);
            serial_puts(" → ");
            serial_putdec((uint64_t)(i + 1));
            serial_puts(": ");
            serial_puts(r == 0 ? "OK\n" : "FAIL\n");
        }
    }

    if (any_match) {
        if (dyn_match) {
            serial_puts("[PIN] match: dynamic pin\n");
        } else {
            serial_puts("[PIN] match: ");
            serial_puts(match_entry->label);
            serial_puts("\n");
        }
        /* Dynamic capture: chain validated through a *static*
         * pin (not just the dynamic table itself, which would be a
         * tautology), but the leaf isn't yet recorded.  Add it so
         * that we have a leaf-level fingerprint when the static
         * intermediate eventually rotates.  Guarded against the
         * dyn_match case to avoid re-adding entries we just matched
         * against. */
        if (!dyn_match && have_leaf && !dyn_table_has(leaf_digest)) {
            dyn_table_add(leaf_digest);
            serial_puts("[PIN] dyn capture: new leaf added\n");
            dyn_table_save();
        }
        return 0;
    }
    if (g_mode == CERT_PIN_STRICT) {
        serial_puts("[PIN] STRICT: no chain cert matches the pin table — abort\n");
        return -1;
    }
    serial_puts("[PIN] WARN: no chain cert in pin table — accepting (mode=warn)\n");
    return 0;
}

/* ── Operator CA bundle (A12.9) ─────────────────────────────────
 *
 * Reads `osfs2:tls/roots.txt`, one SHA-256 hex digest per line.
 * Each parsed digest is added to the dynamic pin table.  Lets an
 * operator extend trust without rebuilding the kernel.
 *
 * Lines starting with '#' or whitespace-only are skipped.  Trailing
 * comments after a digest (e.g. "<hex> # Some CA") are tolerated.
 */

#define OPERATOR_ROOTS_PATH  "tls/roots.txt"
#define OPERATOR_ROOTS_MAX   16384   /* upper bound on file size */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int cert_pin_load_operator_roots(void)
{
    if (!osfs2_is_mounted()) return -1;
    vfs_stub_t node;
    if (!vfs_find(OPERATOR_ROOTS_PATH, 0, &node)) return -1;

    static uint8_t buf[OPERATOR_ROOTS_MAX + 1];
    uint32_t cap = node.size < OPERATOR_ROOTS_MAX ? (uint32_t)node.size
                                                  : OPERATOR_ROOTS_MAX;
    int n = vfs_read(&node, 0, buf, cap);
    if (n < 0) return -1;
    buf[n] = 0;

    int added = 0;
    uint32_t i = 0;
    while ((int)i < n) {
        /* Skip leading whitespace */
        while ((int)i < n && (buf[i] == ' ' || buf[i] == '\t')) i++;
        /* Comment or blank line? */
        if ((int)i >= n || buf[i] == '#' || buf[i] == '\n' || buf[i] == '\r') {
            while ((int)i < n && buf[i] != '\n') i++;
            if ((int)i < n) i++;
            continue;
        }
        /* Parse 64 hex chars → 32-byte digest. */
        uint8_t digest[32];
        int ok = 1;
        for (int b = 0; b < 32; b++) {
            if ((int)(i + 1) >= n) { ok = 0; break; }
            int hi = hex_nibble((char)buf[i]);
            int lo = hex_nibble((char)buf[i + 1]);
            if (hi < 0 || lo < 0) { ok = 0; break; }
            digest[b] = (uint8_t)((hi << 4) | lo);
            i += 2;
        }
        if (!ok) {
            /* Drain bad line. */
            while ((int)i < n && buf[i] != '\n') i++;
            if ((int)i < n) i++;
            continue;
        }
        /* Trailing space or comment is OK; skip to newline. */
        while ((int)i < n && buf[i] != '\n') i++;
        if ((int)i < n) i++;

        /* Add to dynamic table if not already present. */
        if (!dyn_table_has(digest)) {
            dyn_table_add(digest);
            added++;
        }
    }
    if (added > 0) {
        dyn_table_save();
        serial_puts("[PIN] operator roots loaded: +");
        serial_putdec((uint64_t)added);
        serial_puts(" digests from tls/roots.txt\n");
    }
    return added;
}
