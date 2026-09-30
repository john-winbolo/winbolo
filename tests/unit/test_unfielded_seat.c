/*
 * The unfielded seat: a seat a bot holds in the lobby with nothing behind it.
 *
 * playerConnected is true and the roster reads it as a bot, but there is no
 * bot manager entry, no ClientSim and no tank. It shows in the roster, it is
 * ready (bots always are), and the start sequence walks past it. A spawn
 * naming the seat fields it — brain, ClientSim, tank — keeping the name and
 * team it was seated with, and a removal on a seat seeded that way puts it
 * back to being held rather than emptying it.
 *
 * The cases below pin down what the seat counts for, because almost every
 * one of those numbers is read off a different flag: the roster length off
 * playerConnected, the field off LobbyPlayer.fielded, the human count off
 * the roster's bot test, and the bot pool off its own table.
 *
 * The unit binary has no Lua brain, so luaBrainInstanceCreate is the fixture
 * brain in test_stubs.c, as it is for the roster ops.
 *
 * run_unfielded_seat_counts          — in the roster, out of the field
 * run_unfielded_seat_is_empty        — no bot entry, no ClientSim, no tank
 * run_unfielded_seat_all_ready       — a lobby holding one still starts, and
 *                                      a lobby holding only one does not
 * run_unfielded_seat_start_skips_it  — a round starts without its tank
 * run_unfielded_seat_spawn_fields_it — the spawn builds the bot in the seat
 * run_unfielded_seat_survives_remove — the seat comes back, the ordinary
 *                                      bot's seat does not
 * run_unfielded_seat_unfield_is_quiet— one event out, and the team, the
 *                                      alliance and the name all still there
 * run_unfielded_seat_unfield_keeps_sync
 *                                    — no client is resynced for it
 * run_unfielded_seat_spawn_keeps_alliances
 *                                    — fielding one publishes no alliance
 *                                      reset
 * run_unfielded_seat_on_the_wire     — the byte, set and clear
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, lobbyPlayers, botMgr */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType, CheckAllReady */
#include "server_sim_scenario.h"
#include "bot_manager.h"           /* botManagerIsBot — the pool's own answer */
#include "players.h"               /* playersIsAllie — the seat keeps its team */
#include "client_sim.h"
#include "client_sim_control.h"    /* clientSimApplyControl — the mirror's arm */
#include "control_event.h"
#include "transport_control_codec.h"
#include "transport_udp_internal.h" /* PACKET_HEADER_SIZE */
#include "game_sim.h"
#include "everard_map.h"
#include "test_harness.h"

/* The seat every case seats, so a failure message names the same thing the
 * assertion does. */
#define UF_SEAT   3
#define UF_NAME   "Raider1"
#define UF_TEAM   3

/* A file for the brain path to name. The fixture brain never opens it — the
 * spawn arm reads the path to refuse one that names nothing. Per case,
 * because ctest runs the cases as concurrent processes in one directory. */
static char ufBrainPath[128];

static bool ufMakeBrainFile(const char *tag) {
    FILE *f;

    SDL_snprintf(ufBrainPath, sizeof(ufBrainPath),
                 "test_unfielded_seat_brain_%s.lua", tag);
    f = fopen(ufBrainPath, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

static void ufDropBrainFile(void) {
    remove(ufBrainPath);
}

/* A lobby taking part, configured to run bots. */
static ServerSim *ufEmptyLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, ufBrainPath);
    return sim;
}

/* The same, with one ready human in slot 0. */
static ServerSim *ufLobbySim(void) {
    ServerSim *sim = ufEmptyLobbySim();
    if (sim == NULL) return NULL;
    serverSimAddPlayer(sim, 0, "Human", false);
    sim->lobbyPlayers[0].ready = true;
    return sim;
}

static void ufSpawnOp(ScenarioOp *op, BYTE slot) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ROSTER_SPAWN_BOT;
    op->u.rosterSpawnBot.slot  = slot;
    op->u.rosterSpawnBot.start = SCN_NONE;
}

