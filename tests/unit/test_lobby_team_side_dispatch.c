/*
 * Team start sides on the lobby server (test_lobby_team_side_dispatch.c).
 *
 * A team's start side (TeamMetadata.startSide) steers every lobby start
 * reservation: the pick on join and team change stays on the team's side,
 * a side change re-picks every connected slot, a map change drops the
 * reservations the new map's starts make off-side and backfills them, a
 * departure backfills the empties, and the claim command enforces the
 * side for a player picking for themselves while the host may hand anyone
 * any start. Two claim sentinels differ only in timing: 0xFF (Unassign)
 * leaves the slot empty until the next lobby event, START_CLAIM_TEAM_SIDE
 * (0xFE) empties it and picks again at once. These tests pin that:
 *
 *   (16) the team-meta command clamps an out-of-range side, rejects a
 *        non-host, and the accepted side reaches a ClientSim mirror;
 *   (17) a side change re-picks everyone, humans before bots, the host's
 *        earlier pick included, and publishes only the slots that moved;
 *   (18) Unassign versus Team side, the departure backfill and its
 *        human-before-bot order, and a Team side claim for another slot
 *        from a non-host;
 *   (19) a map change releases the reservations the new map puts
 *        off-side and keeps the rest;
 *   (20) a non-host may not self-claim an off-side start, the host may
 *        hand one over, and a swap that leaves the displaced holder
 *        off-side re-picks the holder.
 *
 * Layouts are injected into the lobby sim's start list the way
 * test_starts_team_side.c does (each start square forced to deep sea);
 * a start's side is read back through startsGetMaxs + startSideMaskFor,
 * independently of the server's own lookup. Commands go through
 * serverSimApplyCommand under the threads mutex. Bots are seeded the way
 * test_lobby_add_bot_dispatch.c does — a joined slot with isBot set — so
 * no brain file is needed.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "game_sim.h"
#include "bolo_map.h"              /* mapSetPos */
#include "client_command.h"        /* CMD_LOBBY_CLAIM_START, CMD_LOBBY_TEAM_META,
                                    * START_CLAIM_TEAM_SIDE, CmdResult */
#include "client_sim.h"            /* clientSimAlloc/Create/Destroy,
                                    * clientSimGetLobbyTeamStartSide */
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"         /* ControlEvent, CTRL_LOBBY_SLOT */
#include "everard_map.h"
#include "starts.h"                /* startsGetNumStarts, startsGetMaxs */
#include "start_sides.h"           /* START_SIDE_*, startSideMaskFor */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.ss, sim->lobbyPlayers, sim->teams */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, serverSimSetTeam,
                                    * serverSimSetLobbyStartIdx, serverSimSetTeamMeta,
                                    * serverSimReloadCompressedInMemory */
#include "threads.h"
#include "test_harness.h"

/* One start of a layout. A start on land still shapes the bounding box
 * but startsIsValidSquare rejects it. */
typedef struct {
    BYTE x;
    BYTE y;
    bool sea;
} LayoutStart;

/* Column layout: a row of four due-north starts (y=40) and a row of four
 * due-south starts (y=210) near x=125, so the sector test gives N or S
 * alone. bbox x 100..150, y 40..210. */
static const LayoutStart k_col_north[4] = {
    { 100,  40, true }, { 115,  40, true }, { 135,  40, true }, { 150,  40, true },
};
static const LayoutStart k_col_south[4] = {
    { 100, 210, true }, { 115, 210, true }, { 135, 210, true }, { 150, 210, true },
};

/* Corner layout: four clusters of four, one per corner, every start a
 * diagonal (NW = N|W and so on). bbox 40..210 on both axes. */
static const LayoutStart k_corners[16] = {
    {  40,  40, true }, {  50,  40, true }, {  40,  50, true }, {  50,  50, true },
    { 200,  40, true }, { 210,  40, true }, { 200,  50, true }, { 210,  50, true },
    {  40, 200, true }, {  50, 200, true }, {  40, 210, true }, {  50, 210, true },
    { 200, 200, true }, { 210, 200, true }, { 200, 210, true }, { 210, 210, true },
};

/* Lobby-enabled ServerSim on Everard Island; slot 0 is the host. */
static ServerSim *make_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

