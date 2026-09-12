/*
 * The in-process game-event channel, and the four sites that feed it.
 *
 * A subscriber has always heard the server's ControlEvents. It can now ask for
 * the tick's GameEvents as well, with serverSimSetSubscriberEventDeliver after
 * it has registered, and serverSimAddEvent hands it every event it raises.
 * Asking is optional: a subscriber that says nothing keeps the control stream
 * alone and is not touched by any of this.
 *
 * The three events below are raised by GameSim callbacks — baseOwnerChanged,
 * pillOwnerChanged and lgmDied — implemented in server_sim_callbacks.c.
 *
 * The two capture events carry [newOwner, prevOwner, index, reserved] on the
 * wire and keep the capture class and the objective's square behind it. Every
 * expected byte in this file is written out by hand, including the ones past
 * gameEventDataSize(), so a payload that shifts fails here rather than
 * agreeing with whatever the emit happens to produce. The wire case composes
 * its bytes by hand for the same reason: it must not ask the codec what the
 * codec should be answering.
 *
 * Pinned here:
 *   1. A base capture reaches an asking subscriber on the event channel, not
 *      the control one, with all seven meaningful bytes — from neutral and as
 *      a steal, which are the two capture classes this path can produce.
 *   2. The same for a pillbox.
 *   3. The same for a builder death, which puts the two slots and a quiet
 *      byte on the wire and carries the man's map cell behind them rather
 *      than a class and index.
 *   4. A subscriber that registered no event callback hears none of it and
 *      still receives control events; the setter refuses a handle that names
 *      no live subscriber.
 *   5. A ClientSim fires none of the three. The callbacks are NULL there, and
 *      the isServer test around each call site is what keeps them unreached —
 *      so the client is driven through all three actions with recording
 *      callbacks installed, and must still fire nothing.
 *   6. An objective handed to nobody is published like a capture with no new
 *      owner, from the two sites that can reach one; the third cannot, and
 *      the case says why.
 *   7. The index on the wire is the 0-based item[] slot, at both ends of the
 *      pill and base lists.
 *   8. A capture packs to five bytes — type and four — with the reserved byte
 *      zero and the class and square left behind.
 *   9. A neutralisation draws no newswire line on a client and credits no
 *      capture, against a control that shows the same fixture drawing one.
 *  10. The stats funnel reads the class, the index and the square from their
 *      new offsets.
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
#include "messages.h"              /* messageType, newsWireMessage */
#include "transport_udp_internal.h" /* packGameEvent / unpackGameEvent */
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
    want[2] = 0;                    /* index: base number 1 is item[0] */
    want[3] = 0;                    /* reserved */
    want[4] = CAPTURE_CLASS_NEUTRAL;
    want[5] = bx;
    want[6] = by;
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
    want[2] = 0;
    want[3] = 0;
    want[4] = CAPTURE_CLASS_ENEMY;
    want[5] = bx;
    want[6] = by;
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
    want[2] = 0;
    want[3] = 0;
    want[4] = CAPTURE_CLASS_NEUTRAL;
    want[5] = bx;
    want[6] = by;
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
    want[2] = 0;                    /* index: pill number 1 is item[0] */
    want[3] = 0;                    /* reserved */
    want[4] = CAPTURE_CLASS_NEUTRAL;
    want[5] = pill.x;
    want[6] = pill.y;
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
    want[2] = 0;
    want[3] = 0;
    want[4] = CAPTURE_CLASS_ENEMY;
    want[5] = pill.x;
    want[6] = pill.y;
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

    /* [victim, killer, quiet] on the wire, then the man's map cell, read
       after lgmKill had already moved him to the start he flies back in
       from. data[5] upwards stays zero: this event carries no class and no
       index. With no policy registered the quiet byte is 0. */
    memset(want, 0, sizeof(want));
    want[0] = GEC_HOLDER;
    want[1] = GEC_THIEF;
    want[2] = 0;
    want[3] = (BYTE)(gs->lgmen[GEC_HOLDER]->x >> M_W_SHIFT_SIZE);
    want[4] = (BYTE)(gs->lgmen[GEC_HOLDER]->y >> M_W_SHIFT_SIZE);
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
    want[2] = 0;
    want[3] = (BYTE)(gs->lgmen[GEC_THIEF]->x >> M_W_SHIFT_SIZE);
    want[4] = (BYTE)(gs->lgmen[GEC_THIEF]->y >> M_W_SHIFT_SIZE);
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
 * 4b. The sim counts the slots with an event channel, so serverSimAddEvent
 * can return before its scan when nobody is listening — which is every sim
 * with no scenario attached. Taking the channel, handing it back and
 * unregistering with it still held all keep the count right.
 * ================================================================ */
