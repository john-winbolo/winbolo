/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Events
 *Filename:      scenario_events.h
 *Author:        John Morrison
 *Purpose:
 *  The fixed queue a scenario's events wait in, and the
 *  bounded drain that empties it.
 *
 *  The server has two event channels and the host listens to
 *  both. A ControlEvent reaches every subscriber through
 *  serverSimPublishControl; a GameEvent reaches the ones that
 *  asked for the second channel through serverSimAddEvent.
 *  Both arrive from inside the work that raised them, and the
 *  sim asserts that a subscriber neither publishes nor raises
 *  an event from either callback. So both of the host's
 *  callbacks copy into this one queue and return, and
 *  everything that acts on an event happens later, from the
 *  tick callback, where the state is settled and a publish is
 *  legal.
 *
 *  One queue and one drain, so a round's facts are consumed
 *  in the order they happened whichever channel carried each
 *  of them. A fact is published once and arrives once.
 *
 *  Whoever raised the event holds the sim mutex and so does
 *  the tick that drains, so the queue needs no lock of its
 *  own.
 *
 *  This is the library's header and the tests', not a
 *  frontend's, as scenario_lua.h is: scenario_static
 *  publishes its own directory to whatever links it.
 *********************************************************/

#ifndef SCENARIO_EVENTS_H
#define SCENARIO_EVENTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input_packet.h"   /* GameEvent */
#include "control_event.h"  /* ControlEvent */

#include "scenario_host.h"  /* ScenarioHost, SCN_EVENT_QUEUE_MAX */

/* Which of the server's two channels an entry came in on. This says only
 * that: whether the fact was published as a ControlEvent or raised as a
 * GameEvent. It is not about how an event travels to a client — that is
 * reliable against best-effort, which gameEventIsReliable answers from the
 * type and which says nothing about where the host heard it.
 *
 * An entry's type byte is a CTRL_* on one channel and an EVENT_* on the
 * other, and the two numberings overlap, so the channel is what a reader
 * has to look at first. */
#define SCN_EVENT_CHANNEL_GAME    0
#define SCN_EVENT_CHANNEL_CONTROL 1

/* How much of an event the queue keeps, measured against both channels.
 *
 * A game event is a type and eight data bytes, so the eight are kept whole
 * and the width is not what binds there.
 *
 * A control event is a type and a variant, and what the queue keeps is the
 * front of that variant, which is where each of them puts the slot the
 * event is about. Two variants have to be read a long way into:
 *
 *   - a lobby slot, whose team number sits behind a whole player name,
 *     sixty-seven bytes in, and which is where both the team change and the
 *     roster change are read from;
 *   - a chat line, which is the sender and the destination, the length, and
 *     up to PACKET_MAX_CHAT_MESSAGE bytes of text.
 *
 * The chat line is the wider of the two and sets the number. A line held
 * short would be a line a script reads wrong rather than one it knows it is
 * missing, which is why this is sized to carry it and not to an average.
 *
 * scenario_events.c holds this against both sources, so a game event with a
 * wider data[], a control union that no longer has room for the copy, or
 * either of those two variants growing past it, fails the build there. */
#define SCN_EVENT_DATA_MAX 132

/* Who caused it. Neither channel carries any notion of that — a
 * ControlEvent has no actor field and a game event has no room for one — so
 * the mark is the host's own annotation, read off the sim at the moment of
 * queueing and written here.
 *
 * Nobody is the same spelling scenario_defs.h gives SCN_NONE, written out
 * here so this header needs nothing from src/bolo/scenario_api/;
 * scenario_events.c sees both and holds them against each other.
 *
 * The two values below are the two answers a host can give today. They sit
 * at the top of the byte and leave the player slots free: if a fact ever
 * names the player who caused it, that is where it goes and nothing here
 * has to move. */
#define SCN_EVENT_ACTOR_NONE   0xFF
#define SCN_EVENT_ACTOR_SCRIPT 0xFE

/* One event, as the host keeps it.
 *
 * channel says which of the two it came in on and has to be read before
 * type means anything. data is the front of what that channel delivered:
 * a game event's eight bytes whole, or the first SCN_EVENT_DATA_MAX of a
 * control event's variant.
 *
 * actor is who caused it, as the queueing callback read it off the sim —
 * SCN_EVENT_ACTOR_SCRIPT for a fact the running scenario's own op or its
 * queued spawn produced, SCN_EVENT_ACTOR_NONE for everything else. It is
 * what a hook's trailing scripted boolean is built from. */
