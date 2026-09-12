/*
 * The in-process game-event channel, and the four sites that feed it.
 *
 * A subscriber has always heard the server's ControlEvents. It can now ask for
 * the tick's GameEvents as well, with serverSimSetSubscriberEventDeliver after
 * it has registered, and serverSimAddEvent hands it every event it raises.
 * Asking is optional: a subscriber that says nothing keeps the control stream
 * alone and is not touched by any of this.
 *
 * The three events below used to be built in shared code, which reached the
 * server's queue by casting GameSim's callback context back to a ServerSim *.
 * They are now GameSim callbacks — baseOwnerChanged, pillOwnerChanged and
 * lgmDied — implemented in server_sim_callbacks.c. Nothing about the event
 * changed, and that is what most of this file is for: every expected byte
 * here, the server-internal ones past gameEventDataSize() included, is
 * written out by hand from what the old inline emit built, so the test fails
 * if the bytes move rather than agreeing with whatever the new path happens
 * to produce.
 *
 * Pinned here:
 *   1. A base capture reaches an asking subscriber on the event channel, not
 *      the control one, with all eight bytes unchanged — from neutral and as
 *      a steal, which are the two capture classes data[2] can hold here.
 *   2. The same for a pillbox.
 *   3. The same for a builder death, whose internal bytes are the man's map
 *      cell rather than a class and an index.
 *   4. A subscriber that registered no event callback hears none of it and
 *      still receives control events; the setter refuses a handle that names
 *      no live subscriber.
 *   5. A ClientSim fires none of the three. The callbacks are NULL there, and
 *      the isServer test around each call site is what keeps them unreached —
 *      so the client is driven through all three actions with recording
 *      callbacks installed, and must still fire nothing.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* eventCount — drained between steps */
#include "control_event.h"
#include "client_sim.h"
#include "game_sim.h"              /* GameSim: bs, pb, ss, lgmen[], callbacks */
#include "input_packet.h"          /* GameEvent, EVENT_*, CAPTURE_CLASS_* */
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "tank.h"
#include "lgm.h"
#include "players.h"               /* playersIsAllie — the capture-class premise */
#include "test_harness.h"

#define GEC_HOLDER 0   /* takes the objectives first */
#define GEC_THIEF  1   /* steals them, and kills the builder */
#define GEC_LATE   2   /* joins late, to raise a control event on demand */

/* ----------------------------------------------------------------
 * A subscriber that records both channels.
 * ---------------------------------------------------------------- */

#define GEC_MAX_KEPT 32

typedef struct {
    int       controlCount;             /* ControlEvents delivered */
    int       eventCount;               /* GameEvents delivered */
    GameEvent events[GEC_MAX_KEPT];     /* the first GEC_MAX_KEPT of them */
} GecSink;

static void gecDeliverControl(void *ctx, const ControlEvent *evt) {
    GecSink *s = (GecSink *)ctx;
    (void)evt;
    s->controlCount++;
}

static void gecDeliverEvent(void *ctx, const GameEvent *evt) {
    GecSink *s = (GecSink *)ctx;
    if (s->eventCount < GEC_MAX_KEPT) {
        s->events[s->eventCount] = *evt;
    }
    s->eventCount++;
}

static int gecKept(const GecSink *s) {
    return s->eventCount < GEC_MAX_KEPT ? s->eventCount : GEC_MAX_KEPT;
}

static int gecCount(const GecSink *s, BYTE type) {
    int found = 0;
    int i;
    for (i = 0; i < gecKept(s); i++) {
        if (s->events[i].type == type) found++;
    }
    return found;
}

static const GameEvent *gecFind(const GecSink *s, BYTE type) {
    int i;
    for (i = 0; i < gecKept(s); i++) {
        if (s->events[i].type == type) return &s->events[i];
    }
    return NULL;
}

/* Register, and take the event channel if asked for. Registration replays the
 * current server state through the control callback, so the sink is cleared
 * afterwards and measures only what happens next. */
static SubscriberHandle gecSubscribe(ServerSim *sim, GecSink *s,
                                     bool wantEvents) {
    SubscriberHandle h;
    memset(s, 0, sizeof(*s));
    h = serverSimRegisterSubscriber(sim, gecDeliverControl, s);
    if (h != SUBSCRIBER_HANDLE_INVALID && wantEvents) {
        if (!serverSimSetSubscriberEventDeliver(sim, h, gecDeliverEvent)) {
            return SUBSCRIBER_HANDLE_INVALID;
        }
    }
    memset(s, 0, sizeof(*s));
    return h;
}

