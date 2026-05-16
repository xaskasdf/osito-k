# TLS 1.3 — roadmap for full client implementation

The kernel currently completes TLS 1.2 handshakes against
Cloudflare-fronted endpoints using `kernel/tls.c`
(ECDHE-ECDSA-AES128-GCM-SHA256, X25519 ECDHE, SKE signature
verification against the leaf cert pulled via `kernel/x509.c`, and
cert pinning against the static + dynamic pin tables).  TLS 1.3
support is a stub in `kernel/tls13.c` that sends a ClientHello with
the `supported_versions` extension but does not complete the
handshake.

This doc tracks what's needed to finish the 1.3 path.

## Why deferred (2026-05-16)

- **Cost.**  ~620 LoC of careful crypto wiring (see breakdown
  below).  Each other item on the roadmap is 30–100 LoC.
- **Marginal upside.**  CF serves TLS 1.2 with the same X25519
  ECDHE we already support; our SKE verify + leaf+intermediate
  pin chain gives us the security property TLS 1.3 advertises
  (the cert+key actually belong to the entity we negotiated
  with).  We don't enable 0-RTT, so the 1-RTT vs 2-RTT cost
  saving doesn't apply to our workload.
- **No forced upgrade.**  No endpoint we depend on (CF / R2 /
  Workers AI / our own broker) is TLS-1.3-only.  Will revisit
  when one arrives.

## Scope breakdown (~620 LoC total)

| Piece                                          | LoC |
|------------------------------------------------|-----|
| ServerHello extension parser (`supported_versions`, `key_share`) | 80 |
| Transcript hash (running SHA-256 over ClientHello ‖ ServerHello ‖ …) | 40 |
| HKDF key schedule (RFC 8446 §7.1)               | 120 |
| AEAD record framing TLS 1.3 (inner_type + AAD)  | 80 |
| Decrypt `EncryptedExtensions`, `Certificate`, `CertificateVerify`, `Finished` | 150 |
| `CertificateVerify` ECDSA (structure differs from 1.2 SKE)  | 60 |
| Server Finished MAC verify + client Finished    | 50 |
| Switch to application traffic secrets           | 40 |

## Key schedule (for reference)

```
early_secret      = HKDF-Extract(salt=zero32, IKM=zero32)
derived           = HKDF-Expand-Label(early_secret, "derived", H(""), 32)
handshake_secret  = HKDF-Extract(salt=derived, IKM=ECDH_shared)
c_hs_traffic      = HKDF-Expand-Label(handshake_secret, "c hs traffic", th, 32)
s_hs_traffic      = HKDF-Expand-Label(handshake_secret, "s hs traffic", th, 32)
{c,s}_write_key   = HKDF-Expand-Label({c,s}_hs_traffic, "key", "", 16)
{c,s}_write_iv    = HKDF-Expand-Label({c,s}_hs_traffic, "iv",  "", 12)
```

Where `th` is the running transcript hash at the point of
derivation.

## Primitives that are already in the tree

- `crypto.c::x25519_scalarmult` — ECDH base/scalar multiply
- `crypto.c::sha256`, `crypto.c::hmac_sha256` — for HKDF
- `crypto2.c::hkdf_extract`, `crypto2.c::hkdf_expand` — base HKDF
- **`crypto2.c::hkdf_expand_label`** — TLS-1.3-specific wrapper
  (added 2026-05-16, verified against RFC 8448 §3 vectors via
  `hkdf_tls13_self_test()` run at boot)
- **`crypto2.c::hkdf_derive_secret`** — TLS 1.3 §7.1
  Derive-Secret convenience wrapper
- `crypto2.c::chacha20_poly1305_*` — AEAD record protection
- `crypto.c::aes128_gcm_encrypt/decrypt` — alternate AEAD

So the missing work is ~all wiring; no new crypto primitives.

## When we'll revisit

- An endpoint we need only speaks 1.3 (e.g. some future
  hyperscaler API drops 1.2 support).
- We want 0-RTT for an explicitly idempotent workload that
  benefits from saving an RTT and accepts the replay risk.
- A research need for post-quantum hybrid key exchange (the
  hybrid x25519+ml-kem-768 share is 1.3-only).
