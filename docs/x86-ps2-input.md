# x86 PS/2 Input

The i8042 auxiliary port was not initialized and PIC IRQ12 had no IDT handler.
Only USB HID supplied pointer reports. A working PS/2 keyboard therefore did
not imply working support for an integrated PS/2 touchpad.

`drivers/ps2.c` owns the shared i8042 receive path. IRQ1 and IRQ12 check OBF,
source, parity and timeout bits before routing a byte. Keyboard bytes retain
their existing raw-input/console path; mouse packets are queued until normal
input polling. The IRQ never calls the Win32 window manager. The same bounded,
re-entry-guarded poll runs in the compositor, console, Win32 message pump, input
syscall and native DOS input path.

Initialization preserves controller translation/system bits, restores keyboard
operation when AUX is absent, validates ACK/BAT/device ID, and bounds waits and
RESEND retries. Only a successful Synaptics identify signature enables the
vendor mode-byte command: mode zero selects relative reporting, with the pad
awake and firmware gestures enabled. There is no ThinkPad/model-specific bypass.
The decoder supports 9-bit X/Y, three buttons, overflow suppression, partial
packet timeout/resynchronization and IDs 0/3/4. It accepts an already selected
wheel protocol, but does not negotiate IntelliMouse extensions.

## Hardware Scope

The tested ThinkPad P1 Gen 2 is type 20QU. Linux lists `LEN0402` for the P1 Gen 2
and X1 Extreme Gen 2 in its Synaptics PS/2/SMBus support. This is model evidence,
not a hardware ID read from this particular machine. The user subsequently
confirmed that the integrated touchpad works with the PS/2 build on this P1.

The user confirmed that the preceding UEFI build reached a stable 1080p desktop
for over 11 minutes, with working keyboard and an OsitoFS3 mount. Reading the
reconnected USB found no persisted `/diag` entries; input transport must therefore
be checked on screen, not inferred from a boot log that was not captured.

The user-provided `n2ogb01w_v2.exe` is a Lenovo-signed touchpad firmware package,
not an OS driver to execute on the development PC. Its metadata/signature were
read without executing it or updating the device. SHA256:
`e02300674964a5accb95869999b5bb61e6a4770f58a976087434d5929688480c`.

This change does not implement I2C HID, native RMI/SMBus, absolute Synaptics
multitouch, palm rejection, hot-plug/reset recovery or suspend/resume. Relative
mode is the documented basic pointer protocol, not a claim of full touchpad
gesture support. A device available only over I2C/SMBus needs that transport.

## Diagnostics And Tests

Run `ps2`, move/click the pointer, then run `ps2` again:

- `ready=1`: command negotiation and stream enable completed.
- `synaptics=1`: the device returned the Synaptics query signature.
- `irq12`: hardware AUX interrupts, excluding polling fallback.
- `aux_bytes`, `packets`, `delivered`: transport, decoder and input delivery.
- `bad_bytes`, `resyncs`, `timeouts`, `resends`, `dropped`: failure counters.
- Cursor and left-button counters report the shared input state.

Initialization diagnostics also enter `dmesg` and persistent `/diag` boot logs.

```sh
bash arch/x86/scripts/test-ps2.sh
make -C arch/x86 CLANG=1 QUIET=1 SKIP_MODEL=1 -j4 build/kernel.elf
```

Host tests compile the production driver with mocked port I/O using GCC and
Clang, normally and with ASan/UBSan. They cover the full 9-bit delta ranges,
controller/ACK/BAT failures, Synaptics mode negotiation, mixed keyboard/AUX
traffic, bounded waits, queue pressure, bad bytes and deferred GUI delivery.
The local QEMU smoke uses a managed disposable USB image, removes USB keyboard
and mouse devices, types a command over PS/2 and checks motion/click/release
against compositor input counters. It cannot emulate a real Synaptics pad.

Validation on 2026-09-22 UTC: 2,085 host checks passed in each of the four
compiler/sanitizer configurations. QEMU's PS/2-only run recorded IRQ12=6,
9 AUX bytes, 3 packets delivered, cursor (640,400) -> (696,364), one left press
and release, and zero bad bytes/timeouts. The USB regression produced the same
cursor/button result without generating AUX packets. Both runs also booted from
USB, mounted OsitoFS3, read files, persisted diagnostics and completed the DOS
bit-operations probe. Managed scratch images were removed after QEMU exited.

The new kernel SHA256 is
`394281e526f0400435efa8957e6b5db81df4c0775d82f77074974ee86ef3ca17`.
Only `EFI/BOOT/kernel.elf` was updated on the test USB; the previously working
kernel is retained as `EFI/BOOT/KERNHND.ELF`. Bootloader, GPT and the data
superblock were verified unchanged. The user confirmed the touchpad works, then
reported a separate desktop keyboard problem: physical QWERTY typed MNOPQR.

## Keyboard Event Contract

The desktop consumed HID keyboard-page usages, but `kb_handle_hw_byte` posted
set-1 codes unchanged. Physical Q is set-1 `0x10`, which is HID M; W (`0x11`)
became N, and so on. This was a transport-encoding mismatch, not a language
layout setting or a Synaptics firmware issue.

