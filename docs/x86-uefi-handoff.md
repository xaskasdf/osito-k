# x86 UEFI handoff

For a kernel-to-kernel restart without firmware, see [kexec](x86-kexec.md).

The loader keeps the active usable GOP mode. It does not select a fixed desktop
resolution. A framebuffer address is a physical aperture, not an amount of RAM:
the reported T1000 GOP aperture at `0x4000000000` is at 256 GiB despite 32 GiB RAM.

## Ownership and mappings

`boot.efi` builds its tables without activating them while firmware owns the
machine. The bootstrap direct map uses 2 MiB pages with 4 KiB edges, maps the
RAM descriptors plus the GOP aperture and local APIC explicitly, and leaves
large address holes unmapped. GOP and APIC start uncached. The current direct-map
contract is one PML4 slot, 512 GiB; out-of-range descriptors fail visibly.

The loader allocates a 512 KiB kernel stack below 4 GiB as `EfiLoaderData` and
keeps `boot_info` in loader-owned storage. The low firmware mappings retain the
handoff trampoline and boot data. Kernel page tables preserve the GOP aperture
before replacing CR3; WC is applied later after configuring PAT.

The final sequence is:

1. Finish allocations, table construction and firmware console output.
2. Obtain a fresh memory map and call `ExitBootServices` without intervening
   protocol calls or allocations. Retry stale keys with a fresh map, bounded
   to four attempts. Buffer growth uses only memory services.
3. Disable interrupts after the exit returns; never call firmware console
   protocols after the first exit attempt, even on error.
4. On success, load the new CR3, install the aligned kernel stack, clear DF and
   enter the ELF entry point with the SysV ABI.

These rules follow [UEFI boot services, section 7.4.6](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html).

## Early checkpoints

The bottom-right badge uses direct framebuffer stores, without firmware or UART:

| Code | Last completed step |
| --- | --- |
| E0 | Tables prepared; about to obtain final map and exit firmware |
| E1 | ExitBootServices succeeded; about to activate CR3 and kernel stack |
| EF | Exit sequence failed; following 16 hex digits are EFI_STATUS |
| C1 | Kernel reached using the new CR3 and stack |
| C2 | Kernel BSS cleared |

The normal framebuffer console replaces these badges once initialized. A frozen
`Exiting boot services...` line by itself does not locate the failure. Early
COM1 probes are bounded and skip an absent UART. `/diag` is available only after
the kernel mounts the data filesystem, not for these earliest stages.

## Regression commands

```sh
bash arch/x86/scripts/test-boot-gop.sh
bash arch/x86/scripts/test-boot-handoff.sh
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/boot.efi build/boot_efi_pxe.o build/kernel.elf
```

Host tests run GCC and Clang, with and without ASan/UBSan. They cover the exact
256 GiB aperture, sparse RAM, APIC, range edges, allocation failures, MapKey
retries and descriptor growth. QEMU smoke coverage does not replace testing on
the actual GPU/firmware combination.