/* The frame's event buffer, so a second capture in the same test is not read
 * against the first one's leftovers. */
static void gecDrain(ServerSim *sim, GecSink *s) {
    sim->eventCount = 0;
    memset(s, 0, sizeof(*s));
}

/* -1 when every one of the eight data bytes matches, else the first index
 * that does not. */
static int gecFirstDiff(const GameEvent *ev, const BYTE *want) {
    int i;
    for (i = 0; i < GAME_EVENT_MAX_DATA; i++) {
        if (ev->data[i] != want[i]) return i;
    }
    return -1;
}

#define GEC_ASSERT_BYTES(ev, want, what)                                     \
    do {                                                                     \
        int _d = gecFirstDiff((ev), (want));                                 \
        int _at = _d < 0 ? 0 : _d;                                           \
        UT_ASSERT_MSG(_d < 0, "%s: data[%d] = %u, want %u", (what), _at,     \
                      (unsigned)(ev)->data[_at], (unsigned)(want)[_at]);     \
    } while (0)

/* The event the tick buffered, which is what the snapshot stream sends and
 * what the stats funnel read. It must be the same event the subscriber was
 * handed, byte for byte. */
static const GameEvent *gecBuffered(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) return &evs[i];
    }
    return NULL;
}

/* ================================================================
 * 1. A base capture.
 * ================================================================ */
