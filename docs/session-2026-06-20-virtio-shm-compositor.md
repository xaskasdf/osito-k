# Session 2026-06-20: Virtio-GPU SHM Present Path

## Summary

This change set hardens the x86 display path used by Win32, DXVK, and Venus
present flows.  The main goal is to keep compositor-backed SHM surfaces visible
and flushable when QEMU/virtio-gpu is the active scanout path, including boots
where the UEFI GOP framebuffer is unavailable.

## Display And SHM

- `shm_create_surface()` now records surface width and height on the SHM region,
  so later flushes do not have to infer common sizes from total byte count.
- `shm_flush_surface()` marks the owning compositor window dirty when the
  compositor is running, and otherwise blits directly to the boot framebuffer or
  the virtio-gpu framebuffer fallback.
- The direct virtio-gpu fallback throttles flushes by scheduler ticks and logs
  the first activation, avoiding runaway flush storms while preserving visible
  progress.
- Fullscreen dirty windows are raised to the front, and fullscreen rendering
  walks the render list front-to-back so the active fullscreen surface wins.

## Virtio-GPU

- Control queue submissions are serialized with a spinlock.
- Descriptor free-list handling now uses explicit constants and a terminal
  marker, validates used-ring IDs, and marks the queue broken after corruption
  or timeout.
- Command wait logic uses tick-based timeout plus a high spin cap, which is more
  tolerant of slow host GL paths without recycling descriptors still owned by the
  host.

## Win32, Venus, And Boot

- DXVK and Venus swapchain paths now pass SHM access flags instead of the old
  BGRA format literal when creating compositor surfaces.
- `win32_abi_reset()` clears stale ABI tables before registering a new PE run,
  and MSVC ABI parsing now uses bounded string walks to reject malformed export
  names safely.
- Boot framebuffer setup tolerates missing GOP state and skips WC/shadow setup
  when there is no framebuffer to map.

## GitNexus Review

`gitnexus detect-changes --scope all` reports medium risk for this batch:
179 changed symbols, 4 affected compositor execution flows.  Focused upstream
impact checks for the main entry points stayed low risk; the only nonzero direct
callers were `gpu_send_cmd`, `compositor_render_frame`, and
`msvc_demangle_abi`.