static void begin_layout(ServerSim *sim) {
    GameSim *gs = &sim->sim;
    gs->pb->numPills = 0;
    gs->bs->numBases = 0;
    gs->ss->numStarts = 0;
}

static void add_start(ServerSim *sim, BYTE x, BYTE y, bool sea) {
    GameSim *gs = &sim->sim;
    BYTE idx = gs->ss->numStarts++;
    mapSetPos(gs, &gs->mp, x, y, sea ? DEEP_SEA : GRASS, FALSE, TRUE);
    gs->ss->item[idx].x = x;
    gs->ss->item[idx].y = y;
    gs->ss->item[idx].dir = 0;
}

static void add_starts(ServerSim *sim, const LayoutStart *items, int n, bool sea) {
    int i;
    for (i = 0; i < n; i++) {
        add_start(sim, items[i].x, items[i].y, sea && items[i].sea);
    }
}

/* Side mask of a 1-based start, computed the way the batch does it. */
static BYTE mask_of(ServerSim *sim, BYTE idx1) {
    int leftPos;
    int rightPos;
    int topPos;
    int bottomPos;
    GameSim *gs = &sim->sim;
    startsGetMaxs(&gs->ss, &leftPos, &rightPos, &topPos, &bottomPos);
    return startSideMaskFor(gs->ss->item[idx1 - 1].x, gs->ss->item[idx1 - 1].y,
                            leftPos, topPos, rightPos, bottomPos);
}

static bool is_real(ServerSim *sim, BYTE idx1) {
    return idx1 >= 1 && idx1 <= startsGetNumStarts(&sim->sim.ss);
}

static bool is_north(ServerSim *sim, BYTE idx1) {
    return is_real(sim, idx1) && (mask_of(sim, idx1) & START_SIDE_BIT_N) != 0;
}

static bool is_south(ServerSim *sim, BYTE idx1) {
    return is_real(sim, idx1) && (mask_of(sim, idx1) & START_SIDE_BIT_S) != 0;
}

static uint8_t start_of(ServerSim *sim, BYTE slot) {
    const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, slot);
    return lp ? lp->startIdx : 0u;
}

/* Connected slot holding a 1-based start, or 0xFF when nobody does. */
static BYTE holder_of(ServerSim *sim, BYTE idx1) {
    BYTE k;
    for (k = 0; k < MAX_TANKS; k++) {
        if (!serverSimIsPlayerConnected(sim, k)) continue;
        if (start_of(sim, k) == idx1) return k;
    }
    return 0xFF;
}

/* Join a human into a slot and put it on a team. */
static void add_human(ServerSim *sim, BYTE slot, BYTE team) {
    char name[16];
    snprintf(name, sizeof(name), "P%u", (unsigned)slot);
    serverSimAddPlayer(sim, slot, name, false);
    serverSimSetTeam(sim, slot, team);
}

/* Same, flagged as a bot the way the lobby marks one. */
static void add_bot(ServerSim *sim, BYTE slot, BYTE team) {
    add_human(sim, slot, team);
    sim->lobbyPlayers[slot].isBot = true;
}

static CmdResult apply_cmd(ServerSim *sim, int senderSlot, const ClientCommand *cmd) {
    CmdResult r;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, cmd);
    threadsReleaseMutex();
    return r;
}

static CmdResult apply_claim(ServerSim *sim, int senderSlot, BYTE target, BYTE idx) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_CLAIM_START;
    cmd.cmdSeq = 1;
    cmd.u.lobbyClaimStart.targetSlot = target;
    cmd.u.lobbyClaimStart.startIdx   = idx;
    return apply_cmd(sim, senderSlot, &cmd);
}

static CmdResult apply_team_side(ServerSim *sim, int senderSlot, BYTE teamId, BYTE side) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_TEAM_META;
    cmd.cmdSeq = 1;
    cmd.u.lobbyTeamMeta.teamId     = teamId;
    cmd.u.lobbyTeamMeta.color      = 0;
    cmd.u.lobbyTeamMeta.namingPool = 0;
    cmd.u.lobbyTeamMeta.startSide  = side;
    cmd.u.lobbyTeamMeta.nameLen    = 0;
    return apply_cmd(sim, senderSlot, &cmd);
}

/* Counts CTRL_LOBBY_SLOT publishes. */
typedef struct {
    int slotEvents;
} SlotCounter;

