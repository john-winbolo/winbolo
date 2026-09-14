/*
 * The scenario host's events: the two subscriber callbacks that only queue,
 * the one queue they share, and the bounded drain at the end of each tick.
 *
 * The server has two channels and the host listens to both — control events
 * through serverSimPublishControl, game events through serverSimAddEvent —
 * and both land in the same queue in the order they were published, so one
 * drain gives a round's facts back in the order they happened.
 *
 * The queue cases drive ScnEventQueue directly, with a consume function the
 * case owns. That is what makes the ordering and the bound provable without
 * a seam in the host: the record is the case's own array, written by the
 * case's own function, and nothing else can reach it. The end-to-end cases
 * go through a sim and read the queue the host is holding.
 *
 * run_scenario_events_queued_then_drained
 *      — three events published reach the host's queue in publish order,
 *        and the next tick consumes them
 * run_scenario_events_drain_reads_the_length_once
 *      — a consume that queues more of its own still consumes exactly the
 *        number that was waiting when the drain started
 * run_scenario_events_queued_during_a_drain_waits
 *      — and what it queued is still there afterwards, for the next drain
 * run_scenario_events_overflow_boundary
 *      — the queue takes SCN_EVENT_QUEUE_MAX and refuses the next; the
 *        oldest is kept, the drain runs the whole of it, and the drop is
 *        reported once
 * run_scenario_events_overflow_counts_errors
 *      — a tick that dropped anything counts one error toward
 *        SCN_ERROR_LIMIT, so it plays on from nothing and switches the
 *        scenario off from one error short of the limit
 * run_scenario_events_no_scenario_delivers_nothing
 *      — a sim with no scenario has no event subscriber, so the fan-out
 *        returns before it walks the slots
 * run_scenario_events_lobby_tick_drains
 *      — the non-running branch of serverSimTick drains as the running one
 *        does
 * run_scenario_events_control_reaches_the_queue
 *      — a control event arrives and is drained the way a game event is
 * run_scenario_events_both_channels_in_publish_order
 *      — game, control, game, control, drained in that order: one publish,
 *        one arrival, whichever channel carried it
 * run_scenario_events_channel_says_which
 *      — and each entry says which channel it came in on
 * run_scenario_events_overflow_covers_both
 *      — a queue filled from both channels refuses the next of either and
 *        drains the whole of what it took
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* numEventSubscribers — the fan-out's own
                                    * test for whether anybody is listening */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "server_sim_scenario.h"   /* the policy depth bracket the lobby's
                                    * question is asked inside */
#include "control_event.h"         /* CTRL_SERVER_TEXT */
#include "input_packet.h"          /* GameEvent, EVENT_MINE_PLACED */
#include "everard_map.h"
#include "scenario_host.h"
#include "scenario_events.h"
#include "test_harness.h"

/* An inert event type: nothing in serverSimAddEvent's stats funnel reads it
   and no client in these cases acts on it, so a case can raise as many as it
   likes and measure only what the queue did with them. */
#define SE_TYPE EVENT_MINE_PLACED

/* ── Fixtures ─────────────────────────────────────────────────────── */

static void seSidecarFor(const char *mapPath, char *out, size_t outLen) {
    size_t n = strlen(mapPath);
    if (n > 4) {
        n -= 4;                     /* drop ".map" */
    }
    snprintf(out, outLen, "%.*s%s", (int)n, mapPath, SCN_SIDECAR_SUFFIX);
}

static bool sePut(const char *mapPath, const char *lua) {
    char  side[512];
    FILE *f;
    seSidecarFor(mapPath, side, sizeof(side));
    f = fopen(side, "wb");
    if (f == NULL) {
        return false;
    }
    fputs(lua, f);
    fclose(f);
    return true;
}

static void seDrop(const char *mapPath) {
    char side[512];
    seSidecarFor(mapPath, side, sizeof(side));
    remove(side);
}

static ServerSim *seSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    return sim;
}

/* ── Marked events ────────────────────────────────────────────────── */

/* A mark wide enough that a case with more than 256 events in it can still
   tell the first from the last: the low byte and the high byte of one
   number, both inside the three bytes this event type carries. */
static void seFill(GameEvent *ev, uint16_t mark) {
    memset(ev, 0, sizeof(*ev));
    ev->type    = SE_TYPE;
    ev->data[0] = (uint8_t)(mark & 0xFF);
    ev->data[1] = (uint8_t)(mark >> 8);
}

static uint16_t seMarkOf(const ScnQueuedEvent *e) {
    return (uint16_t)(e->data[0] | ((uint16_t)e->data[1] << 8));
}

/* Queued as nobody's doing: these cases drive the queue straight, with no
   sim to read an actor off. What the actor byte is for is the hook cases'
   business. */
