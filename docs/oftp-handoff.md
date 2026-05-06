# OFTP — Autonomous bare-metal debug loop (handoff)

This is a recipe for setting up the same Mac↔OsitoK file transfer loop in
a sister project (osito-x). With this in place, you no longer shuttle a
USB stick between the dev box and the target — push kernels with
`kdownload`, pull dmesg with `kupload --dmesg`, iterate.

---

## What you copy from osito-k

Three files cover 95% of the work:

1. **`tools/oftp-server.py`** — Python UDP server. Self-contained. ~150 LOC.
   Drop it under `osito-x/tools/`. No deps beyond Python 3 stdlib.

2. **Shell-side `cmd_kdownload`** — find it in `arch/x86/kernel/shell.c`
   in osito-k. Look for the `OFTP_BUF_MAX` define and the `oftp_state`
   struct.

3. **Shell-side `cmd_kupload`** — same file, just below `cmd_kdownload`.
   Includes the `--dmesg` variant (reads klog ring buffer directly so it
   works even if the FS write path is broken).

Plus the integration:

4. **`net_udp_send` SG path** — currently disabled in osito-k due to a
   pending bug (chunks transmit as all-NUL on the wire even with the
   IFCS/PAYLEN fix in `i211_send_sg`). Disable it on your side too:
   `if (len >= 256 && frame_len >= 60)` block → comment out or guard
   with `if (0)`. Memcpy fallback is fast enough.

---

## Wire format (so you can extend it)

All UDP, both directions on the same socket.

| Magic   | Direction          | Layout                                                                 |
|---------|--------------------|------------------------------------------------------------------------|
| `OFTQ`  | client → server    | 4 B magic + 60 B filename (NUL-padded)                                 |
| `OFTU`  | client → server    | 4 B magic + 4 B total_size (BE32) + 60 B filename                      |
| `OFTD`  | both               | 4 B magic + 4 B total + 4 B offset + 4 B chunk_len + cklen B data      |
| `OFTN`  | client → server    | 4 B magic + 4 B offset (BE32) — "resend from offset" (download NAK)    |
| `OFTA`  | server → client    | 4 B magic + 4 B total (echo) — upload acknowledge                      |
| `OFTE`  | server → client    | 4 B magic + ASCII error message                                        |
| EOF     | either             | OFTD with chunk_len = 0 and offset == total                            |

Default ports: server on `7779`, kernel listener on `7780`. Pacing 3 ms
between download chunks, 1 ms between upload chunks. Server allows
NAK-based resume so a tail-loss of 3 chunks at the end of a 1.2 MB
transfer is recoverable.

---

## Kernel APIs you need to wire

Your kernel needs:

```c
/* Network */
int  net_udp_send(uint8_t dst[4], uint16_t dport, uint16_t sport,
                  const void *data, uint32_t len);
void net_udp_listen(uint16_t port,
                    void (*cb)(const uint8_t *src, uint16_t sport,
                               const void *data, uint32_t len));
int  net_arp_lookup_nowait(const uint8_t ip[4], uint8_t mac[6]);
void net_arp_probe(const uint8_t ip[4]);
void net_poll(void);

/* Klog ring buffer (for --dmesg) */
uint32_t klog_read(char *buf, uint32_t max_len);

/* OsitoFS (for non-dmesg uploads / kdownload save) */
void  *osfs2_find(const char *name);
void  *osfs2_create(const char *name, uint64_t size);
int    osfs2_write(void *file, uint64_t off, const void *buf, uint64_t len);
int    osfs2_read(void *file, uint64_t off, void *buf, uint64_t len);
int    disk_flush(void);
```

If your FS or net stack uses different names, just grep the OsitoK
shell.c for these symbols and rename to match.

---

## Setup steps

### 1. Mac side
```bash
mkdir -p ~/osito-x/tools
cp /Users/pc/osito-k/tools/oftp-server.py ~/osito-x/tools/
chmod +x ~/osito-x/tools/oftp-server.py
```

Run before iterating:
```bash
python3 ~/osito-x/tools/oftp-server.py 0.0.0.0 7779 ~/osito-x/arch/x86/build
```

Uploads land in `~/osito-x/arch/x86/build/uploads/<name>`.

### 2. Kernel side