static void count_slot_events(void *ctx, const ControlEvent *evt) {
    SlotCounter *c = (SlotCounter *)ctx;
    if (evt->type == CTRL_LOBBY_SLOT) c->slotEvents++;
}

/* (16) A startSide of 9 from the host is stored as START_SIDE_ANY; the
 *      same command from a non-host is NOT_HOST and changes nothing; an
 *      accepted side rides the CTRL_LOBBY_TEAM_META event into a ClientSim
 *      and reads back through clientSimGetLobbyTeamStartSide. */
int run_lobby_team_side_clamps_and_rejects_non_host(void) {
    ServerSim *sim = make_lobby();
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);

    UT_ASSERT(apply_team_side(sim, 0, 1, 9) == CMD_OK);
    UT_ASSERT_MSG(sim->teams[1].startSide == START_SIDE_ANY,
                  "side 9 should clamp to START_SIDE_ANY, stored %u",
                  (unsigned)sim->teams[1].startSide);

    UT_ASSERT_MSG(apply_team_side(sim, 1, 1, START_SIDE_E) == CMD_REJECT_NOT_HOST,
                  "a non-host setting a team side must be rejected as not-host");
    UT_ASSERT(sim->teams[1].startSide == START_SIDE_ANY);

    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_E) == CMD_OK);
    UT_ASSERT(sim->teams[1].startSide == START_SIDE_E);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyTeamMetaEvent(sim, 1, &evt);
    UT_ASSERT(evt.type == CTRL_LOBBY_TEAM_META);
    UT_ASSERT_MSG(evt.u.lobbyTeamMeta.startSide == START_SIDE_E,
                  "team meta event carries side %u, want START_SIDE_E",
                  (unsigned)evt.u.lobbyTeamMeta.startSide);

    ClientSim *cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetLobbyTeamStartSide(cs, 1) == START_SIDE_E,
                  "client mirror shows side %u, want START_SIDE_E",
                  (unsigned)clientSimGetLobbyTeamStartSide(cs, 1));
    clientSimDestroy(cs);

    serverSimDestroy(sim);
    return 0;
}

/* (17) Corner layout, twelve bots on team 2 (no side) join first and take
 *      twelve of the sixteen starts, then four humans on team 1 sit at
 *      0xFF. The host picks a south start for itself. Setting team 1 to
 *      north re-picks everyone: the four humans hold four distinct north
 *      starts (the host's south pick moved like any other), no bot holds
 *      a north start, exactly four bots are at 0xFF, and exactly the slots
 *      whose reservation changed were published. */