static void seQueue(ScnEventQueue *q, uint16_t mark) {
    GameEvent ev;
    seFill(&ev, mark);
    scenarioEventsQueueGame(q, &ev, SCN_EVENT_ACTOR_NONE);
}

static void seRaise(ServerSim *sim, uint16_t mark) {
    GameEvent ev;
    seFill(&ev, mark);
    serverSimAddEvent(sim, &ev);
}

/* A control event carrying the same kind of mark. CTRL_PLAYER_LEAVE puts
   playerNum at the front of its variant, which is where the queue's copy
   starts, so the mark reads back out of data[0] exactly as a game event's
   does — the rest of the variant is zeroed, so the high byte is zero and one
   reader serves both channels. Nothing else in the server acts on a leave
   published at it: the sim is not a subscriber of its own bus, and these
   cases register no client. */
static void seFillControl(ControlEvent *evt, uint16_t mark) {
    memset(evt, 0, sizeof(*evt));
    evt->type                    = CTRL_PLAYER_LEAVE;
    evt->u.playerLeave.playerNum = (BYTE)mark;
}

static void seQueueControl(ScnEventQueue *q, uint16_t mark) {
    ControlEvent evt;
    seFillControl(&evt, mark);
    scenarioEventsQueueControl(q, &evt, SCN_EVENT_ACTOR_NONE);
}

static void sePublish(ServerSim *sim, uint16_t mark) {
    ControlEvent evt;
    seFillControl(&evt, mark);
    serverSimPublishControl(sim, &evt);
}

/* The entry n places behind the front of the queue. Cases read this before
   any tick runs: nothing consumes but the drain, the drain runs only from
   the tick callback, and the case is what decides when that happens. */
static const ScnQueuedEvent *seAt(const ScnEventQueue *q, uint16_t n) {
    return &q->entries[(q->head + n) % SCN_EVENT_QUEUE_MAX];
}

/* What a channel is called, for a failure message. */
static const char *seChannelName(uint8_t channel) {
    return (channel == SCN_EVENT_CHANNEL_CONTROL) ? "control" : "game";
}

/* ── The record a drain leaves in a case's own hands ───────────────── */

#define SE_SEEN_MAX (SCN_EVENT_QUEUE_MAX + 8)

typedef struct {
    int            count;                 /* events consumed */
    uint16_t       seen[SE_SEEN_MAX];     /* the marks, in the order consumed */
    uint8_t        channels[SE_SEEN_MAX]; /* and which channel each came in on */
    ScnEventQueue *feed;                  /* queue one of its own per event */
    int            perEvent;              /* how many, each time */
    uint16_t       nextMark;              /* what the next one is marked */
} SeRun;

static void seConsume(void *ctx, const ScnQueuedEvent *e) {
    SeRun *r = (SeRun *)ctx;
    int    i;

    if (r->count < SE_SEEN_MAX) {
        r->seen[r->count]     = seMarkOf(e);
        r->channels[r->count] = e->channel;
    }
    r->count++;

    /* A consume that causes more events is the feedback case: what it
       queues must wait for the next drain rather than be swept up by this
       one. */
    for (i = 0; i < r->perEvent && r->feed != NULL; i++) {
        seQueue(r->feed, r->nextMark++);
    }
}

/* ── The line a switched-off round sends ──────────────────────────── */

typedef struct {
    int  count;
    char last[192];
} SeText;

static void seTextCb(void *ctx, const ControlEvent *evt) {
    SeText *t = (SeText *)ctx;
    if (evt->type != CTRL_SERVER_TEXT) {
        return;
    }
    t->count++;
    snprintf(t->last, sizeof(t->last), "%s", evt->u.serverText.text);
}

/* Registration replays the current server state, so the record is cleared
   afterwards and counts only what happens next. */
static void seWatchText(ServerSim *sim, SeText *t) {
    memset(t, 0, sizeof(*t));
    (void)serverSimRegisterSubscriber(sim, seTextCb, t);
    memset(t, 0, sizeof(*t));
}

/* ── The one question the sim asks ────────────────────────────────── */

/* allow_extra_teams, asked the way the lobby asks it: through the registered
   vtable with the sim's policy depth held across the call. A script that
   raises here is how a case puts errors behind a round, and one that answers
   is how it reads whether the round is still running its Lua at all. */
static bool seAskExtraTeams(ServerSim *sim) {
    bool allow;

    if (sim->scenarioPolicy == NULL ||
        sim->scenarioPolicy->allowExtraTeams == NULL) {
        return true;
    }
    serverSimScenarioPolicyEnter(sim);
    allow = sim->scenarioPolicy->allowExtraTeams(sim->scenarioPolicy->ctx);
    serverSimScenarioPolicyLeave(sim);
    return allow;
}

/* ── 1. Published, queued, drained ────────────────────────────────── */

