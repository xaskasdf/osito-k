# NTP-thru-vmnet deferral notes (2026-05-16)

`ntp_sync()` (`arch/x86/kernel/ntp.c`) works under QEMU slirp
(`-netdev user`) but does not complete under
`-netdev vmnet-shared,id=net0` (`sudo NET=vmnet bash arch/x86/scripts/qemu-agent-hvf.sh`).
DNS via the same `net_udp_send` path resolves `time.google.com` and
`pool.ntp.org` correctly under vmnet (DNS RAG queries succeed end-to-end
in the same session — see `session-2026-05-16-tls-pki-agent.md`),
so the bug is specific to UDP/123 round-trip, not the entire UDP path.

## Hypotheses, with code evidence

| H | Hypothesis | Status | Evidence |
|---|------------|--------|----------|
| H1 | `net_udp_send` uses stale source IP when vmnet has a different DHCP lease than slirp's `10.0.2.15` | **partially refuted** | `net.c:853` copies `our_ip`. `main.c:738` runs `dhcp_discover()` before any user-driven `ntp_sync` call, and `dhcp.c:268` calls `net_set_ip(offered_ip)`. So by the time NTP fires, `our_ip` matches vmnet's lease. However: if `ntp_sync` is invoked early (e.g. via syscall before DHCP ACK), `our_ip = 0.0.0.0` and vmnet's pf NAT will not install a mapping — silently dropped. |
| H2 | Source-port rotation per `net_udp_send` call breaks vmnet NAT state | **refuted** | `ntp.c:113, 123` pin `src_port = 12321` across all 3 retries. |
| H3 | virtio-net TX path drops/wrong-checksums UDP | **refuted** | `virtio_net.c:343-377` `virtio_net_send` writes a zeroed `virtio_net_hdr` (no GSO/CSUM offload). vmnet doesn't validate UDP checksum on outbound. DNS-over-UDP via the same path succeeds. |
| H4 | UDP listener dedup eats second NTP exchange | **already fixed** | `net.c:2438-2443` short-circuits when `(port,handler)` already registered. Commit `c01e4a7`. |
| H5 (new) | macOS `pf` NAT in vmnet-shared mode does not install a UDP-123 mapping for an internal-source-port that is not in its allowed range, OR the reverse NAT times out faster than NTP's 2s retry window | **most likely, not verifiable from inside guest** | Needs host-side `pfctl -ss \| grep 123` while the kernel is hitting NTP. |

## What would close this

1. Run the kernel under vmnet with a packet capture (`tcpdump -i bridge100 -w /tmp/ntp-vmnet.pcap udp port 123`) and check whether:
   - The NTP request egresses with src IP matching DHCP lease (not 0.0.0.0).
   - The NTP reply arrives at the host but is filtered before it hits the guest interface.
2. Inspect `pfctl -ss` for any UDP/123 entry while NTP is in-flight.
3. ~If H5 confirmed: add a source-port range constraint (use 32768-60999 ephemeral range, not 12321) and/or implement UDP send retry with port rotation **specifically** when no reply within 2s.~
   **LANDED 2026-05-17 in commit `2e3716a`** — `ntp.c` now uses a fresh
   `random_get_bytes`-driven src_port in [49152, 65535] per retry, and
   re-registers `net_udp_listen(src_port, ntp_handler)` per attempt.
   The IANA-registered ephemeral band is the tightest overlap of
   Linux/Darwin/BSD defaults.  Verification (pending live run under
   vmnet): watch for `[NTP] src_port=<N>` followed by `[NTP] UTC time
   = …` in serial.log.  If that appears, H5 confirmed and this
   deferral is closeable.  If it does NOT appear with ephemeral, H5
   refuted → revisit with host `pfctl -ss` capture.

## Workaround in tree

`syscall.c:3078-3079` (`ntp_is_synced` guard) lets the validity-window
check skip cleanly when NTP=0 — TLS cert validity check is informative
under vmnet, fully enforcing under slirp. PKI roadmap A12.6 documents
this as acceptable.

## References

- `docs/pki-roadmap.md` line 59 (deferral entry).
- `docs/session-2026-05-16-tls-pki-agent.md` lines 396-397, 413.
- `docs/session-2026-05-15-network-fixes.md` for prior vmnet TCP fixes
  (window scaling, MSS, virtio RX ring 32→256).

## Out-of-scope to fix here

The fix is not in `ntp.c` or `net.c` directly; it requires host-side
pf/vmnet plumbing (or a QEMU `-netdev` flag tweak) that cannot be
validated from inside the guest. Estimated 1-2 hours of host-side
debugging with `pfctl`, `tcpdump`, and possibly a `dtrace` probe on
`vmnet_*` kext entry points. Deferred until either:

- A bare-metal NTP use case demands wall-clock accuracy (currently
  TLS cert validity is the only consumer and degrades gracefully).
- vmnet is required as the primary test backend (currently slirp
  with the 2026-05-15 fixes is the primary; vmnet is a fallback for
  large-payload TCP tests).
