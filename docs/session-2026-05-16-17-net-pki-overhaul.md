# 2026-05-16 → 2026-05-17 — Network + PKI overhaul

A ~25 h session that took osito-k's HTTPS stack from "TLS 1.2 with informal
pinning + RAG truncated at 4758 bytes" to "TLS 1.3 preferred, full RFC 5280 +
RFC 6125 + RFC 6960 chain validation with stapling + AIA chase + CRL
fallback, all over a polled virtio-net with RFC 7323/6298/2018-complete TCP".

All code is bare-metal C, no OpenSSL / BoringSSL / libcrypto.

## Commit timeline (network/PKI subset)

Ports from `~/osito-a` (the agent-focused fork) and original work in
`experiment` branch. Total 55 commits in the window; 26 of those are
network/PKI/TLS work (rest are UT99, win32 compat, GPU, etc. in parallel
sessions).

### Day 1 — TCP stack catch-up + RAG client + cert_pin foundation

```
3c69a90  port osito-a@c01e4a7: native RAG client + UDP listener dedupe + DNS sti;hlt fix
921551f  port osito-a@f43cbc5: rag Path 2 + TCP_RX 8K → 64K
49ff45a  port osito-a@4007d3e: dynamic TCP receive window + window-update ACK
ddd816c  port osito-a@0fcacbc: http_read_body distinguishes timeout vs hard close
0947334  port osito-a@15fe11a: MSS option in SYN + vmnet test path
db41032  port osito-a@5999926: virtio-net modern PCI transport rewrite + RX ring 32 → 256
d7292c6  native kernel RAG client + `wiki` shell command (factory.naranjositos.tech)
1e0bc1b  shell: fix `wiki` dispatch — escape outer __EMSCRIPTEN__ wrapper
c5d3596  cert_pin MVP + HKDF foundation + RFC 7323 window scaling + virtio-net wire-up
630172c  docs: port osito-a deferral notes (slirp, PKI, TLS 1.3)
```

### Day 1 evening — TLS 1.3 cliente real + PKI primitives

```
6bed531  port osito-a: TLS 1.3 real (X25519 + AES-128-GCM + HKDF) + RSA-2048
         PKCS#1 v1.5 SHA-256 verify + ECDSA-P256 verify + x509 chain link verify
6cea637  port osito-a@21850a4+b474d04: A12.3 closeout — SHA-384 + RSA-SHA384 +
         X.509 validity window check + NTP fallback time.google.com
c2dffac  port osito-a@81bda8f+7e6c789+3c90bbe+a5ff6f0: A12.3 fully done —
         ECDSA P-384 + SAN/hostname matching (RFC 6125) + operator CA bundle
```

### Day 2 night — TCP RFC stack overhaul + chain extensions

```
35f2a68  net: RFC 2018 SACK — negotiate (SACK_PERMITTED in SYN) + receiver-side
         block emission (kind=5, single-block)
99329ea  net: RFC 7323 timestamps + PAWS — TS option (kind=8) on every segment,
         ts_recent tracking, PAWS drop of stale segments
8d0d2a9  net: sender-side SACK + Jacobson RTT smoothing (RFC 6298) — SRTT,
         RTTVAR, RTO clamped [100, 60000] ticks, dynamic backoff
e7d1769  port osito-a@c60a23b+acd6426+8ae5cd5: A12.10/.11/.12 — v3 extension
         parsing + chain constraints (BasicConstraints, KeyUsage, pathLen) +
         AIA URL extraction + plain HTTP client + OCSP client (RFC 6960)
```

### Day 2 night — parallel agent burst 1 (4 agents)

```
d7aece1  net: SACK multi-block emission (RFC 2018 §3, up to 4 blocks)
38fe94f  x509: AIA chase end-to-end — fetch + chain extend + osfs2 cache
6fc1e5f  crl: RFC 5280 §5 revocation list check (informative mode)
4dcf2b1  ocsp: responder signature verification (RFC 6960 §4.2.2) — RSA-SHA256
         + ECDSA-P256-SHA256 dispatch over tbsResponseData
```

### Day 2 dawn — parallel agent burst 2 (3 agents) + H1 fix + final wave