int run_game_event_channel_subscriber_count(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GecSink a;
    GecSink b;
    SubscriberHandle ha;
    SubscriberHandle hb;
    int before;

    UT_ASSERT(sim != NULL);
    before = sim->numEventSubscribers;

    ha = gecSubscribe(sim, &a, /*wantEvents*/ false);
    UT_ASSERT(ha != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(sim->numEventSubscribers == before,
                  "a control-only subscriber moved the event count to %d",
                  sim->numEventSubscribers);

    UT_ASSERT(serverSimSetSubscriberEventDeliver(sim, ha, gecDeliverEvent));
    UT_ASSERT_MSG(sim->numEventSubscribers == before + 1,
                  "taking the channel left the count at %d",
                  sim->numEventSubscribers);
    /* Setting it again is not a second listener. */
    UT_ASSERT(serverSimSetSubscriberEventDeliver(sim, ha, gecDeliverEvent));
    UT_ASSERT_MSG(sim->numEventSubscribers == before + 1,
                  "re-setting the channel counted it twice: %d",
                  sim->numEventSubscribers);

    hb = gecSubscribe(sim, &b, /*wantEvents*/ true);
    UT_ASSERT(hb != SUBSCRIBER_HANDLE_INVALID);
    UT_ASSERT_MSG(sim->numEventSubscribers == before + 2,
                  "a second listener left the count at %d",
                  sim->numEventSubscribers);

    UT_ASSERT(serverSimSetSubscriberEventDeliver(sim, ha, NULL));
    UT_ASSERT_MSG(sim->numEventSubscribers == before + 1,
                  "handing the channel back left the count at %d",
                  sim->numEventSubscribers);

    serverSimUnregisterSubscriber(sim, hb);
    UT_ASSERT_MSG(sim->numEventSubscribers == before,
                  "unregistering a listener left the count at %d",
                  sim->numEventSubscribers);

    serverSimUnregisterSubscriber(sim, ha);
    UT_ASSERT_MSG(sim->numEventSubscribers == before,
                  "unregistering a control-only subscriber moved the count "
                  "to %d", sim->numEventSubscribers);

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

/* ================================================================
 * 6. An objective handed to nobody.
 * ================================================================ */
int run_game_event_channel_neutralised(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    GecSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE bx, by;
    pillbox pill;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) >= 1 && basesIsActive(&gs->bs, 1));
    UT_ASSERT(pillsGetNumPills(&gs->pb) >= 1 && pillsIsActive(&gs->pb, 1));

    h = gecSubscribe(sim, &sink, /*wantEvents*/ true);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    bx = (*gs->bs).item[0].x;
    by = (*gs->bs).item[0].y;

    /* A base taken off a player and handed to nobody publishes the capture
       event with no new owner. */
    (*gs->bs).item[0].owner = GEC_HOLDER;
    gecDrain(sim, &sink);
    UT_ASSERT(basesSetBaseOwner(gs, 1, NEUTRAL, FALSE, FALSE) == GEC_HOLDER);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_BASE_CAPTURED) == 1,
                  "a neutralised base published %d events",
                  gecCount(&sink, EVENT_BASE_CAPTURED));
    ev = gecFind(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = NEUTRAL;
    want[1] = GEC_HOLDER;
    want[2] = 0;
    want[3] = 0;
    /* The class expression at the emit site is unchanged, and an owner that is
       nobody is not an ally of the slot that held it, so the internal byte
       reads as a steal. It credits nobody: the derivation drops a record whose
       new owner is not a seat. */
    want[4] = CAPTURE_CLASS_ENEMY;
    want[5] = bx;
    want[6] = by;
    GEC_ASSERT_BYTES(ev, want, "base neutralised");

    /* And a pillbox, through the other site. */
    memset(&pill, 0, sizeof(pill));
    pillsGetPill(&gs->pb, &pill, 1);
    (*gs->pb).item[0].owner = GEC_HOLDER;
    gecDrain(sim, &sink);
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, NEUTRAL, FALSE) == GEC_HOLDER);

    UT_ASSERT_MSG(gecCount(&sink, EVENT_PILL_CAPTURED) == 1,
                  "a neutralised pill published %d events",
                  gecCount(&sink, EVENT_PILL_CAPTURED));
    ev = gecFind(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = NEUTRAL;
    want[1] = GEC_HOLDER;
    want[2] = 0;
    want[3] = 0;
    want[4] = CAPTURE_CLASS_ENEMY;
    want[5] = pill.x;
    want[6] = pill.y;
    GEC_ASSERT_BYTES(ev, want, "pill neutralised");

    /* The third emit site, basesSetOwner, publishes nothing for one — and
       dropping the owner test from its guard did not change that. The arm
       above the emit (bases.c, "else if (owner == NEUTRAL)") finishes every
       neutralising call before the emit below it is reached, and the one
       caller that exists hands it the driving tank's slot, never NEUTRAL.
       Pinned here so a later change to that chain is noticed. */
    (*gs->bs).item[0].owner = GEC_HOLDER;
    gecDrain(sim, &sink);
    UT_ASSERT(basesSetOwner(gs, bx, by, NEUTRAL, FALSE) == GEC_HOLDER);
    UT_ASSERT_MSG((*gs->bs).item[0].owner == NEUTRAL,
                  "the base did not go neutral at all");
    UT_ASSERT_MSG(gecCount(&sink, EVENT_BASE_CAPTURED) == 0,
                  "the by-square form published %d events for a neutralisation",
                  gecCount(&sink, EVENT_BASE_CAPTURED));

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 7. The index on the wire is the 0-based one.
 * ================================================================ */
int run_game_event_channel_capture_index_base(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    GecSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE nb, np;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    nb = basesGetNumBases(&gs->bs);
    np = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(nb >= 2, "map has %u bases, this case needs two", (unsigned)nb);
    UT_ASSERT_MSG(np >= 2, "map has %u pills, this case needs two", (unsigned)np);
    UT_ASSERT(basesIsActive(&gs->bs, 1) && basesIsActive(&gs->bs, nb));
    UT_ASSERT(pillsIsActive(&gs->pb, 1) && pillsIsActive(&gs->pb, np));

    h = gecSubscribe(sim, &sink, /*wantEvents*/ true);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Base number 1 is item[0], so the event says 0 and not 1. */
    (*gs->bs).item[0].owner = NEUTRAL;
    gecDrain(sim, &sink);
    UT_ASSERT(basesSetBaseOwner(gs, 1, GEC_HOLDER, FALSE, FALSE) == NEUTRAL);
    ev = gecFind(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT_MSG(ev != NULL, "base 1 published nothing");
    UT_ASSERT_MSG(ev->data[2] == 0, "base 1 reported index %u, want 0",
                  (unsigned)ev->data[2]);

    /* The last base is item[nb - 1] — the end the off-by-one shows at. */
    (*gs->bs).item[nb - 1].owner = NEUTRAL;
    gecDrain(sim, &sink);
    UT_ASSERT(basesSetBaseOwner(gs, nb, GEC_HOLDER, FALSE, FALSE) == NEUTRAL);
    ev = gecFind(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT_MSG(ev != NULL, "base %u published nothing", (unsigned)nb);
    UT_ASSERT_MSG(ev->data[2] == (BYTE)(nb - 1),
                  "base %u reported index %u, want %u", (unsigned)nb,
                  (unsigned)ev->data[2], (unsigned)(nb - 1));

    /* The same both ends for pillboxes. */
    (*gs->pb).item[0].owner = NEUTRAL;
    gecDrain(sim, &sink);
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, GEC_HOLDER, FALSE) == NEUTRAL);
    ev = gecFind(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT_MSG(ev != NULL, "pill 1 published nothing");
    UT_ASSERT_MSG(ev->data[2] == 0, "pill 1 reported index %u, want 0",
                  (unsigned)ev->data[2]);

    (*gs->pb).item[np - 1].owner = NEUTRAL;
    gecDrain(sim, &sink);
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, np, GEC_HOLDER, FALSE) == NEUTRAL);
    ev = gecFind(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT_MSG(ev != NULL, "pill %u published nothing", (unsigned)np);
    UT_ASSERT_MSG(ev->data[2] == (BYTE)(np - 1),
                  "pill %u reported index %u, want %u", (unsigned)np,
                  (unsigned)ev->data[2], (unsigned)(np - 1));

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 8. What a capture puts on the wire.
 * ================================================================ */

/* The byte vectors below are written out by hand rather than derived from
 * gameEventDataSize, so a change to that table shows up here as a failure
 * instead of quietly agreeing with itself. */
int run_game_event_channel_capture_wire_bytes(void) {
    static const uint8_t wantBase[5] = { EVENT_BASE_CAPTURED, 3, 7, 5, 0 };
    static const uint8_t wantPill[5] = { EVENT_PILL_CAPTURED, NEUTRAL, 2, 9, 0 };
    GameEvent in;
    GameEvent out;
    uint8_t buf[GAME_EVENT_MAX_WIRE_SIZE];
    int packed, consumed, i;

    /* A base stolen by slot 3 from slot 7, base index 5. */
    memset(&in, 0, sizeof(in));
    in.type = EVENT_BASE_CAPTURED;
    in.data[0] = 3;
    in.data[1] = 7;
    in.data[2] = 5;
    in.data[3] = 0;
    in.data[4] = CAPTURE_CLASS_ENEMY;   /* internal — must stay off the wire */
    in.data[5] = 41;
    in.data[6] = 42;

    memset(buf, 0xAA, sizeof(buf));
    packed = packGameEvent(buf, &in);
    UT_ASSERT_MSG(packed == 5, "a base capture packed %d bytes, want 5", packed);
    for (i = 0; i < 5; i++) {
        UT_ASSERT_MSG(buf[i] == wantBase[i],
                      "base capture wire byte %d = 0x%02x, want 0x%02x",
                      i, (unsigned)buf[i], (unsigned)wantBase[i]);
    }
    UT_ASSERT_MSG(buf[5] == 0xAA,
                  "a byte past the payload was written: 0x%02x", (unsigned)buf[5]);

    memset(&out, 0xEE, sizeof(out));
    consumed = unpackGameEvent(buf, (size_t)packed, &out);
    UT_ASSERT_MSG(consumed == 5, "a base capture consumed %d bytes, want 5",
                  consumed);
    UT_ASSERT(out.type == EVENT_BASE_CAPTURED);
    UT_ASSERT_MSG(out.data[0] == 3 && out.data[1] == 7,
                  "owners decoded as %u/%u", (unsigned)out.data[0],
                  (unsigned)out.data[1]);
    UT_ASSERT_MSG(out.data[2] == 5, "index decoded as %u", (unsigned)out.data[2]);
    UT_ASSERT_MSG(out.data[3] == 0, "the reserved byte decoded as %u",
                  (unsigned)out.data[3]);
    UT_ASSERT_MSG(out.data[4] == 0 && out.data[5] == 0 && out.data[6] == 0,
                  "an internal byte crossed the wire (%u/%u/%u)",
                  (unsigned)out.data[4], (unsigned)out.data[5],
                  (unsigned)out.data[6]);

    /* A pillbox neutralised: no new owner, pill index 9. */
    memset(&in, 0, sizeof(in));
    in.type = EVENT_PILL_CAPTURED;
    in.data[0] = NEUTRAL;
    in.data[1] = 2;
    in.data[2] = 9;
    in.data[3] = 0;
    in.data[4] = CAPTURE_CLASS_ENEMY;
    in.data[5] = 12;
    in.data[6] = 13;

    memset(buf, 0xAA, sizeof(buf));
    packed = packGameEvent(buf, &in);
    UT_ASSERT_MSG(packed == 5, "a pill capture packed %d bytes, want 5", packed);
    for (i = 0; i < 5; i++) {
        UT_ASSERT_MSG(buf[i] == wantPill[i],
                      "pill capture wire byte %d = 0x%02x, want 0x%02x",
                      i, (unsigned)buf[i], (unsigned)wantPill[i]);
    }
    UT_ASSERT_MSG(buf[5] == 0xAA,
                  "a byte past the payload was written: 0x%02x", (unsigned)buf[5]);

    return 0;
}

/* ================================================================
 * 9. A neutralisation draws no newswire line on a client.
 * ================================================================ */

#define GEC_LOCAL 0   /* the viewing client; the events below name other slots */

static int gecClientLines;

static void gecSpyMessageAdd(void *ctx, messageType msgType, langid topId,
                             langid bodyId, const MessageArgs *args) {
    (void)ctx; (void)topId; (void)bodyId; (void)args;
    if (msgType == newsWireMessage) gecClientLines++;
}

/* Apply one game event to the client under the local slot. */
static void gecClientApply(ClientSim *cs, BYTE type, BYTE newOwner,
                           BYTE prevOwner, BYTE index) {
    GameEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.data[0] = newOwner;
    e.data[1] = prevOwner;
    e.data[2] = index;
    e.data[3] = 0;
    clientSimApplyGameEvents(cs, &e, 1, GEC_LOCAL);
}

int run_game_event_channel_neutralised_no_client_line(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    int i;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, GEC_LOCAL);
    /* The line sites are the human ones, so this fixture must not be a bot. */
    clientSimSetIsBot(cs, false);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs != NULL);
    gs->callbacks.messageAdd = gecSpyMessageAdd;

    /* Positive control: a pillbox taken off nobody says so. Without this the
       case below passes on a fixture that could never draw a line at all. */
    gecClientLines = 0;
    gecClientApply(cs, EVENT_PILL_CAPTURED, /*new*/ 1, /*prev*/ NEUTRAL, 0);
    UT_ASSERT_MSG(gecClientLines == 1,
                  "a pill capture drew %d newswire lines, want 1",
                  gecClientLines);

    /* The same pillbox handed to nobody draws none. */
    gecClientLines = 0;
    gecClientApply(cs, EVENT_PILL_CAPTURED, /*new*/ NEUTRAL, /*prev*/ 1, 0);
    UT_ASSERT_MSG(gecClientLines == 0,
                  "a neutralised pill drew %d newswire lines", gecClientLines);

    /* And a base. Its line site debounces rather than emitting on the spot,
       so the queue is pumped afterwards: a line held back for later is still
       a line. */
    gecClientLines = 0;
    gecClientApply(cs, EVENT_BASE_CAPTURED, /*new*/ NEUTRAL, /*prev*/ 1, 0);
    for (i = 0; i < 600; i++) {
        basesTickMessageQueue(gs, cs);
    }
    UT_ASSERT_MSG(gecClientLines == 0,
                  "a neutralised base drew %d newswire lines", gecClientLines);

    /* The live scoreboard is untouched too: NEUTRAL is not a seat. */
    {
        const ClientPlayerStats *row = clientSimGetPlayerStats(cs, 1);
        UT_ASSERT(row != NULL);
        UT_ASSERT_MSG(row->pillCaptures == 1,
                      "slot 1 holds %u pill captures, want the one real capture",
                      (unsigned)row->pillCaptures);
        UT_ASSERT_MSG(row->baseCaptures == 0,
                      "a neutralisation credited slot 1 with %u base captures",
                      (unsigned)row->baseCaptures);
    }

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 10. The stats funnel still reads the right bytes.
 * ================================================================ */
int run_game_event_channel_capture_attribution(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    GameSim *gs;
    AttrCaptureRecord r;
    BYTE bx, by;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, GEC_THIEF, "Thief", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) >= 2);
    UT_ASSERT(basesIsActive(&gs->bs, 2));
    UT_ASSERT_MSG(playersIsAllie(&gs->plyrs, GEC_THIEF, GEC_HOLDER) == FALSE,
                  "the two players must not be allies for the steal below");

    bx = (*gs->bs).item[1].x;
    by = (*gs->bs).item[1].y;

    /* Base 2 stolen from a player, so class, index and square are all
       non-zero and a byte read from the wrong offset cannot pass. */
    (*gs->bs).item[1].owner = GEC_HOLDER;
    sim->trackLen = 0;
    sim->trackRecordCount = 0;
    sim->trackTruncated = false;
    UT_ASSERT(basesSetBaseOwner(gs, 2, GEC_THIEF, FALSE, FALSE) == GEC_HOLDER);

    UT_ASSERT_MSG(sim->trackLen >= sizeof r,
                  "the capture appended %u bytes of record",
                  (unsigned)sim->trackLen);
    memcpy(&r, sim->trackBuf, sizeof r);
    UT_ASSERT_MSG(r.type == ATTR_REC_CAPTURE, "record tag %u", (unsigned)r.type);
    UT_ASSERT_MSG(r.target == ATTR_CAP_TGT_BASE, "target %u", (unsigned)r.target);
    UT_ASSERT_MSG(r.targetIndex == 1, "index %u, want 1 for base 2",
                  (unsigned)r.targetIndex);
    UT_ASSERT_MSG(r.newOwner == GEC_THIEF && r.prevOwner == GEC_HOLDER,
                  "owners %u/%u", (unsigned)r.newOwner, (unsigned)r.prevOwner);
    UT_ASSERT_MSG(r.captureClass == CAPTURE_CLASS_ENEMY,
                  "class %u, want the enemy steal", (unsigned)r.captureClass);
    UT_ASSERT_MSG(r.mapX == bx && r.mapY == by,
                  "cell %u,%u want %u,%u", (unsigned)r.mapX, (unsigned)r.mapY,
                  (unsigned)bx, (unsigned)by);

    serverSimDestroy(sim);
    return 0;
}