/* The order is read off the queue before any tick runs. Nothing empties it
 * until scnTick does, and the case is what decides when that is, so what it
 * reads there is what the subscriber copied in and in the order it copied
 * them.
 *
 * Read against what was already waiting rather than against an empty queue:
 * a round start publishes the round's rule table on its way out, and that
 * is a control event, so it is in the queue like any other. The three the
 * case raises are the three behind whatever is there.
 *
 * The counts after the tick are "at least", not "exactly", for the same
 * reason: a running tick raises whatever the world raises, on both
 * channels. What the case pins exactly is what it can — three events in, in
 * order, and a drain that took them.
 */
int run_scenario_events_queued_then_drained(void) {
    static const char *const kMap = "scnev_drained.map";
    static const char *const kLua =
        "scenario = { name = \"Queued\", api = 1 }\n";
    ServerSim           *sim;
    ScenarioHost        *h;
    const ScnEventQueue *q;
    uint32_t             queuedBase;
    uint32_t             drainedBase;
    uint16_t             waitingBase;
    char                 err[512];
    int                  i;

    UT_ASSERT(sePut(kMap, kLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);

    q = scenarioHostEventQueue(h);
    UT_ASSERT(q != NULL);
    queuedBase  = q->queued;
    drainedBase = q->drained;
    waitingBase = scenarioEventsWaiting(q);

    seRaise(sim, 11);
    seRaise(sim, 22);
    seRaise(sim, 33);

    UT_ASSERT_MSG(q->queued == queuedBase + 3,
                  "the subscriber took in %lu events, expected the three "
                  "that were published",
                  (unsigned long)(q->queued - queuedBase));
    UT_ASSERT_MSG(scenarioEventsWaiting(q) == waitingBase + 3,
                  "%u events are waiting, expected the %u already there and "
                  "three more", (unsigned)scenarioEventsWaiting(q),
                  (unsigned)waitingBase);
    UT_ASSERT_MSG(q->drained == drainedBase,
                  "%lu events were consumed before a tick ran",
                  (unsigned long)(q->drained - drainedBase));

    for (i = 0; i < 3; i++) {
        const ScnQueuedEvent *e    = seAt(q, (uint16_t)(waitingBase + i));
        uint16_t              want = (uint16_t)(11 * (i + 1));
        UT_ASSERT_MSG(seMarkOf(e) == want,
                      "the event %d places behind what was waiting is marked "
                      "%u, expected %u: they are not in publish order",
                      i, (unsigned)seMarkOf(e), (unsigned)want);
        UT_ASSERT_MSG(e->channel == SCN_EVENT_CHANNEL_GAME,
                      "the event %d places behind what was waiting came in "
                      "on the %s channel, expected game", i,
                      seChannelName(e->channel));
    }

    serverSimTick(sim);
    UT_ASSERT_MSG(q->drained >= drainedBase + 3,
                  "the tick consumed %lu events, expected at least the three "
                  "that were waiting",
                  (unsigned long)(q->drained - drainedBase));
    UT_ASSERT_MSG(q->drained <= q->queued,
                  "the numbers do not add up: %lu in, %lu out",
                  (unsigned long)q->queued, (unsigned long)q->drained);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    seDrop(kMap);
    return 0;
}

/* ── 2. The drain reads the length once ───────────────────────────── */

/* Five waiting, and a consume that queues one more each time it runs. The
 * drain must take five: the number it read when it started. If it worked off
 * the live count instead it would never finish, and this case would hang
 * rather than fail — which is what CTest's per-case timeout catches. */
#define SE_RUN     5
#define SE_FEED_AT 100

int run_scenario_events_drain_reads_the_length_once(void) {
    ScnEventQueue q;
    SeRun         run;
    uint16_t      took;
    int           i;

    scenarioEventsReset(&q);
    for (i = 0; i < SE_RUN; i++) {
        seQueue(&q, (uint16_t)i);
    }
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == SE_RUN,
                  "%u waiting, expected %d",
                  (unsigned)scenarioEventsWaiting(&q), SE_RUN);

    memset(&run, 0, sizeof(run));
    run.feed     = &q;
    run.perEvent = 1;
    run.nextMark = SE_FEED_AT;

    took = scenarioEventsDrain(&q, seConsume, &run);

    UT_ASSERT_MSG(took == SE_RUN, "the drain reported %u, expected %d",
                  (unsigned)took, SE_RUN);
    UT_ASSERT_MSG(run.count == SE_RUN,
                  "the drain consumed %d events, expected the %d that were "
                  "waiting when it started", run.count, SE_RUN);
    for (i = 0; i < SE_RUN; i++) {
        UT_ASSERT_MSG(run.seen[i] == (uint16_t)i,
                      "the event consumed %d places in is marked %u, "
                      "expected %d: the drain did not keep publish order",
                      i, (unsigned)run.seen[i], i);
    }
    UT_ASSERT_MSG(q.drained == SE_RUN, "the queue counted %lu consumed, "
                  "expected %d", (unsigned long)q.drained, SE_RUN);
    UT_ASSERT_MSG(q.queued == (uint32_t)(SE_RUN * 2),
                  "the queue took in %lu, expected the %d that started there "
                  "and the %d the consuming caused",
                  (unsigned long)q.queued, SE_RUN, SE_RUN);
    UT_ASSERT_MSG(q.dropped == 0, "%lu events were dropped",
                  (unsigned long)q.dropped);
    return 0;
}

