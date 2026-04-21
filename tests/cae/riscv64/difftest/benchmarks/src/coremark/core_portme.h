/*
 * CAE bare-metal porting layer for CoreMark.
 *
 * Targets qemu-system-riscv64 -accel cae and XiangShan GEM5.
 * No stdio, no malloc, no OS dependencies.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stdint.h>
#include <stddef.h>

#define HAS_FLOAT      0
#define HAS_TIME_H     0
#define HAS_STDIO      0
#define HAS_PRINTF     0

#define SEED_METHOD    SEED_VOLATILE
#define MEM_METHOD     MEM_STACK

#define MAIN_HAS_NOARGC   1
#define MAIN_HAS_NORETURN 0

#ifndef ITERATIONS
#define ITERATIONS     1
#endif

#ifndef TOTAL_DATA_SIZE
#define TOTAL_DATA_SIZE 50
#endif

#ifndef PERFORMANCE_RUN
#define PERFORMANCE_RUN 1
#endif

#ifndef FLAGS_STR
#define FLAGS_STR      "rv64gc -Os -accel cae"
#endif

#ifndef COMPILER_VERSION
#define COMPILER_VERSION "GCC"__VERSION__
#endif

#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS FLAGS_STR
#endif

#ifndef MEM_LOCATION
#define MEM_LOCATION "STACK"
#endif

#define MULTITHREAD 1
#define USE_PTHREAD 0

typedef int32_t  ee_s32;
typedef uint32_t ee_u32;
typedef int16_t  ee_s16;
typedef uint16_t ee_u16;
typedef int8_t   ee_s8;
typedef uint8_t  ee_u8;
typedef ee_u32   ee_ptr_int;
typedef size_t   ee_size_t;

typedef uint64_t CORE_TICKS;
#define NSECS_PER_SEC  1000000000ULL
#define GETMYTIME(_t)  (*_t = read_mcycle())
#define MYTIMEDIFF(fin, ini) ((fin) - (ini))
#define TIMER_RES_DIVIDER 1
#define SAMPLE_TIME_IMPLEMENTATION 1
#define EE_TICKS_PER_SEC 1000000000ULL

static inline uint64_t read_mcycle(void)
{
#ifdef GEM5_SE
    return 0;
#else
    uint64_t val;
    __asm__ volatile("csrr %0, mcycle" : "=r"(val));
    return val;
#endif
}

int ee_printf(const char *fmt, ...);

void uart_send_char(char c);

typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

extern ee_u32 default_num_contexts;

#endif /* CORE_PORTME_H */