static void ufRemoveOp(ScenarioOp *op, BYTE slot) {
    memset(op, 0, sizeof(*op));
    op->type = SCN_OP_ROSTER_REMOVE_BOT;
    op->u.rosterRemoveBot.slot = slot;
}

static void ufSeatName(ServerSim *sim, BYTE slot, char *out, size_t cap) {
    playersGetPlayerName(&sim->sim.plyrs, slot, out, cap, TRUE);
}

/* ── In the roster, out of the field ──────────────────────────────── */

int run_unfielded_seat_counts(void) {
    ServerSim *sim;
    ServerSimRosterSlot rs;

    UT_ASSERT(ufMakeBrainFile("counts"));
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);

    /* One human, both numbers agree. */
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 1, "roster %u, expected 1",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 1, "fielded %u, expected 1",
                  (unsigned)serverSimGetNumFielded(sim));

    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));

    /* Both numbers, not one: the seat is in the roster and out of the
       field, and a count that moved together would pass on either alone. */
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 2,
                  "roster %u with a held seat, expected 2",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 1,
                  "fielded %u with a held seat, expected 1",
                  (unsigned)serverSimGetNumFielded(sim));

    /* The read a script gets through game.lobby_slot(). */
    UT_ASSERT(serverSimGetRosterSlot(sim, UF_SEAT, &rs));
    UT_ASSERT(rs.connected);
    UT_ASSERT(rs.is_bot);
    UT_ASSERT_MSG(!rs.fielded, "the held seat reads as fielded");
    UT_ASSERT_MSG(rs.team == UF_TEAM, "the seat is on team %u, expected %u",
                  (unsigned)rs.team, (unsigned)UF_TEAM);
    UT_ASSERT(strcmp(rs.name, UF_NAME) == 0);
    UT_ASSERT_MSG(!rs.alive, "a seat with no tank must not read as alive");

    /* The human count must not pick it up — the tracker and the all-ready
       test both read that, and the seat has no bot pool entry to be found
       by, which is the trap. */
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 1,
                  "humans %u with a held bot seat, expected 1",
                  (unsigned)serverSimGetNumHumans(sim));
    /* And the bot cap counts it, because it is a seat a bot holds. */
    UT_ASSERT_MSG(serverSimGetLobbyBotCount(sim) == 1,
                  "the lobby bot count is %u, expected 1",
                  (unsigned)serverSimGetLobbyBotCount(sim));

    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── Nothing behind it ────────────────────────────────────────────── */

int run_unfielded_seat_is_empty(void) {
    ServerSim *sim;

    UT_ASSERT(ufMakeBrainFile("is_empty"));
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));

    UT_ASSERT_MSG(sim->playerConnected[UF_SEAT],
                  "the seat must hold its place in the roster");
    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].isBot,
                  "the seat must read as a bot seat");
    UT_ASSERT_MSG(!sim->lobbyPlayers[UF_SEAT].fielded,
                  "the seat must not be on the field");

    /* The two bot tests part company here, and that is the whole point of
       the seat: the roster says bot, the pool says it has no such bot. */
    UT_ASSERT_MSG(serverSimIsBot(sim, UF_SEAT),
                  "serverSimIsBot must see the held seat");
    UT_ASSERT_MSG(!botManagerIsBot(sim, UF_SEAT),
                  "botManagerIsBot must not see the held seat");

    UT_ASSERT_MSG(sim->botMgr.bots[UF_SEAT].cs == NULL,
                  "the held seat must have no ClientSim");
    UT_ASSERT_MSG(sim->sim.tanks[UF_SEAT] == NULL,
                  "the held seat must have no tank");
    UT_ASSERT_MSG(serverSimGetNumBots(sim) == 0,
                  "the bot pool holds %u bots, expected 0",
                  (unsigned)serverSimGetNumBots(sim));

    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── The all-ready test ───────────────────────────────────────────── */

