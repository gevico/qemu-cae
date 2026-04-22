/* Memory-level parallelism: 8 independent load streams in parallel. */

#include <stdint.h>

#define STREAM_LEN 512
#define OUTER_ITERS 100

static volatile uint64_t streams[8][STREAM_LEN];

void workload(void)
{
    for (int iter = 0; iter < OUTER_ITERS; iter++) {
        for (int j = 0; j < STREAM_LEN; j++) {
            uint64_t s0 = streams[0][j];
            uint64_t s1 = streams[1][j];
            uint64_t s2 = streams[2][j];
            uint64_t s3 = streams[3][j];
            uint64_t s4 = streams[4][j];
            uint64_t s5 = streams[5][j];
            uint64_t s6 = streams[6][j];
            uint64_t s7 = streams[7][j];
            streams[0][j] = s0 + s4;
            streams[1][j] = s1 + s5;
            streams[2][j] = s2 + s6;
            streams[3][j] = s3 + s7;
        }
    }
}