typedef struct {
    uint8_t type;
    uint8_t channel;
    uint8_t actor;
    uint8_t data[SCN_EVENT_DATA_MAX];
} ScnQueuedEvent;

/* The queue itself, and the numbers that make what it did visible.
 *
 * queued, dropped and drained only ever rise, so a caller reading one of
 * them is reading something nothing else puts back. count is what is waiting
 * and is the only one the drain lowers. */
typedef struct {
    ScnQueuedEvent entries[SCN_EVENT_QUEUE_MAX];
    uint16_t       head;      /* the next entry to consume */
    uint16_t       count;     /* entries waiting */

    uint32_t       queued;    /* taken in since the queue was reset */
    uint32_t       dropped;   /* refused for want of room */
    uint32_t       reported;  /* of those, already told to the operator */
    uint32_t       drained;   /* consumed */
} ScnEventQueue;

/* What a drain does with one entry. The queue knows nothing about what an
 * event means; the caller passes this and decides, reading the channel to
 * tell a control event from a game one. */
typedef void (*ScnEventConsumeFn)(void *ctx, const ScnQueuedEvent *e);

/*********************************************************
 *NAME:          scenarioEventsReset
 *PURPOSE:
 *  Empties the queue and puts every number back to zero.
 *********************************************************/
void scenarioEventsReset(ScnEventQueue *q);

/*********************************************************
 *NAME:          scenarioEventsQueueGame
 *                scenarioEventsQueueControl
 *PURPOSE:
 *  Copies one event in, from one channel or the other. This
 *  is all either of the host's subscriber callbacks does:
 *  both run from inside the work that raised the event, so
 *  they call nothing back into the engine, run no Lua and
 *  write no log line.
 *
 *  They share the one queue and the one order, so a round's
 *  facts come back out in the order they happened however
 *  each of them arrived.
 *
 *  A queue with no room drops the event and counts it. The
 *  oldest is kept rather than overwritten: what a script
 *  needs is the start of a run it can still make sense of,
 *  not the tail of one.
 *
 *  actor is what the caller read off the sim as it was
 *  handed the event, and is the only thing either of them
 *  knows that the event itself does not carry.
 *********************************************************/
void scenarioEventsQueueGame(ScnEventQueue *q, const GameEvent *evt,
                             uint8_t actor);
void scenarioEventsQueueControl(ScnEventQueue *q, const ControlEvent *evt,
                                uint8_t actor);

/*********************************************************
 *NAME:          scenarioEventsWaiting
 *PURPOSE:
 *  How many entries are waiting. Zero for a NULL queue.
 *********************************************************/
uint16_t scenarioEventsWaiting(const ScnEventQueue *q);

/*********************************************************
 *NAME:          scenarioEventsDrain
 *PURPOSE:
 *  Reads the number waiting once and consumes exactly that
 *  many, in the order they arrived, through consume.
 *
 *  Anything queued while the drain is running — including
 *  events the consuming itself causes — is left for the next
 *  one. A run that feeds itself therefore moves one event per
 *  tick, shows up in the numbers above, and cannot spin
 *  inside a tick.
 *
 *  Returns how many it consumed. consume may be NULL, which
 *  empties the queue and does nothing else.
 *********************************************************/
uint16_t scenarioEventsDrain(ScnEventQueue *q, ScnEventConsumeFn consume,
                             void *ctx);

/*********************************************************
 *NAME:          scenarioEventsTakeDropped
 *PURPOSE:
 *  How many events have been dropped since the last time
 *  this was asked, and marks them as told. The drop itself
 *  happens inside a publish, where there is nothing safe to
 *  say; this is what the tick asks so it can say it once.
 *********************************************************/
uint32_t scenarioEventsTakeDropped(ScnEventQueue *q);

/*********************************************************
 *NAME:          scenarioHostEventQueue
 *PURPOSE:
 *  The queue the host is holding, for the library's own code
 *  and for the tests that check what arrived. NULL for a
 *  NULL host.
 *********************************************************/
const ScnEventQueue *scenarioHostEventQueue(const ScenarioHost *h);

#endif /* SCENARIO_EVENTS_H */