int run_lobby_side_change_repicks_everyone(void) {
    ServerSim *sim = make_lobby();
    UT_ASSERT(sim != NULL);
    begin_layout(sim);
    add_starts(sim, k_corners, 16, true);

    BYTE s;
    for (s = 4; s < MAX_TANKS; s++) add_bot(sim, s, 2);
    for (s = 0; s < 4; s++) add_human(sim, s, 1);
    for (s = 4; s < MAX_TANKS; s++) {
        UT_ASSERT_MSG(is_real(sim, start_of(sim, s)),
                      "bot slot %u should hold a start before the side change, holds %u",
                      (unsigned)s, (unsigned)start_of(sim, s));
    }
    for (s = 0; s < 4; s++) serverSimSetLobbyStartIdx(sim, s, 0xFF);

    /* Host pick before the side change: the SW start (40,200), 1-based 9. */
    const BYTE kSouthPick = 9;
    UT_ASSERT(is_south(sim, kSouthPick) && !is_north(sim, kSouthPick));
    UT_ASSERT(apply_claim(sim, 0, 0, kSouthPick) == CMD_OK);
    UT_ASSERT(start_of(sim, 0) == kSouthPick);

    BYTE before[MAX_TANKS];
    for (s = 0; s < MAX_TANKS; s++) before[s] = start_of(sim, s);

    SlotCounter counter;
    SubscriberHandle h = serverSimRegisterSubscriber(sim, count_slot_events, &counter);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    memset(&counter, 0, sizeof(counter));   /* drop the registration replay */

    UT_ASSERT(apply_team_side(sim, 0, 1, START_SIDE_N) == CMD_OK);

    for (s = 0; s < 4; s++) {
        BYTE r = start_of(sim, s);
        BYTE t;
        UT_ASSERT_MSG(is_north(sim, r),
                      "human slot %u holds %u after the side change, not a north start",
                      (unsigned)s, (unsigned)r);
        for (t = 0; t < s; t++) {
            UT_ASSERT_MSG(start_of(sim, t) != r,
                          "human slots %u and %u share start %u",
                          (unsigned)t, (unsigned)s, (unsigned)r);
        }
    }
    UT_ASSERT_MSG(start_of(sim, 0) != kSouthPick,
                  "the host's south pick should have moved with the side change");

    int botsEmpty = 0;
    for (s = 4; s < MAX_TANKS; s++) {
        BYTE r = start_of(sim, s);
        if (r == 0xFF) { botsEmpty++; continue; }
        UT_ASSERT_MSG(is_real(sim, r) && !is_north(sim, r),
                      "bot slot %u holds %u, a north start closed to team 2",
                      (unsigned)s, (unsigned)r);
    }
    UT_ASSERT_MSG(botsEmpty == 4,
                  "eight south starts for twelve bots should leave four empty, left %d",
                  botsEmpty);

    int changed = 0;
    for (s = 0; s < MAX_TANKS; s++) {
        if (start_of(sim, s) != before[s]) changed++;
    }
    UT_ASSERT_MSG(counter.slotEvents == changed,
                  "side change published %d slot events for %d changed reservations",
                  counter.slotEvents, changed);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* (18) Team 1 side S with six members and four south starts (the north
 *      row on land): holders in slots 0..3, a bot in slot 4 and a human
 *      in slot 5 both empty. A holder leaves and the human, not the
 *      lower-numbered bot, takes the freed start. The host's Unassign
 *      leaves its start free — nobody is backfilled onto it — until the
 *      host hands it to the empty bot, and the host itself stays empty.
 *      A holder's Team side claim lands it back on a free south start at
 *      once; the empty host's Team side claim leaves it at 0xFF because
 *      the side is full; a Team side claim for another slot from a
 *      non-host is rejected as not-host. */
int run_lobby_unassign_and_team_side_claims(void) {
    ServerSim *sim = make_lobby();
    UT_ASSERT(sim != NULL);
    begin_layout(sim);
    add_starts(sim, k_col_north, 4, false);   /* on land: shapes the bbox only */
    add_starts(sim, k_col_south, 4, true);

    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_S, NULL, 0);
    add_human(sim, 0, 1);
    add_human(sim, 1, 1);
    add_bot(sim, 2, 1);
    add_bot(sim, 3, 1);
    add_bot(sim, 4, 1);
    add_human(sim, 5, 1);

    BYTE s;
    for (s = 0; s < 4; s++) {
        BYTE t;
        UT_ASSERT_MSG(is_south(sim, start_of(sim, s)),
                      "slot %u should hold a south start, holds %u",
                      (unsigned)s, (unsigned)start_of(sim, s));
        for (t = 0; t < s; t++) UT_ASSERT(start_of(sim, t) != start_of(sim, s));
    }
    UT_ASSERT(start_of(sim, 4) == 0xFF);
    UT_ASSERT(start_of(sim, 5) == 0xFF);

    /* A holder leaves: the human in slot 5 takes the freed start ahead of
     * the bot in slot 4. */
    BYTE freed = start_of(sim, 1);
    serverSimRemovePlayer(sim, 1);
    UT_ASSERT_MSG(start_of(sim, 5) == freed,
                  "human slot 5 should take freed start %u, holds %u",
                  (unsigned)freed, (unsigned)start_of(sim, 5));
    UT_ASSERT_MSG(start_of(sim, 4) == 0xFF,
                  "bot slot 4 should still be empty, holds %u",
                  (unsigned)start_of(sim, 4));

    /* Unassign: the start stays free. */
    BYTE released = start_of(sim, 0);
    UT_ASSERT(is_south(sim, released));
    UT_ASSERT(apply_claim(sim, 0, 0, 0xFF) == CMD_OK);
    UT_ASSERT(start_of(sim, 0) == 0xFF);
    UT_ASSERT_MSG(holder_of(sim, released) == 0xFF,
                  "start %u released by Unassign was backfilled onto slot %u",
                  (unsigned)released, (unsigned)holder_of(sim, released));
    UT_ASSERT(start_of(sim, 4) == 0xFF);

    /* The host hands the free start to the empty bot; the host stays empty. */
    UT_ASSERT(apply_claim(sim, 0, 4, released) == CMD_OK);
    UT_ASSERT(start_of(sim, 4) == released);
    UT_ASSERT_MSG(start_of(sim, 0) == 0xFF,
                  "the host that unassigned itself should stay empty, holds %u",
                  (unsigned)start_of(sim, 0));

    /* Team side from a holder: re-picked at once onto a free south start. */
    UT_ASSERT(apply_claim(sim, 5, 5, START_CLAIM_TEAM_SIDE) == CMD_OK);
    {
        BYTE r = start_of(sim, 5);
        UT_ASSERT_MSG(is_south(sim, r),
                      "Team side should land slot 5 on a south start, holds %u",
                      (unsigned)r);
        for (s = 0; s < MAX_TANKS; s++) {
            if (s == 5 || !serverSimIsPlayerConnected(sim, s)) continue;
            UT_ASSERT_MSG(start_of(sim, s) != r,
                          "slot %u shares start %u with slot 5", (unsigned)s, (unsigned)r);
        }
    }

    /* Team side from the empty host with every south start held: stays empty. */
    UT_ASSERT(apply_claim(sim, 0, 0, START_CLAIM_TEAM_SIDE) == CMD_OK);
    UT_ASSERT_MSG(start_of(sim, 0) == 0xFF,
                  "Team side with the side full should leave slot 0 empty, holds %u",
                  (unsigned)start_of(sim, 0));

    /* A non-host may not Team side another slot. */
    UT_ASSERT(apply_claim(sim, 5, 4, START_CLAIM_TEAM_SIDE) == CMD_REJECT_NOT_HOST);
    UT_ASSERT(start_of(sim, 4) == released);

    serverSimDestroy(sim);
    return 0;
}

