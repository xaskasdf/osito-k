# A12.3 — Full PKI chain validation roadmap

> **2026-05-16 status update (afternoon):** A12.3 mostly done.
> Chain link verify covers RSA-SHA256/384 and
> ECDSA-P256-SHA256/384 (A12.5).  Validity-window check parses
> UTCTime + GeneralizedTime and compares against NTP UTC (A12.6,
> with boot-time self-test).  GTS Root R4 pinned as a third
> entry in the static table — chain anchored at root depth
> even without a P-384 verify primitive.  See "Remaining work"
> at the bottom for the residual debt.

The kernel's TLS trust model currently rests on three layers:

1. **Static leaf+intermediate pin** in `kernel/cert_pin.c::pin_table[]`.
2. **Dynamic leaf capture** (added 2026-05-16) that records new leaves
   after a static-intermediate match and persists them to
   `osfs2:tls/pins.bin`, so leaf rotations don't break the next boot.
3. **ECDSA SKE signature verification** in `kernel/tls.c` against the
   leaf cert's SubjectPublicKey extracted via `kernel/x509.c`.

What's missing to claim a full RFC 5280 chain validator:

| Piece                                                 | LoC |
|-------------------------------------------------------|-----|
| RSA-2048 PKCS#1 v1.5 signature verify (bigint expmod) | 400 |
| Full X.509 ASN.1 parser (NotBefore/NotAfter, SAN, KeyUsage, BasicConstraints) | 300 |
| Chain walker (find issuer in trust store + bundled chain, validate each link) | 200 |
| Hostname → SAN matching (DNS-id, wildcard, IP-id)     | 150 |
| CA root bundle parser + on-disk storage (~250 KB of trusted roots) | 200 |
| RTC clock readback for time-window check              | 50  |
| **Total realistic**                                   | **1300** |

## Why still deferred (2026-05-16)

- **Scope cost.**  Same magnitude as TLS 1.3.
- **Endpoint set is closed.**  This fork talks to exactly one
  endpoint: our own Cloudflare-fronted broker (rag.naranjositos.tech
  / factory.naranjositos.tech / inferconnect.naranjositos.tech).
  All three terminate on the same CF + GTS WE1 chain, both pinned.
- **Dynamic leaf capture** (this session) handles the leaf-rotation
  failure mode without operator intervention.  The previous biggest
  PKI-pin-only concern is now mostly addressed.
- **No 3rd-party endpoint pending.**  No item on the roadmap pulls
  in a new TLS endpoint outside our pin coverage.

## Remaining work after A12.6 (2026-05-16 PM)

| Piece                                                       | LoC | Status |
|-------------------------------------------------------------|-----|--------|
| ~~sha384~~                                                  | ~~95~~ | ✅ done (`crypto2.c::sha384` + RFC 180-4 self-test) |
| ~~SHA-384 sig algorithm OIDs (RSA + ECDSA variants)~~       | ~~30~~ | ✅ done (`x509.c::sigalg_recognize`) |
| ~~Validity-window check (`notBefore`/`notAfter`)~~          | ~~150~~ | ✅ done (A12.6, `x509.c::x509_check_validity`) |
| ~~Root anchoring~~                                          | ~~200~~ | ✅ done (GTS Root R4 in `cert_pin.c::pin_table`) |
| Hostname → SAN matching                                     | 150 | deferred (only useful with a non-pinned 3rd party) |
| Full CA root bundle on disk                                 | 200 | deferred (root-anchoring via pin works for our endpoint set) |
| P-384 ECDSA verify primitive                                | 500 | deferred (would crypto-verify WE1→Root; root is pin-anchored instead) |
| NTP sync through vmnet                                      |  ?  | deferred (validity check skips cleanly when NTP=0) |

The remaining items are all "deferred" rather than "TODO" because
they only matter once we add a third-party non-pinned endpoint.
For the closed CF-fronted endpoint set we have today, the
combination of static intermediate pin + static root pin +
dynamic leaf capture + chain-link signature verify + validity
window check is the full security property of RFC 5280 chain
validation minus the trust anchoring that we replace with our
own pin table.

## When to revisit

- An external agent or library we want to integrate ships its own
  TLS endpoint with a different chain (e.g. an outside MCP server,
  a different LLM provider).
- We need to validate against an arbitrary cert that we can't pin
  ahead of time (e.g. an opportunistic capture of a "found-in-the-
  wild" peer).
- Threat model tightens to defend against an adversary with both
  CF intermediate compromise AND control over our boot-time pin
  table — at which point pinning alone is insufficient and we need
  proper trust anchoring.

For all current workloads the pin+ECDSA-SKE+dynamic-capture stack
is the right cost/benefit balance.
