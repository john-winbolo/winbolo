/*
 * The announce policy: what the scenario lets the newswire say.
 *
 * Before a newswire-worthy fact goes to players the server asks the policy,
 * and the answer rides with the fact as one quiet byte — data[3] on the two
 * capture events, data[2] on the builder death, a field in the variant struct
 * of the four roster control events. Every client line site reads that byte
 * before it writes a line. The one fact the server writes as text, a vote
 * line, has no byte and no event: the publish is skipped instead.
 *
 * Pinned here:
 *   1. With no policy registered every fact carries quiet 0 — the classic
 *      newswire, byte for byte.
 *   2. The policy is asked once per fact, with the kind, subject and actor
 *      Appendix D's table names.
 *   3. A policy that answers false stamps 1 on every one of them.
 *   4. Every client line site reads the byte: the line appears at 0 and is
 *      held at 1, across all four surfaces that draw one.
 *   5. The byte survives the wire. Expected bytes are written out by hand
 *      rather than round-tripped through the encoder, so encoder and decoder
 *      drifting together fails here instead of agreeing with itself.
 *   6. A pill capture draws exactly one line, now that pillsSetPillOwner
 *      writes none of its own.
 *   7. A vote line the policy turns down is never published, and the
 *      surrender itself still goes ahead.
 *   8. Loopback: a remote client over real sockets takes delivery of the
 *      capture either way, and writes a line only for the announced one.
 *      Arrival is proved off the client's own received-event buffer before
 *      either leg looks at the line, so "no line" can never be an event that
 *      never turned up. The capture is made from the scenario tick hook
 *      because the frame's event buffer is cleared at the top of every
 *      running tick — one raised between two pumps is wiped before the drain
 *      that would have sent it.
 *
 * Alliance changes carry the byte but have no client line site reading it:
 * the client draws no newswire line for an alliance today. The byte is
 * asserted on the event and on the wire, which is all there is to assert.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"    /* eventCount, trackBuf, serverSimGameVoteToggle */
#include "server_sim_lifecycle.h"   /* serverSimSetLobbyEnabled, serverSimSetTeam */
#include "scenario_defs.h"          /* ScenarioPolicy, ANNOUNCE_KIND_* */
#include "server_sim_scenario.h"    /* serverSimSetScenarioPolicy */
#include "control_event.h"
#include "client_sim.h"
#include "client_sim_internal.h"    /* cs->inLobby — the lobby chat lines need it */
#include "client_sim_control.h"     /* clientSimApplyControl */
#include "client_net.h"             /* clientSimGetConnectState */
#include "client_connect_state.h"   /* CLIENT_CONNECT_CONNECTED */
#include "game_sim.h"
#include "input_packet.h"           /* GameEvent, EVENT_*, gameEventDataSize */
#include "bases.h"
#include "pillbox.h"
#include "lgm.h"
#include "players.h"                /* playersGetPlayerName — the roster still applies */
#include "messages.h"               /* messageType, newsWireMessage, messageSetNewswire */
#include "attribution_track.h"      /* AttrLgmRecord — the funnel's moved offsets */
#include "netpacks.h"               /* GAME_VOTE_KIND_*, GAME_VOTE_TOGGLE_* */
#include "transport_control_codec.h"
#include "transport_udp_internal.h" /* packGameEvent / unpackGameEvent */
#include "wire_limits.h"            /* PACKET_MAX_PLAYER_NAME */
#include "test_harness.h"
#include "loopback_harness.h"

#define SA_SELF   0   /* the local slot in every client fixture */
#define SA_OTHER  1   /* the slot the facts below are about */
#define SA_THIRD  2

/* ----------------------------------------------------------------
 * A policy that records every ask and answers as the case sets it.
 * ---------------------------------------------------------------- */

#define SA_KIND_COUNT 8

typedef struct {
    bool allow;                     /* what announce answers */
    int  asks[SA_KIND_COUNT];       /* how many times each kind was asked */
    BYTE lastSubject[SA_KIND_COUNT];
    BYTE lastActor[SA_KIND_COUNT];
    bool sawInPolicy;               /* the enter/leave bracket was armed */
} SaCtx;

/* The sim the policy belongs to, so the bracket can be observed from inside
 * the answer. One test process runs one case at a time. */
static ServerSim *s_saSim;

static bool saAnnounce(void *ctx, BYTE kind, BYTE subject, BYTE actor) {
    SaCtx *p = (SaCtx *)ctx;
    if (kind < SA_KIND_COUNT) {
        p->asks[kind]++;
        p->lastSubject[kind] = subject;
        p->lastActor[kind] = actor;
    }
    if (s_saSim != NULL && s_saSim->inScenarioPolicy) {
        p->sawInPolicy = true;
    }
    return p->allow;
}

static void saFillPolicy(ScenarioPolicy *pol, SaCtx *pc, bool allow) {
    memset(pol, 0, sizeof(*pol));
    memset(pc, 0, sizeof(*pc));
    pc->allow = allow;
    pol->announce = saAnnounce;
    pol->ctx = pc;
}

/* ----------------------------------------------------------------
 * A subscriber that keeps both channels' events whole.
 * ---------------------------------------------------------------- */

#define SA_MAX_KEPT 64

typedef struct {
    int          controlCount;
    ControlEvent control[SA_MAX_KEPT];
    int          eventCount;
    GameEvent    events[SA_MAX_KEPT];
} SaSink;

static void saDeliverControl(void *ctx, const ControlEvent *evt) {
    SaSink *s = (SaSink *)ctx;
    if (s->controlCount < SA_MAX_KEPT) {
        s->control[s->controlCount] = *evt;
    }
    s->controlCount++;
}

static void saDeliverEvent(void *ctx, const GameEvent *evt) {
    SaSink *s = (SaSink *)ctx;
    if (s->eventCount < SA_MAX_KEPT) {
        s->events[s->eventCount] = *evt;
    }
    s->eventCount++;
}

static const ControlEvent *saFindControl(const SaSink *s, ControlEventType t) {
    int kept = s->controlCount < SA_MAX_KEPT ? s->controlCount : SA_MAX_KEPT;
    int i;
    for (i = 0; i < kept; i++) {
        if (s->control[i].type == t) return &s->control[i];
    }
    return NULL;
}

static const GameEvent *saFindEvent(const SaSink *s, BYTE type) {
    int kept = s->eventCount < SA_MAX_KEPT ? s->eventCount : SA_MAX_KEPT;
    int i;
    for (i = 0; i < kept; i++) {
        if (s->events[i].type == type) return &s->events[i];
    }
    return NULL;
}

