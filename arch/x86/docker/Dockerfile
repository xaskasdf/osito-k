FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    qemu-system-x86 ovmf mtools \
    gcc make binutils gnu-efi \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /osito-k