int run_unfielded_seat_all_ready(void) {
    ServerSim *sim;

    UT_ASSERT(ufMakeBrainFile("all_ready"));

    /* A ready human and a held seat: the seat is ready like any bot, so the
       lobby is all-ready and leaves the lobby state. */
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));
    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].ready,
                  "a held seat must be ready, as every bot seat is");
    sim->lobbyPlayers[0].ready = true;
    serverSimLobbyCheckAllReady(sim);
    UT_ASSERT_MSG(sim->state != serverStateLobby,
                  "an all-ready lobby holding a seat should have started, "
                  "state %d", (int)sim->state);
    serverSimDestroy(sim);

    /* A lobby holding nothing but a held seat must not start: the seat is a
       bot, and a bot-only lobby never auto-starts. The human count is read
       off the roster's bot test, and the seat has no pool entry to be found
       by — read off the pool it would count as a human and start a game. */
    sim = ufEmptyLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "humans %u in a lobby holding only a bot seat, expected 0",
                  (unsigned)serverSimGetNumHumans(sim));
    serverSimLobbyCheckAllReady(sim);
    UT_ASSERT_MSG(sim->state == serverStateLobby,
                  "a lobby holding only a bot seat must not start, state %d",
                  (int)sim->state);
    serverSimDestroy(sim);

    ufDropBrainFile();
    return 0;
}

/* ── The start sequence ───────────────────────────────────────────── */

int run_unfielded_seat_start_skips_it(void) {
    ServerSim *sim;

    UT_ASSERT(ufMakeBrainFile("start_skips_it"));
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);

    /* A second human, so the round has a fielded seat to compare against. */
    serverSimAddPlayer(sim, 1, "Human2", false);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));

    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    UT_ASSERT_MSG(sim->sim.tanks[0] != NULL, "the host has no tank");
    UT_ASSERT_MSG(sim->sim.tanks[1] != NULL, "the second player has no tank");
    UT_ASSERT_MSG(sim->sim.tanks[UF_SEAT] == NULL,
                  "the start sequence built a tank for the held seat");
    UT_ASSERT_MSG(sim->sim.lgmen[UF_SEAT] == NULL,
                  "the start sequence built a man for the held seat");

    /* Still in the roster, still off the field, after the round started. */
    UT_ASSERT_MSG(sim->playerConnected[UF_SEAT],
                  "the seat must survive the start");
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 3, "roster %u, expected 3",
                  (unsigned)serverSimGetNumPlayers(sim));
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 2, "fielded %u, expected 2",
                  (unsigned)serverSimGetNumFielded(sim));

    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── A spawn fields the seat ──────────────────────────────────────── */

int run_unfielded_seat_spawn_fields_it(void) {
    ServerSim *sim;
    ScenarioOp op;
    char name[PLAYER_NAME_LEN];

    UT_ASSERT(ufMakeBrainFile("spawn_fields_it"));
    ut_brain_stub_arm(true);
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));
    serverSimStartGame(sim);
    UT_ASSERT(sim->state == serverStateRunning);

    /* The op names the seat and nothing else: no name, no team, no brain.
       All three come off the seat or the server. */
    ufSpawnOp(&op, UF_SEAT);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED,
                  "a spawn naming a held seat must be accepted");
    UT_ASSERT_MSG(sim->sim.tanks[UF_SEAT] == NULL,
                  "a queued spawn must not have landed yet");

    serverSimTick(sim);

    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].fielded,
                  "the spawn did not field the seat");
    UT_ASSERT_MSG(botManagerIsBot(sim, UF_SEAT),
                  "the spawn left no bot pool entry");
    UT_ASSERT_MSG(ut_brain_stub_made(UF_SEAT),
                  "the spawn made no brain for the seat");
    UT_ASSERT_MSG(sim->botMgr.bots[UF_SEAT].cs != NULL,
                  "the spawn made no ClientSim for the seat");
    UT_ASSERT_MSG(sim->sim.tanks[UF_SEAT] != NULL,
                  "the spawn made no tank for the seat");

    /* The name and the team the seat was seated with, not the op's empty
       ones. */
    ufSeatName(sim, UF_SEAT, name, sizeof(name));
    UT_ASSERT_MSG(strcmp(name, UF_NAME) == 0,
                  "the fielded seat is called '%s', expected '%s'",
                  name, UF_NAME);
    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].teamNumber == UF_TEAM,
                  "the fielded seat is on team %u, expected %u",
                  (unsigned)sim->lobbyPlayers[UF_SEAT].teamNumber,
                  (unsigned)UF_TEAM);

    /* And it is on the field now. */
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 2, "fielded %u, expected 2",
                  (unsigned)serverSimGetNumFielded(sim));

    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── A removal gives the seat back ────────────────────────────────── */