/* ── 3. Queued during a drain, taken by the next one ──────────────── */

/* The same shape, read from the other end: what the consuming caused is
 * still waiting when the drain returns, none of it was consumed by that
 * drain, and the next drain takes exactly it, in order. */
int run_scenario_events_queued_during_a_drain_waits(void) {
    ScnEventQueue q;
    SeRun         first;
    SeRun         second;
    int           i;

    scenarioEventsReset(&q);
    for (i = 0; i < SE_RUN; i++) {
        seQueue(&q, (uint16_t)i);
    }

    memset(&first, 0, sizeof(first));
    first.feed     = &q;
    first.perEvent = 1;
    first.nextMark = SE_FEED_AT;
    (void)scenarioEventsDrain(&q, seConsume, &first);

    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == SE_RUN,
                  "%u events are waiting after the drain, expected the %d it "
                  "caused", (unsigned)scenarioEventsWaiting(&q), SE_RUN);
    for (i = 0; i < first.count; i++) {
        UT_ASSERT_MSG(first.seen[i] < SE_FEED_AT,
                      "the drain consumed the event marked %u, which it "
                      "caused itself: anything queued during a drain belongs "
                      "to the next one", (unsigned)first.seen[i]);
    }

    /* The next drain, with a consume that causes nothing. */
    memset(&second, 0, sizeof(second));
    (void)scenarioEventsDrain(&q, seConsume, &second);

    UT_ASSERT_MSG(second.count == SE_RUN,
                  "the next drain consumed %d, expected the %d left over",
                  second.count, SE_RUN);
    for (i = 0; i < SE_RUN; i++) {
        UT_ASSERT_MSG(second.seen[i] == (uint16_t)(SE_FEED_AT + i),
                      "the event consumed %d places into the next drain is "
                      "marked %u, expected %d", i, (unsigned)second.seen[i],
                      SE_FEED_AT + i);
    }
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == 0,
                  "%u events are still waiting",
                  (unsigned)scenarioEventsWaiting(&q));
    return 0;
}

/* ── 4. The queue's boundary ──────────────────────────────────────── */

/* SCN_EVENT_QUEUE_MAX in and the next one refused. The oldest is what
 * survives: the event at the front after the refusal is still the first one
 * queued, not the one that arrived with no room. */
