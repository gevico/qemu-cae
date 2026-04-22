/* Store-to-load forwarding: consecutive store+load to the same address. */

#include <stdint.h>

#define ITERS 100000

void workload(void)
{
    volatile uint64_t buf[4];

    for (int i = 0; i < ITERS; i++) {
        buf[0] = (uint64_t)i;
        uint64_t v = buf[0];
        buf[1] = v + 1;
        uint64_t w = buf[1];
        buf[2] = w + 1;
        (void)buf[2];
    }
}