```
64be6da  crl: signature verification over tbsCertList (RFC 5280 §5.1.1+§5.2.5)
940453e  tls13: wire CRL fallback after OCSP (policy: OCSP wins, CRL on error)
         + recursive AIA chase up to depth 3 with FNV-1a-32 URL dedup
fe17ce0  docs: ntp-vmnet deferral notes (H1-H5 hypothesis matrix)
693fa47  net: drain polled NIC in ARP/SYN-ACK/close wait loops (H1 from
         kupdate-hung-at-Connecting diagnosis)
```

### Day 2 morning — final 3-agent burst + TLS 1.2/1.3 negotiation

```
7b1240e  net: SACK sender multi-block parse + RFC 6675 §4 prefix advance +
         tx_buf 4096 → 16384 (~11 MSS, BDP-sized)
7d31910  tls13: CRL wiring uses 4-arg API (issuer plumbed → sig verify active) +
         AIA dedup adds head-prefix check (FNV collision defense)
e7ca940  tls13: OCSP stapling (RFC 6066 §8 status_request) — client-side
         extension + per-cert ext walker + ocsp_parse_stapled
4a757f7  http: prefer TLS 1.3, fallback to TLS 1.2 on handshake failure —
         http_open dispatch + http_tls_{send,recv,close} inline wrappers
```

## Architecture post-overhaul

### TLS handshake pipeline

```
http_open(hostname) → DNS resolve → TCP connect (port 443)
  ↓
tls13_connect(tcp_conn, hostname):
  • ClientHello + supported_versions [1.3] + key_share x25519 +
    SNI + status_request (OCSP staple req)
  • ServerHello → parse supported_versions + key_share → ECDH
  • HKDF key schedule (RFC 8446 §7.1) → handshake traffic keys
  • EncryptedExtensions
  • Certificate (per-entry ext walker)
    - Leaf has stapled OCSP? → parse + verify in-handshake → skip outbound
    - AIA caIssuers URL? → chase missing intermediates (depth ≤ 3, head+hash
      dedup, osfs2 cache: tls/aia-<hash8>.der)
  • CertificateVerify
  • For each link: x509_verify_chain_link
    - ECDSA-P256-SHA256 / ECDSA-P384-SHA256 / ECDSA-P256-SHA384 /
      ECDSA-P384-SHA384 / RSA-SHA256 / RSA-SHA384 (curve from issuer's SPKI)
  • x509_chain_constraints (BasicConstraints CA=TRUE, KeyUsage keyCertSign,
    pathLenConstraint propagation along the chain)
  • x509 validity window check (UTCTime + GeneralizedTime + Hinnant
    civil_to_unix → compare against ntp_get_utc())
  • x509_match_hostname (RFC 6125 §6.4.3 SAN dNSName, single-label wildcard,
    CN fallback)
  • cert_pin: static (3 pins: ISRG X1, GTS R4, GTS WE1) +
    dynamic (osfs2 tls/pins.bin, captured leaves) +
    operator (osfs2 tls/roots.txt, hex digests one per line)
  • For each non-leaf link without staple: ocsp_check (build OCSP request,
    POST via http_plain to responder URL, parse BasicOCSPResponse, verify
    responder sig via ocsp_verify_sig).
  • On OCSP ERROR / UNKNOWN: crl_check_revoked_with_issuer fallback
    (download via http_plain, parse revokedCertificates, verify sig
    via crl_verify_sig, osfs2 cache: tls/crl-<hash8>.der)
  • server Finished MAC verify → client Finished → app traffic keys live
  ↓
fail → close TCP + reconnect + tls_connect (TLS 1.2 legacy fallback)

http_request / http_read_body: send/recv dispatch via http_tls_{send,recv}
based on session->use_tls13 flag.
```

### TCP stack post-overhaul

