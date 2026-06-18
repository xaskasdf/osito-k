# Real-Hardware USB Bundle

This bundle is for booting OsitoK on the i5/HD 530 class target from a USB
disk. It keeps the existing GTA5 OsitoFS image as the base payload and adds
the current kernel/EFI plus UT99, Doom, and Quake2 payloads.

## Disk Layout

Target device for the current lab machine:

```text
/dev/disk2 (USB, external, physical, ~125 GB)
  disk2s1  EFI System Partition, FAT32, 512 MiB
           /EFI/BOOT/BOOTX64.EFI
           /EFI/BOOT/kernel.elf
           /startup.nsh

  disk2s2  GPT label: osito
           OsitoFS v2, grown to the rest of the disk
```

The kernel GPT scanner mounts `disk2s2` by GPT name containing `osito` or by
probing the OsitoFS magic at the partition start.

## Build

```sh
make -C /Users/pc/osito-k/arch/x86
make -C /Users/pc/osito-k/tools/ositofs
```

The USB script expects:

- `/Users/pc/osito-k/arch/x86/build/boot.efi`
- `/Users/pc/osito-k/arch/x86/build/kernel.elf`
- `sgdisk` in `PATH` on macOS, so the script can create the exact two-entry
  GPT layout without the automatic 209 MB macOS ESP
- `/Users/pc/osito-k/nvme_gtav_full.img`
- `/Users/pc/osito-k/nvme_ut99.img`
- `/Users/pc/osito-k/doom.elf` and WADs, if present
- `/Users/pc/ok-ported/quake-2-ok/quake2.elf`
- `/Users/pc/ok-ported/quake-2-ok/build_wasm/baseq2/pak0.pak`

`GTAV_Source/GTA5.elf` is copied over the base image only when it exists and is
non-empty. Otherwise the existing `GTA5.elf` already present in the GTA base
image is kept.

## Burn

Revalidate the device immediately before the destructive write:

```sh
diskutil list /dev/disk2
diskutil info -plist /dev/disk2
```

Then run:

```sh
sudo bash /Users/pc/osito-k/tools/build-realhw-usb-bundle.sh /dev/disk2
```

The script refuses non-USB/internal/non-physical media, requires `/dev/disk2`,
and expects a size in the 120-130 GB range.

## Validation

After writing, inspect the OsitoFS partition:

```sh
/Users/pc/osito-k/tools/ositofs/ositofs-ls /dev/disk2s2
```

Expected high-level entries:

- `GTA5.elf`
- `doom.elf`, `DOOM.WAD`, `DOOM2.WAD`, `PLUTONIA.WAD`, `TNT.WAD` when present
- `quake2.elf`, `baseq2/pak0.pak`
- `UnrealTournament.exe` and the UT99 support files from `nvme_ut99.img`

## Intel Gen9 Display

The Intel path is intentionally conservative. `drivers/intel_gfx.c` recognizes
Gen9/Gen9.5 Intel display devices such as HD 530 (`8086:1912`), maps BAR0,
reads the firmware-programmed pipe and primary plane state, and reports:

```text
[IGFX] Gen9/HD 530 modeset ready; GOP scanout retained
```

This means the driver found an active pipe/plane and will keep using the GOP
framebuffer programmed by UEFI. It does not implement full i915 power-well,
GGTT, stolen-memory, or acceleration logic, and it does not page-flip arbitrary
kernel RAM into the Intel scanout. If the state is not safe, the kernel remains
on the existing GOP fallback.

Display priority remains:

1. virtio-gpu in QEMU
2. NVIDIA/GSP display path when initialized
3. Intel Gen9 GOP-retained scanout when detected as safe
4. GOP framebuffer fallback

NVIDIA/GSP code paths are left intact.