int run_scenario_events_overflow_boundary(void) {
    ScnEventQueue q;
    SeRun         run;
    uint16_t      took;
    int           i;

    scenarioEventsReset(&q);
    for (i = 0; i < SCN_EVENT_QUEUE_MAX; i++) {
        seQueue(&q, (uint16_t)i);
    }
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == SCN_EVENT_QUEUE_MAX,
                  "%u waiting, expected the full %d",
                  (unsigned)scenarioEventsWaiting(&q), SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(q.dropped == 0,
                  "%lu were dropped filling the queue to its size",
                  (unsigned long)q.dropped);

    /* One past it. */
    seQueue(&q, (uint16_t)SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(q.dropped == 1,
                  "the event past the %dth was not dropped: %lu dropped",
                  SCN_EVENT_QUEUE_MAX, (unsigned long)q.dropped);
    UT_ASSERT_MSG(q.queued == (uint32_t)SCN_EVENT_QUEUE_MAX,
                  "the queue took in %lu, expected the %d it has room for",
                  (unsigned long)q.queued, SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == SCN_EVENT_QUEUE_MAX,
                  "%u waiting after the refusal, expected the full %d",
                  (unsigned)scenarioEventsWaiting(&q), SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(seMarkOf(&q.entries[q.head]) == 0,
                  "the event at the front is marked %u, expected the first "
                  "one queued: a full queue keeps the oldest",
                  (unsigned)seMarkOf(&q.entries[q.head]));

    /* Said once, and not again. */
    UT_ASSERT_MSG(scenarioEventsTakeDropped(&q) == 1,
                  "the drop was not reported");
    UT_ASSERT_MSG(scenarioEventsTakeDropped(&q) == 0,
                  "the same drop was reported twice");

    memset(&run, 0, sizeof(run));
    took = scenarioEventsDrain(&q, seConsume, &run);
    UT_ASSERT_MSG(took == SCN_EVENT_QUEUE_MAX,
                  "the drain ran %u, expected the %d that were waiting",
                  (unsigned)took, SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(run.count == SCN_EVENT_QUEUE_MAX,
                  "the drain consumed %d, expected %d", run.count,
                  SCN_EVENT_QUEUE_MAX);
    for (i = 0; i < SCN_EVENT_QUEUE_MAX; i++) {
        UT_ASSERT_MSG(run.seen[i] == (uint16_t)i,
                      "the event consumed %d places in is marked %u, "
                      "expected %d", i, (unsigned)run.seen[i], i);
    }
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == 0,
                  "%u events are still waiting",
                  (unsigned)scenarioEventsWaiting(&q));
    return 0;
}

/* ── 5. An overflowing tick counts one error ──────────────────────── */

/* However many events a tick loses, losing them is one thing that went
 * wrong: the operator's line says how many and the count rises by one. So a
 * round with nothing behind it plays on through an overflow, and one already
 * a single error short of the limit is switched off by the same tick.
 *
 * The drain's count is exact here and the drop count is not: the queue is
 * already full when the tick begins, so everything the running frame raises
 * on its own is dropped too, and the case asks for at least the drops it
 * forced rather than exactly them. */
int run_scenario_events_overflow_counts_errors(void) {
    static const char *const kFreshMap = "scnev_flood_fresh.map";
    static const char *const kBrinkMap = "scnev_flood_brink.map";
    static const char *const kFreshLua =
        "scenario = { name = \"Flood\", api = 1 }\n"
        "function allow_extra_teams() return false end\n";
    static const char *const kBrinkLua =
        "scenario = { name = \"Brink\", api = 1 }\n"
        "function allow_extra_teams() error(\"boom\") end\n";
    ServerSim           *sim;
    ScenarioHost        *h;
    const ScnEventQueue *q;
    SeText               text;
    char                 err[512];
    int                  i;
    int                  total = SCN_EVENT_QUEUE_MAX + SCN_ERROR_LIMIT;

    /* The first half: a round that has erred at nothing, and one tick that
       loses more events than the limit is errors. */
    UT_ASSERT(sePut(kFreshMap, kFreshLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kFreshMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    seWatchText(sim, &text);

    q = scenarioHostEventQueue(h);
    UT_ASSERT(q != NULL);
    for (i = 0; i < total; i++) {
        seRaise(sim, (uint16_t)i);
    }
    UT_ASSERT_MSG(q->dropped >= (uint32_t)SCN_ERROR_LIMIT,
                  "%lu events were dropped, expected at least the %d past "
                  "the queue's %d", (unsigned long)q->dropped,
                  SCN_ERROR_LIMIT, SCN_EVENT_QUEUE_MAX);

    serverSimTick(sim);
    UT_ASSERT_MSG(q->drained == (uint32_t)SCN_EVENT_QUEUE_MAX,
                  "the tick consumed %lu, expected the %d the queue was "
                  "holding", (unsigned long)q->drained, SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(text.count == 0,
                  "%d lines reached the game: one overflowing tick counts one "
                  "error and this round had none behind it", text.count);
    UT_ASSERT_MSG(strstr(scenarioHostLastError(h), "dropped") != NULL,
                  "the tick said nothing about what it lost: '%s'",
                  scenarioHostLastError(h));
    UT_ASSERT_MSG(!seAskExtraTeams(sim),
                  "the round answered the classic yes, so the overflow "
                  "switched the scenario off");

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    seDrop(kFreshMap);

    /* The second half: the same tick against a round one error short of the
       limit, which is the error that reaches it. */
    UT_ASSERT(sePut(kBrinkMap, kBrinkLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kBrinkMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the script was refused: %s", err);
    serverSimStartGame(sim);
    seWatchText(sim, &text);

    for (i = 1; i < SCN_ERROR_LIMIT; i++) {
        UT_ASSERT_MSG(seAskExtraTeams(sim),
                      "error %d of %d answered anything but the classic yes",
                      i, SCN_ERROR_LIMIT);
    }
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 0,
                  "%d errors in a row sent %d lines to the game; the limit is "
                  "%d", SCN_ERROR_LIMIT - 1, text.count, SCN_ERROR_LIMIT);

    q = scenarioHostEventQueue(h);
    UT_ASSERT(q != NULL);
    for (i = 0; i < total; i++) {
        seRaise(sim, (uint16_t)i);
    }
    UT_ASSERT_MSG(q->dropped > 0, "nothing was dropped filling the queue with "
                                  "%d events", total);
    serverSimTick(sim);
    UT_ASSERT_MSG(text.count == 1,
                  "%d lines reached the game, expected the one that says the "
                  "scenario is off: the overflow is the %dth error",
                  text.count, SCN_ERROR_LIMIT);
    UT_ASSERT_MSG(strstr(text.last, "Brink") != NULL,
                  "the line does not name the scenario: %s", text.last);

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    seDrop(kBrinkMap);
    return 0;
}

/* ── 6. No scenario, no event subscriber ──────────────────────────── */

/* serverSimAddEvent still runs for every event a sim raises — it is the
 * function that fills the frame's buffer — but the in-process fan-out is
 * behind a count of subscribers that asked for the event channel, so with no
 * scenario attached it reads that count, finds nothing, and never walks the
 * slots. This case reads the same count. */
int run_scenario_events_no_scenario_delivers_nothing(void) {
    static const char *const kMap = "scnev_none.map";
    static const char *const kLua =
        "scenario = { name = \"Listening\", api = 1 }\n";
    ServerSim    *sim;
    ScenarioHost *h;
    char          err[512];

    UT_ASSERT(sePut(kMap, kLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(sim->numEventSubscribers == 0,
                  "a fresh sim has %d event subscribers, expected none",
                  sim->numEventSubscribers);
    seRaise(sim, 1);
    UT_ASSERT_MSG(sim->numEventSubscribers == 0,
                  "raising an event gave the sim %d event subscribers",
                  sim->numEventSubscribers);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    UT_ASSERT_MSG(sim->numEventSubscribers == 1,
                  "an attached scenario left %d event subscribers, expected "
                  "the one it registers", sim->numEventSubscribers);

    scenarioHostDetach(h);
    UT_ASSERT_MSG(sim->numEventSubscribers == 0,
                  "a detached scenario left %d event subscribers behind",
                  sim->numEventSubscribers);
    seRaise(sim, 2);

    serverSimDestroy(sim);
    seDrop(kMap);
    return 0;
}

/* ── 7. The lobby branch drains too ───────────────────────────────── */

/* A freshly created sim sits in the lobby state, and serverSimTick calls the
 * host's per-tick callback on that branch as it does on the running one. */
int run_scenario_events_lobby_tick_drains(void) {
    static const char *const kMap = "scnev_lobby.map";
    static const char *const kLua =
        "scenario = { name = \"Lobby\", api = 1 }\n";
    ServerSim           *sim;
    ScenarioHost        *h;
    const ScnEventQueue *q;
    char                 err[512];

    UT_ASSERT(sePut(kMap, kLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "the sim is in state %d, expected the lobby",
                  (int)serverSimGetState(sim));

    q = scenarioHostEventQueue(h);
    UT_ASSERT(q != NULL);
    seRaise(sim, 7);
    seRaise(sim, 8);
    UT_ASSERT_MSG(scenarioEventsWaiting(q) == 2,
                  "%u events are waiting, expected two",
                  (unsigned)scenarioEventsWaiting(q));

    serverSimTick(sim);
    UT_ASSERT_MSG(q->drained >= 2,
                  "a lobby tick consumed %lu events, expected the two that "
                  "were waiting: the non-running branch drains as the "
                  "running one does", (unsigned long)q->drained);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "the tick left the sim in state %d",
                  (int)serverSimGetState(sim));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    seDrop(kMap);
    return 0;
}

/* ── 8. A control event arrives the same way ──────────────────────── */

/* The control channel is the one the host has always been registered on,
 * and its callback now does what the game one does: copy and return. Read,
 * like the game case, off the queue before any tick runs. */
int run_scenario_events_control_reaches_the_queue(void) {
    static const char *const kMap = "scnev_control.map";
    static const char *const kLua =
        "scenario = { name = \"Control\", api = 1 }\n";
    ServerSim           *sim;
    ScenarioHost        *h;
    const ScnEventQueue *q;
    uint32_t             queuedBase;
    uint32_t             drainedBase;
    uint16_t             waitingBase;
    const ScnQueuedEvent *e;
    char                 err[512];

    UT_ASSERT(sePut(kMap, kLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);

    q = scenarioHostEventQueue(h);
    UT_ASSERT(q != NULL);
    queuedBase  = q->queued;
    drainedBase = q->drained;
    waitingBase = scenarioEventsWaiting(q);

    sePublish(sim, 42);

    UT_ASSERT_MSG(q->queued == queuedBase + 1,
                  "the control callback took in %lu events, expected the one "
                  "that was published",
                  (unsigned long)(q->queued - queuedBase));
    e = seAt(q, waitingBase);
    UT_ASSERT_MSG(e->channel == SCN_EVENT_CHANNEL_CONTROL,
                  "the entry came in on the %s channel, expected control",
                  seChannelName(e->channel));
    UT_ASSERT_MSG(e->type == (uint8_t)CTRL_PLAYER_LEAVE,
                  "the entry's type reads %u, expected the one it was "
                  "published as", (unsigned)e->type);
    UT_ASSERT_MSG(seMarkOf(e) == 42,
                  "the entry is marked %u, expected 42: the copy did not "
                  "start at the front of the variant",
                  (unsigned)seMarkOf(e));

    serverSimTick(sim);
    UT_ASSERT_MSG(q->drained >= drainedBase + 1,
                  "the tick consumed %lu events, expected at least the one "
                  "that was waiting",
                  (unsigned long)(q->drained - drainedBase));

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    seDrop(kMap);
    return 0;
}

/* ── 9. Both channels, one order ──────────────────────────────────── */

/* Game, control, game, control. One queue takes all four in the order they
 * were published and one drain gives them back in it: a fact published once
 * arrives once, whichever channel carried it.
 *
 * Read off the queue before the tick, against what was already waiting, and
 * then off the drain's own record — the case's own array, written by the
 * case's own consume function passed to scenarioEventsDrain, which nothing
 * in the library can reach. The queue read says the two callbacks wrote in
 * publish order; the record says the drain handed them back in it. */
int run_scenario_events_both_channels_in_publish_order(void) {
    static const char *const kMap = "scnev_interleaved.map";
    static const char *const kLua =
        "scenario = { name = \"Interleaved\", api = 1 }\n";
    static const uint8_t kWantChannel[4] = {
        SCN_EVENT_CHANNEL_GAME, SCN_EVENT_CHANNEL_CONTROL,
        SCN_EVENT_CHANNEL_GAME, SCN_EVENT_CHANNEL_CONTROL
    };
    ServerSim           *sim;
    ScenarioHost        *h;
    const ScnEventQueue *q;
    uint16_t             waitingBase;
    char                 err[512];
    int                  i;

    UT_ASSERT(sePut(kMap, kLua));
    sim = seSim();
    UT_ASSERT(sim != NULL);

    h = scenarioHostAttach(sim, kMap, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the sidecar was refused: %s", err);
    serverSimStartGame(sim);

    q = scenarioHostEventQueue(h);
    UT_ASSERT(q != NULL);
    waitingBase = scenarioEventsWaiting(q);

    seRaise(sim, 1);
    sePublish(sim, 2);
    seRaise(sim, 3);
    sePublish(sim, 4);

    UT_ASSERT_MSG(scenarioEventsWaiting(q) == waitingBase + 4,
                  "%u events are waiting, expected the %u already there and "
                  "the four that were published",
                  (unsigned)scenarioEventsWaiting(q), (unsigned)waitingBase);

    for (i = 0; i < 4; i++) {
        const ScnQueuedEvent *e = seAt(q, (uint16_t)(waitingBase + i));
        UT_ASSERT_MSG(seMarkOf(e) == (uint16_t)(i + 1),
                      "the event %d places behind what was waiting is marked "
                      "%u, expected %d: the two channels did not land in "
                      "publish order", i, (unsigned)seMarkOf(e), i + 1);
        UT_ASSERT_MSG(e->channel == kWantChannel[i],
                      "the event marked %d came in on the %s channel, "
                      "expected %s", i + 1, seChannelName(e->channel),
                      seChannelName(kWantChannel[i]));
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    seDrop(kMap);

    /* And the drain hands them back in that order. Driven on a queue of the
       case's own so the record is its own too. */
    {
        ScnEventQueue own;
        SeRun         run;

        scenarioEventsReset(&own);
        seQueue(&own, 1);
        seQueueControl(&own, 2);
        seQueue(&own, 3);
        seQueueControl(&own, 4);

        memset(&run, 0, sizeof(run));
        UT_ASSERT_MSG(scenarioEventsDrain(&own, seConsume, &run) == 4,
                      "the drain did not run all four");
        UT_ASSERT_MSG(run.count == 4, "the drain consumed %d, expected four",
                      run.count);
        for (i = 0; i < 4; i++) {
            UT_ASSERT_MSG(run.seen[i] == (uint16_t)(i + 1),
                          "the event consumed %d places in is marked %u, "
                          "expected %d: one drain did not keep the order the "
                          "two channels arrived in", i,
                          (unsigned)run.seen[i], i + 1);
        }
    }
    return 0;
}

/* ── 10. Each entry says which channel ────────────────────────────── */

/* The byte is about where the host heard the fact, and it has to be read
 * before the type byte means anything: a 2 is EVENT_MINE_PLACED on one
 * channel and a different event entirely on the other. */
int run_scenario_events_channel_says_which(void) {
    ScnEventQueue q;
    SeRun         run;
    int           i;

    scenarioEventsReset(&q);
    seQueue(&q, 10);
    seQueueControl(&q, 11);
    seQueue(&q, 12);

    UT_ASSERT_MSG(seAt(&q, 0)->channel == SCN_EVENT_CHANNEL_GAME,
                  "the first entry reads %s, expected game",
                  seChannelName(seAt(&q, 0)->channel));
    UT_ASSERT_MSG(seAt(&q, 1)->channel == SCN_EVENT_CHANNEL_CONTROL,
                  "the second entry reads %s, expected control",
                  seChannelName(seAt(&q, 1)->channel));
    UT_ASSERT_MSG(seAt(&q, 2)->channel == SCN_EVENT_CHANNEL_GAME,
                  "the third entry reads %s, expected game",
                  seChannelName(seAt(&q, 2)->channel));

    UT_ASSERT_MSG(seAt(&q, 0)->type == (uint8_t)SE_TYPE,
                  "the first entry's type reads %u, expected the game event "
                  "it was queued as", (unsigned)seAt(&q, 0)->type);
    UT_ASSERT_MSG(seAt(&q, 1)->type == (uint8_t)CTRL_PLAYER_LEAVE,
                  "the second entry's type reads %u, expected the control "
                  "event it was queued as", (unsigned)seAt(&q, 1)->type);

    /* The channel survives the drain, which is where a consume reads it. */
    memset(&run, 0, sizeof(run));
    UT_ASSERT(scenarioEventsDrain(&q, seConsume, &run) == 3);
    for (i = 0; i < 3; i++) {
        uint8_t want = (i == 1) ? SCN_EVENT_CHANNEL_CONTROL
                                : SCN_EVENT_CHANNEL_GAME;
        UT_ASSERT_MSG(run.channels[i] == want,
                      "the entry consumed %d places in reads %s, expected "
                      "%s", i, seChannelName(run.channels[i]),
                      seChannelName(want));
    }
    return 0;
}

/* ── 11. The bound covers both channels ───────────────────────────── */

/* A queue filled from the two channels together behaves as one filled from
 * either: it takes SCN_EVENT_QUEUE_MAX, refuses the next of whichever
 * channel offers it, keeps the oldest, and drains the whole of what it
 * took. */
int run_scenario_events_overflow_covers_both(void) {
    ScnEventQueue q;
    SeRun         run;
    uint16_t      took;
    int           i;

    scenarioEventsReset(&q);
    for (i = 0; i < SCN_EVENT_QUEUE_MAX; i++) {
        if ((i & 1) == 0) {
            seQueue(&q, (uint16_t)i);
        } else {
            seQueueControl(&q, (uint16_t)i);
        }
    }
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == SCN_EVENT_QUEUE_MAX,
                  "%u waiting, expected the full %d",
                  (unsigned)scenarioEventsWaiting(&q), SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(q.dropped == 0,
                  "%lu were dropped filling the queue from both channels",
                  (unsigned long)q.dropped);

    /* One more of each, and neither finds room. */
    seQueue(&q, 900);
    seQueueControl(&q, 901);
    UT_ASSERT_MSG(q.dropped == 2,
                  "%lu were dropped, expected the one from each channel that "
                  "arrived with the queue full", (unsigned long)q.dropped);
    UT_ASSERT_MSG(q.queued == (uint32_t)SCN_EVENT_QUEUE_MAX,
                  "the queue took in %lu, expected the %d it has room for",
                  (unsigned long)q.queued, SCN_EVENT_QUEUE_MAX);
    UT_ASSERT_MSG(seMarkOf(seAt(&q, 0)) == 0,
                  "the event at the front is marked %u, expected the first "
                  "one queued: a full queue keeps the oldest",
                  (unsigned)seMarkOf(seAt(&q, 0)));
    UT_ASSERT_MSG(scenarioEventsTakeDropped(&q) == 2,
                  "the two drops were not reported together");
    UT_ASSERT_MSG(scenarioEventsTakeDropped(&q) == 0,
                  "the same drops were reported twice");

    memset(&run, 0, sizeof(run));
    took = scenarioEventsDrain(&q, seConsume, &run);
    UT_ASSERT_MSG(took == SCN_EVENT_QUEUE_MAX,
                  "the drain ran %u, expected the %d that were waiting",
                  (unsigned)took, SCN_EVENT_QUEUE_MAX);
    for (i = 0; i < SCN_EVENT_QUEUE_MAX; i++) {
        uint8_t want = ((i & 1) == 0) ? SCN_EVENT_CHANNEL_GAME
                                      : SCN_EVENT_CHANNEL_CONTROL;
        UT_ASSERT_MSG(run.seen[i] == (uint16_t)i,
                      "the event consumed %d places in is marked %u, "
                      "expected %d", i, (unsigned)run.seen[i], i);
        UT_ASSERT_MSG(run.channels[i] == want,
                      "the event consumed %d places in reads %s, expected %s",
                      i, seChannelName(run.channels[i]),
                      seChannelName(want));
    }
    UT_ASSERT_MSG(scenarioEventsWaiting(&q) == 0,
                  "%u events are still waiting",
                  (unsigned)scenarioEventsWaiting(&q));
    return 0;
}
