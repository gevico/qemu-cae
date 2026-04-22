/* Strided memory access: stride-16 walk over a 32 KiB array. */

#include <stdint.h>

#define ARRAY_SIZE  (32768 / sizeof(uint64_t))
#define STRIDE      16
#define OUTER_ITERS 200

static volatile uint64_t array[ARRAY_SIZE];

void workload(void)
{
    for (int iter = 0; iter < OUTER_ITERS; iter++) {
        for (unsigned i = 0; i < ARRAY_SIZE; i += STRIDE) {
            array[i] = array[i] + 1;
        }
    }
}