int run_game_event_channel_base_captured(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    GecSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    const GameEvent *buffered;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE bx, by;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, GEC_THIEF, "Thief", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) >= 1);
    UT_ASSERT(basesIsActive(&gs->bs, 1));
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, GEC_THIEF, GEC_HOLDER) == FALSE,
                  "the two players must not be allies for the steal below");

    h = gecSubscribe(sim, &sink, /*wantEvents*/ true);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    bx = (*gs->bs).item[0].x;
    by = (*gs->bs).item[0].y;

    /* Taken from neutral. The old emit wrote [owner, prevOwner, class, index,
       x, y] with the index the 0-based item[] slot — base number 1 less one. */
    (*gs->bs).item[0].owner = NEUTRAL;
    gecDrain(sim, &sink);
    UT_ASSERT(basesSetBaseOwner(gs, 1, GEC_HOLDER, FALSE, FALSE) == NEUTRAL);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_BASE_CAPTURED) == 1,
                  "%d capture events on the channel",
                  gecCount(&sink, EVENT_BASE_CAPTURED));
    UT_ASSERT_MSG(sink.controlCount == 0,
                  "%d control events — a game event took the wrong channel",
                  sink.controlCount);
    ev = gecFind(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GEC_HOLDER;
    want[1] = NEUTRAL;
    want[2] = CAPTURE_CLASS_NEUTRAL;
    want[3] = 0;
    want[4] = bx;
    want[5] = by;
    GEC_ASSERT_BYTES(ev, want, "base taken from neutral");

    buffered = gecBuffered(sim, EVENT_BASE_CAPTURED);
    UT_ASSERT_MSG(buffered != NULL, "the tick buffered no capture event");
    GEC_ASSERT_BYTES(buffered, want, "base taken from neutral, as buffered");

    /* Stolen from a player who is not an ally. */
    gecDrain(sim, &sink);
    UT_ASSERT(basesSetBaseOwner(gs, 1, GEC_THIEF, FALSE, FALSE) == GEC_HOLDER);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_BASE_CAPTURED) == 1,
                  "%d capture events for the steal",
                  gecCount(&sink, EVENT_BASE_CAPTURED));
    ev = gecFind(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GEC_THIEF;
    want[1] = GEC_HOLDER;
    want[2] = CAPTURE_CLASS_ENEMY;
    want[3] = 0;
    want[4] = bx;
    want[5] = by;
    GEC_ASSERT_BYTES(ev, want, "base stolen from an enemy");

    /* basesSetOwner, the by-square form, builds the identical event from its
       own loop counter — also the 0-based slot. */
    gecDrain(sim, &sink);
    (*gs->bs).item[0].owner = NEUTRAL;
    UT_ASSERT(basesSetOwner(gs, bx, by, GEC_HOLDER, FALSE) == NEUTRAL);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_BASE_CAPTURED) == 1,
                  "%d capture events from the by-square form",
                  gecCount(&sink, EVENT_BASE_CAPTURED));
    ev = gecFind(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GEC_HOLDER;
    want[1] = NEUTRAL;
    want[2] = CAPTURE_CLASS_NEUTRAL;
    want[3] = 0;
    want[4] = bx;
    want[5] = by;
    GEC_ASSERT_BYTES(ev, want, "base taken by square");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. A pillbox capture.
 * ================================================================ */
int run_game_event_channel_pill_captured(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    GecSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    const GameEvent *buffered;
    BYTE want[GAME_EVENT_MAX_DATA];
    pillbox pill;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, GEC_THIEF, "Thief", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(pillsGetNumPills(&gs->pb) >= 1);
    UT_ASSERT(pillsIsActive(&gs->pb, 1));
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, GEC_THIEF, GEC_HOLDER) == FALSE,
                  "the two players must not be allies for the steal below");

    h = gecSubscribe(sim, &sink, /*wantEvents*/ true);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    memset(&pill, 0, sizeof(pill));
    pillsGetPill(&gs->pb, &pill, 1);

    /* Taken from neutral. pillsSetPillOwner decrements the pill number before
       the emit and puts it back afterwards, so the index in the event is the
       0-based slot, as the base's is. */
    (*gs->pb).item[0].owner = NEUTRAL;
    gecDrain(sim, &sink);
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, GEC_HOLDER, FALSE) == NEUTRAL);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_PILL_CAPTURED) == 1,
                  "%d capture events on the channel",
                  gecCount(&sink, EVENT_PILL_CAPTURED));
    UT_ASSERT_MSG(sink.controlCount == 0,
                  "%d control events — a game event took the wrong channel",
                  sink.controlCount);
    ev = gecFind(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GEC_HOLDER;
    want[1] = NEUTRAL;
    want[2] = CAPTURE_CLASS_NEUTRAL;
    want[3] = 0;
    want[4] = pill.x;
    want[5] = pill.y;
    GEC_ASSERT_BYTES(ev, want, "pill taken from neutral");

    buffered = gecBuffered(sim, EVENT_PILL_CAPTURED);
    UT_ASSERT_MSG(buffered != NULL, "the tick buffered no capture event");
    GEC_ASSERT_BYTES(buffered, want, "pill taken from neutral, as buffered");

    /* Stolen from a player who is not an ally. */
    gecDrain(sim, &sink);
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, GEC_THIEF, FALSE) == GEC_HOLDER);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_PILL_CAPTURED) == 1,
                  "%d capture events for the steal",
                  gecCount(&sink, EVENT_PILL_CAPTURED));
    ev = gecFind(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GEC_THIEF;
    want[1] = GEC_HOLDER;
    want[2] = CAPTURE_CLASS_ENEMY;
    want[3] = 0;
    want[4] = pill.x;
    want[5] = pill.y;
    GEC_ASSERT_BYTES(ev, want, "pill stolen from an enemy");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A builder death.
 * ================================================================ */
int run_game_event_channel_lgm_lost(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    GecSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    const GameEvent *buffered;
    BYTE want[GAME_EVENT_MAX_DATA];

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, GEC_THIEF, "Thief", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(gs->lgmen[GEC_HOLDER] != NULL);
    UT_ASSERT(gs->lgmen[GEC_THIEF] != NULL);

    h = gecSubscribe(sim, &sink, /*wantEvents*/ true);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    gecDrain(sim, &sink);
    lgmKill(gs, &gs->lgmen[GEC_HOLDER], &gs->tanks[GEC_HOLDER], GEC_THIEF);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_LGM_LOST) == 1,
                  "%d builder-lost events on the channel",
                  gecCount(&sink, EVENT_LGM_LOST));
    UT_ASSERT_MSG(sink.controlCount == 0,
                  "%d control events — a game event took the wrong channel",
                  sink.controlCount);
    ev = gecFind(&sink, EVENT_LGM_LOST);
    UT_ASSERT(ev != NULL);

    /* The old emit wrote [victim, killer] and then the man's map cell into
       data[2]/data[3], read after lgmKill had already moved him to the start
       he flies back in from. data[4] upwards stayed zero: this event carries
       no class and no index. */
    memset(want, 0, sizeof(want));
    want[0] = GEC_HOLDER;
    want[1] = GEC_THIEF;
    want[2] = (BYTE)(gs->lgmen[GEC_HOLDER]->x >> M_W_SHIFT_SIZE);
    want[3] = (BYTE)(gs->lgmen[GEC_HOLDER]->y >> M_W_SHIFT_SIZE);
    GEC_ASSERT_BYTES(ev, want, "builder lost");

    buffered = gecBuffered(sim, EVENT_LGM_LOST);
    UT_ASSERT_MSG(buffered != NULL, "the tick buffered no builder-lost event");
    GEC_ASSERT_BYTES(buffered, want, "builder lost, as buffered");

    /* A death nobody caused credits NEUTRAL, which is what a mine produces. */
    gecDrain(sim, &sink);
    lgmKill(gs, &gs->lgmen[GEC_THIEF], &gs->tanks[GEC_THIEF], NEUTRAL);

    ev = gecFind(&sink, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ev != NULL, "an unattributed death published nothing");

    memset(want, 0, sizeof(want));
    want[0] = GEC_THIEF;
    want[1] = NEUTRAL;
    want[2] = (BYTE)(gs->lgmen[GEC_THIEF]->x >> M_W_SHIFT_SIZE);
    want[3] = (BYTE)(gs->lgmen[GEC_THIEF]->y >> M_W_SHIFT_SIZE);
    GEC_ASSERT_BYTES(ev, want, "builder lost to nobody");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. A subscriber that asked for nothing.
 * ================================================================ */
