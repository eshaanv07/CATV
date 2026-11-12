#include "internal.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#define CCX_DEFAULT_STACK_SIZE (64U * 1024U)

int ccx_stack_alloc(ccx_fiber_t *fiber, size_t requested_size)
{
    if (fiber == NULL)
        return EINVAL;

    if (requested_size == 0)
        requested_size = CCX_DEFAULT_STACK_SIZE;

    long page_result = sysconf(_SC_PAGESIZE);

    if (page_result <= 0)
        return ENOMEM;

    size_t page_size = (size_t)page_result;

    /*
     * Round usable stack size up to a whole number of pages.
     * Check arithmetic before adding page_size - 1.
     */
    if (requested_size > SIZE_MAX - (page_size - 1))
        return ENOMEM;

    size_t usable_size =
        ((requested_size + page_size - 1) / page_size) * page_size;

    /* One additional page is reserved as the guard page. */
    if (usable_size > SIZE_MAX - page_size)
        return ENOMEM;

    size_t total_size = usable_size + page_size;

    /*
     * Initially map the entire region as inaccessible.
     * This makes the guard page inaccessible by default.
     */
    void *mapping = mmap(
        NULL,
        total_size,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (mapping == MAP_FAILED)
        return ENOMEM;

    /*
     * The usable stack starts immediately after the guard page.
     */
    void *usable_base = (char *)mapping + page_size;

    if (mprotect(usable_base, usable_size, PROT_READ | PROT_WRITE) != 0) {
        munmap(mapping, total_size);
        return ENOMEM;
    }

    fiber->stack_mapping = mapping;
    fiber->mapping_size = total_size;
    fiber->stack_base = usable_base;
    fiber->stack_size = usable_size;

    return 0;
}

void ccx_stack_free(ccx_fiber_t *fiber)
{
    if (fiber == NULL || fiber->stack_mapping == NULL)
        return;

    munmap(fiber->stack_mapping, fiber->mapping_size);

    fiber->stack_mapping = NULL;
    fiber->mapping_size = 0;
    fiber->stack_base = NULL;
    fiber->stack_size = 0;
}