static SubscriberHandle saSubscribe(ServerSim *sim, SaSink *s) {
    SubscriberHandle h;
    memset(s, 0, sizeof(*s));
    h = serverSimRegisterSubscriber(sim, saDeliverControl, s);
    if (h != SUBSCRIBER_HANDLE_INVALID) {
        if (!serverSimSetSubscriberEventDeliver(sim, h, saDeliverEvent)) {
            return SUBSCRIBER_HANDLE_INVALID;
        }
    }
    /* Registration replays the current state through the control callback;
       clear so the sink measures only what happens next. */
    memset(s, 0, sizeof(*s));
    return h;
}

static void saDrain(ServerSim *sim, SaSink *s) {
    sim->eventCount = 0;
    memset(s, 0, sizeof(*s));
}

/* ----------------------------------------------------------------
 * Driving the facts.
 * ---------------------------------------------------------------- */

/* A base and a pillbox, each taken from nobody by SA_SELF, and a builder
   killed by SA_OTHER. Run against a sim whose subscriber is `s`. */
static void saTakeBase(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    (*gs->bs).item[0].owner = NEUTRAL;
    basesSetBaseOwner(gs, 1, SA_SELF, FALSE, FALSE);
}

static void saTakePill(ServerSim *sim) {
    GameSim *gs = serverSimGetGameSim(sim);
    (*gs->pb).item[0].owner = NEUTRAL;
    pillsSetPillOwner(gs, &gs->pb, 1, SA_SELF, FALSE);
}

static bool saKillBuilder(ServerSim *sim, BYTE victim, BYTE killer) {
    GameSim *gs = serverSimGetGameSim(sim);
    if (gs->lgmen[victim] == NULL) return false;
    lgmKill(gs, &gs->lgmen[victim], &gs->tanks[victim], killer);
    return true;
}

/* ----------------------------------------------------------------
 * A client that counts the lines it was asked to draw.
 * ---------------------------------------------------------------- */

static int s_saLines;

static void saSpyMessageAdd(void *ctx, messageType msgType, langid topId,
                            langid bodyId, const MessageArgs *args) {
    (void)ctx; (void)topId; (void)bodyId; (void)args;
    if (msgType == newsWireMessage) {
        s_saLines++;
    }
}

/* A human client seated at SA_SELF with the newswire on and the spy
   installed. Caller destroys it. */
static ClientSim *saMakeClient(void) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, SA_SELF);
    clientSimSetIsBot(cs, false);
    messageSetNewswire(clientSimGetMessages(cs), true);
    gs = clientSimGetGameSim(cs);
    gs->callbacks.messageAdd = saSpyMessageAdd;
    return cs;
}

/* One game event through the client, with the quiet byte at `quietIdx`. */
static void saClientGameEvent(ClientSim *cs, BYTE type, BYTE d0, BYTE d1,
                              BYTE d2, BYTE d3) {
    GameEvent e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.data[0] = d0;
    e.data[1] = d1;
    e.data[2] = d2;
    e.data[3] = d3;
    clientSimApplyGameEvents(cs, &e, 1, SA_SELF);
}

static void saFillJoin(ControlEvent *evt, BYTE slot, const char *name,
                       BYTE quiet) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_PLAYER_JOIN;
    evt->u.playerJoin.playerNum = slot;
    SDL_strlcpy(evt->u.playerJoin.name, name,
                sizeof(evt->u.playerJoin.name));
    evt->u.playerJoin.country[0] = 'A';
    evt->u.playerJoin.country[1] = 'U';
    evt->u.playerJoin.quiet = quiet;
}

static void saFillLeave(ControlEvent *evt, BYTE slot, const char *name,
                        BYTE quiet) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_PLAYER_LEAVE;
    evt->u.playerLeave.playerNum = slot;
    SDL_strlcpy(evt->u.playerLeave.name, name,
                sizeof(evt->u.playerLeave.name));
    evt->u.playerLeave.country[0] = 'A';
    evt->u.playerLeave.country[1] = 'U';
    evt->u.playerLeave.quiet = quiet;
}

static void saFillRename(ControlEvent *evt, BYTE slot, const char *name,
                         BYTE quiet) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_PLAYER_NAME;
    evt->u.playerName.playerNum = slot;
    SDL_strlcpy(evt->u.playerName.name, name,
                sizeof(evt->u.playerName.name));
    evt->u.playerName.quiet = quiet;
}

/* ================================================================
 * 1. No policy is the classic newswire.
 * ================================================================ */
