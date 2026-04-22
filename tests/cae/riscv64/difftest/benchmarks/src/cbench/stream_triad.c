/* STREAM Triad: a[i] = b[i] + s * c[i], 3-way streaming bandwidth. */

#include <stdint.h>

#define ARRAY_SIZE 256
#define OUTER_ITERS 22

static double a[ARRAY_SIZE];
static double b[ARRAY_SIZE];
static double c[ARRAY_SIZE];

void workload(void)
{
    for (int i = 0; i < ARRAY_SIZE; i++) {
        b[i] = (double)(i + 1);
        c[i] = (double)(i * 2 + 1);
    }

    double scalar = 3.0;

    for (int iter = 0; iter < OUTER_ITERS; iter++) {
        for (int i = 0; i < ARRAY_SIZE; i++)
            a[i] = b[i] + scalar * c[i];

        scalar = a[ARRAY_SIZE / 2];
        if (scalar == 0.0)
            scalar = 1.0;
    }
}
