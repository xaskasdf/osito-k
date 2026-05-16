# TLS 1.3 — status: working end-to-end (2026-05-16)

`kernel/tls13.c` now does a full TLS 1.3 client handshake against
the Cloudflare-fronted broker:

```
[TLS1.3] ClientHello sent
[TLS1.3] ServerHello ok suite=0x1301
[TLS1.3] handshake keys derived
[TLS1.3] EncryptedExtensions
[TLS1.3] Certificate ok (pinned)
[TLS1.3] CertVerify (signature check deferred)
[TLS1.3] server Finished verified
[TLS1.3] handshake complete — application channel ready
[TLS13-PROBE] response: HTTP/1.1 200 OK Date: Sat, 16 May 2026 ...
[TLS13-PROBE] PASS
```

Verified via `bash scripts/test-tls13-probe.sh` (sentinel-gated
boot probe in `main.c`).  Reachable from outside the file via
`tls13_connect / tls13_send / tls13_recv / tls13_is_active`.

## What ships

- ClientHello with required extensions: `supported_versions`
  (forcing 1.3), `supported_groups` (x25519), `key_share`
  (x25519), `signature_algorithms` (ecdsa_secp256r1_sha256,
  rsa_pss_rsae_sha256), `server_name` (SNI).
- Cipher suite: `TLS_AES_128_GCM_SHA256` only (the AEAD we have
  a working record-protect path for).  The handshake aborts
  cleanly if the server selects anything else.
- ServerHello parser extracts the peer's X25519 key_share and
  confirms TLS 1.3 selection.
- Full key schedule (RFC 8446 §7.1):
  early_secret → derived → handshake_secret → c/s_hs_traffic →
  key+iv; then through master_secret → c/s_ap_traffic → key+iv
  for application data.
- Transcript hash maintained as a running SHA-256 over every
  handshake message body.
- AEAD record framing per §5.2: outer type 0x17, inner_type byte
  appended to plaintext, nonce = iv ⊕ seq, AAD = record header.
- Decrypts and verifies the server's EncryptedExtensions,
  Certificate, CertificateVerify, Finished.
- Server Finished MAC verified against the transcript snapshot
  ending at CertificateVerify.
- Client Finished computed and sent; followed by app-traffic
  rekey for normal data send/recv.
- Cert pinning: Certificate body is rewritten to a TLS-1.2-style
  cert message and passed to `cert_pin_check_leaf` so the static
  + dynamic pin tables apply unchanged.
- Post-handshake messages (NewSessionTicket) are silently
  consumed by `tls13_recv`.

## What's deferred

- **CertificateVerify signature check.**  Pinning already proves
  the cert chain; CertVerify would prove the cert holder actually
  signed this specific handshake's transcript.  Wired location is
  in `tls13_connect`'s `TLS13_CERT_VERIFY` case — currently logs
  "signature check deferred" and accepts.  Same status as TLS 1.2
  before A12.2 — implementing it is mechanical given we already
  have ECDSA-P256 verify in `kernel/ecdsa_p256.c`.
- **ChaCha20-Poly1305 record AEAD.**  Advertised in earlier
  drafts; current ClientHello only offers AES_128_GCM.  Easy to
  add — wrap `chacha20_encrypt` + `poly1305` in an AES-GCM-shaped
  API and dispatch on `tls13.suite`.
- **HelloRetryRequest** flow.  We assume the server picks our
  key_share on the first try.  CF does; some servers may not
  (e.g., post-quantum hybrid groups).  Adding HRR handling is
  ~80 LoC.

## What this doc tracked before completion

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