```
SYN options (40 bytes — RFC 1122 ceiling):
  MSS (4)  + NOP (1) + WS (3) + SACK_OK (2) + TS (10)
                                              ↑ all 4 RFC features
non-SYN options:
  TS (12) if tsopt_ok | SACK (variable, up to 4 blocks) if blocks pending
  Bare 20-byte header for plain ACKs without TS+SACK

SACK state per conn (tcp_conn_t):
  - sack_ok           ← SACK_PERMITTED received in peer's SYN
  - sack_blocks[4][2] ← OOO ranges, most-recent-first, coalesced on overlap
  - n_sack_blocks
  - peer_sack_ranges parsed on incoming ACK → walked in tx_sacked decision +
    RFC 6675 §4 step (a) prefix advance into tx_buf

TS state per conn:
  - tsopt_ok          ← TS option received in peer's SYN
  - ts_recent         ← most-recent in-order peer TSval (echoed in TSecr)
  - PAWS drop: incoming segments with peer_tsval < ts_recent get refresh-ACK
    and data discarded (RFC 7323 §5.3)

Jacobson RTT (RFC 6298):
  - srtt, rttvar      ← seeded from first sample (sample, sample/2)
  - update: RTTVAR ← 3/4·RTTVAR + 1/4·|diff|; SRTT ← SRTT + diff/8
  - rto = SRTT + 4·RTTVAR clamped to [100, 60000] ticks
  - net_poll retransmit loop uses conn->rto instead of fixed 300

Buffers:
  - rx_buf 128 KB (WS=3 advertised) ← RFC 7323 window scaling
  - tx_buf 16 KB  (~11 MSS)         ← BDP-sized; SACK multi-block savings real

Polled-NIC drain:
  - virtio-net binds with irq_pending=NULL (no IRQ)
  - All `sti; hlt; cli` wait loops (ARP, SYN-ACK, close, recv, accept)
    invoke net_poll() right after `cli` so the queue gets drained on each
    APIC tick wake.
```

## RFCs implemented (cumulative across this overhaul)

| RFC | Section | Feature |
|---|---|---|
| 2018  | §2–4    | TCP SACK — negotiate, receiver emit (up to 4 blocks), sender parse |
| 5280  | §4.2.1.6 | SubjectAltName matching |
| 5280  | §4.2.1.9 | BasicConstraints + pathLen |
| 5280  | §4.2.1.10 | NameConstraints (deferred — pinned model covers) |
| 5280  | §4.1.2.5 | validity (NotBefore/NotAfter) |
| 5280  | §5      | CRL revoked-cert lookup + signature verify |
| 6066  | §3       | SNI in TLS ClientHello |
| 6066  | §8       | OCSP stapling — status_request extension |
| 6125  | §6.4.3   | hostname identity check + wildcards |
| 6298  | —       | Jacobson RTT smoothing + RTO computation |
| 6675  | §4       | SACK-aware loss recovery — partial-coverage advance |
| 6960  | §3.2.2   | OCSP request + response + responder sig verify |
| 7323  | §2       | TCP window scaling |
| 7323  | §3–5    | TCP timestamps + PAWS |
| 8446  | §4–7    | TLS 1.3 client handshake + key schedule |

## Crypto primitives written

| Primitive | LoC | File |
|---|---|---|
| SHA-256 / SHA-384 (FIPS 180-4) | ~250 | crypto2.c |
| HKDF (Extract + Expand + Expand-Label, RFC 5869 + 8446 §7.1) | ~120 | crypto2.c |
| ChaCha20-Poly1305 / AES-128-GCM (records) | ~400 | crypto2.c |
| X25519 (RFC 7748) | ~180 | crypto2.c |
| RSA-2048 PKCS#1 v1.5 SHA-256 / SHA-384 verify | ~410 | rsa.c |
| ECDSA P-256 verify (FIPS 186-4 D.2.3) | ~620 | ecdsa_p256.c |
| ECDSA P-384 verify (FIPS 186-4 D.2.4) | ~500 | ecdsa_p384.c |
| X.509 parsing (cert split, SPKI, validity, AIA, CRLDP, v3 exts) | ~1260 | x509.c |
| OCSP client (request build, response parse, sig verify) | ~550 | ocsp.c |
| CRL client (parse, serial check, sig verify) | ~560 | crl.c |
| Plain HTTP/1.1 GET + POST | ~260 | http_plain.c |
| cert_pin (static + dynamic + operator + chain walker) | ~390 | cert_pin.c |
| TLS 1.3 client (handshake + records + AEAD framing) | ~840 | tls13.c |
| TLS 1.2 client (legacy, kept for fallback) | ~ unchanged | tls.c |

