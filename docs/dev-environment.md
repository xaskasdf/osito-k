# OsitoK Dev Environment

Three test environments, in order of fidelity:

|  Mode  | Where | What it tests | When to use |
|--------|-------|---------------|-------------|
| QEMU `--no-gl` (macOS) | dev box | Compositor SHM path, no real GPU | Software fallbacks (okgl_software.c, hello-gl-clear) |
| QEMU `+virgl` (Linux) | Linux VM/box | Real Vulkan host via virglrenderer | Mesa/Zink/DXVK runtime — the path that actually exercises GPU |
| Bare-metal | i5 / R7+3090 USB SSD | Everything; real NVK/AMDVLK | Final validation |

## Mode 1 — QEMU `--no-gl` (current macOS dev loop)

What works:
- OsitoK boots, shell, all syscalls
- SHM compositor surface + SYS_GUI_FLIP (visible cocoa window)
- Vulkan ICD guest-local fallback path (W3b.x)
- Software-bypass GL (okgl_software.c) — paints rectangles

What doesn't work:
- Real Vulkan path: no virgl on macOS QEMU (cocoa GL is 4.1, virgl needs GLES 3)
- Anything beyond `glClear`/triangle CPU rasterizer

One-liner:
```bash
tools/deploy.sh ~/ok-ported/vulkan-tests/hello-gl-clear/hello-gl-clear.elf --tail
```

## Mode 2 — QEMU + virgl (Linux host)

Setup on a Linux VM (Ubuntu 24.04 / Fedora 40 etc.):

```bash
# Install QEMU with virgl
sudo apt install -y qemu-system-x86 ovmf qemu-utils mtools \
                    libvirglrenderer-dev mesa-vulkan-drivers
# Or build virgl QEMU from source if your distro is old
git clone https://gitlab.freedesktop.org/qemu/qemu
cd qemu && ./configure --enable-virglrenderer --enable-gtk --enable-opengl
make -j$(nproc)

# Pull the OsitoK build artifacts (rsync from macOS dev box)
rsync -avz dev-mac:~/osito-k/arch/x86/build/ ~/osito-k-build/

# Boot QEMU with virgl + GTK display (gl=on works on Linux X11/Wayland)
qemu-system-x86_64 \
    -bios /usr/share/OVMF/OVMF_CODE.fd \
    -drive file=esp.img,format=raw,if=ide \
    -drive file=nvme.img,format=raw,if=none,id=nvme0 \
    -device nvme,serial=deadbeef,drive=nvme0 \
    -m 4G -smp 4 -machine q35 -cpu Nehalem \
    -device virtio-gpu-gl-pci,hostmem=256M,blob=on \
    -display gtk,gl=on \
    -serial stdio
```

Why this matters: this is the only QEMU mode where Mesa+Zink can ACTUALLY
talk to a real Vulkan implementation (lavapipe via virgl). Our `--no-gl`
loop on macOS will never paint via the real Mesa path.

## Mode 3 — Bare-metal (i5 / R7+3090)

Workflow:

```bash
# On dev box: build everything
cd ~/osito-k/arch/x86 && make
cd ~/ok-ported/vulkan-tests/hello-gl-clear && make

# Insert USB SSD, find device path
# macOS:
diskutil list external
# Linux:
lsblk

# Write boot image + ELFs (ASKS confirmation)
~/osito-k/tools/deploy-usb.sh /dev/diskN \
    ~/ok-ported/vulkan-tests/hello-gl-clear/hello-gl-clear.elf \
    ~/ok-ported/vulkan-tests/mesa-zink-screen-test/mesa-zink-screen-test.elf

# Eject, plug into target machine, boot from USB.
```

For network-attached serial logging (no serial cable needed):
```bash
# On dev box:
~/osito-k/tools/serial-collector.sh 7779
# OsitoK boot logs land in serial.log via UDP; same tail/grep flow.
```

## Iteration loop comparison

| Phase | macOS `--no-gl` | Linux QEMU+virgl | Bare-metal USB |
|-------|----------------|------------------|----------------|
| build | ~30s incremental | ~30s | ~30s |
| deploy | <1s (NVMe write) | <1s | ~10s (USB write) |
| boot | ~3s (UEFI) | ~3s | ~15s (BIOS+UEFI) |
| iterate | yes (10s/cycle) | yes (10s/cycle) | painful (60s/cycle, physical access) |
| GPU fidelity | none | full virgl/lavapipe | real NVK/AMDVLK |

**Recommended development cycle:**

1. Iterate on dev box `--no-gl` for 90% of work (kernel, ICD wire format, build issues, software bypass).
2. Smoke-test on Linux QEMU+virgl when adding Mesa runtime paths or DXVK draw calls.
3. Bare-metal validation only at milestone points (e.g., end of each wave).

## Troubleshooting

- `cocoa: OpenGL not supported` — macOS QEMU + `--gl`. Don't; use `--no-gl` on macOS, virgl on Linux.
- `OVMF not found` — install OVMF firmware (`sudo apt install ovmf` / use brew + manual copy on macOS, see `arch/x86/scripts/qemu-test.sh` for paths).
- `failed to load BOOTX64.EFI` (bare-metal) — the USB BIOS doesn't see the ESP. Try the GPT path with sgdisk (deploy-usb.sh does this on Linux automatically).
- ELF too big for NVMe image — grow it: `truncate -s 1G ~/osito-k/nvme_vulkan.img`.
- `usleep` crash — that lib func isn't in OsitoK libc; we shim via `__syscall2(35, ...)` in osito_compat.
