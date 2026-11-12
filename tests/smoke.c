#include "fiber.h"

#include <stdio.h>

int main(void)
{
    fiber_t id = fiber_self();

    printf("Current fiber ID: %ld\n", (long)id);

    if (id != FIBER_MAIN) {
        fprintf(stderr, "FAIL: main fiber should have ID 0\n");
        return 1;
    }

    printf("PASS: runtime initialized correctly\n");

    return 0;
}