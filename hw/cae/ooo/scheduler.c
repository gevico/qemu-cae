/*
 * CAE Out-of-Order KMHV3Scheduler.
 *
 * Round 48 (AC-K-5): segmented issue queue modeling the KMH-V3
 * 3-segment × 2-port issue stage. The scheduler owns three
 * independent FIFO rings (one per segment); enqueue picks the
 * segment from the uop's FU class, issue_cycle drains up to 2
 * entries round-robin across segments. The segmented-issue
 * accounting is separate from the M3' flat-IQ allocation tracking
 * in `iq.c`; cpu_ooo.c keeps charging per-retired-uop latency
 * through the default-latency table while scheduler.c models the
 * structural issue pressure as a distinct counter surface.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/ooo.h"

void cae_ooo_scheduler_init(CaeOooScheduler *s)
{
    memset(s, 0, sizeof(*s));
    for (uint32_t i = 0u; i < CAE_OOO_SCHED_SEGMENTS; i++) {
        s->segments[i].capacity = CAE_OOO_SCHED_SEGMENT_CAPACITY;
    }
}

void cae_ooo_scheduler_reset(CaeOooScheduler *s)
{
    memset(s, 0, sizeof(*s));
    for (uint32_t i = 0u; i < CAE_OOO_SCHED_SEGMENTS; i++) {
        s->segments[i].capacity = CAE_OOO_SCHED_SEGMENT_CAPACITY;
    }
}

/*
 * Segment mapping:
 *   0 = INT/ALU  (general, branch, system, fence, atomic)
 *   1 = MEM      (load, store)
 *   2 = FPU/MUL/DIV
 *
 * Matches the plan's 3-segment grouping; the FU-type enum is
 * arch-neutral so this mapping stays stable across target
 * extensions.
 */
uint8_t cae_ooo_scheduler_segment_for(uint8_t fu_type)
{
    switch (fu_type) {
    case CAE_FU_LOAD:
    case CAE_FU_STORE:
        return 1u;
    case CAE_FU_FPU:
    case CAE_FU_MUL:
    case CAE_FU_DIV:
        return 2u;
    case CAE_FU_ALU:
    case CAE_FU_BRANCH:
    case CAE_FU_SYSTEM:
    case CAE_FU_NONE:
    default:
        return 0u;
    }
}

bool cae_ooo_scheduler_enqueue(CaeOooScheduler *s, uint64_t pc,
                               uint8_t fu_type)
{
    uint8_t seg_idx = cae_ooo_scheduler_segment_for(fu_type);
    CaeOooSchedSegment *seg = &s->segments[seg_idx];

    if (seg->count >= seg->capacity) {
        s->backpressure++;
        return false;
    }
    CaeOooSchedEntry *e = &seg->slots[seg->tail];
    e->pc = pc;
    e->fu_type = fu_type;
    e->segment = seg_idx;
    e->valid = 1u;
    seg->tail = (seg->tail + 1u) % seg->capacity;
    seg->count++;
    s->enqueued++;
    return true;
}

uint8_t cae_ooo_scheduler_issue_cycle(CaeOooScheduler *s,
                                      CaeOooSchedEntry *out)
{
    return cae_ooo_scheduler_issue_cycle_bounded(s, 0u, out);
}

uint8_t cae_ooo_scheduler_issue_cycle_bounded(CaeOooScheduler *s,
                                              uint8_t max_issue,
                                              CaeOooSchedEntry *out)
{
    uint8_t issued = 0u;
    uint8_t cap = max_issue == 0u ? CAE_OOO_SCHED_PORTS : max_issue;
    if (cap > CAE_OOO_SCHED_PORTS_MAX) {
        cap = CAE_OOO_SCHED_PORTS_MAX;
    }

    for (uint32_t probe = 0u;
         probe < CAE_OOO_SCHED_SEGMENTS &&
             issued < cap;
         probe++) {
        uint32_t seg_idx = (s->next_segment + probe) %
                            CAE_OOO_SCHED_SEGMENTS;
        CaeOooSchedSegment *seg = &s->segments[seg_idx];
        if (seg->count == 0u) {
            continue;
        }
        CaeOooSchedEntry *src = &seg->slots[seg->head];
        if (out) {
            out[issued] = *src;
        }
        memset(src, 0, sizeof(*src));
        seg->head = (seg->head + 1u) % seg->capacity;
        seg->count--;
        issued++;
    }

    /*
     * Round-robin advance: if we issued anything, move the head
     * forward by ports so the next cycle starts from a different
     * segment. Keeps the segmented issue balanced across FU
     * classes under sustained pressure.
     */
    if (issued > 0u) {
        /*
         * Round 50 AC-K-5: advance round-robin by issued count so
         * the pointer tracks actual issue pressure rather than the
         * structural port count. Avoids starving the same segment
         * when `max_issue` < CAE_OOO_SCHED_PORTS.
         */
        s->next_segment = (s->next_segment + issued) %
                          CAE_OOO_SCHED_SEGMENTS;
        s->issued += issued;
    }
    return issued;
}
