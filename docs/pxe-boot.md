# PXE Boot Plan

## Goal

PXE should remove the USB write/reboot loop for real-hardware bring-up while
leaving the current USB/ESP boot path unchanged. The proposed first target is:

```sh
make -C arch/x86 pxe
```

This builds `arch/x86/build/boot-pxe.efi`, a UEFI app that embeds the current
`kernel.elf`. The normal `make -C arch/x86` output stays split as
`boot.efi + kernel.elf`.

## Why Embed The Kernel

PXE firmware can fetch and execute a single EFI binary easily. The current
`boot.efi` then expects `\EFI\BOOT\kernel.elf` on a filesystem, which PXE does
not provide. Embedding `kernel.elf` in a PXE-only EFI binary avoids writing a
network downloader in UEFI before we know the hardware PXE path is stable.

## Relationship To Hot Swap

PXE is not meant to replace the current hot-swap path. Once OsitoK is alive,
`kexec` from USB or a downloaded `kernel.elf` from the R2/cloudflared endpoint is
still the faster loop because it avoids firmware reboot and device
re-enumeration. PXE is mainly useful when the first boot image itself needs to
change, or when a bug kills the kernel before shell/network/storage hot-swap is
available.

For the current punctual debugging loop:

- Use hot swap when the machine reaches shell, network, or USB storage.
- Use PXE when the failing code is before those paths are reliable.
- Keep USB as the fallback for firmware PXE quirks or when the network link is
  the thing being debugged.

## Pros

- One PXE artifact: serve `boot-pxe.efi` as `BOOTX64.EFI`.
- No TFTP/HTTP fetch logic inside the bootloader yet.
- Normal USB workflow is untouched.
- Bootloader still parses the same ELF program headers and fills the same
  `boot_info_t`.
- Good for debugging one punctual hardware bug because every build produces a
  self-contained boot image.

## Cons

- Larger EFI binary; every kernel rebuild changes the whole PXE payload.
- TFTP may be slower and less reliable with multi-megabyte EFI files than HTTP.
- Firmware size limits vary; some PXE stacks may reject large EFI apps.
- Cannot swap only `kernel.elf` server-side without rebuilding `boot-pxe.efi`.
- The embedded path is a second loader path, so it needs occasional testing.
- Worse than `kexec` for inner-loop kernel iteration after OsitoK is already
  running.

## Host Setup Sketch

On the development host, run a DHCP/ProxyDHCP service that points UEFI PXE at
`BOOTX64.EFI`, then serve `arch/x86/build/boot-pxe.efi` as that file over TFTP
or HTTP, depending on firmware support.

Useful capture while testing:

```sh
sudo tcpdump -ni en5 'port 67 or port 68 or port 69 or arp'
```

The kernel IP reported later, such as `169.254.62.16`, is not used by PXE; PXE
networking happens in firmware before OsitoK starts.