int run_unfielded_seat_survives_remove(void) {
    ServerSim *sim;
    ScenarioOp op;
    char name[PLAYER_NAME_LEN];
    BYTE plain = SCN_NONE;
    BYTE i;

    UT_ASSERT(ufMakeBrainFile("survives_remove"));
    ut_brain_stub_arm(true);
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));
    serverSimStartGame(sim);

    /* Field the seat, then take the bot out of it again. */
    ufSpawnOp(&op, UF_SEAT);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    UT_ASSERT(sim->lobbyPlayers[UF_SEAT].fielded);

    ufRemoveOp(&op, UF_SEAT);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED,
                  "a remove naming the fielded seat must be accepted");
    serverSimTick(sim);

    UT_ASSERT_MSG(sim->playerConnected[UF_SEAT],
                  "the removal emptied a seat that should have been kept");
    UT_ASSERT_MSG(!sim->lobbyPlayers[UF_SEAT].fielded,
                  "the kept seat should be back to being held");
    UT_ASSERT_MSG(!botManagerIsBot(sim, UF_SEAT),
                  "the kept seat still has a bot pool entry");
    UT_ASSERT_MSG(sim->sim.tanks[UF_SEAT] == NULL,
                  "the kept seat still has a tank");
    ufSeatName(sim, UF_SEAT, name, sizeof(name));
    UT_ASSERT_MSG(strcmp(name, UF_NAME) == 0,
                  "the kept seat is called '%s', expected '%s'", name, UF_NAME);
    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].teamNumber == UF_TEAM,
                  "the kept seat is on team %u, expected %u",
                  (unsigned)sim->lobbyPlayers[UF_SEAT].teamNumber,
                  (unsigned)UF_TEAM);

    /* An ordinary bot — one nothing seated for it — is emptied as it always
       was. Asked for by first free seat, so it carries no keep. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ROSTER_SPAWN_BOT;
    op.u.rosterSpawnBot.slot  = SCN_NONE;
    op.u.rosterSpawnBot.start = SCN_NONE;
    op.u.rosterSpawnBot.team  = 2;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (botManagerIsBot(sim, i)) { plain = i; break; }
    }
    UT_ASSERT_MSG(plain != SCN_NONE, "the ordinary bot never landed");

    ufRemoveOp(&op, plain);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    UT_ASSERT_MSG(!sim->playerConnected[plain],
                  "removing an ordinary bot must empty its seat, slot %u",
                  (unsigned)plain);

    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── Taking the seat off the field ────────────────────────────────── */

/* Every control event the sim publishes, kept by type, so a case can say
 * both how many went out and which. */
typedef struct {
    int total;
    int lobbySlot;
    int lobbySlotSeat;   /* CTRL_LOBBY_SLOT naming UF_SEAT               */
    int playerLeave;
    int playerJoin;
    int allianceLeave;
    int allianceAccept;
    int allianceReset;
} UfEvents;

static void ufCountEvents(void *ctx, const ControlEvent *evt) {
    UfEvents *c = (UfEvents *)ctx;
    c->total++;
    switch (evt->type) {
        case CTRL_LOBBY_SLOT:
            c->lobbySlot++;
            if (evt->u.lobbySlot.playerNum == UF_SEAT) c->lobbySlotSeat++;
            break;
        case CTRL_PLAYER_LEAVE:    c->playerLeave++;    break;
        case CTRL_PLAYER_JOIN:     c->playerJoin++;     break;
        case CTRL_ALLIANCE_LEAVE:  c->allianceLeave++;  break;
        case CTRL_ALLIANCE_ACCEPT: c->allianceAccept++; break;
        case CTRL_ALLIANCE_RESET:  c->allianceReset++;  break;
        default: break;
    }
}

