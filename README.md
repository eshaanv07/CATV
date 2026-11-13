# Problem 4 — The Cat-V Minimalist Stack Challenge

Assemble and explain a minimal, non-mainstream OS stack · Intermediate to advanced. Full statement: Part E of `00-Common/OS_Track_Problem_Statements_FINAL.pdf`.

## What is in this folder

| Path | What |
|---|---|
| `judge/verify.sh` | The harness the judges run (wrapper for `verify.py`). |
| `judge/qemu.sh` | The exact QEMU profile. Do not modify it; the judges use their own copy. |
| `judge/osprobe.c` | Sample primitive-conformance probe for L2 and L4. The judges' copy differs only in its seed, which changes every run. |
| `judge/verify.yaml.template` | Copy to your repository as `verify.yaml` and fill it in. |

In the image, `judge/` is at `/opt/os-track/judge`, and base sources and images are under `/opt/os-track/src/bases` (BusyBox and Linux are at `/opt/os-track/src`). With the `full` image you also get FreeBSD, OpenBSD, NetBSD, 9front, Haiku and Redox images in `/opt/os-track/src/bases/images`.

## The machine your OS boots on

`judge/qemu.sh`: q35, `-cpu max`, 512 MiB, 2 CPUs, `-accel tcg`, no network, no display, serial console on stdio, `-no-reboot`, three AHCI disks:

| AHCI port | Disk | Notes |
|---|---|---|
| 0 | system disk (`boot.system_image`) | raw (qcow2 if the name ends in `.qcow2`); attached as a snapshot, so writes never persist |
| 1 | assets disk | a ustar archive at offset 0: `osprobe.c` (fresh seed every run) and your host-built `osprobe` if you ask for one; read it with e.g. `tar -xf /dev/sdb` |
| 2 | data disk | raw, blank (zeros) at the start of every run, kept across the reboot inside a run: create a file system on first use |

Without `boot.kernel`, SeaBIOS boots the system disk (bring your own boot loader or boot stub). With `boot.kernel`, QEMU loads it directly (a stock boot path).

## Run the harness

```sh
cp /opt/os-track/judge/verify.yaml.template /work/myos/verify.yaml   # then edit it
/opt/os-track/judge/verify.sh --config /work/myos/verify.yaml --verbose
/opt/os-track/judge/verify.sh --config /work/myos/verify.yaml --runs 10 --no-perf   # Robustness
```

What one run does (all timing on the host):

1. **L0** boot, wait for `prompt_regex`, send `nonce_command` with a random nonce split in two; the joined nonce must come back within 120 s of launch (up to 3 attempts).
2. Performance: QEMU RSS after 30 s idle at the prompt; launch-to-nonce time (skip with `--no-perf`).
3. **L1** read `HEARTBEAT <pid> <n>` lines from your supervised service, send `kill_command` for that pid, expect a new pid counting from 1 within 5 s.
4. **L2** run `osprobe.setup`/`osprobe.build` and `osprobe all`; each of the six `PASS <check> <value>` lines must carry the value the host computed from the seed.
5. **L3** run `data.setup` and `data.write` with a fresh value, then `poweroff_command` (QEMU must exit within 15 s; this is also part of L1).
6. Boot again: `data.setup`, `data.read` must print the value. **L4** `osprobe burn` sizes the work on the host clock (about 3 s), then `osprobe smp`: two busy workers must take at most 1.3× one worker.
7. Performance: `xz -9e` size of your image files.

To answer commands, `verify.sh` types each one into your console followed by `nonce_command` for a split marker, and waits for the joined marker. Your shell only has to run commands in order and print what they print.

Result line:

```
  run 1: L0 PASS  L1 PASS  L2 6/6  L3 PASS  L4 PASS  -> correctness 30.0 / 30
```

Logs of the whole console session are kept in the work directory it prints.

## Notes

- Build `osprobe` either on the host (`osprobe.host_build`, for example a static musl binary: the image has `musl-gcc`) or in the guest (`osprobe.build`, for example with TinyCC; the source is in `/opt/os-track/src/bases/tcc-0.9.27`).
- Hobby kernels without a POSIX layer: set `osprobe.native: true` and bring a native program covering the same six primitives; the judges review it with the source and a live run.
- Get L0 working first; nothing else is scored without it.

## Deliverables

- [ ] the bootable image and the exact QEMU command (`judge/qemu.sh` plus your `verify.yaml`)
- [ ] `verify.yaml`, build scripts and configs (a clean checkout must rebuild the image)
- [ ] the architecture manifest with self-scoring (esotericism table and authored components)
- [ ] a recorded demo of at most 5 minutes, and the other common deliverables in `00-Common/README.md`
