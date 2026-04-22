/* Clean-room quicksort: unpredictable branches + pointer-intensive access. */

#include <stdint.h>

#define N 256

static uint32_t data[N];

static void swap(uint32_t *a, uint32_t *b)
{
    uint32_t t = *a;
    *a = *b;
    *b = t;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void quicksort(uint32_t *arr, int lo, int hi)
{
    if (lo >= hi)
        return;

    uint32_t pivot = arr[hi];
    int i = lo;

    for (int j = lo; j < hi; j++) {
        if (arr[j] <= pivot) {
            swap(&arr[i], &arr[j]);
            i++;
        }
    }
    swap(&arr[i], &arr[hi]);

    quicksort(arr, lo, i - 1);
    quicksort(arr, i + 1, hi);
}

void workload(void)
{
    uint32_t rng = 0xDEADBEEF;

    for (int pass = 0; pass < 20; pass++) {
        for (int i = 0; i < N; i++)
            data[i] = xorshift32(&rng);
        quicksort(data, 0, N - 1);
    }
}