The PS/2 producer now translates complete make/break sequences to HID usages.
E0 keys, right modifiers, navigation, the numeric keypad, ISO key, PrintScreen
fake shifts and the E1 Pause sequence have explicit decoding. DOS owners still
receive the original bytes, including prefixes. Console injection does not
repost events. The existing US character-layout tables are unchanged; this is
not localization or support for every vendor/Fn key.

Shared event flags track left/right modifier keys separately and publish their
post-transition state. Caps Lock toggles once per press. The compositor uses
these flags, with no ambiguous set-1 aliases for Alt/Tab: HID slash (`0x38`) and
L (`0x0F`) must not be treated as Alt and Tab. The 24-byte event ABI is unchanged.

Desktop regression testing also exposed a missing terminal layer: VT100 arrow
sequences were emitted correctly but inserted as literal `[D`/`[C` text.
The line editor now parses CSI/SS3 incrementally for cursor movement, Home,
End, Delete and history navigation, without blocking on a lone Escape.

```sh
bash arch/x86/scripts/test-keyboard.sh
python3 arch/x86/scripts/check-keyboard-qmp.py \
    --qmp /path/to/qmp.sock --serial-log /path/to/serial.log \
    --output /path/to/new-keyboard-result.json
```

The QMP checker requires a booted desktop with an idle, focused terminal and
Caps Lock off. Run it on PS/2-only and USB-keyboard QEMU configurations. It sends
actual key transitions through QMP, never UART text, and checks shell output
for physical rows/digits, punctuation, two-sided modifiers, Caps Lock, editing
and Alt+Tab. Host tests exercise the real PS/2 decoder/event producer (4,067
checks) and line editor (1,225 checks), with GCC/Clang and ASan/UBSan. The pointer
suite still passes all 2,085 checks per configuration, as does the xHCI polling
regression. No new hardware keyboard validation is claimed by these host tests.

The complete PS/2 QEMU run `smoke-ps2-keyboard-v3` passed all nine desktop
keyboard scenarios, pointer input, USB boot/filesystem checks and the DOS probe.
Its serial SHA256 is
`37526a83a1939173253f92e7b29947595d4be7a1ecdf03f1c31c486b8cb12dd1`.
The earlier v1 run exposed the missing terminal CSI parser. The v2 run stopped
on a duplicated character in serial file output (`bunndle`), before desktop
testing; its log is retained rather than counted as a pass.

At the keyboard-fix checkpoint, USB desktop regression had NOT fully passed.
`smoke-usb-keyboard-v3` lost the
first character of the second command; v4, with a one-second post-prompt pause,
passed rows, punctuation, Shift, Caps Lock and arrow editing, then stopped
receiving keys. A code audit found that the existing `bulk_xfer` advances the
shared xHCI event ring and discards nonmatching events, without the `xhci_poll`
guard. This can discard a HID completion without rearming its interrupt endpoint,
consistent with the failure during concurrent diagnostic writes to USB storage.
The synchronous bulk/control waiters and HID poller need coordinated event
ownership/dispatch; a timing workaround is not included in the checker. That
separate xHCI correction was still open then. All QEMU children exited and managed test
disks were removed.

The keyboard-fix kernel is 6,716,728 bytes, SHA256
`de758286d4d0f46d5fa9a77d81fd2ae366fe788a7d16a506b32db6b6b281acdd`.
After the user reconnected the test USB, this kernel was installed on disk 4
(USB serial `00000000CY02`) at 2026-09-22 02:37 UTC. Only the EFI kernel was
replaced; the touchpad-confirmed kernel is retained as `EFI/BOOT/KERNPS2.ELF`
and in the local deployment backup. Installed/backup hashes, all existing boot
files, both GPT copies and the OsitoFS3 superblock were checked after flushing
the EFI volume cache. Bootloader and data partition were not written. Evidence
is in `arch/x86/build/realhw-usb-20260921/keyboard-input/update-result.json`.
The P1 keyboard test was pending at this checkpoint.

## USB Storage Follow-Up, 2026-09-22

The shared-event-ring failure above is now fixed and the full USB desktop
regression passes, including concurrent USB storage traffic. The follow-up
also corrects bulk TDs crossing physical 64-KiB boundaries and adds a paged
`storage` boot snapshot that works without a mounted filesystem. The test USB
was updated again at 03:57 UTC, retaining the keyboard kernel as `KERNKEY.ELF`.
See [USB storage diagnostics](x86-usb-storage.md) for exact kernel hashes,
read-only filesystem inspection, final test evidence, and remaining scope.
Physical P1 confirmation is not implied by these QEMU results.

## Protocol References

- [Synaptics TouchPad Interfacing Guide, second edition](https://ccdw.org/~cjj/l/docs/ACF126.pdf), PS/2 protocol and mode-byte definitions.
- [Linux Synaptics driver](https://github.com/torvalds/linux/blob/master/drivers/input/mouse/synaptics.c), identity signature, relative reset and `LEN0402` model entry.
- [Linux PS/2 mouse decoder](https://github.com/torvalds/linux/blob/master/drivers/input/mouse/psmouse-base.c), standard and wheel packet layouts.
