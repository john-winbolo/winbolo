/*
 * The tank's destroyed state (test_tank_death_state.c).
 *
 * A tank's armour is a plain 0..TANK_FULL_ARMOUR value that clamps at zero
 * instead of wrapping, and whether the tank has been destroyed is stored on the
 * tank rather than worked back out of the armour value. These cases pin the
 * boundary that separates the two, because it is the part a later edit is most
 * likely to move without noticing.
 *
 * The rule is that a hit STRICTLY GREATER than the armour remaining destroys
 * the tank. A hit that exactly empties the armour does not: the tank lives on
 * at zero armour and the next hit destroys it. That is the behaviour the sim
 * has always had — death used to be the unsigned subtraction wrapping, which
 * happens only when the damage exceeds the armour — and the committed bot
 * baselines record tanks sitting alive at zero armour, so it is reachable in
 * ordinary play rather than a corner case.
 *
 * exact:    armour == DAMAGE survives at zero, and the follow-up hit destroys.
 * overkill: armour < DAMAGE destroys and leaves armour at zero rather than
 *           wrapping back up into the living range — the case the old
 *           subtract-and-test could not express.
 * partial:  armour > DAMAGE survives with the expected armour left.
 * wire:     a destroyed tank reads as destroyed through the predicate, and
 *           still does after a snapshot has carried it to a client — the
 *           destroyed bit rides in tankStatus, so neither end infers death
 *           from an armour value (test_tank_status_wire.c pins the bits).
 */

#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "input_packet.h"
#include "tank.h"
#include "test_harness.h"

/* Put the slot-0 tank in a known living state with the given armour. Re-applied
 * per hit so each arm of a case starts from the same place. */
static void td_arm(GameSim *gs, BYTE armour) {
    tankSetDestroyed(&gs->tanks[0], FALSE);
    tankSetArmour(&gs->tanks[0], armour);
    tankSetDeathWait(&gs->tanks[0], 0);
    tankSetOnBoat(&gs->tanks[0], FALSE);
    gs->inStartFind = FALSE;
}

/* One shell landing on the tank's own centre, so the hit-zone test cannot be
 * what decides the outcome. Owned by another player, which is what makes it
 * count as damage taken. */
static tankHit td_hit(GameSim *gs) {
    WORLD wx;
    WORLD wy;

    tankGetWorld(&gs->tanks[0], &wx, &wy);
    return tankIsTankHit(gs, &gs->tanks[0], wx, wy, 0, 1);
}

int run_tank_damage_exact_armour_survives(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    tankHit hit;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    /* Exactly enough armour to absorb the shell: the armour empties and the
     * tank lives. Death is "damage greater than armour", not "damage at least
     * armour", so equality is the living side of the line. */
    td_arm(gs, (BYTE)DAMAGE);
    hit = td_hit(gs);
    UT_ASSERT_MSG(hit == TH_HIT,
                  "a hit equal to the armour left reported %d, wanted TH_HIT",
                  (int)hit);
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]) == FALSE,
                  "a hit equal to the armour left destroyed the tank");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[0]) == 0,
                  "armour after an exactly-emptying hit was %u, wanted 0",
                  (unsigned)tankGetArmour(&gs->tanks[0]));

    /* The follow-up hit lands on a tank with nothing left, and that one does
     * destroy it. The pair is what pins the boundary from both sides. */
    hit = td_hit(gs);
    UT_ASSERT_MSG(hit == TH_KILL_SMALL || hit == TH_KILL_BIG,
                  "the hit on a zero-armour tank reported %d, wanted a kill",
                  (int)hit);
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]) == TRUE,
                  "a hit on a zero-armour tank left it undestroyed");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[0]) == 0,
                  "armour after the killing hit was %u, wanted 0",
                  (unsigned)tankGetArmour(&gs->tanks[0]));

    serverSimDestroy(sim);
    return 0;
}

int run_tank_damage_overkill_destroys(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    tankHit hit;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");
    UT_ASSERT_MSG(DAMAGE > 1, "DAMAGE is %u — too small to overshoot",
                  (unsigned)DAMAGE);

    /* Less armour than the shell takes off. The subtraction clamps instead of
     * wrapping, so the tank ends at zero and destroyed — never at a value that
     * has come back round into the range a living tank reads as. */
    td_arm(gs, (BYTE)(DAMAGE - 1));
    hit = td_hit(gs);
    UT_ASSERT_MSG(hit == TH_KILL_SMALL || hit == TH_KILL_BIG,
                  "an overkill hit reported %d, wanted a kill", (int)hit);
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]) == TRUE,
                  "an overkill hit left the tank undestroyed");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[0]) == 0,
                  "armour after an overkill hit was %u, wanted 0 — a wrap "
                  "would leave it near the top of the byte",
                  (unsigned)tankGetArmour(&gs->tanks[0]));
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[0]) <= TANK_FULL_ARMOUR,
                  "armour after an overkill hit was %u, outside 0..%u",
                  (unsigned)tankGetArmour(&gs->tanks[0]),
                  (unsigned)TANK_FULL_ARMOUR);

    serverSimDestroy(sim);
    return 0;
}