int run_game_event_channel_control_only_subscriber(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    GecSink withEvents;
    GecSink controlOnly;
    SubscriberHandle hEvents;
    SubscriberHandle hControl;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) >= 1);

    hEvents  = gecSubscribe(sim, &withEvents,  /*wantEvents*/ true);
    hControl = gecSubscribe(sim, &controlOnly, /*wantEvents*/ false);
    UT_ASSERT(hEvents != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT(hControl != SUBSCRIBER_HANDLE_INVALID);

    /* A handle naming no live subscriber is refused rather than written. */
    UT_ASSERT_MSG(serverSimSetSubscriberEventDeliver(
                      sim, SUBSCRIBER_HANDLE_INVALID, gecDeliverEvent) == false,
                  "an invalid handle was accepted");

    (*gs->bs).item[0].owner = NEUTRAL;
    sim->eventCount = 0;
    memset(&withEvents, 0, sizeof(withEvents));
    memset(&controlOnly, 0, sizeof(controlOnly));
    UT_ASSERT(basesSetBaseOwner(gs, 1, GEC_HOLDER, FALSE, FALSE) == NEUTRAL);

    UT_ASSERT_MSG(gecCount(&withEvents, EVENT_BASE_CAPTURED) == 1,
                  "the asking subscriber got %d capture events",
                  gecCount(&withEvents, EVENT_BASE_CAPTURED));
    UT_ASSERT_MSG(controlOnly.eventCount == 0,
                  "a subscriber that asked for no events got %d",
                  controlOnly.eventCount);

    /* And it still hears the control stream. A late join publishes on it. */
    serverSimAddPlayer(sim, GEC_LATE, "Late", false);
    UT_ASSERT_MSG(controlOnly.controlCount > 0,
                  "the control-only subscriber heard no control event");
    UT_ASSERT_MSG(withEvents.controlCount > 0,
                  "the asking subscriber lost the control channel");

    /* Handing back the channel stops the delivery and leaves control alone. */
    UT_ASSERT(serverSimSetSubscriberEventDeliver(sim, hEvents, NULL) == true);
    sim->eventCount = 0;
    memset(&withEvents, 0, sizeof(withEvents));
    (*gs->bs).item[0].owner = NEUTRAL;
    UT_ASSERT(basesSetBaseOwner(gs, 1, GEC_HOLDER, FALSE, FALSE) == NEUTRAL);
    UT_ASSERT_MSG(withEvents.eventCount == 0,
                  "a withdrawn channel still delivered %d events",
                  withEvents.eventCount);

    serverSimUnregisterSubscriber(sim, hEvents);
    serverSimUnregisterSubscriber(sim, hControl);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. A ClientSim fires none of the three.
 * ================================================================ */

static int gecClientFired;   /* any of the three callbacks, on the client */

static void gecSpyBaseOwner(void *ctx, BYTE index, BYTE oldOwner,
                            BYTE newOwner, BYTE captureClass,
                            BYTE mapX, BYTE mapY) {
    (void)ctx; (void)index; (void)oldOwner; (void)newOwner;
    (void)captureClass; (void)mapX; (void)mapY;
    gecClientFired++;
}

static void gecSpyPillOwner(void *ctx, BYTE index, BYTE oldOwner,
                            BYTE newOwner, BYTE captureClass,
                            BYTE mapX, BYTE mapY) {
    (void)ctx; (void)index; (void)oldOwner; (void)newOwner;
    (void)captureClass; (void)mapX; (void)mapY;
    gecClientFired++;
}

static void gecSpyLgmDied(void *ctx, BYTE victim, BYTE killer,
                          BYTE mapX, BYTE mapY) {
    (void)ctx; (void)victim; (void)killer; (void)mapX; (void)mapY;
    gecClientFired++;
}

int run_game_event_channel_client_emits_nothing(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    lgm *man = NULL;
    tank *tnk = NULL;
    pillbox pill;
    base bse;
    start st;
    BYTE i;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs != NULL);

    /* A ClientSim registers none of the three, which is the first half of why
       it publishes nothing. */
    UT_ASSERT_MSG(gs->isServer == false, "the client sim thinks it is a server");
    UT_ASSERT_MSG(gs->callbacks.baseOwnerChanged == NULL,
                  "the client registered a base-capture callback");
    UT_ASSERT_MSG(gs->callbacks.pillOwnerChanged == NULL,
                  "the client registered a pill-capture callback");
    UT_ASSERT_MSG(gs->callbacks.lgmDied == NULL,
                  "the client registered a builder-death callback");

    /* The second half is the isServer test around each call site. Install
       callbacks the client would never have and drive all three actions: the
       test only passes if nothing reaches them. */
    gecClientFired = 0;
    gs->callbacks.baseOwnerChanged = gecSpyBaseOwner;
    gs->callbacks.pillOwnerChanged = gecSpyPillOwner;
    gs->callbacks.lgmDied = gecSpyLgmDied;

    pillsSetNumPills(&gs->pb, 1);
    basesSetNumBases(&gs->bs, 1);
    startsSetNumStarts(&gs->ss, 1);

    memset(&pill, 0, sizeof(pill));
    pill.x = 41;
    pill.y = 40;
    pill.owner = NEUTRAL;
    pill.armour = PILLS_MAX_ARMOUR;
    pill.speed = PILLBOX_ATTACK_NORMAL;
    pillsSetPill(&gs->pb, &pill, 1);

    memset(&bse, 0, sizeof(bse));
    bse.x = 61;
    bse.y = 60;
    bse.owner = NEUTRAL;
    bse.armour = BASE_FULL_ARMOUR;
    bse.shells = BASE_FULL_SHELLS;
    bse.mines = BASE_FULL_MINES;
    basesSetBase(&gs->bs, &bse, 1);

    memset(&st, 0, sizeof(st));
    st.x = 81;
    st.y = 80;
    st.dir = 0;
    startsSetStart(&gs->ss, &st, 1);

    UT_ASSERT(basesSetBaseOwner(gs, 1, GEC_HOLDER, FALSE, FALSE) == NEUTRAL);
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, GEC_HOLDER, FALSE) == NEUTRAL);

    for (i = 0; i < MAX_TANKS; i++) {
        if (gs->lgmen[i] != NULL) {
            man = &gs->lgmen[i];
            tnk = gs->tanks[i] != NULL ? &gs->tanks[i] : NULL;
            break;
        }
    }
    UT_ASSERT_MSG(man != NULL, "the client sim has no builder to kill");
    lgmKill(gs, man, tnk, GEC_THIEF);

    UT_ASSERT_MSG(gecClientFired == 0,
                  "the client fired %d of the three callbacks", gecClientFired);

    /* And the actions did happen — the test would pass on a client that
       simply refused to change anything. */
    UT_ASSERT_MSG(basesGetBaseOwner(&gs->bs, 1) == GEC_HOLDER,
                  "the base did not change hands on the client");
    UT_ASSERT_MSG((*man)->isDead, "the builder did not die on the client");

    clientSimDestroy(cs);
    return 0;
}
