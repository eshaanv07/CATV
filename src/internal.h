#ifndef CCX_INTERNAL_H
#define CCX_INTERNAL_H

#include "fiber.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Cooperative context.
 *
 * The assembly switch routine will save and restore:
 *   rsp, rbx, rbp, r12, r13, r14, r15
 *
 * We will implement that routine in the next milestone.
 */
typedef struct {
    uint64_t rsp;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
} ccx_context_t;

/* Internal representation of one fiber. */
typedef struct ccx_fiber {
    fiber_t id;

    ccx_context_t context;

    /* Stack information. */
    void *stack_mapping;
    size_t mapping_size;

    void *stack_base;
    size_t stack_size;

    /* Function to execute. */
    void (*start_routine)(void *);
    void *arg;
    void *retval;

    /* Scheduling information. */
    fiber_state_t state;
    int priority;

    uint64_t ticks;
    uint64_t switches_in;
    uint64_t yields;
    uint64_t preemptions;

    /* Scheduler bookkeeping. */
    uint64_t remaining_slice;

    /* Reserved for later milestones. */
    int preempt_disable_depth;
    int preempt_pending;
} ccx_fiber_t;

/* Global runtime state. */
typedef struct {
    ccx_fiber_t **fibers;

    size_t count;
    size_t capacity;

    ccx_fiber_t *current;

    uint64_t global_ticks;

    int initialized;
} ccx_runtime_t;

/* Runtime functions shared between implementation files. */
int ccx_runtime_init(void);
int ccx_reserve_fibers(size_t needed);

extern ccx_runtime_t ccx_runtime;
/* Own-stack memory management. */
int ccx_stack_alloc(ccx_fiber_t *fiber, size_t requested_size);
void ccx_stack_free(ccx_fiber_t *fiber);

#endif