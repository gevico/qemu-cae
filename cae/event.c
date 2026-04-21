/*
 * CAE (Cycle Approximate Engine) - Event Queue
 *
 * Min-heap event queue using GTree, ordered by cycle.
 * Events scheduled in the past (cycle < current_cycle) are rejected.
 *
 * Copyright (c) 2024-2025 Chao Liu <chao.liu.zevorn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "cae/engine.h"

static gint cae_event_compare(gconstpointer a, gconstpointer b,
                              gpointer user_data)
{
    const CaeEvent *ea = a;
    const CaeEvent *eb = b;

    if (ea->cycle < eb->cycle) {
        return -1;
    } else if (ea->cycle > eb->cycle) {
        return 1;
    }
    /*
     * Tie-break on the monotonic schedule sequence number: FIFO for
     * same-cycle events, deterministic across toolchains. Pointer
     * comparison of unrelated objects is UB in C, so seq is required.
     */
    if (ea->seq < eb->seq) {
        return -1;
    } else if (ea->seq > eb->seq) {
        return 1;
    }
    return 0;
}

static void cae_event_queue_ensure_init(CaeEngine *engine)
{
    if (!engine->event_queue) {
        engine->event_queue = g_tree_new_full(cae_event_compare, NULL,
                                              NULL, NULL);
    }
}

bool cae_engine_schedule_event(CaeEngine *engine, CaeEvent *event)
{
    if (event->cycle < engine->current_cycle) {
        return false;
    }

    cae_event_queue_ensure_init(engine);
    event->seq = engine->next_event_seq++;
    g_tree_insert(engine->event_queue, event, event);
    return true;
}

/*
 * GLib 2.66-compatible helper: g_tree_foreach callback that captures
 * the first (minimum) key and returns TRUE to stop traversal.
 */
static gboolean cae_event_get_first(gpointer key, gpointer value,
                                    gpointer data)
{
    CaeEvent **out = data;
    *out = key;
    return TRUE; /* stop after first element */
}

CaeEvent *cae_engine_pop_event(CaeEngine *engine)
{
    CaeEvent *event = NULL;

    if (!engine->event_queue || g_tree_nnodes(engine->event_queue) == 0) {
        return NULL;
    }

    g_tree_foreach(engine->event_queue, cae_event_get_first, &event);
    if (event) {
        g_tree_remove(engine->event_queue, event);
    }
    return event;
}

void cae_engine_process_events(CaeEngine *engine, uint64_t until_cycle)
{
    while (engine->event_queue && g_tree_nnodes(engine->event_queue) > 0) {
        CaeEvent *event = NULL;

        g_tree_foreach(engine->event_queue, cae_event_get_first, &event);
        if (!event || event->cycle > until_cycle) {
            break;
        }

        g_tree_remove(engine->event_queue, event);
        /* Don't modify current_cycle here; advance_cycle already set it */
        if (event->handler) {
            event->handler(event->opaque);
        }
    }
}
