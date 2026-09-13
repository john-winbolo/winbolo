/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Events
 *Filename:      scenario_events.c
 *Author:        John Morrison
 *Purpose:
 *  The queue a scenario's events wait in and the bounded
 *  drain that empties it. The two are one piece of work:
 *  what makes the drain safe is what the queue refuses to do
 *  when it is written to.
 *
 *  Both of the server's channels come in here, in the order
 *  they were published, and one drain takes them back out in
 *  that order.
 *
 *  Nothing here calls Lua, issues an op or publishes. Both
 *  write sides run from inside the work that raised the
 *  event, where the sim asserts that a subscriber does none
 *  of those; the read side is the tick's, and what it does
 *  with an entry is its caller's business.
 *
 *  This file compiles under the scenario_host profile: it
 *  sees src/bolo/public/ and src/bolo/scenario_api/, and
 *  nothing under src/bolo/internal/.
 *********************************************************/

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "platform_types.h"   /* BOLO_STATIC_ASSERT */
#include "wire_limits.h"      /* PACKET_MAX_CHAT_MESSAGE */
#include "scenario_defs.h"    /* SCN_NONE, held against the local spelling */

#include "scenario_events.h"

/* ── What the width was measured against ──────────────────────────── */

/* The game channel. A game event's eight data bytes are kept whole; a type
 * that grew a wider data[] than the queue carries would be cut short on its
 * way in, so it fails here instead. */
BOLO_STATIC_ASSERT(SCN_EVENT_DATA_MAX >= (int)sizeof(((GameEvent *)0)->data),
                   queued_event_keeps_a_whole_game_event_payload);

/* The control channel, from both ends. The copy takes SCN_EVENT_DATA_MAX
 * bytes off the front of the variant union, so the union has to be at least
 * that wide or the copy reads past it. */
BOLO_STATIC_ASSERT(SCN_EVENT_DATA_MAX <= (int)sizeof(((ControlEvent *)0)->u),
                   queued_event_copy_stays_inside_the_control_union);

/* And it has to reach the far side of the two variants a hook reads deepest
 * into: a chat line's text, and a lobby slot's team number, which sits
 * behind a whole player name. Written as offsets rather than as numbers so
 * either variant growing moves the check with it. */
BOLO_STATIC_ASSERT(
    SCN_EVENT_DATA_MAX >= (int)(offsetof(ControlEvent, u.chat.body) -
                                offsetof(ControlEvent, u) +
                                PACKET_MAX_CHAT_MESSAGE),
    queued_event_carries_a_whole_chat_line);

BOLO_STATIC_ASSERT(
    SCN_EVENT_DATA_MAX >= (int)sizeof(((ControlEvent *)0)->u.lobbySlot),
    queued_event_carries_a_whole_lobby_slot);

/* An entry's type is one byte on either channel. */
BOLO_STATIC_ASSERT((int)CTRL_EVENT_TYPE_COUNT <= 256,
                   control_event_type_fits_a_queued_entry);

/* scenario_events.h writes the "nobody" byte out rather than reaching for
 * the funnel's header. This is where the two are visible together. */
BOLO_STATIC_ASSERT(SCN_EVENT_ACTOR_NONE == SCN_NONE,
                   queued_event_actor_none_matches_the_funnel);

/* ── Taking one in ────────────────────────────────────────────────── */

/* The one write. Both channels reach it, and neither does anything else:
 * this runs inside the publish or the mutation that raised the event, with
 * the sim's state half-written, so the only thing it can safely do is
 * copy.
 *
 * len is what the channel has to give; anything past it is zero, so a
 * reader that knows the type knows where the bytes stop. */
static void scnEventPush(ScnEventQueue *q, uint8_t channel, uint8_t type,
                         const void *src, size_t len) {
    ScnQueuedEvent *slot;

    if (q->count >= SCN_EVENT_QUEUE_MAX) {
        /* Counted here and said later: there is nothing safe to say from
           inside the work that raised this. */
        q->dropped++;
        return;
    }
    if (len > SCN_EVENT_DATA_MAX) {
        len = SCN_EVENT_DATA_MAX;
    }

    slot = &q->entries[(q->head + q->count) % SCN_EVENT_QUEUE_MAX];
    slot->type    = type;
    slot->channel = channel;
    slot->actor   = SCN_EVENT_ACTOR_NONE;
    memcpy(slot->data, src, len);
    if (len < SCN_EVENT_DATA_MAX) {
        memset(slot->data + len, 0, SCN_EVENT_DATA_MAX - len);
    }

    q->count++;
    q->queued++;
}

void scenarioEventsReset(ScnEventQueue *q) {
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
}

void scenarioEventsQueueGame(ScnEventQueue *q, const GameEvent *evt) {
    if (q == NULL || evt == NULL) {
        return;
    }
    scnEventPush(q, SCN_EVENT_CHANNEL_GAME, evt->type, evt->data,
                 sizeof(evt->data));
}

void scenarioEventsQueueControl(ScnEventQueue *q, const ControlEvent *evt) {
    if (q == NULL || evt == NULL) {
        return;
    }
    /* The front of the variant, whichever variant it is. Every control
       event puts the slot it is about at or near the front, and reading
       which one this is belongs to whoever consumes it. */
    scnEventPush(q, SCN_EVENT_CHANNEL_CONTROL, (uint8_t)evt->type, &evt->u,
                 SCN_EVENT_DATA_MAX);
}

/* ── Taking them back out ─────────────────────────────────────────── */

uint16_t scenarioEventsWaiting(const ScnEventQueue *q) {
    return (q != NULL) ? q->count : (uint16_t)0;
}

uint16_t scenarioEventsDrain(ScnEventQueue *q, ScnEventConsumeFn consume,
                             void *ctx) {
    uint16_t run;
    uint16_t i;

    if (q == NULL) {
        return 0;
    }

    /* Read once. Everything below works off this number and never off
       q->count, so an event that arrives while the drain is running is
       still there when it finishes. */
    run = q->count;

    for (i = 0; i < run; i++) {
        /* Taken off the queue before it is consumed, and by value. The
           entry it was sitting in is free from this point, so a consume
           that queues an event of its own can be handed that same slot
           without writing over the one being read. */
        ScnQueuedEvent e = q->entries[q->head];

        q->head = (uint16_t)((q->head + 1) % SCN_EVENT_QUEUE_MAX);
        q->count--;
        q->drained++;

        if (consume != NULL) {
            consume(ctx, &e);
        }
    }
    return run;
}

uint32_t scenarioEventsTakeDropped(ScnEventQueue *q) {
    uint32_t fresh;

    if (q == NULL) {
        return 0;
    }
    fresh       = q->dropped - q->reported;
    q->reported = q->dropped;
    return fresh;
}