Verified self-tests at boot:

```
[KERN] HKDF TLS 1.3 self-test: PASS              (RFC 8448 §3 vectors)
[KERN] RSA-2048 verify self-test: PASS           (openssl pos+neg vectors)
[KERN] ECDSA P-384 verify self-test: PASS        (CAVS vectors)
[KERN] SHA-384 self-test: PASS                   (FIPS 180-4 "abc" vector)
[KERN] X.509 validity self-test: PASS            (4 cases: in/before/after/no-clock)
[NIC] backend: virtio-net (modern, polled)
[PIN] operator roots loaded: +N digests from tls/roots.txt  (if present)
```

## Files added (sizes at end of session)

```
arch/x86/kernel/rag.c            839
arch/x86/kernel/rag.h             65
arch/x86/kernel/cert_pin.c       391
arch/x86/kernel/cert_pin.h        76
arch/x86/kernel/rsa.c            364
arch/x86/kernel/x509.c          1260
arch/x86/kernel/x509.h           184
arch/x86/kernel/ecdsa_p256.c     609
arch/x86/kernel/ecdsa_p256.h      59
arch/x86/kernel/ecdsa_p384.c     473
arch/x86/kernel/ecdsa_p384.h      26
arch/x86/kernel/http_plain.c     261
arch/x86/kernel/ocsp.c           ~550
arch/x86/kernel/ocsp.h            38
arch/x86/kernel/crl.c            ~560
arch/x86/kernel/crl.h             23
arch/x86/scripts/qemu-agent-hvf.sh   98
arch/x86/scripts/test-tls13-probe.sh ~120
```

Plus significant additions to `tls13.c` (~840 lines after all
edits), `net.c` (~+400 lines), `crypto2.c` (~+300 lines), and 6 new
docs files in `docs/`.

## Open deferrals (documented, not lost)

| Item | Why deferred | Doc |
|---|---|---|
| NTP-thru-vmnet | host-side macOS pf NAT; fix is out of kernel scope | [ntp-vmnet-deferral.md](ntp-vmnet-deferral.md) |
| AIA chain extend beyond depth 3 | pathological CA configs not seen in our endpoints | code comment in tls13.c |
| CRL multi-issuer / DN-by-name lookup | only matters with on-disk CA bundle (replaced by pin model) | [pki-roadmap.md](pki-roadmap.md) |
| Full ASN.1 X.509 v3 extension parser (KeyUsage value strictness, NameConstraints, PolicyConstraints) | pinned endpoints don't need it | [pki-roadmap.md](pki-roadmap.md) |
| Parallel TLS 1.3 sessions | tls13.c uses global state; serialized at http_open today | code comment |
| Slirp upstream truncation > 5 KB | upstream libslirp bug; workaround is `NET=vmnet` | [slirp-defer.md](slirp-defer.md) |

## Process notes

- **Parallel agents with worktree isolation** ran 3-4 simultaneous tasks
  twice (rounds 1 and 2). Conflict-free when each agent stayed in a
  single file or in non-overlapping sections of tls13.c. The runtime
  auto-merged worktree commits back to the parent `experiment` branch
  on agent exit — no manual rebase needed.
- **Snapshot-restore vs incremental cherry-pick**: for ports where the
  fork had multiple sequential commits touching the same file (e.g. the
  TLS 1.3 wave 4b2a773 + f04a54a totaling 1369 inserted lines), snapshot
  copy was faster and safer than chained `git apply`. Cherry-picks
  worked cleanly when files were disjoint between commits.
- **The other Claude session** (running in parallel on osito-a's
  agent branch) drove the upstream port queue. We synced by `git fetch
  osito-a` at the start of each round and reported back which commits
  landed locally.
- **Conflict handling**: three orphan UU files (int2e_stub.S,
  dos/cpu8086.c, dos/dos_dpmi.c) left by a parallel stash-pop were
  resolved with `git checkout --ours` and the alternate version
  preserved in `stash@{0}`.
