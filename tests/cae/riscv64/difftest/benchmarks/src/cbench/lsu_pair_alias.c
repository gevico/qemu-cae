/* Address aliasing: alternating aliased and non-aliased store+load pairs. */

#include <stdint.h>

#define ITERS  80000
#define NSLOTS 16

static volatile uint64_t slots[NSLOTS];

void workload(void)
{
    for (int i = 0; i < ITERS; i++) {
        unsigned idx = (unsigned)i % NSLOTS;

        slots[idx] = (uint64_t)i;
        uint64_t v = slots[idx];

        unsigned alias_idx = (idx + (NSLOTS / 2)) % NSLOTS;
        slots[alias_idx] = v + 1;
        uint64_t w = slots[idx];

        slots[idx] = w ^ v;
        (void)slots[idx];
    }
}
