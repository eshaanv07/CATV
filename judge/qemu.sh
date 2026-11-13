#!/bin/sh
# judge/qemu.sh - the exact QEMU profile for Problem 4. DO NOT MODIFY.
# The judges run their own copy of this file; a modified copy changes
# nothing for scoring.
#
#   qemu.sh SYSTEM_IMG ASSETS_IMG DATA_IMG [KERNEL [INITRD [APPEND]]]
#
#   SYSTEM_IMG  your system disk, AHCI port 0 (raw, or qcow2 by extension).
#               Attached as a snapshot: the guest may write to it, but
#               nothing it writes survives. Persistence only comes from
#               the data disk.
#   ASSETS_IMG  the assets disk, AHCI port 1: a raw disk holding a tar
#               archive (ustar) at offset 0 with osprobe.c, and your
#               host-built osprobe if verify.yaml asks for one. Snapshot.
#   DATA_IMG    the data disk, AHCI port 2: raw, blank at the start of a
#               run, writable, kept across the reboot inside a run.
#   KERNEL...   optional: boot with QEMU -kernel/-initrd/-append (counts as
#               a stock boot path). Without it the firmware (SeaBIOS)
#               boots SYSTEM_IMG.
set -eu
SYS=$1; ASSETS=$2; DATA=$3; KERNEL=${4:-}; INITRD=${5:-}; APPEND=${6:-}
fmt=raw; case "$SYS" in *.qcow2) fmt=qcow2 ;; esac
set -- qemu-system-x86_64 \
  -machine q35 -cpu max -m 512M -smp 2 -accel tcg \
  -nic none -display none -monitor none -serial stdio -no-reboot \
  -device ahci,id=ahci \
  -drive if=none,id=sys,file="$SYS",format=$fmt,snapshot=on \
  -device ide-hd,drive=sys,bus=ahci.0 \
  -drive if=none,id=assets,file="$ASSETS",format=raw,snapshot=on \
  -device ide-hd,drive=assets,bus=ahci.1 \
  -drive if=none,id=data,file="$DATA",format=raw \
  -device ide-hd,drive=data,bus=ahci.2
[ -n "$KERNEL" ] && set -- "$@" -kernel "$KERNEL"
[ -n "$INITRD" ] && set -- "$@" -initrd "$INITRD"
[ -n "$APPEND" ] && set -- "$@" -append "$APPEND"
exec "$@"