int run_scenario_announce_null_policy_is_classic(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    SaSink sink;
    SubscriberHandle h;
    const ControlEvent *ce;
    const GameEvent *ge;

    UT_ASSERT(sim != NULL);
    h = saSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* A join, an alliance, a rename and a departure. */
    serverSimAddPlayer(sim, SA_OTHER, "Bob", false);
    ce = saFindControl(&sink, CTRL_PLAYER_JOIN);
    UT_ASSERT_MSG(ce != NULL, "no join event was published");
    UT_ASSERT_MSG(ce->u.playerJoin.quiet == 0,
                  "a join with no policy carried quiet %u",
                  (unsigned)ce->u.playerJoin.quiet);

    saDrain(sim, &sink);
    serverSimAcceptAlliance(sim, SA_SELF, SA_OTHER);
    ce = saFindControl(&sink, CTRL_ALLIANCE_ACCEPT);
    UT_ASSERT_MSG(ce != NULL, "no alliance-accept event was published");
    UT_ASSERT_MSG(ce->u.allianceAccept.quiet == 0,
                  "an alliance accept with no policy carried quiet %u",
                  (unsigned)ce->u.allianceAccept.quiet);

    saDrain(sim, &sink);
    serverSimLeaveAlliance(sim, SA_OTHER);
    ce = saFindControl(&sink, CTRL_ALLIANCE_LEAVE);
    UT_ASSERT_MSG(ce != NULL, "no alliance-leave event was published");
    UT_ASSERT_MSG(ce->u.allianceLeave.quiet == 0,
                  "an alliance leave with no policy carried quiet %u",
                  (unsigned)ce->u.allianceLeave.quiet);

    saDrain(sim, &sink);
    serverSimSetPlayerName(sim, SA_OTHER, "Robert");
    ce = saFindControl(&sink, CTRL_PLAYER_NAME);
    UT_ASSERT_MSG(ce != NULL, "no rename event was published");
    UT_ASSERT_MSG(ce->u.playerName.quiet == 0,
                  "a rename with no policy carried quiet %u",
                  (unsigned)ce->u.playerName.quiet);

    /* The two captures and the builder death. */
    saDrain(sim, &sink);
    saTakeBase(sim);
    ge = saFindEvent(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT_MSG(ge != NULL, "no base-capture event was published");
    UT_ASSERT_MSG(ge->data[3] == 0, "a base capture carried quiet %u",
                  (unsigned)ge->data[3]);

    saDrain(sim, &sink);
    saTakePill(sim);
    ge = saFindEvent(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT_MSG(ge != NULL, "no pill-capture event was published");
    UT_ASSERT_MSG(ge->data[3] == 0, "a pill capture carried quiet %u",
                  (unsigned)ge->data[3]);

    saDrain(sim, &sink);
    UT_ASSERT_MSG(saKillBuilder(sim, SA_OTHER, SA_SELF),
                  "the fixture has no builder to kill");
    ge = saFindEvent(&sink, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ge != NULL, "no builder-lost event was published");
    UT_ASSERT_MSG(ge->data[2] == 0, "a builder loss carried quiet %u",
                  (unsigned)ge->data[2]);

    saDrain(sim, &sink);
    serverSimRemovePlayer(sim, SA_OTHER);
    ce = saFindControl(&sink, CTRL_PLAYER_LEAVE);
    UT_ASSERT_MSG(ce != NULL, "no leave event was published");
    UT_ASSERT_MSG(ce->u.playerLeave.quiet == 0,
                  "a departure with no policy carried quiet %u",
                  (unsigned)ce->u.playerLeave.quiet);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. What each kind is asked about.
 * ================================================================ */
int run_scenario_announce_policy_asked_per_kind(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    ScenarioPolicy pol;
    SaCtx pc;
    SaSink sink;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    s_saSim = sim;
    saFillPolicy(&pol, &pc, /*allow*/ true);
    serverSimSetScenarioPolicy(sim, &pol);
    h = saSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    serverSimAddPlayer(sim, SA_OTHER, "Bob", false);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_JOINED] >= 1,
                  "the join asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_JOINED]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_JOINED] == SA_OTHER &&
                      pc.lastActor[ANNOUNCE_KIND_JOINED] == SA_OTHER,
                  "join asked about %u/%u, want the joining slot for both",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_JOINED],
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_JOINED]);

    serverSimAcceptAlliance(sim, SA_SELF, SA_OTHER);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_ALLIANCE] >= 1,
                  "the alliance accept asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_ALLIANCE]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_ALLIANCE] == SA_OTHER &&
                      pc.lastActor[ANNOUNCE_KIND_ALLIANCE] == SA_SELF,
                  "alliance accept asked about %u/%u, want new member / accepter",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_ALLIANCE],
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_ALLIANCE]);

    serverSimSetPlayerName(sim, SA_OTHER, "Robert");
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_NAME_CHANGED] >= 1,
                  "the rename asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_NAME_CHANGED]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_NAME_CHANGED] == SA_OTHER &&
                      pc.lastActor[ANNOUNCE_KIND_NAME_CHANGED] == SA_OTHER,
                  "rename asked about %u/%u, want the renamed slot for both",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_NAME_CHANGED],
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_NAME_CHANGED]);

    saDrain(sim, &sink);
    saTakeBase(sim);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_BASE_CAPTURED] == 1,
                  "the base capture asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_BASE_CAPTURED]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_BASE_CAPTURED] == 0,
                  "base capture asked about index %u, want the 0-based slot",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_BASE_CAPTURED]);
    UT_ASSERT_MSG(pc.lastActor[ANNOUNCE_KIND_BASE_CAPTURED] == SA_SELF,
                  "base capture named actor %u, want the taker",
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_BASE_CAPTURED]);

    saTakePill(sim);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_PILL_CAPTURED] == 1,
                  "the pill capture asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_PILL_CAPTURED]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_PILL_CAPTURED] == 0,
                  "pill capture asked about index %u, want the 0-based slot",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_PILL_CAPTURED]);
    UT_ASSERT_MSG(pc.lastActor[ANNOUNCE_KIND_PILL_CAPTURED] == SA_SELF,
                  "pill capture named actor %u, want the taker",
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_PILL_CAPTURED]);

    UT_ASSERT_MSG(saKillBuilder(sim, SA_OTHER, SA_SELF),
                  "the fixture has no builder to kill");
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_BUILDER_LOST] == 1,
                  "the builder death asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_BUILDER_LOST]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_BUILDER_LOST] == SA_OTHER &&
                      pc.lastActor[ANNOUNCE_KIND_BUILDER_LOST] == SA_SELF,
                  "builder loss asked about %u/%u, want victim / killer",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_BUILDER_LOST],
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_BUILDER_LOST]);

    serverSimRemovePlayer(sim, SA_OTHER);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_LEFT] >= 1,
                  "the departure asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_LEFT]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_LEFT] == SA_OTHER &&
                      pc.lastActor[ANNOUNCE_KIND_LEFT] == SA_OTHER,
                  "departure asked about %u/%u, want the leaving slot for both",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_LEFT],
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_LEFT]);

    /* Every one of those ran inside the policy bracket, which is what stops a
       policy writing back through the op funnel while it is answering. */
    UT_ASSERT_MSG(pc.sawInPolicy,
                  "no announce call ran between the policy enter and leave");
    UT_ASSERT_MSG(!sim->inScenarioPolicy,
                  "the policy bracket was left armed after the last answer");

    serverSimSetScenarioPolicy(sim, NULL);
    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    s_saSim = NULL;
    return 0;
}

/* ================================================================
 * 3. A policy that says no stamps every fact.
 * ================================================================ */
