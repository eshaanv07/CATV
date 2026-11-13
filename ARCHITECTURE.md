# Cat-V Minimalist Stack — Architecture Manifest

## 1. Project Overview

Cat-V is a minimal Linux-based operating-system stack developed for
Problem 4 of the OS Track challenge.

The system uses Buildroot, BusyBox and the Linux kernel to provide
a minimal bootable environment.

The implementation addresses five challenge levels:

- L0: Boot and serial-console interaction
- L1: Service supervision and poweroff
- L2: Operating-system primitives
- L3: Data persistence
- L4: Multiprocessing performance

## 2. System Architecture

The system consists of these main layers:

| Layer | Component | Purpose |
|---|---|---|
| Virtual machine | QEMU 8.2.2 | Provides virtual hardware |
| CPU | 2 virtual CPUs | Supports multiprocessing |
| Kernel | Linux 6.12.9 | Core operating-system functionality |
| Userspace | BusyBox 1.37.0 | Minimal shell and utilities |
| Build system | Buildroot 2025.02 | Builds the system |
| Service supervision | Heartbeat supervisor | Monitors and restarts heartbeat |
| Storage | Separate data disk | Maintains data across reboot |
| Console | Serial ttyS0 | Communicates with verifier |

## 3. Boot Architecture

The system boots a Linux kernel using the QEMU virtual machine
environment configured by the official challenge harness.

The boot configuration uses:

- Machine type: QEMU q35
- CPU model: max
- Virtual CPUs: 2
- Memory: 512 MiB
- Accelerator: TCG
- Serial console: ttyS0
- Root filesystem: /dev/sda

The kernel command line is configured to use the system disk
as its writable root filesystem.

The kernel image is build/images/bzImage and the bootable
system image is build/system.img.

## 4. Kernel Configuration

The Linux kernel is based on version 6.12.9.

Relevant configuration includes:

- SMP support enabled
- Two virtual CPUs provided by the QEMU configuration
- Dynamic preemption support enabled
- PREEMPT_NONE selected as the configured default preemption model
- 250 Hz kernel timer frequency

The kernel configuration is stored in kernel.config.

## 5. Userspace and Service Supervision

BusyBox provides the minimal userspace environment.

The project includes these custom service-related files:

- overlay/usr/sbin/heartbeat
- overlay/usr/sbin/heartbeat-supervisor
- overlay/etc/init.d/S90heartbeat

The heartbeat process periodically reports its process information.

The supervisor monitors the heartbeat process and restarts it
when it terminates.

The official L1 test terminates the running heartbeat process and
checks whether a replacement starts within the required time.

The system also supports guest poweroff.

## 6. Storage Architecture

The QEMU verification environment provides three virtual disks:

| Device | Purpose |
|---|---|
| /dev/sda | Operating-system disk |
| /dev/sdb | Verification assets and OS probe |
| /dev/sdc | Persistent data disk |

The operating-system image is attached as a snapshot.

The separate data disk is used for persistence testing.

During L3:

1. The guest prepares the data disk.
2. A generated value is written to persistent storage.
3. The guest powers off.
4. The guest boots again.
5. The stored value is read.
6. The verifier checks that the value matches.

This separates persistent application data from the operating-system image.

## 7. OS Primitive Verification

The official OS probe tests six operating-system capabilities:

| Primitive | Functionality |
|---|---|
| PROC | Process creation and waiting |
| PIPE | Inter-process communication |
| SIGNAL | Signal handling |
| MMAP | Memory mapping |
| THREAD | Thread creation and joining |
| CLOCK | Clock and sleep operations |

The verifier uses a fresh seed for each run and checks the expected
results produced by the guest.

The project configures host-side static musl compilation for the probe.

## 8. Multiprocessing Architecture

The Linux kernel supports SMP, and QEMU provides two virtual CPUs.

The official L4 benchmark compares one CPU-bound worker against
two concurrent CPU-bound workers.

The acceptance condition is:

Two-worker time / One-worker time <= 1.30

Final recorded result:

| Measurement | Time |
|---|---:|
| One worker | 2.94 seconds |
| Two workers | 3.35 seconds |
| Ratio | 1.14 |
| Maximum allowed ratio | 1.30 |
| Result | PASS |

## 9. Esotericism Self-Scoring