int run_tank_damage_partial_survives(void) {
    ServerSim *sim = ut_make_running_sim("P0");
    GameSim *gs;
    tankHit hit;

    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(gs->tanks[0] != NULL, "slot-0 tank not valid");

    /* Plenty of armour: the tank survives with exactly the shell's worth
     * taken off. */
    td_arm(gs, (BYTE)TANK_FULL_ARMOUR);
    hit = td_hit(gs);
    UT_ASSERT_MSG(hit == TH_HIT,
                  "a survivable hit reported %d, wanted TH_HIT", (int)hit);
    UT_ASSERT_MSG(tankIsDestroyed(&gs->tanks[0]) == FALSE,
                  "a survivable hit destroyed the tank");
    UT_ASSERT_MSG(tankGetArmour(&gs->tanks[0]) ==
                      (BYTE)(TANK_FULL_ARMOUR - DAMAGE),
                  "armour after a survivable hit was %u, wanted %u",
                  (unsigned)tankGetArmour(&gs->tanks[0]),
                  (unsigned)(TANK_FULL_ARMOUR - DAMAGE));

    serverSimDestroy(sim);
    return 0;
}

/* Rounds of the input-pair warm-up before the client's tank is expected, and
 * the ceiling on how long to keep pumping. The local transport runs two server
 * half-steps per clientSimNetTick, so two tick numbers go per tick. */
#define TD_WARMUP_TICKS     4
#define TD_MAX_WARMUP_TICKS 32

/* Pumps one round of the local transport, which builds a snapshot on the
 * server and applies it on the client. */
static void td_pump(ClientSim *cs, BYTE me, uint32_t *inputTick) {
    InputPacket a;
    InputPacket b;

    memset(&a, 0, sizeof(a));
    a.tick      = *inputTick;
    a.playerNum = me;
    memset(&b, 0, sizeof(b));
    b.tick      = *inputTick + 1;
    b.playerNum = me;
    clientSimNetSendInput(cs, &a);
    clientSimNetSendInput(cs, &b);
    clientSimNetTick(cs);
    *inputTick += 2;
}

int run_tank_destroyed_snapshot_round_trip(void) {
    ServerSim *sim = NULL;
    ClientSim *cs = NULL;
    GameSim *sgs;
    BYTE me;
    BYTE mx;
    BYTE my;
    uint32_t inputTick = 1;
    int i;

    sim = ut_make_running_sim("Host");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    cs = clientSimAlloc();
    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);
    UT_ASSERT_MSG(clientSimConnectLocal(cs, sim, "Joiner", "", 0, 0) == TRUE,
                  "clientSimConnectLocal failed");

    me = clientSimGetMyPlayerNum(cs);
    for (i = 0; i < TD_MAX_WARMUP_TICKS; i++) {
        td_pump(cs, me, &inputTick);
        if (i + 1 >= TD_WARMUP_TICKS &&
            clientSimGetMyTankMapPos(cs, &mx, &my) == TRUE) {
            break;
        }
    }
    UT_ASSERT_MSG(clientSimGetMyTankMapPos(cs, &mx, &my) == TRUE,
                  "the client still has no tank after the warm-up");
    UT_ASSERT_MSG(clientSimTankIsDead(cs) == FALSE,
                  "the client's freshly joined tank already reads as dead");

    /* Destroy the server's copy and hold it in the respawn wait so it stays
     * destroyed across the snapshots below. */
    sgs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(sgs != NULL, "serverSimGetGameSim returned NULL");
    UT_ASSERT_MSG(sgs->tanks[me] != NULL, "server tank for the joiner is NULL");
    tankSetArmour(&sgs->tanks[me], 0);
    tankSetDestroyed(&sgs->tanks[me], TRUE);
    tankSetDeathWait(&sgs->tanks[me], TANK_DEATH_WAIT);

    /* The predicate answers on the server without anything being encoded. */
    UT_ASSERT_MSG(tankIsDestroyed(&sgs->tanks[me]) == TRUE,
                  "the server's destroyed tank does not read as destroyed");

    /* Carry it over the wire. The destroyed bit in tankStatus is what the
     * client reads, so it agrees without either end comparing a live tank's
     * armour against anything. */
    for (i = 0; i < TD_WARMUP_TICKS; i++) {
        td_pump(cs, me, &inputTick);
    }
    UT_ASSERT_MSG(clientSimTankIsDead(cs) == TRUE,
                  "a destroyed tank did not read as dead on the client after "
                  "a snapshot round trip");
    UT_ASSERT_MSG(clientSimIsMyTankAlive(cs) == FALSE,
                  "a destroyed tank still read as alive on the client");

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}