int run_scenario_announce_quiet_stamped_on_every_fact(void) {
    ServerSim *sim = ut_make_running_sim("Holder");
    ScenarioPolicy pol;
    SaCtx pc;
    SaSink sink;
    SubscriberHandle h;
    const ControlEvent *ce;
    const GameEvent *ge;
    AttrLgmRecord r;
    BYTE lgmX, lgmY;

    UT_ASSERT(sim != NULL);
    s_saSim = sim;
    saFillPolicy(&pol, &pc, /*allow*/ false);
    serverSimSetScenarioPolicy(sim, &pol);
    h = saSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    serverSimAddPlayer(sim, SA_OTHER, "Bob", false);
    ce = saFindControl(&sink, CTRL_PLAYER_JOIN);
    UT_ASSERT_MSG(ce != NULL && ce->u.playerJoin.quiet == 1,
                  "a refused join carried quiet %u",
                  ce ? (unsigned)ce->u.playerJoin.quiet : 99u);

    saDrain(sim, &sink);
    serverSimAcceptAlliance(sim, SA_SELF, SA_OTHER);
    ce = saFindControl(&sink, CTRL_ALLIANCE_ACCEPT);
    UT_ASSERT_MSG(ce != NULL && ce->u.allianceAccept.quiet == 1,
                  "a refused alliance accept carried quiet %u",
                  ce ? (unsigned)ce->u.allianceAccept.quiet : 99u);

    saDrain(sim, &sink);
    serverSimLeaveAlliance(sim, SA_OTHER);
    ce = saFindControl(&sink, CTRL_ALLIANCE_LEAVE);
    UT_ASSERT_MSG(ce != NULL && ce->u.allianceLeave.quiet == 1,
                  "a refused alliance leave carried quiet %u",
                  ce ? (unsigned)ce->u.allianceLeave.quiet : 99u);

    saDrain(sim, &sink);
    serverSimSetPlayerName(sim, SA_OTHER, "Robert");
    ce = saFindControl(&sink, CTRL_PLAYER_NAME);
    UT_ASSERT_MSG(ce != NULL && ce->u.playerName.quiet == 1,
                  "a refused rename carried quiet %u",
                  ce ? (unsigned)ce->u.playerName.quiet : 99u);

    saDrain(sim, &sink);
    saTakeBase(sim);
    ge = saFindEvent(&sink, EVENT_BASE_CAPTURED);
    UT_ASSERT_MSG(ge != NULL && ge->data[3] == 1,
                  "a refused base capture carried quiet %u",
                  ge ? (unsigned)ge->data[3] : 99u);
    UT_ASSERT_MSG(ge->data[0] == SA_SELF && ge->data[1] == NEUTRAL,
                  "the owners moved: %u/%u", (unsigned)ge->data[0],
                  (unsigned)ge->data[1]);

    saDrain(sim, &sink);
    saTakePill(sim);
    ge = saFindEvent(&sink, EVENT_PILL_CAPTURED);
    UT_ASSERT_MSG(ge != NULL && ge->data[3] == 1,
                  "a refused pill capture carried quiet %u",
                  ge ? (unsigned)ge->data[3] : 99u);

    /* The builder death, whose payload grew: [victim, killer, quiet] on the
       wire, the man's cell behind it. Both are checked, and so is the stats
       funnel that reads the cell from its new offsets. */
    saDrain(sim, &sink);
    sim->trackLen = 0;
    sim->trackRecordCount = 0;
    sim->trackTruncated = false;
    UT_ASSERT_MSG(saKillBuilder(sim, SA_OTHER, SA_SELF),
                  "the fixture has no builder to kill");
    ge = saFindEvent(&sink, EVENT_LGM_LOST);
    UT_ASSERT_MSG(ge != NULL, "no builder-lost event was published");
    UT_ASSERT_MSG(ge->data[0] == SA_OTHER && ge->data[1] == SA_SELF,
                  "victim/killer read %u/%u", (unsigned)ge->data[0],
                  (unsigned)ge->data[1]);
    UT_ASSERT_MSG(ge->data[2] == 1, "a refused builder loss carried quiet %u",
                  (unsigned)ge->data[2]);
    UT_ASSERT_MSG(gameEventDataSize(EVENT_LGM_LOST) == 3,
                  "EVENT_LGM_LOST puts %d bytes on the wire, want 3",
                  gameEventDataSize(EVENT_LGM_LOST));

    lgmX = ge->data[3];
    lgmY = ge->data[4];
    UT_ASSERT_MSG(sim->trackLen >= sizeof r,
                  "the builder death appended %u bytes of record",
                  (unsigned)sim->trackLen);
    memcpy(&r, sim->trackBuf, sizeof r);
    UT_ASSERT_MSG(r.type == ATTR_REC_LGM, "record tag %u", (unsigned)r.type);
    UT_ASSERT_MSG(r.victim == SA_OTHER && r.killer == SA_SELF,
                  "record slots %u/%u", (unsigned)r.victim,
                  (unsigned)r.killer);
    UT_ASSERT_MSG(r.mapX == lgmX && r.mapY == lgmY,
                  "the funnel read the cell as %u,%u; the event carries %u,%u",
                  (unsigned)r.mapX, (unsigned)r.mapY, (unsigned)lgmX,
                  (unsigned)lgmY);

    saDrain(sim, &sink);
    serverSimRemovePlayer(sim, SA_OTHER);
    ce = saFindControl(&sink, CTRL_PLAYER_LEAVE);
    UT_ASSERT_MSG(ce != NULL && ce->u.playerLeave.quiet == 1,
                  "a refused departure carried quiet %u",
                  ce ? (unsigned)ce->u.playerLeave.quiet : 99u);

    serverSimSetScenarioPolicy(sim, NULL);
    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    s_saSim = NULL;
    return 0;
}

/* ================================================================
 * 4. Every client line site reads the byte.
 * ================================================================ */