/* (19) Two humans on a north team hold two starts that are north on the
 *      lobby's start list. The list is then replaced by the map's own
 *      starts, on which one of those indices is south and the other still
 *      north: the south one is released and re-picked onto a north start
 *      of the new list, the north one is kept. */
int run_lobby_map_change_releases_off_side(void) {
    ServerSim *sim = make_lobby();
    UT_ASSERT(sim != NULL);

    /* Classify the map's own starts first: one that is north and one that
     * is off a north team's side (some side, no N bit). */
    BYTE mapStarts = startsGetNumStarts(&sim->sim.ss);
    BYTE idxNorth = 0;
    BYTE idxOff = 0;
    int mapNorthCount = 0;
    BYTE i;
    UT_ASSERT(mapStarts >= 2);
    for (i = 1; i <= mapStarts; i++) {
        BYTE m = mask_of(sim, i);
        if ((m & START_SIDE_BIT_N) != 0) {
            mapNorthCount++;
            if (idxNorth == 0) idxNorth = i;
        } else if (m != 0 && idxOff == 0) {
            idxOff = i;
        }
    }
    UT_ASSERT_MSG(idxNorth != 0 && idxOff != 0 && mapNorthCount >= 2,
                  "the test map needs two north starts and one off-side start "
                  "(north %d, off-side index %u)", mapNorthCount, (unsigned)idxOff);

    /* The lobby's start list: the same count, with idxNorth and idxOff
     * north and every other index south. */
    begin_layout(sim);
    for (i = 1; i <= mapStarts; i++) {
        if (i == idxNorth) {
            add_start(sim, 100, 40, true);
        } else if (i == idxOff) {
            add_start(sim, 115, 40, true);
        } else {
            add_start(sim, (BYTE)(60 + (i % 20) * 9), 210, true);
        }
    }
    UT_ASSERT(is_north(sim, idxNorth) && is_north(sim, idxOff));

    add_human(sim, 0, 1);
    add_human(sim, 1, 1);
    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_N, NULL, 0);
    UT_ASSERT(apply_claim(sim, 0, 0, idxOff) == CMD_OK);
    UT_ASSERT(apply_claim(sim, 0, 1, idxNorth) == CMD_OK);
    UT_ASSERT(start_of(sim, 0) == idxOff);
    UT_ASSERT(start_of(sim, 1) == idxNorth);

    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(serverSimReloadCompressedInMemory(sim, emap, 5097,
                                                        "Everard Island") == TRUE,
                      "map reload failed");
    }
    UT_ASSERT(startsGetNumStarts(&sim->sim.ss) == mapStarts);
    UT_ASSERT(!is_north(sim, idxOff) && is_north(sim, idxNorth));

    UT_ASSERT_MSG(start_of(sim, 1) == idxNorth,
                  "slot 1's start %u is still north and should be kept, holds %u",
                  (unsigned)idxNorth, (unsigned)start_of(sim, 1));
    {
        BYTE r = start_of(sim, 0);
        UT_ASSERT_MSG(r != idxOff,
                      "slot 0 kept start %u, now off its north side", (unsigned)idxOff);
        UT_ASSERT_MSG(is_north(sim, r),
                      "slot 0 should be re-picked onto a north start, holds %u",
                      (unsigned)r);
        UT_ASSERT(r != idxNorth);
    }

    serverSimDestroy(sim);
    return 0;
}

