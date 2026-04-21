/*
 * CAE AccelOps internal declarations
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 */

#ifndef CAE_ACCEL_OPS_H
#define CAE_ACCEL_OPS_H

#include "exec/cpu-common.h"

void cae_start_vcpu_thread(CPUState *cpu);
void cae_kick_vcpu_thread(CPUState *cpu);
int cae_cpu_exec(CPUState *cpu);
void cae_cpu_destroy(CPUState *cpu);

#endif /* CAE_ACCEL_OPS_H */