/* A running round with the human and the held seat on one team, the seat
 * fielded by a spawn. The team is shared so there is an alliance to lose:
 * the round start bakes one from the two matching team numbers. */
static ServerSim *ufFieldedSeatSim(void) {
    ServerSim *sim = ufLobbySim();
    ScenarioOp op;

    if (sim == NULL) return NULL;
    if (!serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM)) {
        serverSimDestroy(sim);
        return NULL;
    }
    /* The brain the seat is written to run, as the lobby template writes it
       when it seats one. */
    SDL_strlcpy(sim->seatBrain[UF_SEAT], ufBrainPath,
                sizeof(sim->seatBrain[UF_SEAT]));
    serverSimSetTeamBatch(sim, 0, UF_TEAM);
    serverSimStartGame(sim);

    ufSpawnOp(&op, UF_SEAT);
    if (serverSimApplyScenarioOp(sim, &op, NULL) != SCN_OP_QUEUED) {
        serverSimDestroy(sim);
        return NULL;
    }
    serverSimTick(sim);
    if (!sim->lobbyPlayers[UF_SEAT].fielded) {
        serverSimDestroy(sim);
        return NULL;
    }
    return sim;
}

int run_unfielded_seat_unfield_is_quiet(void) {
    ServerSim *sim;
    ScenarioOp op;
    UfEvents ev;
    SubscriberHandle h;
    char name[PLAYER_NAME_LEN];

    UT_ASSERT(ufMakeBrainFile("unfield_is_quiet"));
    ut_brain_stub_arm(true);
    sim = ufFieldedSeatSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(playersIsAllie(&sim->sim.plyrs, 0, UF_SEAT),
                  "the round start did not ally the seat with its team");

    memset(&ev, 0, sizeof(ev));
    h = serverSimRegisterSubscriber(sim, ufCountEvents, &ev);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    /* Registering replays the state of the game to the new subscriber, which
       is not what is being counted. */
    memset(&ev, 0, sizeof(ev));

    /* A tick with nothing asked of it, so the count below is the removal's
       own and not the tick's. */
    serverSimTick(sim);
    UT_ASSERT_MSG(ev.total == 0, "a quiet tick published %d control events",
                  ev.total);

    ufRemoveOp(&op, UF_SEAT);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);

    UT_ASSERT_MSG(!sim->lobbyPlayers[UF_SEAT].fielded,
                  "the removal left the seat on the field");
    UT_ASSERT_MSG(ev.total == 1,
                  "taking the seat off the field published %d control events, "
                  "expected 1", ev.total);
    UT_ASSERT_MSG(ev.lobbySlotSeat == 1,
                  "the one event was not the seat's CTRL_LOBBY_SLOT");
    UT_ASSERT_MSG(ev.playerLeave == 0, "the seat published a leave");
    UT_ASSERT_MSG(ev.playerJoin == 0, "the seat published a join");
    UT_ASSERT_MSG(ev.allianceLeave == 0,
                  "the seat published an alliance leave");
    UT_ASSERT_MSG(ev.allianceReset == 0,
                  "the seat published an alliance reset");

    /* Team, alliance and identity as they were. */
    UT_ASSERT_MSG(sim->playerConnected[UF_SEAT], "the seat left the roster");
    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].teamNumber == UF_TEAM,
                  "the seat is on team %u, expected %u",
                  (unsigned)sim->lobbyPlayers[UF_SEAT].teamNumber,
                  (unsigned)UF_TEAM);
    UT_ASSERT_MSG(playersIsAllie(&sim->sim.plyrs, 0, UF_SEAT),
                  "the seat lost the alliance it kept its team through");
    ufSeatName(sim, UF_SEAT, name, sizeof(name));
    UT_ASSERT_MSG(strcmp(name, UF_NAME) == 0,
                  "the seat is called '%s', expected '%s'", name, UF_NAME);
    /* And the brain it was written to run, which is what fields it again. */
    UT_ASSERT_MSG(strcmp(sim->seatBrain[UF_SEAT], ufBrainPath) == 0,
                  "the seat is written to run '%s', expected '%s'",
                  sim->seatBrain[UF_SEAT], ufBrainPath);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── Nobody is resynced for it ────────────────────────────────────── */

