# WSL2 Native Kernel Wishlist

Status: wishlist / future research, not an active roadmap item.

## Finding

Osito-K is closer to a native WSL2 kernel experiment than a generic
"port an OS to Linux compatibility" estimate would suggest. The repo already
has large parts of the Linux userspace contract:

- x86-64 Linux syscall dispatch in `arch/x86/kernel/syscall.c`
- static musl ELF support, `execve`, `fork`/`wait4`, `clone`, TLS, futex
- POSIX-ish VFS with `/dev/*`, `/proc/self/*`, `getdents64`, `statx`
- `mmap`/`mprotect`/`munmap`, file-backed VMAs, demand paging notes
- signals sufficient for current musl/static workloads
- virtio-net, virtio-gpu, virtio block/QEMU-oriented platform support
- existing WSL2+KVM runner path for QEMU (`arch/x86/scripts/run-wsl-kvm.sh`)

So the hard part is probably not "make Osito-K Linux-compatible enough to run
musl". That work is substantially underway. The unknowns are WSL2-specific.

## The Real Gaps

### 1. WSL2 boot contract

Current x86 boot path:

```text
UEFI -> boot.efi -> kernel.elf -> kernel_entry(boot_info)
```

WSL2 custom kernels are configured through `%UserProfile%\.wslconfig` as a
custom Linux kernel. Microsoft documents the setting as applying to the WSL2
VM kernel, not to an arbitrary UEFI application or ELF payload:

- https://learn.microsoft.com/windows/wsl/wsl-config

Research question:

```text
Can Osito-K be packaged as a Linux boot protocol / bzImage-compatible payload
that WSL2 will start via kernel=... ?
```

If yes, this becomes mostly boot glue. If no, native WSL2 is blocked unless
WSL's launch path can be made to enter a non-Linux-kernel image.

### 2. Hyper-V / WSL device model

The repo has virtio drivers for QEMU/KVM, but there is no clear native
Hyper-V/WSL device stack yet:

- VMBus
- synthetic storage (`storvsc`)
- synthetic network (`netvsc`)
- Hyper-V sockets / hvsock
- WSL console / pty integration
- host filesystem interop equivalent to DrvFs / 9P behavior

This is the likely main engineering cost after first boot.

### 3. WSL distro/init contract

For `wsl -d OsitoK` to feel real, Windows expects a running distro-like
environment:

- an init or shell entrypoint
- usable stdio/pty console
- root filesystem semantics
- `/dev`, `/proc`, and enough `/sys` stubs for tooling
- user/default-shell behavior
- clean process lifetime when `wsl.exe` exits or terminates the instance

Osito-K has pieces of this, but not specifically the WSL control-plane
contract.

## Suggested Future Spike

Goal: answer the boot-contract question before investing in Hyper-V devices.

1. Add a minimal `arch/x86/boot/linuxboot/` target.
2. Produce the smallest Linux boot protocol or bzImage-shaped artifact that
   jumps into Osito-K early init.
3. Pass a synthetic `boot_info` equivalent from Linux boot params where
   possible, or build a reduced WSL boot path that does not need UEFI GOP.
4. Configure:

   ```ini
   [wsl2]
   kernel=C:\\path\\to\\ositok-bzimage
   ```

5. Run `wsl --shutdown`, then start any WSL2 distro and check whether Osito-K
   reaches early serial/log/debug output.

Success criteria for the first spike:

- WSL2 loads the image instead of rejecting it.
- Osito-K reaches a controlled early panic/log point.
- Memory map, CPU mode, command line, and initrd/rootfs inputs are observable.

## Rough Difficulty

| Track | Estimate | Notes |
| --- | ---: | --- |
| WSL wrapper that runs QEMU/KVM | days | Already mostly exists via `run-wsl-kvm.sh`. |
| First native WSL2 boot attempt | 1-3 weeks | Mostly boot protocol packaging and early-entry debug. |
| Usable `wsl -d OsitoK` shell | 1-2 months | Depends on WSL console/rootfs expectations. |
| Full WSL-quality integration | 2-4+ months | Mostly Hyper-V/WSL device/control-plane work. |

## Non-Goals For Now

- Do not replace the current UEFI boot path.
- Do not block QEMU/KVM-in-WSL workflow.
- Do not implement Hyper-V drivers before validating that WSL2 will load an
  Osito-K-shaped kernel image.

