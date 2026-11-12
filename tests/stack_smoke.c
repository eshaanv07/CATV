#include "internal.h"

#include <stdint.h>
#include <stdio.h>

int main(void)
{
    ccx_fiber_t fiber = {0};

    int err = ccx_stack_alloc(&fiber, 64 * 1024);

    if (err != 0) {
        printf("FAIL: stack allocation returned %d\n", err);
        return 1;
    }

    printf("Stack mapping : %p\n", fiber.stack_mapping);
    printf("Stack base    : %p\n", fiber.stack_base);
    printf("Stack size    : %zu bytes\n", fiber.stack_size);

    /*
     * Check that the usable region is writable.
     * Do not touch the guard page.
     */
    volatile unsigned char *base =
        (volatile unsigned char *)fiber.stack_base;

    volatile unsigned char *top =
        base + fiber.stack_size - 1;

    *base = 0xAA;
    *top = 0xBB;

    if (*base != 0xAA || *top != 0xBB) {
        printf("FAIL: stack memory check\n");
        ccx_stack_free(&fiber);
        return 1;
    }

    printf("PASS: usable stack is writable\n");

    ccx_stack_free(&fiber);

    if (fiber.stack_mapping != NULL) {
        printf("FAIL: stack mapping was not cleared\n");
        return 1;
    }

    printf("PASS: stack was released\n");

    return 0;
}