int run_unfielded_seat_unfield_keeps_sync(void) {
    ServerSim *sim;
    ScenarioOp op;
    uint32_t before[MAX_TANKS];
    BYTE i;

    UT_ASSERT(ufMakeBrainFile("unfield_keeps_sync"));
    ut_brain_stub_arm(true);
    sim = ufFieldedSeatSim();
    UT_ASSERT(sim != NULL);

    /* The unit sim has no wire client to build a snapshot for, so the clocks
       are written here rather than waited for: a departure memsets the whole
       array, which forces a full base and pill sync with a whole-map checksum
       to every client on the next tick. Distinct values, so a clock restarted
       from another slot's is caught too. */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->lastFullSyncTick[i] = (uint32_t)(1000 + i);
        before[i] = sim->lastFullSyncTick[i];
    }

    ufRemoveOp(&op, UF_SEAT);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    UT_ASSERT(!sim->lobbyPlayers[UF_SEAT].fielded);

    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(sim->lastFullSyncTick[i] == before[i],
                      "slot %u's full-sync clock moved from %u to %u — the "
                      "unfield forced a resync",
                      (unsigned)i, (unsigned)before[i],
                      (unsigned)sim->lastFullSyncTick[i]);
    }

    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── Fielding one leaves the alliances alone ──────────────────────── */

int run_unfielded_seat_spawn_keeps_alliances(void) {
    ServerSim *sim;
    ScenarioOp op;
    UfEvents ev;
    SubscriberHandle h;

    UT_ASSERT(ufMakeBrainFile("spawn_keeps_alliances"));
    ut_brain_stub_arm(true);
    sim = ufLobbySim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimAddUnfieldedSeat(sim, UF_SEAT, UF_NAME, UF_TEAM));
    serverSimSetTeamBatch(sim, 0, UF_TEAM);
    serverSimStartGame(sim);
    UT_ASSERT(playersIsAllie(&sim->sim.plyrs, 0, UF_SEAT));

    memset(&ev, 0, sizeof(ev));
    h = serverSimRegisterSubscriber(sim, ufCountEvents, &ev);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);
    /* The registration's own replay is not what is being counted. */
    memset(&ev, 0, sizeof(ev));

    ufSpawnOp(&op, UF_SEAT);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_QUEUED);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->lobbyPlayers[UF_SEAT].fielded,
                  "the spawn did not field the seat");

    /* The seat was allied with its team before the spawn and is allied with
       it after, so there is nothing to publish: not the batched matrix, which
       every receiver applies by dropping all sixteen slots' alliances and
       taking them back, and not a per-pair accept either. A seat new to the
       roster is the one that needs those; this one is not. */
    UT_ASSERT_MSG(ev.allianceReset == 0,
                  "fielding a held seat published %d alliance resets",
                  ev.allianceReset);
    UT_ASSERT_MSG(ev.allianceAccept == 0,
                  "fielding a held seat published %d alliance accepts",
                  ev.allianceAccept);
    UT_ASSERT_MSG(playersIsAllie(&sim->sim.plyrs, 0, UF_SEAT),
                  "the fielded seat is no longer allied with its team");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    ufDropBrainFile();
    return 0;
}

/* ── The byte on the wire ─────────────────────────────────────────── */

/* Encode the event, then find the decoder by the packet type the encoder
 * stamped at byte 2 of the header, as the wire client does. */
static int ufRoundTrip(const ControlEvent *in, ControlEvent *out,
                       uint8_t *body, size_t bodyCap, size_t *bodyLen) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(CTRL_LOBBY_SLOT);
    ControlDecodeFn dec;
    if (enc == NULL) return -1;
    if (enc(in, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK) return -2;
    if (outLen < PACKET_HEADER_SIZE) return -3;
    dec = transportControlCodecDecoder(buf[2]);
    if (dec == NULL) return -4;
    *bodyLen = outLen - PACKET_HEADER_SIZE;
    if (*bodyLen > bodyCap) return -5;
    memcpy(body, buf + PACKET_HEADER_SIZE, *bodyLen);
    if (!dec(body, *bodyLen, out)) return -6;
    return 0;
}

