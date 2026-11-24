# x86 kexec handoff

## Failure and scope

The 2026-09-24 P1 freeze during `LOAD` was reproduced in QEMU with the old
kernel. The loader wrote to unreserved physical address `0x08000000` (128 MiB).
The live kernel occupied roughly 32-187 MiB including BSS, so staging overwrote
the running kernel before the trampoline. A successful OFTP transfer did not
make that loader safe. Replacing the kernel file cannot repair the loader
already executing: the first corrected kernel must be booted through UEFI.

`kexec` restarts the OS. It does not preserve processes, sockets, WLAN
association or IP configuration. It does not change the USB's EFI boot file.

## Ownership and transition

`kernel/kexec.c` prepares the whole handoff before touching final destinations:

- Validate ELF64/x86-64 ET_EXEC, program headers, arithmetic, alignment,
  non-overlapping physical ranges and a file-backed executable entry point.
  Load destinations must be RAM below 4 GiB with the kernel direct-map alias.
  At most 16 nonempty PT_LOAD entries are accepted; extras are not truncated.
- Allocate staging, a 1-MiB entry stack, tables, copied GDT and private IDT
  through the physical allocator. Reject allocations overlapping the new
  kernel range. No fixed scratch addresses are used.
- Stage file bytes only. Large BSS segments are cleared at their final
  addresses by the trampoline, after the old kernel stops executing.
- Copy the complete saved UEFI map (up to 64 KiB), rather than silently
  truncating it. The private CR3 maps bootstrap RAM, APIC and the actual GOP
  aperture, including the P1's address at 256 GiB. The direct-map window is
  512 GiB; a framebuffer outside it is rejected.
- Flush the mounted filesystem, then acquire the AX200 handoff guard and
  verify DMA shutdown. Invalid files and preparation failures return to the
  old shell, release allocations and do not stop WLAN.
- On the BSP, disable interrupts, mask its timer and send bounded INIT to
  peer CPUs. Disable PCI endpoint bus mastering, verify readback and bound
  the PCIe Transactions Pending wait. Preserve bridges and BAR decode.
- The position-independent trampoline installs its own stack, GDT, IDT and
  CR3, invalidates global translations, copies segments, zeroes BSS and calls
  the entry with the SysV ABI. Its optional UART marker cannot wait forever.

Boot-info v3 explicitly transfers ownership of the temporary arena and stack.
The new kernel reserves both before any physical allocation, copies boot data,
then frees staging after replacing the IDT/GDT and CR3. The entry stack remains
reserved. Boot-info v2 remains supported without reading beyond its old size.
LoaderData already reserved by the original EFI map is not treated as a runtime
allocation conflict during this pre-allocation reservation step.

Bootstrap table reservation precedes `idt_init`, deduplicates identity/direct
aliases, and tracks up to 4096 pages with an explicit failure at the bound.
The former 32-entry array could silently miss tables on larger machines.

NVMe now establishes PCI memory decode and bus mastering itself, only after
stopping the old controller and installing admin queues. xHCI keeps DMA off
through halt/reset, then enables it before programming new event tables.
QEMU reads the table when ERSTBA is written, before RUN, as seen in
[its xHCI implementation](https://github.com/qemu/qemu/blob/v8.2.2/hw/usb/hcd-xhci.c#L752).

## Validation

Use fresh output directories. QEMU tests allocate managed scratch disks only:

```sh
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
bash arch/x86/scripts/test-kexec-image.sh arch/x86/build/kexec-image-check
bash arch/x86/scripts/test-kexec-qemu.sh arch/x86/build/kexec-nvme-check
KEXEC_TEST_RAM=32G KEXEC_TEST_STORAGE=usb \
  bash arch/x86/scripts/test-kexec-qemu.sh arch/x86/build/kexec-usb-check
bash arch/x86/scripts/test-ssh-qemu.sh arch/x86/build/ssh-kexec-check
```

Host tests use GCC and Clang, normally and with ASan/UBSan: 71 ELF/map/ABI,
17 boot reservation and 170 PCI DMA checks per variant. Coverage includes
overflows, an entry in BSS, segment limits, reserved-memory holes, exact-size
v2 boot data, LoaderData reservations, PCI capability cycles and stuck DMA.

QEMU repeats two full reloads for NVMe/2-GiB and USB/32-GiB configurations,
checks four CPU startup, persistent file access, USB keyboard commands and
bidirectional UDP after each reload. Missing, short and malformed images must
leave the shell and I/O usable. The 32-GiB guest uses sparse, non-preallocated
host backing; this tests address-map size, not full physical-memory pressure.
The SSH suite also invokes kexec over authenticated OpenSSH and verifies
reconnection with the same host identity and persisted files. SSH is opt-in
again after the new boot.

Output directories retain hashes, serial logs and results. Additional UEFI
handoff, xHCI and WLAN-shell regression suites cover the shared initialization
and command paths.

Validated artifact on 2026-09-24: 7,102,888 bytes, SHA-256
`e03b77749bfe0a96f4708df3c1acbd851a8d3cb3435c48485ece8d796f4d5a84`.
Local evidence under `arch/x86/build/`: `kexec-before` reproduces the old hang;
`kexec-image-final-v2`, `kexec-nvme-final-v2`, `kexec-usb-final` and
`ssh-kexec-final-v2` contain the passing final tests. The latter three pin the
same ELF hash. `boot-handoff-kexec-v1`, `xhci-kexec-final` and
`wlan-shell-kexec-v1` passed their regression suites. The development USB was
not modified during this fix; the old temporary OFTP server was stopped and
its temporary address, route and firewall rule were removed successfully.

## Remaining hardware qualification

For persistent USB updates instead of a running-kernel-only reload, see
[kupdate](x86-kupdate.md). It publishes both OsitoFS and EFI kernel copies
before chaining into this `kexec` path.

QEMU is not evidence of a completed P1/AX200/T1000 hot reload. That needs a
UEFI boot of this kernel followed by a local kexec and restoration of WLAN/IP.
Avoid using an older `/kernel.elf` or `kernel-ssh.elf` left on the data partition
as the target; verify which build was copied there.

This is not a general driver shutdown/resume registry. PCI DMA suppression
does not replace per-device reset for all future drivers, power states or
IOMMU configurations. AP INIT delivery is bounded but not a per-CPU software
shutdown acknowledgement. After irreversible commit begins, a quiesce failure
halts with a diagnostic rather than returning to a partially stopped OS.
Preparation failures remain recoverable. Missing memory or excessive ELF/map
sizes are rejected explicitly, not worked around with fixed addresses.
