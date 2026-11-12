/*
 * fiber.h - OS Track, Problem 2 (ContextChaos)
 * Low-Level Hackathon, Synergy '26, IIIT Bangalore
 *
 * Judge-supplied interface. Do NOT modify this file: the judges compile
 * their tests against their own copy. Your library (libcontextchaos.a or
 * libcontextchaos.so) must implement every function declared here.
 *
 * Interface version: 1
 *
 * Conventions used throughout
 * ---------------------------
 *  - The runtime initializes itself lazily on the first call into it.
 *    There is no init function.
 *  - The thread that calls main() becomes the main fiber, id 0. It is a
 *    fiber like any other: it may yield, join, lock mutexes and block in
 *    fiber_read. Its priority is 0.
 *  - Spawned fibers get ids 1, 2, 3, ... in spawn order. Ids are never
 *    reused.
 *  - "Round-robin in spawn order": when the running fiber yields, blocks
 *    or ends its slice, the next fiber to run is the next runnable fiber
 *    after it in increasing id order, wrapping around from the highest id
 *    to fiber 0.
 *  - A fiber's slice is refilled to q(p) = ceil(64 / (p + 1)) ticks every
 *    time it is scheduled in.
 *  - Functions that return int return 0 on success or a positive errno
 *    value on failure (pthread style). They do not set errno.
 */
#ifndef FIBER_H
#define FIBER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIBER_API_VERSION 1

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef int64_t fiber_t;            /* fiber id; 0 is the main fiber */
#define FIBER_MAIN    ((fiber_t)0)
#define FIBER_INVALID ((fiber_t)-1)

#define FIBER_PRIO_MIN 0
#define FIBER_PRIO_MAX 31

typedef enum {
    STACK_OWN    = 0,   /* the fiber gets its own mmap'd stack with a guard page */
    STACK_SHARED = 1,   /* the fiber runs on the shared run stack (R1)          */
} fiber_stack_mode_t;

typedef struct {
    int                priority;    /* 0..31; higher number = shorter slice      */
    fiber_stack_mode_t stack_mode;  /* STACK_OWN or STACK_SHARED                 */
    size_t             stack_size;  /* STACK_OWN: usable stack bytes, 0 = default
                                       (64 KiB). STACK_SHARED: ignored; the run
                                       stack is 256 KiB.                         */
} fiber_attr_t;

/* Defaults used when attr is NULL: priority 0, STACK_OWN, 64 KiB. */
#define FIBER_ATTR_DEFAULT { 0, STACK_OWN, 0 }

typedef enum {
    FIBER_RUNNABLE = 0,   /* ready, or currently running */
    FIBER_BLOCKED  = 1,   /* waiting in fmutex_lock, fiber_join or fiber_read */
    FIBER_DONE     = 2,   /* finished, not yet joined */
} fiber_state_t;

typedef struct {
    fiber_t       id;
    int           priority;
    int           stack_mode;          /* fiber_stack_mode_t */
    int           state;               /* fiber_state_t */
    uint64_t      ticks;               /* ticks charged to this fiber while it ran */
    uint64_t      switches_in;         /* times this fiber was scheduled in */
    uint64_t      yields;              /* voluntary fiber_yield calls */
    uint64_t      preemptions;         /* involuntary switches: slice ended (logical
                                          mode) or timer-signal preemption (real mode) */
    uint64_t      lock_hold_ticks;     /* R2 lock-hold time over completed holds */
    uint64_t      saved_stack_bytes;   /* STACK_SHARED: bytes currently saved, else 0 */
    uint64_t      max_saved_stack_bytes;
} fiber_stat_t;

/*
 * Mutex. The storage is opaque; your implementation may use up to 64
 * bytes of it, with 16-byte alignment. Always call fmutex_init first.
 */
typedef struct {
    _Alignas(16) unsigned char opaque[64];
} fmutex_t;

/* ------------------------------------------------------------------ */
/* Fibers                                                              */
/* ------------------------------------------------------------------ */

/*
 * Create a fiber that runs fn(arg). attr may be NULL for the defaults.
 * Returning from fn is the same as calling fiber_exit(NULL).
 * The new fiber is runnable but does not run until the scheduler picks it.
 * Returns the new id, or FIBER_INVALID on failure with errno set
 * (EINVAL for a bad attr, ENOMEM / EAGAIN when out of resources).
 */