int run_scenario_announce_client_lines_read_the_byte(void) {
    ClientSim *cs = saMakeClient();
    GameSim *gs;
    ControlEvent evt;
    size_t chatBefore;
    int i;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs != NULL);

    /* ── the two captures ──────────────────────────────────────── */
    s_saLines = 0;
    saClientGameEvent(cs, EVENT_PILL_CAPTURED, SA_OTHER, NEUTRAL, 0, /*quiet*/ 0);
    UT_ASSERT_MSG(s_saLines == 1,
                  "an announced pill capture drew %d lines, want 1", s_saLines);

    s_saLines = 0;
    saClientGameEvent(cs, EVENT_PILL_CAPTURED, SA_THIRD, NEUTRAL, 0, /*quiet*/ 1);
    UT_ASSERT_MSG(s_saLines == 0,
                  "a quiet pill capture drew %d lines", s_saLines);

    /* The base line debounces rather than emitting on the spot, so the queue
       is pumped afterwards: a line held for later is still a line. */
    s_saLines = 0;
    saClientGameEvent(cs, EVENT_BASE_CAPTURED, SA_OTHER, NEUTRAL, 0, /*quiet*/ 0);
    for (i = 0; i < 600; i++) basesTickMessageQueue(gs, cs);
    UT_ASSERT_MSG(s_saLines == 1,
                  "an announced base capture drew %d lines, want 1", s_saLines);

    s_saLines = 0;
    saClientGameEvent(cs, EVENT_BASE_CAPTURED, SA_THIRD, NEUTRAL, 0, /*quiet*/ 1);
    for (i = 0; i < 600; i++) basesTickMessageQueue(gs, cs);
    UT_ASSERT_MSG(s_saLines == 0,
                  "a quiet base capture drew %d lines, including any the "
                  "debounce held for later", s_saLines);

    /* ── the builder death, whose byte sits at data[2] ─────────── */
    s_saLines = 0;
    saClientGameEvent(cs, EVENT_LGM_LOST, SA_OTHER, SA_THIRD, /*quiet*/ 0, 0);
    UT_ASSERT_MSG(s_saLines == 1,
                  "an announced builder loss drew %d lines, want 1", s_saLines);

    s_saLines = 0;
    saClientGameEvent(cs, EVENT_LGM_LOST, SA_OTHER, SA_THIRD, /*quiet*/ 1, 0);
    UT_ASSERT_MSG(s_saLines == 0,
                  "a quiet builder loss drew %d lines", s_saLines);

    /* ── joined and left, whose line is the lobby chat one ─────── */
    cs->inLobby = true;
    clientSimClearLobbyChatHistory(cs);

    saFillJoin(&evt, SA_OTHER, "Bob", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(strstr(clientSimGetLobbyChatHistory(cs), "Bob") != NULL,
                  "an announced join wrote no lobby line");

    chatBefore = strlen(clientSimGetLobbyChatHistory(cs));
    saFillJoin(&evt, SA_THIRD, "Carol", /*quiet*/ 1);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(strlen(clientSimGetLobbyChatHistory(cs)) == chatBefore,
                  "a quiet join wrote a lobby line");
    UT_ASSERT_MSG(strstr(clientSimGetLobbyChatHistory(cs), "Carol") == NULL,
                  "a quiet join named the joiner anyway");

    /* The roster itself is applied either way — only the line is withheld. */
    {
        char rosterName[PLAYER_NAME_LEN];
        memset(rosterName, 0, sizeof(rosterName));
        playersGetPlayerName(&gs->plyrs, SA_THIRD, rosterName,
                             sizeof(rosterName), FALSE);
        UT_ASSERT_MSG(strstr(rosterName, "Carol") != NULL,
                      "a quiet join left slot %d holding '%s' rather than "
                      "the joiner's name", SA_THIRD, rosterName);
    }

    chatBefore = strlen(clientSimGetLobbyChatHistory(cs));
    saFillLeave(&evt, SA_THIRD, "Carol", /*quiet*/ 1);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(strlen(clientSimGetLobbyChatHistory(cs)) == chatBefore,
                  "a quiet departure wrote a lobby line");

    saFillLeave(&evt, SA_OTHER, "Bob", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(strlen(clientSimGetLobbyChatHistory(cs)) > chatBefore,
                  "an announced departure wrote no lobby line");

    /* ── the same departure in game, where the line is the newswire ── */
    cs->inLobby = false;
    saFillJoin(&evt, SA_OTHER, "Bob", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);
    s_saLines = 0;
    saFillLeave(&evt, SA_OTHER, "Bob", /*quiet*/ 1);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(s_saLines == 0,
                  "a quiet in-game departure drew %d newswire lines",
                  s_saLines);

    saFillJoin(&evt, SA_OTHER, "Bob", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);
    s_saLines = 0;
    saFillLeave(&evt, SA_OTHER, "Bob", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(s_saLines == 1,
                  "an announced in-game departure drew %d newswire lines, "
                  "want 1", s_saLines);

    /* ── the rename ────────────────────────────────────────────── */
    saFillJoin(&evt, SA_OTHER, "Bob", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);

    s_saLines = 0;
    saFillRename(&evt, SA_OTHER, "Robert", /*quiet*/ 1);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(s_saLines == 0, "a quiet rename drew %d newswire lines",
                  s_saLines);

    s_saLines = 0;
    saFillRename(&evt, SA_OTHER, "Bobby", /*quiet*/ 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(s_saLines == 1,
                  "an announced rename drew %d newswire lines, want 1",
                  s_saLines);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 5. The byte survives the wire.
 * ================================================================ */

/* Expected bytes are written out here rather than taken from the encoder, so
   an encoder and a decoder that drift together fail instead of agreeing. */
int run_scenario_announce_wire_bytes(void) {
    ControlEvent in;
    ControlEvent out;
    ControlEncodeBodyFn enc;
    ControlDecodeBodyFn dec;
    uint8_t buf[MAX_CONTROL_PACKET];
    uint8_t want[MAX_CONTROL_PACKET];
    size_t outLen;
    size_t wantLen;
    size_t i;

    /* ── CTRL_ALLIANCE_ACCEPT: [acceptedBy, newMember, quiet] ──── */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_ALLIANCE_ACCEPT;
    in.u.allianceAccept.acceptedBy = 3;
    in.u.allianceAccept.newMember  = 7;
    in.u.allianceAccept.quiet      = 1;

    memset(want, 0, sizeof(want));
    want[0] = 3; want[1] = 7; want[2] = 1;
    wantLen = 3;

    enc = transportControlCodecBodyEncoder(CTRL_ALLIANCE_ACCEPT);
    UT_ASSERT(enc != NULL);
    memset(buf, 0xAA, sizeof(buf));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT_MSG(outLen == wantLen, "alliance accept encoded %u bytes, want %u",
                  (unsigned)outLen, (unsigned)wantLen);
    for (i = 0; i < wantLen; i++) {
        UT_ASSERT_MSG(buf[i] == want[i],
                      "alliance accept byte %u = 0x%02x, want 0x%02x",
                      (unsigned)i, (unsigned)buf[i], (unsigned)want[i]);
    }
    dec = transportControlCodecBodyDecoder(CTRL_ALLIANCE_ACCEPT);
    UT_ASSERT(dec != NULL);
    memset(&out, 0xEE, sizeof(out));
    UT_ASSERT(dec(buf, outLen, &out));
    UT_ASSERT_MSG(out.u.allianceAccept.quiet == 1,
                  "alliance accept decoded quiet %u",
                  (unsigned)out.u.allianceAccept.quiet);

    /* ── CTRL_ALLIANCE_LEAVE: [playerNum, 0, quiet] ───────────── */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_ALLIANCE_LEAVE;
    in.u.allianceLeave.playerNum = 5;
    in.u.allianceLeave.quiet     = 1;

    memset(want, 0, sizeof(want));
    want[0] = 5; want[1] = 0; want[2] = 1;
    wantLen = 3;

    enc = transportControlCodecBodyEncoder(CTRL_ALLIANCE_LEAVE);
    UT_ASSERT(enc != NULL);
    memset(buf, 0xAA, sizeof(buf));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT_MSG(outLen == wantLen, "alliance leave encoded %u bytes, want %u",
                  (unsigned)outLen, (unsigned)wantLen);
    for (i = 0; i < wantLen; i++) {
        UT_ASSERT_MSG(buf[i] == want[i],
                      "alliance leave byte %u = 0x%02x, want 0x%02x",
                      (unsigned)i, (unsigned)buf[i], (unsigned)want[i]);
    }
    dec = transportControlCodecBodyDecoder(CTRL_ALLIANCE_LEAVE);
    UT_ASSERT(dec != NULL);
    memset(&out, 0xEE, sizeof(out));
    UT_ASSERT(dec(buf, outLen, &out));
    UT_ASSERT_MSG(out.u.allianceLeave.quiet == 1,
                  "alliance leave decoded quiet %u",
                  (unsigned)out.u.allianceLeave.quiet);

    /* ── CTRL_PLAYER_NAME: [slot, name×64, quiet] ─────────────── */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_PLAYER_NAME;
    in.u.playerName.playerNum = 4;
    SDL_strlcpy(in.u.playerName.name, "Zed", sizeof(in.u.playerName.name));
    in.u.playerName.quiet = 1;

    memset(want, 0, sizeof(want));
    want[0] = 4;
    want[1] = 'Z'; want[2] = 'e'; want[3] = 'd';
    want[1 + PACKET_MAX_PLAYER_NAME] = 1;
    wantLen = 1 + PACKET_MAX_PLAYER_NAME + 1;

    enc = transportControlCodecBodyEncoder(CTRL_PLAYER_NAME);
    UT_ASSERT(enc != NULL);
    memset(buf, 0xAA, sizeof(buf));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT_MSG(outLen == wantLen, "rename encoded %u bytes, want %u",
                  (unsigned)outLen, (unsigned)wantLen);
    for (i = 0; i < wantLen; i++) {
        UT_ASSERT_MSG(buf[i] == want[i],
                      "rename byte %u = 0x%02x, want 0x%02x",
                      (unsigned)i, (unsigned)buf[i], (unsigned)want[i]);
    }
    dec = transportControlCodecBodyDecoder(CTRL_PLAYER_NAME);
    UT_ASSERT(dec != NULL);
    memset(&out, 0xEE, sizeof(out));
    UT_ASSERT(dec(buf, outLen, &out));
    UT_ASSERT_MSG(out.u.playerName.quiet == 1, "rename decoded quiet %u",
                  (unsigned)out.u.playerName.quiet);

    /* ── CTRL_PLAYER_LEAVE: [slot, name×64, cc×2, quiet] ──────── */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_PLAYER_LEAVE;
    in.u.playerLeave.playerNum = 6;
    SDL_strlcpy(in.u.playerLeave.name, "Ann", sizeof(in.u.playerLeave.name));
    in.u.playerLeave.country[0] = 'A';
    in.u.playerLeave.country[1] = 'U';
    in.u.playerLeave.quiet = 1;

    memset(want, 0, sizeof(want));
    want[0] = 6;
    want[1] = 'A'; want[2] = 'n'; want[3] = 'n';
    want[1 + PACKET_MAX_PLAYER_NAME]     = 'A';
    want[1 + PACKET_MAX_PLAYER_NAME + 1] = 'U';
    want[1 + PACKET_MAX_PLAYER_NAME + 2] = 1;
    wantLen = 1 + PACKET_MAX_PLAYER_NAME + 2 + 1;

    enc = transportControlCodecBodyEncoder(CTRL_PLAYER_LEAVE);
    UT_ASSERT(enc != NULL);
    memset(buf, 0xAA, sizeof(buf));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT_MSG(outLen == wantLen, "departure encoded %u bytes, want %u",
                  (unsigned)outLen, (unsigned)wantLen);
    for (i = 0; i < wantLen; i++) {
        UT_ASSERT_MSG(buf[i] == want[i],
                      "departure byte %u = 0x%02x, want 0x%02x",
                      (unsigned)i, (unsigned)buf[i], (unsigned)want[i]);
    }
    dec = transportControlCodecBodyDecoder(CTRL_PLAYER_LEAVE);
    UT_ASSERT(dec != NULL);
    memset(&out, 0xEE, sizeof(out));
    UT_ASSERT(dec(buf, outLen, &out));
    UT_ASSERT_MSG(out.u.playerLeave.quiet == 1, "departure decoded quiet %u",
                  (unsigned)out.u.playerLeave.quiet);

    /* ── CTRL_PLAYER_JOIN: quiet sits after the ally list ─────── */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_PLAYER_JOIN;
    in.u.playerJoin.playerNum = 2;
    SDL_strlcpy(in.u.playerJoin.name, "Eve", sizeof(in.u.playerJoin.name));
    in.u.playerJoin.country[0] = 'N';
    in.u.playerJoin.country[1] = 'Z';
    in.u.playerJoin.clientType  = 1;
    in.u.playerJoin.clientFlags = 0x20;
    in.u.playerJoin.numAllies   = 2;
    in.u.playerJoin.allies[0]   = 4;
    in.u.playerJoin.allies[1]   = 9;
    in.u.playerJoin.quiet       = 1;

    memset(want, 0, sizeof(want));
    want[0] = 2;
    want[1] = 'E'; want[2] = 'v'; want[3] = 'e';
    want[1 + PACKET_MAX_PLAYER_NAME]     = 'N';
    want[1 + PACKET_MAX_PLAYER_NAME + 1] = 'Z';
    want[1 + PACKET_MAX_PLAYER_NAME + 2] = 1;      /* clientType */
    want[1 + PACKET_MAX_PLAYER_NAME + 3] = 0x20;   /* clientFlags */
    want[1 + PACKET_MAX_PLAYER_NAME + 4] = 2;      /* numAllies */
    want[1 + PACKET_MAX_PLAYER_NAME + 5] = 4;
    want[1 + PACKET_MAX_PLAYER_NAME + 6] = 9;
    want[1 + PACKET_MAX_PLAYER_NAME + 7] = 1;      /* quiet */
    wantLen = 1 + PACKET_MAX_PLAYER_NAME + 2 + 1 + 1 + 1 + 2 + 1;

    enc = transportControlCodecBodyEncoder(CTRL_PLAYER_JOIN);
    UT_ASSERT(enc != NULL);
    memset(buf, 0xAA, sizeof(buf));
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT_MSG(outLen == wantLen, "join encoded %u bytes, want %u",
                  (unsigned)outLen, (unsigned)wantLen);
    for (i = 0; i < wantLen; i++) {
        UT_ASSERT_MSG(buf[i] == want[i],
                      "join byte %u = 0x%02x, want 0x%02x",
                      (unsigned)i, (unsigned)buf[i], (unsigned)want[i]);
    }
    dec = transportControlCodecBodyDecoder(CTRL_PLAYER_JOIN);
    UT_ASSERT(dec != NULL);
    memset(&out, 0xEE, sizeof(out));
    UT_ASSERT(dec(buf, outLen, &out));
    UT_ASSERT_MSG(out.u.playerJoin.numAllies == 2 &&
                      out.u.playerJoin.allies[0] == 4 &&
                      out.u.playerJoin.allies[1] == 9,
                  "the ally list decoded as %u allies",
                  (unsigned)out.u.playerJoin.numAllies);
    UT_ASSERT_MSG(out.u.playerJoin.quiet == 1, "join decoded quiet %u",
                  (unsigned)out.u.playerJoin.quiet);

    /* ── EVENT_LGM_LOST: [type, victim, killer, quiet] ────────── */
    {
        static const uint8_t wantLgm[4] = { EVENT_LGM_LOST, 5, 3, 1 };
        GameEvent gin;
        GameEvent gout;
        uint8_t gbuf[GAME_EVENT_MAX_WIRE_SIZE];
        int packed, consumed, k;

        memset(&gin, 0, sizeof(gin));
        gin.type = EVENT_LGM_LOST;
        gin.data[0] = 5;
        gin.data[1] = 3;
        gin.data[2] = 1;
        gin.data[3] = 41;   /* the man's cell — server-internal */
        gin.data[4] = 42;

        memset(gbuf, 0xAA, sizeof(gbuf));
        packed = packGameEvent(gbuf, &gin);
        UT_ASSERT_MSG(packed == 4, "a builder loss packed %d bytes, want 4",
                      packed);
        for (k = 0; k < 4; k++) {
            UT_ASSERT_MSG(gbuf[k] == wantLgm[k],
                          "builder loss wire byte %d = 0x%02x, want 0x%02x",
                          k, (unsigned)gbuf[k], (unsigned)wantLgm[k]);
        }
        UT_ASSERT_MSG(gbuf[4] == 0xAA,
                      "a byte past the payload was written: 0x%02x",
                      (unsigned)gbuf[4]);

        memset(&gout, 0xEE, sizeof(gout));
        consumed = unpackGameEvent(gbuf, (size_t)packed, &gout);
        UT_ASSERT_MSG(consumed == 4, "a builder loss consumed %d bytes, want 4",
                      consumed);
        UT_ASSERT(gout.type == EVENT_LGM_LOST);
        UT_ASSERT_MSG(gout.data[2] == 1, "builder loss decoded quiet %u",
                      (unsigned)gout.data[2]);
        UT_ASSERT_MSG(gout.data[3] == 0 && gout.data[4] == 0,
                      "the man's cell crossed the wire (%u,%u)",
                      (unsigned)gout.data[3], (unsigned)gout.data[4]);
    }

    return 0;
}

/* ================================================================
 * 6. A pill capture draws one line, not two.
 * ================================================================ */
int run_scenario_announce_pill_line_written_once(void) {
    ClientSim *cs = saMakeClient();
    GameSim *gs;
    pillbox pill;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(gs != NULL);

    pillsSetNumPills(&gs->pb, 1);
    memset(&pill, 0, sizeof(pill));
    pill.x = 41;
    pill.y = 40;
    pill.owner = NEUTRAL;
    pill.armour = PILLS_MAX_ARMOUR;
    pill.speed = PILLBOX_ATTACK_NORMAL;
    pillsSetPill(gs, &gs->pb, &pill, 1);

    /* The ownership change itself says nothing. This is the workaround that
       went: the two messageAdd calls that used to sit inside this function
       served a second newswire the quiet byte could never reach. */
    s_saLines = 0;
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, SA_OTHER, FALSE) == NEUTRAL);
    UT_ASSERT_MSG(s_saLines == 0,
                  "pillsSetPillOwner drew %d newswire lines of its own",
                  s_saLines);

    /* A steal, which was the second of the two calls. */
    s_saLines = 0;
    UT_ASSERT(pillsSetPillOwner(gs, &gs->pb, 1, SA_THIRD, FALSE) == SA_OTHER);
    UT_ASSERT_MSG(s_saLines == 0,
                  "a stolen pill drew %d newswire lines inside the setter",
                  s_saLines);

    /* The event is the one site that draws it, once. */
    s_saLines = 0;
    saClientGameEvent(cs, EVENT_PILL_CAPTURED, SA_OTHER, NEUTRAL, 0,
                      /*quiet*/ 0);
    UT_ASSERT_MSG(s_saLines == 1,
                  "the capture event drew %d lines, want exactly one",
                  s_saLines);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 7. A vote line the policy turns down is never sent.
 * ================================================================ */

/* How many CTRL_SERVER_TEXT lines in the sink carry `needle`. */
static int saServerTextWith(const SaSink *s, const char *needle) {
    int kept = s->controlCount < SA_MAX_KEPT ? s->controlCount : SA_MAX_KEPT;
    int found = 0;
    int i;
    for (i = 0; i < kept; i++) {
        if (s->control[i].type != CTRL_SERVER_TEXT) continue;
        if (strstr(s->control[i].u.serverText.text, needle) != NULL) found++;
    }
    return found;
}

/* A running two-team server with voting enabled and a human on each side. */
static ServerSim *saVoteSim(void) {
    ServerSim *sim = ut_make_running_sim("Alice");
    if (sim == NULL) return NULL;
    serverSimAddPlayer(sim, SA_OTHER, "Bob", false);
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetTeam(sim, SA_SELF, 1);
    serverSimSetTeam(sim, SA_OTHER, 2);
    return sim;
}

/* Alice surrenders team 1 on her own: one eligible voter, so the pass waits
   out the solo grace and then fires from the tick. */
static void saSurrender(ServerSim *sim) {
    serverSimGameVoteToggle(sim, SA_SELF, GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_YES);
    serverSimGameVoteTick(sim, (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL
                                   + 1000ULL);
}

int run_scenario_announce_vote_line_held(void) {
    ServerSim *sim;
    ScenarioPolicy pol;
    SaCtx pc;
    SaSink sink;
    SubscriberHandle h;

    /* Allowed: the line goes out, which is also the control that says the
       fixture reaches the publish at all. */
    sim = saVoteSim();
    UT_ASSERT(sim != NULL);
    s_saSim = sim;
    saFillPolicy(&pol, &pc, /*allow*/ true);
    serverSimSetScenarioPolicy(sim, &pol);
    h = saSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    saSurrender(sim);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_VOTE] >= 1,
                  "the surrender asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_VOTE]);
    UT_ASSERT_MSG(pc.lastSubject[ANNOUNCE_KIND_VOTE] == 1,
                  "the vote line named team %u, want the surrendering team",
                  (unsigned)pc.lastSubject[ANNOUNCE_KIND_VOTE]);
    UT_ASSERT_MSG(pc.lastActor[ANNOUNCE_KIND_VOTE] == NEUTRAL,
                  "the vote line named actor %u, want nobody",
                  (unsigned)pc.lastActor[ANNOUNCE_KIND_VOTE]);
    UT_ASSERT_MSG(saServerTextWith(&sink, "surrendered") == 1,
                  "an allowed surrender published %d lines, want 1",
                  saServerTextWith(&sink, "surrendered"));
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "the surrender itself did not go ahead");
    serverSimSetScenarioPolicy(sim, NULL);
    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);

    /* Refused: no line at all, and the surrender still happens. */
    sim = saVoteSim();
    UT_ASSERT(sim != NULL);
    s_saSim = sim;
    saFillPolicy(&pol, &pc, /*allow*/ false);
    serverSimSetScenarioPolicy(sim, &pol);
    h = saSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    saSurrender(sim);
    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_VOTE] >= 1,
                  "the refused surrender asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_VOTE]);
    UT_ASSERT_MSG(saServerTextWith(&sink, "surrendered") == 0,
                  "a refused surrender published %d lines",
                  saServerTextWith(&sink, "surrendered"));
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "holding the line back also held back the surrender");
    serverSimSetScenarioPolicy(sim, NULL);
    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    s_saSim = NULL;
    return 0;
}

