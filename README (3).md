# Problem 2 — ContextChaos

A user-space fiber runtime with unconventional rules · Intermediate to advanced · C, assembly permitted. Full statement: Part C of `00-Common/OS_Track_Problem_Statements_FINAL.pdf`.

## What is in this folder

| Path | What |
|---|---|
| `include/fiber.h` | The judge-supplied interface. Use it unchanged; the judges compile against their own copy. **Read its header comment**: it pins down ids, scheduling order and return conventions. |
| `tests/` | Public sample tests (about 30% of the suite) and the runner the judges use. |
| `bench/` | The three Performance benchmarks, including the `swapcontext` baseline. |

## Build your library

Produce `libcontextchaos.a` (or `.so`) from a clean checkout with one documented command. Only libc; no third-party fiber or coroutine libraries; your library must not call `getcontext`, `setcontext`, `makecontext` or `swapcontext`, and must never create threads. Code is built with `-fcf-protection=none` and without `_FORTIFY_SOURCE`.

## Run the public tests and benchmarks

Inside the OS Track image:

```sh
/opt/os-track/contextchaos/tests/run_public.sh /work/libcontextchaos.a
/opt/os-track/contextchaos/bench/run_bench.sh  /work/libcontextchaos.a
```

The runner sets `FIBER_CLOCK` (and `FIBER_QUANTUM_US` where needed) for each test and applies a hard timeout; a hang counts as a failure.

| Test | Group | Clock | Checks |
|---|---|---|---|
| `c01_spawn_join` | Core scheduling | logical | spawn/yield/join/exit, round-robin trace, error codes |
| `c02_share_421` | Core scheduling | logical | 4:2:1 shares for priorities 0, 1, 3, within one quantum |
| `c03_mutex_order` | Core scheduling | logical | the published A, C, B example, plus a lowest-id tie-break |
| `c04_mutex_errors` | Core scheduling | logical | `EDEADLK` (relock, 2- and 3-fiber cycles, join cycles), `EPERM`, `EBUSY`, `EINVAL` |
| `c05_shared_stack` | Shared stacks and preemption | logical | S1, S2, S3 on 40 shared + 4 own-stack fibers |
| `c06_shared_50k` | Shared stacks and preemption | logical | S4: 50,000 parked shared fibers, peak RSS ≤ 128 MiB |
| `c07_preempt_spin` | Shared stacks and preemption | real | fibers that never yield both progress; 4:2:1 within 10% |
| `c13_preempt_disable` | Shared stacks and preemption | real | no preemption inside (nested) disabled regions |
| `c08_preempt_state` | Hardening and I/O | real | rounding modes, x87, SSE and AVX2 state and `errno` bit-identical across preemption |
| `c09_preempt_libc` | Hardening and I/O | real | `malloc`/`snprintf`/stdio under a short quantum: no hang, no corruption |
| `c10_fiber_read` | Hardening and I/O | logical | `fiber_read` on pipes and timerfds blocks only the caller |
| `c11_stack_overflow` | Hardening and I/O | logical | S5 for own and shared stacks: abort, message names the fiber |
| `c12_signal_chain` | Hardening and I/O | both | the application's SIGSEGV and SIGUSR1 handlers keep working |
| `no_forbidden_symbols` | rules | | `nm`: no ucontext calls, no thread creation |
| `no_clone_syscalls` | rules | | `strace`: no `clone`/`clone3`/`fork` while the runtime runs |

`run_bench.sh` reports the three Performance numbers: the yield ping-pong speedup over the `swapcontext` baseline (ranked among teams; plus the check for `rt_sigprocmask` calls in the loop), matmul overhead under 1 ms preemption, and shared-stack ns per KiB (ranked). The judges run them on the reference machine, median of 3.

The hidden tests use the same harness with different counts, durations, quantum values and patterns.

## Clarifications to the statement

All are in the comments of `fiber.h`:

- The runtime initializes lazily on the first call. The thread that runs `main()` is the **main fiber, id 0**, priority 0; it can yield, join, lock and block like any fiber. Spawned fibers get ids 1, 2, 3, ... in spawn order.
- "Round-robin in spawn order" means: the next fiber to run is the next runnable fiber after the current one in increasing id order, wrapping around to fiber 0.
- A fiber's slice is refilled to q(p) each time it is scheduled in.
- `int` functions return 0 or a positive errno value (pthread style). `fiber_spawn` returns `FIBER_INVALID` and sets `errno`.
- `fiber_join` error order: `ESRCH` (unknown or already joined), `EDEADLK` (yourself), `EINVAL` (someone else is already joining it), `EDEADLK` (would close a cycle).
- `fiber_self()` is part of the interface.
- `fiber_stats` works for live fibers, finished-but-unjoined fibers and fiber 0; `ESRCH` after a fiber is joined.
- S5: on overflow, write a message containing `fiber <id>` to stderr and abort.
- `fmutex_t` gives you 64 bytes of 16-byte-aligned storage.

## Deliverables

- [ ] `libcontextchaos.a` or `.so`, plus demo and benchmark executables
- [ ] an architecture spec covering the stack layout, register save and restore (`rsp`, `rbp`, `rip` and the FPU/AVX state) and preemption signal safety
- [ ] the common deliverables in `00-Common/README.md` (including a demo of at most 5 minutes)
