/*
 * cert_pin.h — TLS certificate pinning.
 *
 * Until the kernel grows a real X.509 / ECDSA verifier (A12 follow-up),
 * we get MITM protection by hashing the leaf certificate the server
 * presents and comparing against a small list of trusted SHA-256
 * fingerprints baked into the kernel image.
 *
 * Trust model: the operator explicitly pins each endpoint's cert
 * fingerprint at build time. An attacker who reroutes traffic but
 * doesn't have the matching cert + private key is rejected at the
 * Certificate handshake step.
 *
 * Limitations vs full PKI:
 *   - No chain validation; only the leaf is checked.
 *   - Cert rotation requires a kernel rebuild + re-pin.
 *   - The ServerKeyExchange ECDSA signature is still NOT verified
 *     (separate A12 sub-item) — pinning ensures we got the right
 *     cert; signature verification ensures the SKE actually came
 *     from that cert's owner. Both are needed for full MITM defense.
 */

#ifndef OSITOA_CERT_PIN_H
#define OSITOA_CERT_PIN_H

#include "../include/types.h"

/* Mode controls behavior on pin miss:
 *   STRICT  — reject, abort handshake
 *   WARN    — print warning, accept (for development)
 *   OFF     — never check (compat with old paths)
 *
 * Default is WARN until the pin table is populated for the active
 * endpoints; flip to STRICT once verified. */
typedef enum {
    CERT_PIN_OFF    = 0,
    CERT_PIN_WARN   = 1,
    CERT_PIN_STRICT = 2,
} cert_pin_mode_t;

/* Set the global enforcement mode (default WARN). */
void cert_pin_set_mode(cert_pin_mode_t mode);
cert_pin_mode_t cert_pin_get_mode(void);

/* Check the LEAF cert from a TLS Certificate message body.
 *
 * `cert_msg` points at the start of the message body (the 3-byte
 * total-list-length followed by cert entries). `cert_msg_len` is the
 * full body length. The function extracts the first ASN.1 cert,
 * SHA-256 hashes it, and looks the digest up in the pin table.
 *
 * Returns:
 *    0  pin matched (or mode is OFF / WARN)
 *   -1  pin missed and mode is STRICT (caller should abort handshake)
 */
int cert_pin_check_leaf(const uint8_t *cert_msg, uint32_t cert_msg_len);

/* ── Dynamic pin table ─────────────────────────────────────────
 *
 * In addition to the compiled-in static pin table, the kernel
 * maintains a runtime pin table loaded from osfs2 at boot and
 * appended to whenever a chain matches a static *intermediate* pin
 * but the leaf has rotated.  This gives the kernel leaf-level
 * fingerprint memory across leaf-rotation cycles without an
 * operator rebuild: as long as the static intermediate covers us
 * during the first handshake of each rotation, the new leaf is
 * captured and persisted automatically.
 *
 * Stored on disk as `tls/pins.bin`: 4-byte LE count, followed by
 * N × 32-byte digests.  Maximum CERT_PIN_DYN_MAX entries; oldest
 * entries fall out when full (FIFO).
 *
 * Returns 0 on success, -1 on parse error / fs unavailable. */
int cert_pin_load_dynamic(void);

/* ── Operator CA bundle (A12.9) ────────────────────────────────
 *
 * Reads a text file at `osfs2:tls/roots.txt`, one SHA-256 hex
 * digest per line (with optional `#`-prefixed comments and
 * trailing space/tab tolerated).  Each parsed digest is added to
 * the dynamic pin table — so an operator can extend the kernel's
 * trust set without rebuilding by writing the file to osfs2.
 *
 * Format example:
 *     # Issued by some new CA we want to trust
 *     76b27b80a58027dc3cf1da68dac17010ed93997d0b603e2fadbe85012493b5a7
 *     # Another root
 *     1dfc1605fbad358d8bc844f76d15203fac9ca5c1a79fd4857ffaf2864fbebf96
 *
 * Returns the number of digests added, or -1 on FS unavailable. */
int cert_pin_load_operator_roots(void);

#endif