Copy the `cmd_kdownload`, `cmd_kupload`, the `oftp_state`/`oftp_handler`
plumbing, and the `KEXEC_PROBE` macro (if you're also doing kexec) from
osito-k's `arch/x86/kernel/shell.c` and `arch/x86/kernel/main.c`.

Wire dispatch:
```c
} else if (strcmp(cmd, "kdownload") == 0) {
    cmd_kdownload(argc, argv);
} else if (strcmp(cmd, "kupload") == 0) {
    cmd_kupload(argc, argv);
}
```

### 3. NIC pre-reqs

If your I211 driver also has the issues we hit on osito-k, you'll need
these fixes from `arch/x86/drivers/i211.c`:

- **GPIE programming** (commit `64445cd`): `i211_write(I211_GPIE, PBA |
  EIAME | NSICR)` in `i211_enable_interrupts`. Without this the chip
  stops delivering MSIs after ~22 IRQs.
- **RX RDT off-by-one** (commit `a692452`): `i211_recv` writes
  `RDT = nic.rx_tail` (post-increment), not `RDT = old_tail`.
- **PCI MSI cap fix** (commit `a692452`): `pci_enable_msi` reads dword
  at offset `0x04` (aligned), not `0x06` (unaligned in ECAM).
- **Always-poll fallback** in `sched_tick` (commit `a692452`): call
  `net_poll()` unconditionally each timer tick. Belt-and-suspenders for
  any residual MSI delivery quirks.
- **kexec trampoline uses CALL** (commit `e5cba70`): `call *%r13` not
  `jmp *%r13`, with a `cli;hlt;jmp .` halt-loop after. ABI-compliant
  stack alignment naturally.

Look at osito-k git log for the exact diffs — most are surgical (1-3
lines each).

---

## Vulkan/Mesa-specific tips

For the use case you mentioned (debugging Vulkan/Mesa on bare metal),
augment the basic loop with:

### Targeted klog tags
Add `[VK]`, `[MESA]`, `[ZINK]`, `[NVK]` prefixes to the relevant log
lines so `kupload --dmesg` plus `grep VK` on the Mac side gives a
focused trace per subsystem.

### Per-feature shell commands
Mirror what osito-k does with `nic_stats` and `pred`: expose live state
of the GPU pipeline as a shell command, e.g. `vk_stats` that dumps the
last submitted command buffer, last GSP RPC, last validation error.
Then `kupload vk-state.log` after a failure captures everything.

### Two-step crash recovery
Vulkan validation can panic. With OFTP you can:
1. Add a kernel command `vk_replay <name>` that loads a serialized
   command buffer and replays it.
2. On Mac, `osito> vk_replay broken.cmd` — if it crashes, the kernel
   reboots; on the next boot the dmesg captured via `kupload --dmesg`
   has the panic context.
3. Iterate on the validator without re-flashing: kdownload a new
   kernel.elf with the fix, kexec into it, vk_replay again.

### Bridge to renderdoc / mesa devel
The `kdownload` path can also pull `.spv` shader binaries or pre-recorded
vk command streams from `~/osito-x/arch/x86/build/captures/` (just place
files in the serve root). Replays become `kdownload ... shader.spv;
vk_load shader.spv`.

---

## Known limitations (carry-overs from osito-k)

1. **SG TX path is disabled.** All UDP goes through memcpy → tx_pkt.
   ~1.7 MB/s effective, fine for kernels and logs, slow for big assets.
   Re-enabling needs more work; see `docs/x86-network-stack.md` §9.5.

2. **Mac→OsitoK→Mac reply path is broken.** OsitoK's `net_poll →
   handle_arp/handle_icmp → eth_send` doesn't reach the wire even
   though the chip reports DD=1. **Workaround**: never rely on Mac
   sending unsolicited packets to OsitoK — always have OsitoK initiate
   (kdownload, kupload). Both directions of OFTP transfer satisfy this:
   request goes OsitoK→Mac, response data goes Mac→OsitoK (RX works).

3. **kupload's listener can only register once per boot** (no de-register).
   Not a bug, just don't try to re-register on different ports.

---

## Quick verification (proof the loop works)

1. Run server: `python3 tools/oftp-server.py 0.0.0.0 7779 ~/osito-x/arch/x86/build`
2. Boot target box from a freshly-flashed USB.
3. On the target shell:
   ```
   osito-x> kdownload <mac-ip> 7779 some-file.bin downloaded.bin
   osito-x> kupload <mac-ip> 7779 --dmesg trace.log
   ```
4. On Mac: `tr '\0' '\n' < ~/osito-x/arch/x86/build/uploads/trace.log | head`
   should show ASCII boot log.

If the log is all-NUL, you're hitting the SG bug — disable it (see §1).

---

## Reference commits in osito-k

- `cc3e578` — initial OFTP + kdownload + Python server
- `7f81cde` — kupload + server PUT support  
- `2398adf` — heap-allocated pkt (avoid stack DMA issue)
- `9f7255f` — kvirt_to_phys instead of VIRT_TO_PHYS in SG path
- `99cda9b` — i211_send_sg IFCS+PAYLEN on last desc only
- `abfeea5` — disable SG path (workaround current state)
- `e5cba70` — kexec_tramp uses CALL not JMP (ABI-compliant)

If you hit something we already debugged, the commit message there
usually names the bug + experimental approach + outcome.
