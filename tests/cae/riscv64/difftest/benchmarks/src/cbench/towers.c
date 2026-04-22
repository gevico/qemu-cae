/* Tower of Hanoi: recursive call-intensive, exercises the return address stack. */

#include <stdint.h>

#define NUM_DISCS 15

static volatile uint32_t move_count;

static void hanoi(int n, int from, int to, int aux)
{
    if (n == 0)
        return;
    hanoi(n - 1, from, aux, to);
    move_count++;
    hanoi(n - 1, aux, to, from);
}

void workload(void)
{
    for (int pass = 0; pass < 4; pass++) {
        move_count = 0;
        hanoi(NUM_DISCS, 0, 2, 1);
    }
}