fiber_t fiber_spawn(void (*fn)(void *), void *arg, const fiber_attr_t *attr);

/* Give up the CPU to the next runnable fiber (returns at once if none). */
void    fiber_yield(void);

/*
 * Wait for fiber f to finish and collect its return value (ret may be
 * NULL). Each fiber can be joined once.
 *   0        success
 *   ESRCH    no such fiber, or it was already joined
 *   EINVAL   another fiber is already joining f
 *   EDEADLK  f is the caller, or the join would close a wait cycle
 *            (through joins and mutex waits)
 * Checked in this order: unknown/joined (ESRCH), self (EDEADLK),
 * second joiner (EINVAL), cycle (EDEADLK).
 */
int     fiber_join(fiber_t f, void **ret);

/* End the calling fiber. Must not be called by the main fiber. */
void    fiber_exit(void *ret) __attribute__((noreturn));

/* Id of the calling fiber. */
fiber_t fiber_self(void);

/* ------------------------------------------------------------------ */
/* Clock and preemption                                                */
/* ------------------------------------------------------------------ */
/*
 * Clock mode is read from the environment on initialization:
 *   FIBER_CLOCK=logical (default)  time advances only by fiber_tick()
 *   FIBER_CLOCK=real               a CPU-time timer (CLOCK_PROCESS_CPUTIME_ID,
 *                                  SIGEV_SIGNAL) fires every FIBER_QUANTUM_US
 *                                  microseconds (default 1000); one tick = one
 *                                  timer period; fiber_tick() is a no-op.
 */

/*
 * Logical mode: advance the global tick count by one, charge the tick to
 * the calling fiber, and switch to the next runnable fiber if the
 * caller's slice is used up. Real mode: no-op.
 */
void    fiber_tick(void);

/*
 * Preempt-disabled regions. While disabled (calls nest), the calling
 * fiber is never preempted by the timer; a preemption that falls due is
 * taken at the matching fiber_preempt_enable. Voluntary switches (yield,
 * blocking calls) are still allowed inside the region.
 */
void    fiber_preempt_disable(void);
void    fiber_preempt_enable(void);

/* ------------------------------------------------------------------ */
/* Mutex (R2 policy)                                                   */
/* ------------------------------------------------------------------ */
/*
 * Non-recursive. On unlock with waiters, ownership passes directly to the
 * waiter with the greatest lock-hold time (ties: lowest fiber id); no
 * barging.
 *   fmutex_init      0
 *   fmutex_lock      0; EDEADLK if the caller holds it, or if waiting would
 *                    close a wait cycle of two or more fibers
 *   fmutex_trylock   0; EBUSY if held (by anyone, including the caller)
 *   fmutex_unlock    0; EPERM if the caller does not hold it
 *   fmutex_destroy   0; EBUSY if held or if fibers are waiting
 */
int     fmutex_init(fmutex_t *m);
int     fmutex_lock(fmutex_t *m);
int     fmutex_trylock(fmutex_t *m);
int     fmutex_unlock(fmutex_t *m);
int     fmutex_destroy(fmutex_t *m);

/* ------------------------------------------------------------------ */
/* I/O and introspection                                               */
/* ------------------------------------------------------------------ */

/*
 * read(2) that blocks only the calling fiber. fd is a pipe, eventfd or
 * timerfd. Returns like read(2): bytes read, 0 at end of file, or -1 with
 * errno set. Other fibers keep running while the caller waits.
 */
ssize_t fiber_read(int fd, void *buf, size_t n);

/*
 * Statistics for fiber f (a live or finished-but-unjoined fiber, or 0).
 * Returns 0, or ESRCH.
 */
int     fiber_stats(fiber_t f, fiber_stat_t *out);

/*
 * Stack overflow (S5): overflowing an own stack or the shared run stack
 * must abort the process (for example with abort()) after writing a
 * message to stderr that contains the text "fiber <id>", for example
 *     contextchaos: stack overflow in fiber 7
 */

#ifdef __cplusplus
}
#endif

#endif /* FIBER_H */
