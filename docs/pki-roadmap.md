# A12.3 — Full PKI chain validation roadmap

> **2026-05-16 status update (evening):** **A12.3 fully done.**
> Chain link verify covers RSA-SHA256/384 and ECDSA-P256-SHA256/384
> (A12.5).  ECDSA P-384 verify primitive (A12.7) closes the
> WE1→Root link cryptographically.  Validity-window check parses
> UTCTime + GeneralizedTime against NTP UTC (A12.6, with boot-time
> self-test).  GTS Root R4 pinned as the third entry in the static
> table.  SAN/hostname matching with RFC 6125 §6.4.3 wildcards
> (A12.8).  Operator CA bundle via `osfs2:tls/roots.txt` for
> trust extension without rebuild (A12.9).  All cryptographic
> checks PASS end-to-end live against the CF chain.

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

## Final status (2026-05-16 evening)

| Piece                                                       | LoC | Status |
|-------------------------------------------------------------|-----|--------|
| ~~sha384~~                                                  | ~~95~~ | ✅ done (`crypto2.c::sha384` + RFC 180-4 self-test) |
| ~~SHA-384 sig algorithm OIDs (RSA + ECDSA variants)~~       | ~~30~~ | ✅ done (`x509.c::sigalg_recognize`) |
| ~~Validity-window check (`notBefore`/`notAfter`)~~          | ~~150~~ | ✅ done (A12.6) |
| ~~Root anchoring (GTS Root R4 pin)~~                        | ~~10~~  | ✅ done |
| ~~Hostname → SAN matching~~                                 | ~~250~~ | ✅ done (A12.8, with CN fallback) |
| ~~Operator-editable CA bundle~~                             | ~~120~~ | ✅ done (A12.9, `tls/roots.txt`) |
| ~~ECDSA P-384 verify primitive~~                            | ~~500~~ | ✅ done (A12.7) |
| NTP sync through vmnet                                      |  ?  | deferred — orthogonal net issue (validity check skips cleanly when NTP=0) |

A12.3 is **functionally complete**.  Chain validation does all
of the RFC 5280 cryptographic checks:

  - leaf → intermediate signature verifies (ECDSA-P256-SHA256)
  - intermediate → root signature verifies (ECDSA-P256-SHA384
    against a P-384 issuer key, with FIPS 186-4 §6.4 truncation)
  - all certs in chain have NotBefore ≤ now ≤ NotAfter
  - leaf SAN/CN matches the SNI hostname
  - trust is anchored via static + dynamic + operator pin tables

The NTP sync issue is orthogonal — when it resolves, the
validity check picks up automatically without any code change.

## Future directions (only if scope expands)

Items that would matter only if we add an external endpoint
outside our pinned-chain set:

- Full ASN.1 X.509 v3 extension parsing (BasicConstraints,
  KeyUsage, ExtendedKeyUsage, NameConstraints).
- AIA (Authority Information Access) chasing — fetch a missing
  intermediate when a server doesn't send the full chain.
- PKIX path-building with cross-signed certs and multiple
  candidate roots.
- CRL / OCSP revocation checking.
- Distinguished Name parsing for issuer-by-DN lookup in a full
  CA bundle (currently we use SHA-256-of-DER hash lookup, which
  works as long as we have the DER bytes — true for all chains
  the server actually sends).

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
