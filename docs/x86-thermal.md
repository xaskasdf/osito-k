# Read-only x86 thermal diagnostics

`thermal` reads the current CPU and its package; it is not a fan controller or
a sample of every core. The command is admitted as a concurrent SSH diagnostic.
It does not change MSRs, fan settings, thermal interrupts, clocks, or power limits.
The scheduler and AP workers default to HLT, with an optional native MWAIT path
described below. An online CPU is
not evidence that it is busy, and a process listing is not a utilization sample.

Intel CPUID gates the thermal, package, SpeedStep and HWP queries. Unsupported
processors report unavailable rather than fabricated temperatures. The output
includes raw thermal status, current and sticky historical critical/thermal
flags, the current CPU APIC ID, and the local APIC timer mask state. A historical
flag is not evidence of current overheating.

Core DTS validity comes from bit 31; package status has no equivalent validity
bit. Absolute Celsius uses MSR_TEMPERATURE_TARGET only when readable and nonzero.
Without it, the command reports the sensor delta with no assumed TjMax. HWP
request registers are queried only after IA32_PM_ENABLE confirms HWP is enabled.

MSR reads use a dedicated assembly instruction and fault continuation. The #GP
handler recovers only an error-code-zero, CPL0 fault at that exact instruction.
Both kernel long-mode selectors used by this OS are supported; unrelated
instruction pointers, user faults, and other exceptions keep their normal path.
The result pointer must be valid kernel memory and remains unchanged on failure.
The CPU idle extension below also handles this exact fault on APs, before their
fast interrupt return, without entering the BSP-only C exception handler.
`thermal selftest` deliberately reads an unsupported MSR and checks recovery;
it is not allowed concurrently with a busy foreground console.

## Validation (2026-09-26)

- `bash arch/x86/scripts/test-thermal.sh`: 36 checks, zero failures with GCC
  and Clang, in normal and ASan/UBSan builds.
- `bash arch/x86/scripts/test-shell-remote.sh`: 104 checks, zero failures in
  all four builds, including read-only admission and rejection of redirects
  and the selftest during a foreground command.
- `OSITO_SSH_TEST_THERMAL_ONLY=1 bash arch/x86/scripts/test-ssh-qemu.sh OUT`
  boots isolated managed disks, provokes a real kernel #GP through SSH, verifies
  recovery, repeats diagnostics, and checks a subsequent SSH command. The r2
  run passed; its virtual CPU does not expose Intel thermal sensors, so this
  proves fault recovery and unavailable handling, not temperature accuracy.
- Clang/lld kernel: 7,124,384 bytes, SHA-256
  `345b9dae72bff388d9988f92b9aee0f820350b5282b0464502e4978132e404c5`.
  Evidence: `arch/x86/build/thermal-qemu-20260926-r2/`.

The first QEMU attempt incorrectly restricted recovery to CS=0x08; the live
kernel also uses CS=0x38. The corrected probe above validates that path before
physical deployment. No failed diagnostic kernel was installed on the P1.

## Physical P1 Observation (2026-09-26)

Two uploads while UT was running ended with SSH channel resets and did not
publish the kernel. After the user closed UT and `console status` reported idle,
the same 4 KiB SSH upload completed with a verified SHA-256 commit. The r2 kernel
above was published as `/kernel-thermal-20260926.elf` and loaded with `kexec`.
Wi-Fi and SSH returned automatically at 192.168.1.20. The EFI cold-boot kernel
on the USB was not replaced. This success does not establish the cause of the
earlier transfer resets.

Readings on the Xeon E-2276M, current CPU APIC 0 and its package, were identical
for core/package at each sample. Times are host local time, America/Santiago:

| Time | State | Core/package |
| --- | --- | --- |
| 03:31:55 | Shell after hotreload, no game | 63 C |
| 03:32:38 | Shell idle | 62 C |
| 03:33:40 | Shell idle | 63 C |
| 03:34:52 | Desktop idle, launched at 03:33:50 | 59 C |
| 03:35:28 | Desktop idle | 59 C |

All samples reported current thermal/critical/external-PROCHOT flags zero,
logged thermal/critical flags zero, automatic thermal control enabled,
SpeedStep enabled, HWP disabled, and the local APIC timer unmasked. The readable
temperature target was 100 C. Uptime after the last sample was 4m 17s.
The desktop process list contained eight tasks and no game; a process-state
snapshot is not a CPU utilization measurement.

