/*
 * CAE Memory Access Hook - Lightweight inline hook for softmmu
 *
 * Called from cputlb.c load/store helpers to notify CAE of
 * memory accesses for timing purposes.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 */

#ifndef CAE_MEM_HOOK_H
#define CAE_MEM_HOOK_H

#include <stdbool.h>
#include <stdint.h>

/* Access type matching CaeMemOp but defined here to avoid heavy includes */
#define CAE_MEM_HOOK_READ      0
#define CAE_MEM_HOOK_WRITE     1
#define CAE_MEM_HOOK_FETCH     2
#define CAE_MEM_HOOK_TLB_MISS  3

extern bool cae_allowed;
extern unsigned cae_tbs_started;
extern unsigned cae_tbs_charged;

extern bool cae_tlb_force_slow_active;

bool cae_tlb_gate_default_for_cpu_model(const char *name);

void cae_mem_access_notify(void *cpu, uint64_t addr,
                           uint32_t size, int op,
                           const void *value);

static inline void cae_mem_hook(void *cpu, uint64_t addr,
                                uint32_t size, int op,
                                const void *value)
{
    if (cae_allowed) {
        cae_mem_access_notify(cpu, addr, size, op, value);
    }
}

static inline void cae_tlb_miss_hook(void *cpu, uint64_t addr)
{
    if (cae_allowed) {
        cae_mem_access_notify(cpu, addr, 0, CAE_MEM_HOOK_TLB_MISS, NULL);
    }
}

void cae_charge_executed_tb(void);

#endif /* CAE_MEM_HOOK_H */
