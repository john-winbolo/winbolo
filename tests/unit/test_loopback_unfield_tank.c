/*
 * What a real client draws when a HELD SEAT is taken off the field.
 *
 * Since #345 a scenario's wave does not remove its attackers: it unfields the
 * seats it borrowed. serverSimUnfieldBot destroys the server's tank and keeps
 * everything else the roster knows — the connection, the name, the team, the
 * alliance — because the seat is coming back next wave. So there is no
 * CTRL_PLAYER_LEAVE, and the only thing a client is told is the seat's own
 * CTRL_LOBBY_SLOT, which carries `fielded` and lands in the lobby mirror.
 *
 * The client's players table is untouched by that, and it still holds
 * wherever the bot last was. playersMakeScreenTanks drew every in-use row but
 * the viewer's own, so the tank went on being drawn at the spot it was taken
 * off at: frozen, and with nothing behind it on the server, so shells passed
 * through it. Ten of those at the end of every wave is what the owner saw.
 *
 * This drives the real thing — a real ClientSim over a real localhost socket
 * against a real server — and reads the answer off the CLIENT's own draw
 * list, the screenTanks that screenTanksPrepare builds for a frame.
 *
 * It unfields the seat through serverSimUnfieldBot directly rather than
 * through a five-minute Survival wave: that is the function the scenario's
 * remove_bot op reaches (scenarioTakeBotOut, server_sim_scenario.c), so the
 * server-to-client consequence under test is the same one a wave's departures
 * produce, at a thousandth of the ticks.
 *
 * run_loopback_unfield_tank
 *      — a bot on a held seat is drawn while it is fielded, and is gone from
 *        the draw list once the seat is unfielded, while the seat itself
 *        stays in the roster ready for the next wave.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_enums.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers — the seat's fielded flag */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType */
#include "server_sim_scenario.h"   /* AddUnfieldedSeat / UnfieldBot */
#include "input_packet.h"
#include "screentank.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define LU_CONNECT_MAX 2000
#define LU_SETTLE      400     /* pumps for a snapshot to cross and be drawn */
#define LU_SEAT        3       /* the held seat the "wave" borrows */
#define LU_TEAM        2

static bool luConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           !clientSimIsInLobby(h->cs);
}

/* One input a pump, so the client keeps transmitting: its channel-frame
 * trailer is what acks the server's reliable control, and control is how the
 * seat's fielded flag reaches the lobby mirror this test reads. */
static uint32_t luFeed(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Is `slot` in the client's own draw list for this frame? Built the way a
 * rendered frame builds it — screenTanksPrepare over the whole map, so
 * nothing is missed for being off-viewport — and read back through the
 * accessors the renderer uses. */
static bool luDrawn(ClientSim *cs, BYTE slot) {
    screenTanks list;
    BYTE        n, i;
    bool        found = false;

    screenTanksCreate(&list);
    screenTanksPrepare(cs, &list,
                       &clientSimGetGameSim(cs)->tanks[
                           clientSimGetMyPlayerNum(cs)],
                       0, MAP_ARRAY_SIZE - 1, 0, MAP_ARRAY_SIZE - 1);
    n = screenTanksGetNumEntries(&list);
    for (i = 0; i < n; i++) {
        BYTE mx, my, px, py, frame, pn;
        char name[FILENAME_MAX];
        screenTanksGetItem(&list, (BYTE)(i + 1), &mx, &my, &px, &py, &frame,
                           &pn, name);
        if (pn == slot) { found = true; break; }
    }
    screenTanksDestroy(&list);
    return found;
}

int run_loopback_unfield_tank(void) {
    LoopbackHarness h;
    uint32_t        tick = 1;
    int             i;
    int             at;
    bool            drawnWhileFielded = false;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Viewer", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL,
                                       /*seed*/ 0x5EA7u),
                  "harness start failed");

    at = loopbackHarnessPumpUntil(&h, LU_CONNECT_MAX, luConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached a running game");
    }

    /* A seat held for a wave, and a bot fielded onto it — the two halves of
       what a scenario's spawn does to a held seat. The brain is the unit
       binary's fixture. */
    ut_brain_stub_arm(true);
    serverSimSetBotAiType(h.sim, aiFull);
    if (!serverSimAddUnfieldedSeat(h.sim, LU_SEAT, "Raider", LU_TEAM)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the held seat could not be seated");
    }
    if (!serverSimAddBot(h.sim, LU_SEAT, &(ServerSimBotConfig){
                             .brainPath   = "brains/GoalHunter_1.7/init.lua",
                             .brainName   = "Raider",
                             .ai          = aiFull,
                             .gameType    = gameOpen,
                             .hiddenMines = false,
                             .teamNumber  = LU_TEAM })) {
        loopbackHarnessStop(&h);
        UT_FAIL("the held seat could not be fielded");
    }
    UT_ASSERT_MSG(h.sim->lobbyPlayers[LU_SEAT].fielded,
                  "the add left seat %d off the field", LU_SEAT);
    UT_ASSERT_MSG(h.sim->sim.tanks[LU_SEAT] != NULL,
                  "the add built no tank for seat %d", LU_SEAT);

    /* It reaches the client and is drawn. Without this the case could pass
       by never having drawn the tank at all. */
    for (i = 0; i < LU_SETTLE && !drawnWhileFielded; i++) {
        tick = luFeed(&h, tick);
        loopbackHarnessPump(&h);
        drawnWhileFielded = luDrawn(h.cs, LU_SEAT);
    }
    if (!drawnWhileFielded) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never drew the fielded seat's tank, so this case "
                "cannot say whether unfielding takes it away");
    }

    /* The wave ends. This is what remove_bot on a kept seat reaches. */
    serverSimUnfieldBot(h.sim, LU_SEAT);
    UT_ASSERT_MSG(!h.sim->lobbyPlayers[LU_SEAT].fielded,
                  "the unfield left seat %d on the field", LU_SEAT);
    UT_ASSERT_MSG(h.sim->sim.tanks[LU_SEAT] == NULL,
                  "the unfield left a tank on the server for seat %d",
                  LU_SEAT);
    UT_ASSERT_MSG(h.sim->playerConnected[LU_SEAT],
                  "the unfield took seat %d out of the roster; it is meant "
                  "to be held for the next wave", LU_SEAT);

    for (i = 0; i < LU_SETTLE; i++) {
        tick = luFeed(&h, tick);
        loopbackHarnessPump(&h);
    }

    /* The seat is still a roster row on the client — that is the point of
       holding it — and it must not be a tank on the screen. */
    UT_ASSERT_MSG(clientSimSlotIsUnfielded(h.cs, LU_SEAT),
                  "the client still reads seat %d as on the field, so it was "
                  "never told the seat was taken off", LU_SEAT);
    UT_ASSERT_MSG(!luDrawn(h.cs, LU_SEAT),
                  "the client is still drawing a tank for seat %d after it "
                  "was unfielded: a frozen ghost with nothing behind it",
                  LU_SEAT);

    loopbackHarnessStop(&h);
    return 0;
}
