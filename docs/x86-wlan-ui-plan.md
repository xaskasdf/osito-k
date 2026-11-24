# Desktop Wi-Fi interface: plan only

Status: not implemented. Current deliverables are the shell assistant and the
direct command documented in [AX200 WLAN](x86-ax200-wlan.md). No desktop button,
window, stored profile or automatic connection is added by this change.

## Entry and window

- Add a dedicated Wi-Fi button in the desktop's top panel, separate from
  application/task buttons. Show unavailable, radio off/blocked, connecting,
  connected and failed states with an icon and accessible label/tooltip.
- Open or raise one managed `Wi-Fi` window. Integrate with compositor focus,
  minimize/restore, Alt-Tab and dynamic dock tasks. Do not append another
  demo-window index: the current dock maps its two built-ins by index.
- Use the existing GUI primitives and fixed button dimensions. Validate small
  framebuffers as well as the P1's native 1920x1080, including long SSIDs.
- Present a scrollable network list with signal, band/channel and security.
  Retain the selected BSSID/snapshot on refresh; do not silently select a
  different AP with the same SSID. Label unsupported networks with their reason.
- Provide scan, connect, cancel and disconnect actions, plus IPv4/gateway/DNS
  status. Association, authorized data and configured IPv4 are distinct states;
  none alone is a claim of Internet connectivity.

## Backend work before GUI implementation

1. Move caller-neutral connection coordination out of the shell into an
   asynchronous service. Reuse `iwx_scan_results`, `iwx_connect_network`,
   `iwx_link_status`, `nic_ops` and existing DHCP/link-generation handling.
   Keep presentation, keyboard input and credential collection in each caller.
2. Define one operation owner and generation, bounded progress events and an
   explicit cancellation result. Concurrent shell/GUI requests must not stop
   each other's session, install stale keys or apply DHCP to a replaced link.
   Firmware/regulatory/TX/DMA ownership stays in the driver.
3. Run firmware loading, PBKDF2, command waits, association and DHCP outside
   the compositor thread. Closing the window detaches its observer safely;
   it must not free buffers retained by an in-flight operation or drop an
   established connection merely because its presentation closed.
4. Use ephemeral password buffers with masking and explicit clearing on
   success, error and cancellation. Do not pass passwords through shell
   commands, logs, clipboard or serial. Profile persistence needs its own
   credential-storage design; it is not implied by this plan.
5. Preserve firmware regulatory decisions, RSN validation, closed controlled
   ports before authorization and verified DMA stop. Do not add GUI-only
   country overrides, automatic open-network fallback or NIC switching.

## Acceptance gates

- First validate physical WPA2, DHCP, gateway reachability, DNS/HTTPS and OFTP
  on the P1, with stop/restart and no stale addresses or key material.
- Service tests cover concurrent clients, cancellation at each phase, stale
  completion events, failed credentials, RF-kill, firmware failure and DHCP
  failure without blocking rendering/input.
- GUI tests cover button state, a single reusable window, keyboard navigation,
  password masking, hostile/long SSIDs, scrolling, focus and geometry changes.
- Test the absent-radio path in QEMU, then scan/connect/disconnect on hardware.
  Keep the shell/direct CLI as independent recovery and diagnostic interfaces.
