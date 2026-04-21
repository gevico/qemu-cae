/*
 * CAE Out-of-Order IQ (Issue Queue).
 *
 * M3' flat unified IQ. Tracks occupancy + issue_width only; the M5'
 * refactor (t-scheduler-kmhv3) replaces this with a 3-segment IQ x
 * 2-port scheduler matching the kmhv3.py baseline.
 *
 * Copyright (c) 2024-2026 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/ooo.h"

void cae_ooo_iq_init(CaeOooIq *iq, uint32_t size, uint32_t issue_width)
{
    iq->size = size ? size : 64u;
    iq->count = 0u;
    iq->issue_width = issue_width ? issue_width : CAE_OOO_DEFAULT_ISSUE_WIDTH;
}

bool cae_ooo_iq_has_slot(const CaeOooIq *iq)
{
    return iq->count < iq->size;
}

void cae_ooo_iq_enqueue(CaeOooIq *iq)
{
    if (iq->count < iq->size) {
        iq->count++;
    }
}

uint32_t cae_ooo_iq_try_issue(CaeOooIq *iq, uint32_t ready_uops,
                              uint32_t now_cycle)
{
    uint32_t issued = ready_uops;

    (void)now_cycle;
    if (issued > iq->issue_width) {
        issued = iq->issue_width;
    }
    if (issued > iq->count) {
        issued = iq->count;
    }
    iq->count -= issued;
    return issued;
}

void cae_ooo_iq_flush(CaeOooIq *iq)
{
    iq->count = 0u;
}

/* ------------------------------------------------------------------ */
/*  Speculation save/restore                                          */
/* ------------------------------------------------------------------ */

struct CaeOooIqSpecSnapshot {
    uint32_t size;
    uint32_t count;
    uint32_t issue_width;
};

CaeOooIqSpecSnapshot *cae_ooo_iq_spec_snapshot_save(const CaeOooIq *src)
{
    if (src == NULL) {
        return NULL;
    }
    CaeOooIqSpecSnapshot *snap = g_new0(CaeOooIqSpecSnapshot, 1);
    snap->size = src->size;
    snap->count = src->count;
    snap->issue_width = src->issue_width;
    return snap;
}

void cae_ooo_iq_spec_snapshot_restore(CaeOooIq *dst,
                                      const CaeOooIqSpecSnapshot *snap)
{
    if (dst == NULL || snap == NULL) {
        return;
    }
    dst->size = snap->size;
    dst->count = snap->count;
    dst->issue_width = snap->issue_width;
}

void cae_ooo_iq_spec_snapshot_drop(CaeOooIqSpecSnapshot *snap)
{
    g_free(snap);
}
