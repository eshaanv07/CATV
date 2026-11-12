#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CCX_INITIAL_CAPACITY 16

ccx_runtime_t ccx_runtime = {0};

/*
 * Grow the fiber pointer array when necessary.
 *
 * Fiber IDs are never reused, so the ID of a fiber
 * will also be its index in this array.
 */
int ccx_reserve_fibers(size_t needed)
{
    if (needed <= ccx_runtime.capacity)
        return 0;

    size_t new_capacity = ccx_runtime.capacity;

    if (new_capacity == 0)
        new_capacity = CCX_INITIAL_CAPACITY;

    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2)
            return ENOMEM;

        new_capacity *= 2;
    }

    if (new_capacity > SIZE_MAX / sizeof(ccx_fiber_t *))
        return ENOMEM;

    ccx_fiber_t **new_array =
        realloc(ccx_runtime.fibers,
                new_capacity * sizeof(ccx_fiber_t *));

    if (new_array == NULL)
        return ENOMEM;

    /* Initialize newly allocated entries. */
    memset(new_array + ccx_runtime.capacity,
           0,
           (new_capacity - ccx_runtime.capacity)
               * sizeof(ccx_fiber_t *));

    ccx_runtime.fibers = new_array;
    ccx_runtime.capacity = new_capacity;

    return 0;
}

/*
 * Initialize the runtime lazily.
 *
 * The thread executing main() becomes fiber 0.
 * It continues using the process's existing stack.
 */
int ccx_runtime_init(void)
{
    if (ccx_runtime.initialized)
        return 0;

    ccx_fiber_t *main_fiber = calloc(1, sizeof(*main_fiber));

    if (main_fiber == NULL)
        return ENOMEM;

    int err = ccx_reserve_fibers(1);

    if (err != 0) {
        free(main_fiber);
        return err;
    }

    main_fiber->id = FIBER_MAIN;
    main_fiber->state = FIBER_RUNNABLE;
    main_fiber->priority = 0;

    /*
     * The main fiber does not own an allocated stack.
     * Its execution continues on the original process stack.
     */

    ccx_runtime.fibers[0] = main_fiber;
    ccx_runtime.count = 1;
    ccx_runtime.current = main_fiber;
    ccx_runtime.global_ticks = 0;
    ccx_runtime.initialized = 1;

    return 0;
}

/*
 * Public API: return the ID of the currently executing fiber.
 */
fiber_t fiber_self(void)
{
    int err = ccx_runtime_init();

    /*
     * fiber_self() has no error return channel.
     * If even the initial runtime allocation fails,
     * continuing would leave the runtime unusable.
     */
    if (err != 0)
        abort();

    return ccx_runtime.current->id;
}