int run_unfielded_seat_on_the_wire(void) {
    ControlEvent in, out;
    uint8_t body[MAX_CONTROL_PACKET];
    size_t bodyLen = 0;
    /* The body the encoder writes for a connected slot, counted out by hand
       from the layout rather than read back through the codec under test:
         [playerNum 1][connected 1][nameLen 1][name n]
         [team 1][ready 1][isBot 1][ping 2][cc 2][clientType 1]
         [clientFlags 1][startIdx 1][fielded 1]
       so the body is n + 14 bytes and the flag is the last of them. */
    const size_t nameLen  = strlen(UF_NAME);
    const size_t expected = nameLen + 14;
    const size_t at       = nameLen + 13;
    ClientSim *cs;
    const ClientLobbySlot *mirror;

    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SLOT;
    in.u.lobbySlot.playerNum = UF_SEAT;
    in.u.lobbySlot.slot.connected  = true;
    SDL_strlcpy(in.u.lobbySlot.slot.playerName, UF_NAME,
                PACKET_MAX_PLAYER_NAME);
    in.u.lobbySlot.slot.teamNumber = UF_TEAM;
    in.u.lobbySlot.slot.ready      = true;
    in.u.lobbySlot.slot.isBot      = true;
    in.u.lobbySlot.slot.startIdx   = 0xFF;
    in.u.lobbySlot.slot.fielded    = false;

    /* Clear. */
    memset(&out, 0, sizeof(out));
    UT_ASSERT(ufRoundTrip(&in, &out, body, sizeof(body), &bodyLen) == 0);
    UT_ASSERT_MSG(bodyLen == expected, "body is %u bytes, expected %u",
                  (unsigned)bodyLen, (unsigned)expected);
    UT_ASSERT_MSG(body[at] == 0, "the flag byte reads %u, expected 0",
                  (unsigned)body[at]);
    UT_ASSERT_MSG(!out.u.lobbySlot.slot.fielded,
                  "a clear flag came back set");
    /* The fields either side of the new one, so a byte inserted in the
       wrong place is caught rather than papered over. */
    UT_ASSERT(out.u.lobbySlot.playerNum == UF_SEAT);
    UT_ASSERT(strcmp(out.u.lobbySlot.slot.playerName, UF_NAME) == 0);
    UT_ASSERT(out.u.lobbySlot.slot.teamNumber == UF_TEAM);
    UT_ASSERT(out.u.lobbySlot.slot.isBot);
    UT_ASSERT_MSG(out.u.lobbySlot.slot.startIdx == 0xFF,
                  "startIdx came back %u — the flag went in ahead of it",
                  (unsigned)out.u.lobbySlot.slot.startIdx);

    /* Set. */
    in.u.lobbySlot.slot.fielded = true;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(ufRoundTrip(&in, &out, body, sizeof(body), &bodyLen) == 0);
    UT_ASSERT_MSG(bodyLen == expected, "body is %u bytes, expected %u",
                  (unsigned)bodyLen, (unsigned)expected);
    UT_ASSERT_MSG(body[at] == 1, "the flag byte reads %u, expected 1",
                  (unsigned)body[at]);
    UT_ASSERT_MSG(out.u.lobbySlot.slot.fielded, "a set flag came back clear");

    /* And the apply path drops it on the client's mirror, which is what the
       roster draws from. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    in.u.lobbySlot.slot.fielded = false;
    clientSimApplyControl(cs, &in);
    mirror = clientSimGetLobbySlot(cs, UF_SEAT);
    UT_ASSERT(mirror != NULL);
    UT_ASSERT_MSG(mirror->connected, "the slot did not reach the mirror");
    UT_ASSERT_MSG(!mirror->fielded, "the mirror reads the seat as fielded");
    clientSimDestroy(cs);

    return 0;
}