/* (20) Team 1 side N with the host and a non-host on north starts. The
 *      non-host claiming a south start for itself is INVALID (the lobby
 *      map surfaces send this same command); a free north start is fine;
 *      the host claiming the south start for the non-host succeeds. A
 *      swap that hands the displaced holder a north start keeps it there;
 *      a swap that hands it the south start re-picks the holder north. */
int run_lobby_non_host_off_side_claim_rejected(void) {
    ServerSim *sim = make_lobby();
    UT_ASSERT(sim != NULL);
    begin_layout(sim);
    add_starts(sim, k_col_north, 4, true);   /* 1-based 1..4 */
    add_starts(sim, k_col_south, 4, true);   /* 1-based 5..8 */

    add_human(sim, 0, 1);
    add_human(sim, 1, 1);
    serverSimSetTeamMeta(sim, 1, 0, 0, START_SIDE_N, NULL, 0);

    BYTE n0 = start_of(sim, 0);
    BYTE n1 = start_of(sim, 1);
    UT_ASSERT(is_north(sim, n0) && is_north(sim, n1) && n0 != n1);
    const BYTE kSouth = 5;
    UT_ASSERT(is_south(sim, kSouth) && !is_north(sim, kSouth));

    /* Non-host self-claim of a south start: rejected, nothing moves. */
    UT_ASSERT_MSG(apply_claim(sim, 1, 1, kSouth) == CMD_REJECT_INVALID,
                  "a non-host on a north team must not take a south start");
    UT_ASSERT(start_of(sim, 1) == n1);

    /* Non-host self-claim of a free north start: fine. */
    {
        BYTE freeNorth = 0;
        BYTE i;
        for (i = 1; i <= 4; i++) {
            if (i != n0 && i != n1) { freeNorth = i; break; }
        }
        UT_ASSERT(freeNorth != 0);
        UT_ASSERT(apply_claim(sim, 1, 1, freeNorth) == CMD_OK);
        UT_ASSERT(start_of(sim, 1) == freeNorth);
        n1 = freeNorth;
    }

    /* The host may hand the non-host the south start. */
    UT_ASSERT(apply_claim(sim, 0, 1, kSouth) == CMD_OK);
    UT_ASSERT(start_of(sim, 1) == kSouth);

    /* Host swap: the host takes the south start, the displaced holder
     * inherits the host's north start and keeps it. */
    UT_ASSERT(apply_claim(sim, 0, 0, kSouth) == CMD_OK);
    UT_ASSERT(start_of(sim, 0) == kSouth);
    UT_ASSERT_MSG(start_of(sim, 1) == n0,
                  "displaced holder should keep inherited north start %u, holds %u",
                  (unsigned)n0, (unsigned)start_of(sim, 1));

    /* Host swap the other way: the displaced holder would inherit the
     * south start, so it is re-picked onto a free north start instead. */
    UT_ASSERT(apply_claim(sim, 0, 0, n0) == CMD_OK);
    UT_ASSERT(start_of(sim, 0) == n0);
    {
        BYTE r = start_of(sim, 1);
        UT_ASSERT_MSG(r != kSouth,
                      "displaced holder was left on south start %u", (unsigned)kSouth);
        UT_ASSERT_MSG(is_north(sim, r),
                      "displaced holder should be re-picked north, holds %u", (unsigned)r);
        UT_ASSERT(r != n0);
    }

    serverSimDestroy(sim);
    return 0;
}