/* ================================================================
 * 8. Loopback: a remote client reads the byte off the wire.
 * ================================================================ */

#define SA_CONNECT_PUMPS 4000   /* the join handshake and map transfer */
#define SA_EVENT_PUMPS   2000   /* one capture's trip out to the client */

static bool saLoopbackConnected(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The capture has to be made from inside the tick. serverSimTick clears the
   frame's event buffer as the first thing it does in a running frame, well
   before serverInstanceTick reaches the UDP event drain, so an event raised
   between two pumps is wiped by the next one and never reaches the wire. The
   scenario tick hook runs after the half-steps and before the drain, which is
   where a real scenario changes the world. One-shot: armed by the case, it
   fires on the next tick and disarms itself. */
typedef struct {
    ServerSim *sim;
    BYTE       baseNum;    /* counted from one, as basesSetBaseOwner takes it */
    BYTE       owner;
    bool       armed;
} SaTickCtx;

static void saScenarioTick(void *ctx) {
    SaTickCtx *t = (SaTickCtx *)ctx;
    GameSim *gs;

    if (!t->armed) return;
    t->armed = false;
    gs = serverSimGetGameSim(t->sim);
    (*gs->bs).item[t->baseNum - 1].owner = NEUTRAL;
    basesSetBaseOwner(gs, t->baseNum, t->owner, FALSE, FALSE);
}

/* How many base captures this client has taken delivery of, read off the
   buffer clientApplyGameEventsInner fills before it reaches any line site —
   for a human and a bot alike, and whatever the quiet byte says. Only
   brainDataMakeInfo empties it and no brain runs here, so it is a count of
   arrivals and not a depth that drains while the case is looking away. */
static int saClientCaptureCount(ClientSim *cs) {
    const GameEvent *evs = clientSimGetBrainEvents(cs);
    int n = clientSimGetBrainEventCount(cs);
    int found = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == EVENT_BASE_CAPTURED) found++;
    }
    return found;
}