These are short idle observations, not a steady-state thermal benchmark. They
do not prove that launching the desktop caused cooling, that HWP being disabled
caused chassis warmth, or that every core/GPU/chassis sensor is cool. Fan RPM
and the firmware fan policy remain unmeasured. No fan or power-policy register
was written. Evidence is in the `p1-*.log` files under the r2 build directory.
Do not infer a failed fan from chassis warmth alone.

The user subsequently confirmed that the fan is audibly operating normally.
That observation does not provide an RPM measurement or require overriding
firmware regulation.

## Bounded Power Sampling

`thermal sample` takes two snapshots around a scheduler sleep of 100 ticks
(nominally one second). It does not spin, change affinity, write MSRs, reset
counters or program performance-monitoring events. Failure to sleep aborts
the measurement. This command is also admitted during a foreground console
command over SSH; `thermal selftest` remains exclusive.

The initial model-specific coverage is Intel family 6 models 4E, 5E, 8E and 9E
(client Skylake/Kaby/Coffee Lake, including the P1's Xeon E). Unknown families,
vendors and hybrid models are not probed. CPUID must expose MSRs, TSC and an
invariant TSC; the calibrated kernel TSC frequency supplies the elapsed time.
CPUID.6:ECX[0] separately gates APERF/MPERF. Each MSR read can independently
report unavailable through the existing fault-safe helper.

- APERF/MPERF yields active effective MHz; APERF/TSC yields wall-average MHz.
  MPERF/TSC is the logical CPU's C0 reference activity, not process CPU usage.
  If HDC forces idle, MPERF can include forced-idle periods while APERF does
  not; this diagnostic does not change HDC policy. No active frequency is
  reported when MPERF does not advance.
- RAPL package energy uses the read energy unit and 32-bit modulo subtraction.
  The reported mW is a package average, not whole-system wall power. The short
  interval assumes no more than one energy-counter wrap; filtered firmware
  energy telemetry can limit precision. GPU/chassis/battery readings are not
  inferred from this value.
- Core C3/C6/C7 and package C2/C3/C6/C7 residency use TSC-rate counters. These
  model-specific state names are not AML or MWAIT selectors. Unmeasured states
  (including C1 and deeper package states) are not reported as zero, nor is
  their duration inferred by subtraction. A core is not a logical CPU, so its
  residency percentages must not be added to logical-CPU C0 as a total.
- PERF_CTL, package C-state configuration and POWER_CTL are printed raw,
  read-only, without applying a policy or assuming that a firmware limit is
  the state the CPU actually entered.

Samples reject a changed APIC ID, read spans over 1 ms, elapsed intervals
outside 0.5-5 seconds, counter resets/backwards movement (except RAPL's modulo
counter), impossible residency above the interval, and arithmetic overflow.
The current BSP shell worker is the only execution CPU sampled; the sampler
does not provide per-AP energy readings or whole-machine utilization accounting.

Validation for the sampling extension (2026-09-26):

- Thermal host suite: original 36 checks plus 74 sampling checks, zero failures
  with GCC/Clang and normal/ASan+UBSan configurations.
- Shell remote suite: 107 checks, zero failures in all four configurations.
- QEMU SSH: fault recovery, repeated temperature and sampling commands, and a
  subsequent SSH command passed. This VM reports sampling unavailable, so it
  does not validate physical energy measurements or residency percentages.
- Kernel: 7,124,536 bytes, SHA-256
  `325cb9293201690fb24860097837d15ed862ad2141f88b96cb88ff5ffe152f09`.
  Evidence: `arch/x86/build/thermal-sample-qemu-20260926-r1/`.

### P1 Sampling Results

The verified SSH upload completed and `/kernel-power-sample-20260926.elf`
was hotloaded. Wi-Fi and SSH returned at 192.168.1.20. The cold-boot EFI kernel
was not replaced. CPUID returned signature `0x000906ED`, with a calibrated
TSC of 2,805,265,600 Hz.

The first invocation in the plain shell reported `scheduler could not sleep`;
no fabricated sample or busy wait was substituted. After `console launch
desktop`, the scheduler's idle task was available and three samples completed:

| Host local time | Interval (us) | CPU 0 C0 reference | Active MHz | Wall-average MHz | Package mW |
| --- | --- | --- | --- | --- | --- |
| 05:10:09 | 1008082 | 4.58% | 2501 | 114 | 3990 |
| 05:11:17 | 1008027 | 4.58% | 2497 | 114 | 3844 |
| 05:12:20 | 1008149 | 4.59% | 2497 | 114 | 3894 |

All measured core C3/C6/C7 and package C2/C3/C6/C7 counters reported 0.00%
residency at the displayed precision. The read-only control snapshots stayed:

- PERF_CTL: `0x0000000000001B00`.
- Package C-state configuration: `0x000000001E008008`.
- POWER_CTL: `0x00000000003C005F`.

Temperature queries reported 56 C, then 55 C for the sampled core/package,
without current or logged thermal/critical flags. HWP remained disabled;
SpeedStep and automatic thermal control remained enabled. Eight tasks were
listed, with no game running. A subsequent SSH dmesg retrieval showed no
exception/panic and confirmed the scheduler idle task was online.

The sampler does not establish a whole-machine CPU utilization or cooling
benchmark. In particular, lower temperature is not attributed to these code
changes: they do not modify power policy. The lack of measured deep residency
is consistent with the `sti; hlt` idle paths in `scheduler_idle_thread` and
`ap_worker_loop`; it motivates CPUID/ACPI-aware idle-state selection and wakeup
accounting, followed separately by a measured HWP policy experiment. No such
policy experiment or fan-control change was part of that sampling revision.

## Reversible Native Idle Experiment

`cpuidle` / `cpuidle status` reports per-CPU capabilities and wait attempts.
`cpuidle mwait` opts this boot into native MWAIT; `cpuidle hlt` returns to HLT.
Unsupported or fault-disabled CPUs continue using HLT. The initial policy is
HLT, and this selection is not persisted. Status is a concurrent SSH diagnostic;
policy changes and `cpuidle selftest` require exclusive command ownership.

The initial profile is Intel family 6 models 4E/5E/8E/9E. Selection requires
MONITOR, CPUID.5 power extensions and interrupt-break support, a valid monitor
range fitting the isolated 128-byte per-CPU wake record, invariant TSC, and
ARAT on the BSP. Only advertised substates are selected, with a maximum hint
of `0x20`. These are native CPU hints, not ACPI `_CST` indices. There is no AML
idle driver, broadcast timer backend, exit-latency governor or automatic
platform-wide power policy yet. Firmware package limits remain unchanged.

The scheduler and AP workers retain their IRQ-disabled queue checks. AP wakeup
publishes a per-CPU sequence change before sending the existing IPI. The assembly
helper arms MONITOR and rechecks that sequence before MWAIT with ECX=1, allowing
pending interrupts to break the wait even with IF=0. An earlier notification is
covered by the original CLI/queue-recheck/IPI contract. IRQs are enabled on return;
the HLT fallback retains the atomic `sti; hlt` sequence. The BSP's masked-timer
software-interrupt fallback is unchanged. Plain-shell polling and other HLT sites
are not indiscriminately replaced.

The monitored records have their own alignment/padding; scheduler counters,
worker statistics and unrelated stores cannot share their advertised monitor
range. They reside in ordinary write-back kernel RAM, not MMIO or framebuffer
memory. No DMA-store wakeup guarantee is assumed. IRQ/IPI wakeup is retained.

Exact CPL0, error-code-zero #GP/#UD sites in the MONITOR/MWAIT helper recover
to an unsuccessful return; that CPU latches HLT until reboot instead of repeatedly
faulting. RDMSR recovery remains #GP-only. These fixups execute before AP/BSP
dispatch, preserve the interrupted GPRs, do not send an exception EOI and do not
touch BSP scheduler/FPU/IST cursor state. Other instruction pointers and user
faults follow the existing dispatch paths.

`cpuidle selftest` wakes each owning CPU and requests a fault-safe unsupported
MSR read and a MONITOR with a deliberately mismatched word (never a real wait).
It checks CPU identity and bounded acknowledgments, so BSP work stealing cannot
masquerade as an AP test. An online but uninitialized/unresponsive CPU fails
rather than disappearing from the total. Wait-attempt counters are not residency.

Validation:

- `test-cpu-idle.sh`: 80 checks, zero failures, GCC/Clang normal and ASan/UBSan.
  Covers CPUID/profile/timer gating, padding limits, notification wrap, changed
  predicates, optional-instruction faults, permanent HLT fallback, reversible
  selection, missing APs and bounded probe timeouts.
- `test-shell-remote.sh`: 114 checks, zero failures in all four configurations.
- `test-thermal.sh`: 36 thermal and 74 sampler checks in all four configurations.
- `OSITO_SSH_TEST_CPU_IDLE_ONLY=1 .../test-ssh-qemu.sh OUT`: four-CPU capability
  reporting, repeated policy changes, 4/4/4 probe acknowledgments, real kernel/AP
  MSR fault recovery, worker dispatch, scheduler idle, repeated thermal queries
  and surviving SSH. The VM uses fallback HLT; it does not validate Intel C-state
  residency or physical MWAIT power savings.

### Initial P1 A/B Observations

Revision r2, SHA-256
`924cffa0e1a9f85085785b8819dc9ce11552bbcf18f8f90ebc935d1bb3dff7fb`,
7,129,272 bytes, was hash-verified over SSH and hotloaded. Wi-Fi and SSH returned
automatically. Both HLT and MWAIT selftests passed 12/12/12. The desktop had eight
tasks and no application running. All CPUs advertised hint `0x20` as supported.
The cold-boot EFI kernel was not replaced.

| Sequence | Package samples (mW) | Sampled core C6 | Package C2 |
| --- | --- | --- | --- |
| HLT baseline | 4363, 4285, 4010 | 0.00% | 0.00% |
| MWAIT first | 3390, 3405, 3437 | 94.91-94.93% | 94.74-94.76% |
| HLT restored | 4273, 4243, 4187 | 0.00% | 0.00% |
| MWAIT reselected, first sample | 15723 | 6.59% | 0.00% |
| MWAIT subsequent samples | 3394, 3404 | 94.81-94.94% | 94.64-94.77% |

Do not silently discard the transient 15.7 W sample: AP wait counters showed a
brief burst of rapid returns, then stopped advancing in idle. No instruction
faults were recorded. This motivated isolating the MONITOR wake records in r3;
it does not itself prove that unrelated stores caused the burst. The steady
samples suggest a saving around 0.8 W, but neither this short run nor the 55 to
54 C temperature observations constitute a sustained thermal/whole-system-power
benchmark. Package C3/C6/C7 remained zero, and HWP and firmware controls were
unchanged. A verified 1920x1080 SSH screenshot still showed the normal desktop.
Raw evidence, including the outlier, is under
`arch/x86/build/cpu-idle-qemu-20260926-r2/`.

### HWP Is Separate

No HWP enable/request MSR is written by this extension. IA32_PM_ENABLE is a
package-wide, write-once enable; kexec does not provide a way to clear it.
A future HWP trial must gate each supported field with CPUID, enable the package
before reading dependent capability/request registers, apply bounded requests on
the owning logical CPUs, verify readback/acknowledgments and compare both idle
energy and useful-work performance. Restoring request values is not disabling
HWP; processor RESET is required to return to the disabled baseline. This remains
separate from idle-state selection and from fan/EC regulation.

MONITOR/MWAIT and interrupt-break contracts follow the instruction reference and
multi-processor management chapter in the
[Intel SDM](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf).

## Fan Support Still Missing

The kernel currently has ACPI table discovery for SMP and shutdown, but no
general AML interpreter, EC transaction driver, fan tachometer driver or thermal
policy manager. Fan RPM is explicitly unavailable; no automatic fan mode is
claimed or changed. GPU and chassis sensors are also outside this command.

For the ThinkPad P1, discover the actual EC resources and firmware interfaces
before adding fan access. Validate model-specific capabilities, use bounded
serialized transactions, and preserve firmware regulation. Do not assume fixed
ports, register offsets, fan count or manual PWM behavior from another model.
Temperature readings and workload/idle comparisons should precede policy edits.
Read-only discovery of the P1's ACPI/EC resources remains separate from the
CPU sampling above. Keep fan telemetry separate from optional manual control.

References: [Intel SDM, Volume 3B, chapter 16](https://cdrdv2-public.intel.com/835755/253669-sdm-vol-3b.pdf)
for thermal/HWP contracts, and [Linux ThinkPad ACPI documentation](https://www.kernel.org/doc/html/latest/admin-guide/laptops/thinkpad-acpi.html)
for the distinction between fan status and optional model-dependent control.
Counter contracts and model selection follow [Intel SDM Volume 4, tables 2-2,
2-20/21/29/35/39](https://cdrdv2-public.intel.com/874253/335592-090-sdm-vol-4.pdf).
