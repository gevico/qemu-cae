/*
 * CAE bare-metal porting layer for CoreMark — implementation.
 */
#include "coremark.h"

void *memset(void *s, int c, unsigned long n)
{
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

ee_u32 default_num_contexts = 1;

volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

ee_u32 align_mem(ee_u32 val)
{
    return (val + 3) & ~(ee_u32)3;
}

static CORE_TICKS t_start, t_end;

void start_time(void)
{
    GETMYTIME(&t_start);
}

void stop_time(void)
{
    GETMYTIME(&t_end);
}

CORE_TICKS get_time(void)
{
    return MYTIMEDIFF(t_end, t_start);
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
    return (secs_ret)ticks / (secs_ret)EE_TICKS_PER_SEC;
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
    (void)p;
    (void)argc;
    (void)argv;
}

void portable_fini(core_portable *p)
{
    (void)p;
}