/* The last base capture the client received, or NULL. */
static const GameEvent *saClientLastCapture(ClientSim *cs) {
    const GameEvent *evs = clientSimGetBrainEvents(cs);
    int n = clientSimGetBrainEventCount(cs);
    const GameEvent *last = NULL;
    int i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == EVENT_BASE_CAPTURED) last = &evs[i];
    }
    return last;
}

static bool saCaptureArrived(LoopbackHarness *h, void *u) {
    return saClientCaptureCount(h->cs) > *(int *)u;
}

int run_scenario_announce_loopback_quiet_draws_no_line(void) {
    LoopbackHarness h;
    ScenarioPolicy pol;
    SaCtx pc;
    SaTickCtx tickCtx;
    const GameEvent *arrived;
    GameSim *gs;
    BYTE slot;
    int before;
    int reached;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Alice", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 7u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, SA_CONNECT_PUMPS,
                                           saLoopbackConnected, NULL) > 0,
                  "harness player never reached CONNECTED");
    slot = clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT_MSG(slot < MAX_TANKS, "bad player slot %u", (unsigned)slot);
    /* The whole line site sits inside client_snapshot.c's isHuman test, so a
       harness client that was ever marked a bot would make the quiet leg pass
       for the wrong reason. Stated rather than assumed. */
    UT_ASSERT_MSG(!clientSimIsBot(h.cs),
                  "the harness client is a bot; it draws no lines at all");

    gs = serverSimGetGameSim(h.sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(basesGetNumBases(&gs->bs) >= 2);
    UT_ASSERT(basesIsActive(&gs->bs, 1) && basesIsActive(&gs->bs, 2));

    /* The line is counted at the client's own messageAdd, which is where it is
       written — a tally, not the ticker's pending-cell depth. */
    clientSimGetGameSim(h.cs)->callbacks.messageAdd = saSpyMessageAdd;

    memset(&tickCtx, 0, sizeof(tickCtx));
    tickCtx.sim = h.sim;
    serverSimSetScenarioTick(h.sim, saScenarioTick, &tickCtx);

    s_saSim = h.sim;
    saFillPolicy(&pol, &pc, /*allow*/ true);
    serverSimSetScenarioPolicy(h.sim, &pol);

    /* Control: an announced capture reaches the remote client and draws its
       line there. */
    before = saClientCaptureCount(h.cs);
    s_saLines = 0;
    tickCtx.baseNum = 1;
    tickCtx.owner   = slot;
    tickCtx.armed   = true;
    reached = loopbackHarnessPumpUntil(&h, SA_EVENT_PUMPS, saCaptureArrived,
                                       &before);
    UT_ASSERT_MSG(reached > 0,
                  "an announced base capture never reached the remote client "
                  "in %d pumps", SA_EVENT_PUMPS);
    arrived = saClientLastCapture(h.cs);
    UT_ASSERT(arrived != NULL);
    UT_ASSERT_MSG(arrived->data[3] == 0,
                  "the announced capture arrived carrying quiet %u",
                  (unsigned)arrived->data[3]);
    UT_ASSERT_MSG(s_saLines == 1,
                  "an announced capture drew %d newswire lines at the remote "
                  "client, want 1", s_saLines);

    /* The case: the same capture with the policy silent. Arrival is waited for
       first, so "no line" cannot be an event that never turned up. */
    pc.allow = false;
    before = saClientCaptureCount(h.cs);
    s_saLines = 0;
    tickCtx.baseNum = 2;
    tickCtx.owner   = slot;
    tickCtx.armed   = true;
    reached = loopbackHarnessPumpUntil(&h, SA_EVENT_PUMPS, saCaptureArrived,
                                       &before);
    UT_ASSERT_MSG(reached > 0,
                  "the quiet base capture never reached the remote client in "
                  "%d pumps, so the assertion below would pass for the wrong "
                  "reason", SA_EVENT_PUMPS);
    arrived = saClientLastCapture(h.cs);
    UT_ASSERT(arrived != NULL);
    UT_ASSERT_MSG(arrived->data[3] == 1,
                  "the quiet capture arrived carrying quiet %u",
                  (unsigned)arrived->data[3]);
    UT_ASSERT_MSG(s_saLines == 0,
                  "a quiet base capture drew %d newswire lines at the remote "
                  "client", s_saLines);

    UT_ASSERT_MSG(pc.asks[ANNOUNCE_KIND_BASE_CAPTURED] >= 2,
                  "the two captures asked the policy %d times",
                  pc.asks[ANNOUNCE_KIND_BASE_CAPTURED]);

    serverSimSetScenarioPolicy(h.sim, NULL);
    serverSimSetScenarioTick(h.sim, NULL, NULL);
    loopbackHarnessStop(&h);
    s_saSim = NULL;
    return 0;
}
