/* Partial-overlap store-to-load forwarding: store narrow, load wide. */

#include <stdint.h>

#define ITERS 100000

void workload(void)
{
    volatile uint64_t buf[4];
    volatile uint32_t *narrow = (volatile uint32_t *)buf;

    for (int i = 0; i < ITERS; i++) {
        narrow[0] = (uint32_t)i;
        narrow[1] = (uint32_t)(i + 1);
        uint64_t wide = buf[0];

        narrow[2] = (uint32_t)(wide & 0xFFFFFFFF);
        narrow[3] = (uint32_t)(wide >> 32);
        uint64_t wide2 = buf[1];

        buf[2] = wide + wide2;
        (void)buf[2];
    }
}
