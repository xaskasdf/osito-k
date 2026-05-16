# QEMU slirp — deferred diagnosis

`-netdev user` (libslirp) drops continuation packets on large
responses, manifesting as a TCP stall around 5–15 KiB even when
the kernel's send/recv paths are correct.  Documented across
multiple sessions:

- `docs/session-2026-05-10.md` — first reproduce of the rapid-fire
  regression
- `docs/session-2026-05-15-network-fixes.md` — vmnet-shared
  workaround; chain of TCP fixes had no effect under slirp

## Why deferred

- **Workaround exists.**  `NET=vmnet bash scripts/*-rag-probe.sh`
  on the Mac side hits a real bridge; full body delivered.
- **Cost is high.**  Libslirp is 4.9.1; reproducing the drop
  requires building a minimal C harness that talks to the same
  libslirp from the host side, isolating which code path drops
  the packet, building a fix that doesn't regress NAT lookup,
  and getting it upstream into libslirp + waiting for
  Homebrew QEMU to pick up the new bottle.  Realistic ≥1 session
  + days of upstream review.
- **No osito-a-specific surface area.**  This is a generic libslirp
  issue — anyone running a TCP-heavy guest on QEMU+slirp hits it.
  Upstream is the right venue.

## When to revisit

- An external contributor reports the same issue with a smaller
  repro.
- We need to run osito-a on a CI box without root (no vmnet) for
  network-touching tests.
- A new dev who joins the project hits it cold and gets confused.

## Workaround quick-ref

For any kernel test that hits the network and shows truncated
responses under slirp:

```bash
# Set up sudo askpass once:
echo '#!/bin/bash
echo <PASSWORD>' > /tmp/askpass.sh && chmod +x /tmp/askpass.sh

# Run with vmnet:
export SUDO_ASKPASS=/tmp/askpass.sh
sudo -A NET=vmnet bash arch/x86/scripts/test-rag-probe.sh
```

The scripts (`scripts/test-rag-probe.sh`, `scripts/qemu-agent-hvf.sh`)
detect `NET=vmnet` and swap the netdev backend; without sudo they
error out cleanly instead of silently falling back to slirp.
