# OsitoK — Containerized Bare-Metal OS
#
# Runs OsitoK as a microVM inside Docker via QEMU or Firecracker.
# Boot in <2 seconds, ~50MB container image.
#
# Build:   docker build -t ositok .
# Run:     docker run --rm -it --device /dev/kvm ositok
# With KVM: boot in ~200ms. Without KVM: ~2s (TCG).
#
# For Kubernetes:
#   kubectl run ositok --image=ositok --restart=Never \
#     --overrides='{"spec":{"containers":[{"name":"ositok","securityContext":{"privileged":true}}]}}'

FROM debian:bookworm-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make gnu-efi binutils \
    && rm -rf /var/lib/apt/lists/*

# Copy source
WORKDIR /build
COPY . .

# Build kernel + bootloader
RUN make -C arch/x86 clean && make -C arch/x86

# ── Runtime image ──────────────────────────────────────────────
FROM debian:bookworm-slim

# Install QEMU (minimal, x86_64 system only)
RUN apt-get update && apt-get install -y --no-install-recommends \
    qemu-system-x86 ovmf \
    && rm -rf /var/lib/apt/lists/*

# Copy built artifacts
COPY --from=builder /build/arch/x86/build/boot.efi /ositok/boot.efi
COPY --from=builder /build/arch/x86/build/kernel.elf /ositok/kernel.elf

# Create ESP (EFI System Partition) disk image
RUN mkdir -p /ositok/esp/EFI/BOOT && \
    cp /ositok/boot.efi /ositok/esp/EFI/BOOT/BOOTX64.EFI && \
    cp /ositok/kernel.elf /ositok/esp/ && \
    # Create FAT32 ESP image (64MB)
    dd if=/dev/zero of=/ositok/esp.img bs=1M count=64 && \
    mkfs.fat -F 32 /ositok/esp.img && \
    # Mount and copy files
    mkdir -p /mnt/esp && \
    mount -o loop /ositok/esp.img /mnt/esp && \
    cp -r /ositok/esp/* /mnt/esp/ && \
    umount /mnt/esp && \
    rm -rf /ositok/esp

# Default: boot OsitoK with QEMU
# - UEFI firmware via OVMF
# - 512MB RAM (adjustable via OSITOK_RAM env)
# - Serial on stdio (interactive terminal)
# - Network via SLIRP (DHCP auto-configures)
# - KVM acceleration if /dev/kvm available
EXPOSE 7777/udp
ENV OSITOK_RAM=512M
ENV OSITOK_CPUS=2

ENTRYPOINT ["sh", "-c", "\
    KVM_FLAG=''; \
    [ -c /dev/kvm ] && KVM_FLAG='-enable-kvm'; \
    exec qemu-system-x86_64 \
        $KVM_FLAG \
        -m $OSITOK_RAM \
        -smp $OSITOK_CPUS \
        -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
        -drive if=pflash,format=raw,file=/usr/share/OVMF/OVMF_VARS.fd \
        -drive format=raw,file=/ositok/esp.img \
        -netdev user,id=n0,hostfwd=udp::7777-:7777 \
        -device e1000,netdev=n0 \
        -nographic \
        -serial mon:stdio \
"]
