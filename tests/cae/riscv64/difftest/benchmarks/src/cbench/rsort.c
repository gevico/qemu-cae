/* Radix sort (LSD, base-256): streaming memory, minimal branches. */

#include <stdint.h>

#define N 512

static uint32_t data[N];
static uint32_t temp[N];

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void radix_sort(uint32_t *arr, uint32_t *buf, int n)
{
    for (int shift = 0; shift < 32; shift += 8) {
        uint32_t count[256];
        for (int i = 0; i < 256; i++)
            count[i] = 0;

        for (int i = 0; i < n; i++)
            count[(arr[i] >> shift) & 0xFF]++;

        uint32_t prefix = 0;
        for (int i = 0; i < 256; i++) {
            uint32_t c = count[i];
            count[i] = prefix;
            prefix += c;
        }

        for (int i = 0; i < n; i++) {
            uint32_t digit = (arr[i] >> shift) & 0xFF;
            buf[count[digit]] = arr[i];
            count[digit]++;
        }

        uint32_t *t = arr;
        arr = buf;
        buf = t;
    }
}

void workload(void)
{
    uint32_t rng = 0x12345678;

    for (int pass = 0; pass < 10; pass++) {
        for (int i = 0; i < N; i++)
            data[i] = xorshift32(&rng);
        radix_sort(data, temp, N);
    }
}
