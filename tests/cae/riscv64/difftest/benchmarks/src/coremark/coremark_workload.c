/*
 * CAE CoreMark workload wrapper.
 *
 * Exports the `workload` symbol called by bench.S. Internally
 * calls CoreMark's main() which runs ITERATIONS iterations of
 * the four kernels (list, matrix, state, util).
 */

/* CoreMark's main is renamed to avoid collision with _start */
extern int main(void);

void workload(void)
{
    main();
}