The challenge defines esotericism using a layer-based score.

The total layer value is E, with the final esotericism score calculated as:

**Esotericism points = 6 × E / 130**

Only components used in the booted image are counted.

### Layer Assessment

| Layer | Actual implementation | Challenge points | Basis |
|---|---|---:|---|
| OS base | Buildroot-based Linux system | +15 | Linux built from source using Buildroot |
| Boot path | QEMU direct kernel boot (`-kernel`) | 0 | Challenge assigns 0 to QEMU `-kernel` or stock bootloader |
| Init | Stock BusyBox init | +10 | Minimal stock supervisor/init |
| C library / syscalls | musl | +15 | Challenge assigns +15 to musl, uClibc, or dietlibc |
| Userland | BusyBox | +10 | Challenge assigns +10 to BusyBox, Toybox, or 9base |

### Self-Assessment Calculation

| Measure | Value |
|---|---:|
| Layer total (E) | 50 |
| Maximum layer total | 130 |
| Esotericism points (`6 × 50 / 130`) | 2.31 / 6 |

This is a self-assessment based on the project configuration and boot method.
It assumes the delivered boot image uses the listed components.

The project uses established open-source components rather than claiming
a custom kernel, bootloader, init system, C library, or shell.


## 10. Authored and Configured Components

This section distinguishes project-specific implementation work from
existing upstream software.

### Project-Specific Contributions

| Component | Contribution |
|---|---|
| Heartbeat service | Project-specific heartbeat reporting |
| Heartbeat supervisor | Process monitoring and restart logic |
| Init integration | Startup integration through S90heartbeat |
| Buildroot configuration | Configuration of the minimal Linux system |
| Kernel configuration | Kernel configuration including SMP support |
| Root filesystem overlay | Custom files integrated into the root filesystem |
| Verification configuration | verify.yaml prepared for the official harness |
| Build script | build.sh for reproducing the Buildroot build |
| Architecture manifest | Documentation of system design and self-assessment |

### Existing Upstream Components

The following are existing software projects and are not claimed
as original implementations:

- Linux kernel
- BusyBox
- Buildroot
- QEMU
- Official OS Track verification harness
- Official OS probe

The project contribution is the configuration, integration and
project-specific service implementation built around these components.

### Authored-Work Qualification

The project contains custom heartbeat service and supervision scripts.
The inspected scripts contain 60 lines in total (28-line supervisor,
10-line heartbeat service, and 22-line init integration).

These scripts demonstrate project-specific implementation work, but
they do not meet the challenge's 150-line threshold for an authored init
component. No qualifying authored component from the challenge's
technical-depth candidate list is claimed at this stage.

Accordingly, the project does not claim the authored-work gate or
technical-depth points for an original init, bootloader, libc/syscall
layer, shell/core utilities, or kernel patch/module.

## 11. Verification Evidence

The final official verification completed ten consecutive successful runs.

| Challenge Level | Final Result |
|---|---|
| L0 — Boot | 10/10 PASS |
| L1 — Service supervision and poweroff | 10/10 PASS |
| L2 — OS primitives | 60/60 checks PASS |
| L3 — Data persistence | 10/10 PASS |
| L4 — Multiprocessing performance | 10/10 PASS |

Final aggregate results:

- Successful runs: 10/10
- Correctness per run: 30/30
- Aggregate correctness: 300/300

These results refer to the recorded official verification runs.

## 12. Build and Project Files

| File | Purpose |
|---|---|
| README.md | Project overview and instructions |
| ARCHITECTURE.md | Architecture manifest and self-scoring |
| build.sh | Buildroot build script |
| buildroot.config | Buildroot configuration |
| kernel.config | Linux kernel configuration |
| verify.yaml | Official verification configuration |
| overlay/ | Custom root filesystem overlay |
| build/system.img | System image |
| build/images/bzImage | Linux kernel image |
| judge/ | Verification-related reference files |

The build script uses the Buildroot 2025.02 source tree available
in the OS Track environment.

## 13. Conclusion

The Cat-V Minimalist Stack combines a minimal Linux userspace,
an SMP-enabled kernel, project-specific heartbeat supervision
and separate persistent storage.

The implementation successfully passed all five official
verification levels across ten consecutive runs.

This manifest documents the system architecture, project-specific
contributions, technology choices and self-assessment.
