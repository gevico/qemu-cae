/* Sparse matrix-vector multiply (CSR format): irregular indirect access. */

#include <stdint.h>

#define NROWS   64
#define NNZ     384
#define NITER   50

static uint32_t row_ptr[NROWS + 1];
static uint32_t col_idx[NNZ];
static int32_t  values[NNZ];
static int32_t  x_vec[NROWS];
static int32_t  y_vec[NROWS];

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t s = *state;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *state = s;
    return s;
}

static void init_sparse(void)
{
    uint32_t rng = 0xCAFEBABE;
    unsigned nnz_per_row = NNZ / NROWS;

    for (int r = 0; r <= NROWS; r++)
        row_ptr[r] = (uint32_t)(r * nnz_per_row);

    for (int i = 0; i < NNZ; i++) {
        col_idx[i] = xorshift32(&rng) % NROWS;
        values[i] = (int32_t)(xorshift32(&rng) & 0xFF) - 128;
    }

    for (int i = 0; i < NROWS; i++)
        x_vec[i] = (int32_t)(i + 1);
}

void workload(void)
{
    init_sparse();

    for (int iter = 0; iter < NITER; iter++) {
        for (int r = 0; r < NROWS; r++) {
            int32_t sum = 0;
            for (uint32_t j = row_ptr[r]; j < row_ptr[r + 1]; j++)
                sum += values[j] * x_vec[col_idx[j]];
            y_vec[r] = sum;
        }
        for (int i = 0; i < NROWS; i++)
            x_vec[i] = y_vec[i] >> 4;
    }
}
