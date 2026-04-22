/* Address aliasing: alternating aliased and non-aliased store+load pairs.
 *
 * buf_a and buf_b are separated by exactly 4096 bytes so their
 * page offsets match — stores to buf_a[k] and loads from buf_b[k]
 * exercise the store-disambiguation "same page-offset" path.
 * buf_c is at a distinct page offset for the non-aliased baseline.
 */

#include <stdint.h>

#define PAGE_SIZE 4096
#define NELEM    64
#define ITERS    1000

static volatile uint64_t
    buf_a[NELEM]
    __attribute__((aligned(PAGE_SIZE)));

static volatile uint64_t
    buf_b[NELEM]
    __attribute__((aligned(PAGE_SIZE)));

static volatile uint64_t buf_c[NELEM];

void workload(void)
{
    for (int i = 0; i < ITERS; i++) {
        for (int k = 0; k < NELEM; k++) {
            buf_a[k] = (uint64_t)(i + k);
            uint64_t alias_load = buf_b[k];

            buf_c[k] = alias_load + (uint64_t)k;
            uint64_t no_alias_load = buf_a[k];

            buf_b[k] = no_alias_load ^ buf_c[k];
        }
    }
}
