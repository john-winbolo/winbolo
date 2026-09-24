/*
 * Survival, started the way a host starts it: through the lobby.
 *
 * The shipped map and the script beside it, not a fixture. The lobby seats
 * the scenario's own template — ten HELD seats for the horde — and it seats
 * them from the first free slot upward, so the horde takes the LOW slots and
 * the defenders the host adds land above them. That is the opposite of a
 * headless run, where the -bots are seated first, and it is the first of the
 * two things these cases hold: a script that reads a side off a slot number
 * passes headless and deals the keep to the wrong people in a lobby.
 *
 * The second is who ends up holding what. Every defender BOT is owed exactly
 * one pill dug in, and a human is owed a dead one on the ground to place
 * themselves. A pre-build that hands out the six centre pills by "nearest to
 * my start" takes the human's out from under them, which is only visible
 * once the keep is full — hence the second case.
 *
 * run_survival_lobby_round
 *      — one human and TWO defender bots, so three defenders share six
 *        centre positions and each bot is dealt two pills: it digs in one of
 *        them and the other stays dead. Started through the countdown, which
 *        is the path a host with wire clients takes.
 * run_survival_lobby_round_full
 *      — one human and FIVE defender bots: a full keep, one position each.
 *        Started in place, with no countdown, which is the path a host
 *        playing on their own machine takes. This is the owner's own game.
 * run_survival_lobby_round_ds_order
 *      — a DEDICATED server's order, which is neither of the above: the
 *        template is seated into an empty roster so the horde holds 0..9,
 *        the people join above it, the host browses away to a plain map and
 *        back, and then trims the wave to five. The horde is left on the
 *        very slot numbers the map file gives to the centre puddle, which
 *        is where a slot-numbered placement puts it. Checks that the wave
 *        still comes ashore on the outer ocean ring and the people are
 *        still in the keep. Its own case rather than a third shape of
 *        slrRound: it holds one fact and holds it in the order a DS makes.
 *
 * Both check the same facts:
 *      * the horde really does hold the low seats, so the case is testing
 *        what it says it is
 *      * every centre pill belongs to a seat on the defenders' team
 *      * each defender BOT has exactly one pill dug in, at full armour and
 *        standing on the ring road
 *      * the human still holds at least one pill and every one of them is
 *        dead on the ground
 *      * the round opens with the "dig in" line
 *      * the first wave's first attacker takes the field after the grace
 *        (GRACE_S = 10) and not before, and the whole wave is ashore inside
 *        a second of that tick
 *      * the wave's spawns RESUME the runners behind the held seats instead
 *        of building Lua VMs: zero builds at wave 1 where the countdown
 *        warmed them, and zero at wave 2 where wave 1 parked them
 *      * each attacker holds the WAVE's init table, not the team block's
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, lobbyPlayers, the pill list */
#include "server_sim_lifecycle.h"  /* StartGame / the ready check / bot config */
#include "server_sim_scenario.h"   /* serverSimScenarioSeatLobby */
#include "server_sim_join.h"       /* serverSimFindFreeSlot */
#include "tank.h"                 /* tankKillNow: an attacker dies mid-wave */
#include "control_event.h"
#include "scenario_host.h"
#include "game_sim.h"
#include "players.h"
#include "bot_manager.h"
#include "test_harness.h"

/* Where the shipped maps and their scripts live. */
#ifndef WB_DATA_MAPS_DIR
#define WB_DATA_MAPS_DIR "data/maps"
#endif

#define SLR_DEF_TEAM     1
#define SLR_WAVE_TEAM    2
#define SLR_CENTER_PILLS 6
#define SLR_BUILT_ARMOUR 15
#define SLR_TERRAIN_ROAD ROAD       /* global.h: 4 */
#define SLR_GRACE_TICKS  1000       /* GRACE_S = 10, at the 100-a-second tick */
#define SLR_WAVE_TICKS   24000      /* WAVE_LIMIT_S = 240 in Survival.scenario.lua,
                                       at the 100-a-second tick */
#define SLR_MAX_BOTS     5
#define SLR_HORDE_SEATS  10   /* scenario.lobby team 2: ten seats HELD */

/* What the round told the whole game, and the tick it told it on. Two
   channels feed this.

   CTRL_SERVER_TEXT carries the script's own lines (game.message is server
   text), which is where the round's opener is read off.

   CTRL_LOBBY_SLOT carries a seat going on or off the field, which is where
   the start of a wave is read off. The script used to announce every wave
   on the newswire ("Wave 1/5: 10 attackers inbound!") and this watched for
   that line; the round now has a 128x128 status panel that shows the wave
   number and a live countdown all the time, so the announcement is gone and
   only an operator-log line is left, which a unit test cannot see.

   The roster event was picked over the two other ways of spotting a wave.
   The panel event (CTRL_SCN_PANEL) says "WAVE 1/5" in as many words, but it
   is only redrawn once a second, so the tick it arrives on is up to 100
   ticks after the wave was called and every "how fast did the wave land"
   measurement below would go slack. Polling the roster would work too, but
   these two cases tick the sim from eighteen places and every one of them
   would have to carry the same poll. A wave's seats are HELD until the wave
   fields them, so the first CTRL_LOBBY_SLOT that says a horde seat is on the
   field is the wave landing, and it arrives on the tick it happens. */
typedef struct {
    uint32_t digInTick;
    uint32_t waveTick;     /* the tick the wave's first attacker was fielded */
} SlrSeen;

static ServerSim *slrSim  = NULL;
static SlrSeen   *slrSeen = NULL;

#define SLR_NO_TICK 0xFFFFFFFFu

static void slrWatchCb(void *ctx, const ControlEvent *evt) {
    (void)ctx;
    if (slrSeen == NULL) return;
    if (evt->type == CTRL_LOBBY_SLOT) {
        if (evt->u.lobbySlot.slot.connected &&
            evt->u.lobbySlot.slot.teamNumber == SLR_WAVE_TEAM &&
            evt->u.lobbySlot.slot.fielded &&
            slrSeen->waveTick == SLR_NO_TICK) {
            slrSeen->waveTick = slrSim != NULL ? slrSim->tick : 0;
        }
        return;
    }
    if (evt->type != CTRL_SERVER_TEXT) return;
    if (strstr(evt->u.serverText.text, "SURVIVAL: dig in!") != NULL &&
        slrSeen->digInTick == SLR_NO_TICK) {
        slrSeen->digInTick = slrSim != NULL ? slrSim->tick : 0;
    }
}

/* The team a seat is on, or 0 for a seat nobody is in. */
static BYTE slrTeam(ServerSim *sim, int slot) {
    return sim->playerConnected[slot] ? sim->lobbyPlayers[slot].teamNumber : 0;
}

/* One round: `bots` defender bots beside the host, started down `inPlace`'s
 * path. Everything both cases assert is in here; the two entry points below
 * are the two shapes. */
static int slrRound(int bots, bool inPlace) {
    char          mapPath[512];
    char          err[512];
    ServerSim    *sim;
    ScenarioHost *host;
    SlrSeen       seen;
    int           i;
    int           hordeLow  = 0;    /* horde seats below the first defender */
    int           humanSlot = 0;
    int           botSlot[SLR_MAX_BOTS];
    int           built     = 0;
    BYTE          fieldedAtStart;
    uint32_t      graceFrom;
    uint32_t      buildsAtStart;
    uint32_t      buildsBeforeWave;

    memset(&seen, 0, sizeof(seen));
    seen.digInTick = SLR_NO_TICK;
    seen.waveTick  = SLR_NO_TICK;

    snprintf(mapPath, sizeof(mapPath), "%s/Survival.map", WB_DATA_MAPS_DIR);
    sim = serverSimCreate(mapPath, gameOpen, false, 0, -1);
    if (sim == NULL) UT_FAIL("the map is missing or will not load: %s", mapPath);
    slrSim = sim;
    serverSimSetLobbyEnabled(sim, true);
    /* A server that runs brains at all: the scenario's spawn arm refuses on
       a server with no bot AI, exactly as the lobby's Add Bot does, and the
       waves reach the field through that arm. The brain the template names
       ships beside the binary, so this is the path a host really runs. */
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, "brains/GoalHunter_1.7/init.lua");

    err[0] = '\0';
    host = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (host == NULL) {
        serverSimDestroy(sim);
        UT_FAIL("the script beside %s was refused: %s", mapPath, err);
    }

    /* The host, in slot 0. gamefront puts him there before it seats the
       template, because the lobby's host role lives in that slot. */
    serverSimAddPlayer(sim, (BYTE)humanSlot, "Host", false);
    sim->lobbyPlayers[humanSlot].ready      = true;
    sim->lobbyPlayers[humanSlot].teamNumber = SLR_DEF_TEAM;

    /* The lobby the scenario asks for. */
    serverSimScenarioSeatLobby(sim);

    /* The defender bots, taking the seats a host's Add Bot would take. */
    ut_brain_stub_arm(true);
    for (i = 0; i < bots; i++) {
        int slot = serverSimFindFreeSlot(sim, true);
        if (slot < 0) UT_FAIL("no free slot for defender bot %d", i);
        if (!serverSimCreateBot(sim, (BYTE)slot,
                                "brains/GoalHunter_1.7/init.lua", "Def",
                                serverSimGetBotAiType(sim), gameOpen,
                                sim->sim.hiddenMines, SLR_DEF_TEAM, NULL)) {
            UT_FAIL("defender bot %d was refused at slot %d", i, slot);
        }
        sim->lobbyPlayers[slot].ready = true;
        botSlot[i] = slot;
    }

    /* The premise. Every horde seat below the lowest defender bot is a slot
       the old "defenders are 0..5" rule would have read as a defender. */
    for (i = 0; i < botSlot[0]; i++) {
        if (slrTeam(sim, i) == SLR_WAVE_TEAM) hordeLow++;
    }
    UT_ASSERT_MSG(hordeLow >= SLR_CENTER_PILLS,
                  "only %d horde seat(s) sit below the defenders; this case "
                  "is not exercising the lobby's seating order", hordeLow);
    UT_ASSERT_MSG(botSlot[0] > 5,
                  "the first defender bot landed in slot %d, inside the range "
                  "the old rule read as the defenders", botSlot[0]);

    (void)serverSimRegisterSubscriber(sim, slrWatchCb, NULL);
    slrSeen = &seen;

    /* Start it the way the lobby does. In place is the no-countdown path a
       host on their own machine takes; the countdown is what anything
       fanning over the wire takes. Both build every fielded seat's tank
       before the scenario's setup call, which is what the pills below
       depend on. */
    sim->worldPreLoaded = inPlace ? TRUE : FALSE;
    serverSimLobbyCheckAllReady(sim);
    while (sim->state == serverStateCountdown) serverSimTick(sim);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the round did not start: state %d", (int)sim->state);

    /* The keep is arranged inside the round START, not on some later tick:
       on_setup runs after the tank loop and before the round has ticked at
       all, so the opening snapshot already carries the dug-in pills. Only
       checkable on the in-place path — the countdown's last frame runs the
       setup, on_start and the first on_tick together, so there is no moment
       between them to look at. */
    if (inPlace) {
        int b;
        for (b = 0; b < bots; b++) {
            int n;
            int mine = 0;
            for (n = 0; n < SLR_CENTER_PILLS; n++) {
                const pillbox *p = &(*sim->sim.pb).item[n];
                if (p->owner == (BYTE)botSlot[b] && p->armour > 0) mine++;
            }
            UT_ASSERT_MSG(mine == 1,
                          "before the round's first tick the bot in slot %d "
                          "has %d pill(s) dug in, expected 1", botSlot[b],
                          mine);
        }
    }

    /* One tick, which is the round's first running one: on_start runs there
       and so does the first on_tick, which is where the grace is armed. The
       tick the round started on is the earliest that arming can have been
       measured from, so it is what the grace below is measured from too. */
    graceFrom     = sim->tick;
    buildsAtStart = botManagerRunnerBuildCount(sim);
    serverSimTick(sim);

    UT_ASSERT_MSG(scenarioHostLastError(host)[0] == '\0',
                  "the round's setup complained: %s",
                  scenarioHostLastError(host));

    /* Every defender bot is on the field by the setup, so the arrangement
       has everybody it is arranging for. Asserted rather than assumed: it is
       what tells a pill nobody dug in from a seat that was not there yet. */
    for (i = 0; i < bots; i++) {
        UT_ASSERT_MSG(sim->sim.tanks[botSlot[i]] != NULL,
                      "the defender bot in slot %d had no tank on the round's "
                      "first tick", botSlot[i]);
    }

    /* (a) Every centre pill belongs to a seat on the defenders' team. */
    for (i = 1; i <= SLR_CENTER_PILLS; i++) {
        const pillbox *p = &(*sim->sim.pb).item[i - 1];
        UT_ASSERT_MSG(p->owner < MAX_TANKS,
                      "centre pill %d is nobody's (owner %d)", i, (int)p->owner);
        UT_ASSERT_MSG(slrTeam(sim, p->owner) == SLR_DEF_TEAM,
                      "centre pill %d belongs to slot %d, which is on team %d "
                      "and not the defenders'", i, (int)p->owner,
                      (int)slrTeam(sim, p->owner));
    }

    /* (b) Each defender BOT has exactly one pill dug in: at full armour and
           standing on the ring road, which is where the pre-build slides it.
           Exactly one, because a bot mans one station however many pills the
           deal happened to give it. */
    for (i = 0; i < bots; i++) {
        int n;
        int mineBuilt = -1;
        int mineCount = 0;
        for (n = 0; n < SLR_CENTER_PILLS; n++) {
            const pillbox *p = &(*sim->sim.pb).item[n];
            if (p->owner != (BYTE)botSlot[i]) continue;
            if (p->armour == 0) continue;
            mineCount++;
            if (mineBuilt < 0) mineBuilt = n;
        }
        UT_ASSERT_MSG(mineCount == 1,
                      "the defender bot in slot %d has %d pill(s) dug in, "
                      "expected 1", botSlot[i], mineCount);
        {
            const pillbox *p = &(*sim->sim.pb).item[mineBuilt];
            BYTE tile = (*sim->sim.mp).mapItem[p->x][p->y];
            UT_ASSERT_MSG(p->armour == SLR_BUILT_ARMOUR,
                          "pill %d, the bot in slot %d's, has armour %d and "
                          "is not at full strength", mineBuilt + 1, botSlot[i],
                          (int)p->armour);
            UT_ASSERT_MSG(tile == SLR_TERRAIN_ROAD,
                          "pill %d, the bot in slot %d's, stands on terrain "
                          "%d and not on the ring road", mineBuilt + 1,
                          botSlot[i], (int)tile);
            built++;
        }
    }
    UT_ASSERT_MSG(built == bots,
                  "%d pill(s) dug in for %d defender bot(s)", built, bots);

    /* (c) The human still holds a pill, and every one they hold is dead on
           the ground: a person carries it and builds it where they choose,
           and the pre-build must never take the last one off them. */
    {
        int n;
        int mine = 0;
        for (n = 0; n < SLR_CENTER_PILLS; n++) {
            const pillbox *p = &(*sim->sim.pb).item[n];
            if (p->owner != (BYTE)humanSlot) continue;
            mine++;
            UT_ASSERT_MSG(p->armour == 0,
                          "pill %d, the human's, was built for them "
                          "(armour %d)", n + 1, (int)p->armour);
        }
        UT_ASSERT_MSG(mine > 0,
                      "the human in slot %d was left with no centre pill",
                      humanSlot);
    }

    /* (f) A defender bot that reaches the field AFTER the setup is caught
           up. The pre-build used to run once, at the setup, and never again,
           so a seat that was not there for it never dug in at all. Only
           where there is a seat free and a pill to spare, which is the
           two-bot shape: the full keep has neither. */
    if (bots < SLR_MAX_BOTS) {
        int late = serverSimFindFreeSlot(sim, true);
        int n;
        int lateBuilt = 0;
        UT_ASSERT_MSG(late >= 0, "no free slot for a late defender bot");
        if (!serverSimCreateBot(sim, (BYTE)late,
                                "brains/GoalHunter_1.7/init.lua", "Late",
                                serverSimGetBotAiType(sim), gameOpen,
                                sim->sim.hiddenMines, SLR_DEF_TEAM, NULL)) {
            UT_FAIL("the late defender bot was refused at slot %d", late);
        }
        UT_ASSERT_MSG(sim->sim.tanks[late] != NULL,
                      "the late defender bot in slot %d got no tank", late);
        for (i = 0; i < 20 && lateBuilt == 0; i++) {
            serverSimTick(sim);
            lateBuilt = 0;
            for (n = 0; n < SLR_CENTER_PILLS; n++) {
                const pillbox *p = &(*sim->sim.pb).item[n];
                if (p->owner == (BYTE)late && p->armour > 0) lateBuilt++;
            }
        }
        UT_ASSERT_MSG(lateBuilt == 1,
                      "the late defender bot in slot %d has %d pill(s) dug in "
                      "after 20 ticks, expected 1", late, lateBuilt);
        /* And it was not taken off the human, who still has one to place. */
        {
            int spare = 0;
            for (n = 0; n < SLR_CENTER_PILLS; n++) {
                const pillbox *p = &(*sim->sim.pb).item[n];
                if (p->owner == (BYTE)humanSlot && p->armour == 0) spare++;
            }
            UT_ASSERT_MSG(spare > 0,
                          "catching the late bot up left the human with no "
                          "dead pill of their own");
        }
    }

    /* (e) The round opened with the line that tells the players to dig in. */
    UT_ASSERT_MSG(seen.digInTick != SLR_NO_TICK,
                  "the round opened without the \"dig in\" line");

    /* (d) The grace is a real 10 seconds: no horde seat on the field before
           it and one just after. Ticked in two stretches so a wave that
           landed early is caught where it happened rather than at the end. */
    fieldedAtStart   = serverSimGetNumFielded(sim);
    buildsBeforeWave = botManagerRunnerBuildCount(sim);
    for (;;) {
        /* The wave is due ON the tick the deadline names, so that tick ends
           the loop rather than being asserted against. Everything before it
           is inside the grace and must be quiet. */
        if (sim->tick >= graceFrom + SLR_GRACE_TICKS) break;
        UT_ASSERT_MSG(seen.waveTick == SLR_NO_TICK,
                      "a horde seat was fielded on tick %u, inside the grace "
                      "that began on tick %u",
                      (unsigned)seen.waveTick, (unsigned)graceFrom);
        UT_ASSERT_MSG(serverSimGetNumFielded(sim) == fieldedAtStart,
                      "an attacker took the field by tick %u, inside the "
                      "10 s grace", (unsigned)sim->tick);
        UT_ASSERT_MSG(sim->state == serverStateRunning,
                      "the round ended on tick %u, inside the grace",
                      (unsigned)sim->tick);
        serverSimTick(sim);
    }
    while (sim->tick < graceFrom + SLR_GRACE_TICKS + 400 &&
           (seen.waveTick == SLR_NO_TICK ||
            serverSimGetNumFielded(sim) == fieldedAtStart)) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(seen.waveTick != SLR_NO_TICK,
                  "no wave landed by tick %u, 4 s past the grace",
                  (unsigned)sim->tick);
    UT_ASSERT_MSG(seen.waveTick >= graceFrom + SLR_GRACE_TICKS,
                  "wave 1 landed on tick %u, before the grace that began "
                  "on tick %u was out",
                  (unsigned)seen.waveTick, (unsigned)graceFrom);
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) > fieldedAtStart,
                  "no attacker had taken the field by tick %u, 4 s past the "
                  "grace", (unsigned)sim->tick);

    /* THE WARMED RUNNERS WERE USED. A round started through the countdown
       builds one held seat's runner per countdown tick, so by the time wave
       1 is called all ten are parked and waiting. Every spawn must resume
       one: a wave that builds here is a wave paying ~75 ms a head for VMs
       the countdown already bought, which is what a spawn carrying its own
       init table used to cost. Only on this path — the in-place start has no
       countdown and so warms nothing, and its own proof is wave 2 below. */
    if (!inPlace) {
        UT_ASSERT_MSG(buildsAtStart >= (uint32_t)(bots + SLR_HORDE_SEATS),
                      "only %u runner(s) existed when the round started; the "
                      "countdown did not warm the held seats, so this case "
                      "cannot say whether the wave reused them",
                      (unsigned)buildsAtStart);
        UT_ASSERT_MSG(botManagerRunnerBuildCount(sim) == buildsBeforeWave,
                      "wave 1 built %u fresh runner(s) over the %u already "
                      "standing when it was called",
                      (unsigned)(botManagerRunnerBuildCount(sim) -
                                 buildsBeforeWave),
                      (unsigned)buildsBeforeWave);
    }

    /* And the human's pill is still theirs at the end of the grace: the
       catch-up pass runs every tick and must not come back for it. */
    {
        int n;
        int mine = 0;
        for (n = 0; n < SLR_CENTER_PILLS; n++) {
            const pillbox *p = &(*sim->sim.pb).item[n];
            if (p->owner == (BYTE)humanSlot && p->armour == 0) mine++;
        }
        UT_ASSERT_MSG(mine > 0,
                      "the human's dead pill was taken from them during the "
                      "grace");
    }

    /* (g) The wave, once it is ashore. A wave attacker is a HELD seat the
           script fields mid-round, long after the round start's alliance
           pass, and it fights from its OWN ClientSim's matrix — not the
           server's. Both are checked here, because the server's was always
           right and the bot's own was always empty: an attacker that reads
           its own row as zero has no allies at all, so it shoots its team's
           tanks and the pillboxes they have built. */
    if (bots == SLR_MAX_BOTS) {
        int      n;
        uint16_t horde = 0;     /* every seat on the horde's team */
        int      ashore = 0;
        uint32_t buildsAfterW1;
        uint32_t ashoreBy;

        /* The whole wave is ashore inside a second of the tick its first
           attacker landed on: the spacing is gone and the roster queue's
           one-op-a-tick is the only pacing left. Ten attackers and their ten
           init tables is twenty ops, and a frame is two ticks. Measured from
           the first attacker rather than from the call, because the call is
           an operator-log line now and only the roster is visible here; the
           call is one tick earlier, so this window is a tick tighter than it
           reads. */
        while (serverSimGetNumFielded(sim) < (BYTE)(1 + bots + SLR_HORDE_SEATS) &&
               sim->tick < seen.waveTick + 100) {
            serverSimTick(sim);
        }
        ashoreBy = sim->tick;
        UT_ASSERT_MSG(serverSimGetNumFielded(sim) ==
                          (BYTE)(1 + bots + SLR_HORDE_SEATS),
                      "only %u of the %d seats were on the field a second "
                      "after wave 1 landed",
                      (unsigned)serverSimGetNumFielded(sim),
                      1 + bots + SLR_HORDE_SEATS);
        UT_ASSERT_MSG(ashoreBy <= seen.waveTick + 100,
                      "the wave took %u ticks to land, more than the second "
                      "the queue needs", (unsigned)(ashoreBy - seen.waveTick));

        while (sim->tick < graceFrom + SLR_GRACE_TICKS + 2500) {
            serverSimTick(sim);
        }
        buildsAfterW1 = botManagerRunnerBuildCount(sim);

        /* THE WAVE COST NO LUA VMs. This round was started in place, so no
           countdown warmed its held seats and wave 1 had to build all ten.
           Wave 2 is the one that must be free: the wave-1 runners were
           parked when their attackers were taken off the field, and a spawn
           that carries no init table of its own matches the table each was
           BUILT with and resumes it.

           The park used to be keyed on the LIVE init table, which
           game.bot_init replaces the tick after every spawn, so the next
           wave never matched its own runners and paid ~75 ms a head to
           rebuild what it already had. */
        {
            uint32_t buildsAtW2;
            uint32_t w2From;
            int      killed = -1, fielded;

            /* (k) An attacker dies mid-wave, as most of them do in a real
                   round. The wave's end must still take every attacker off
                   the field: the script's prune used to forget a seat whose
                   tank read as absent, and a dead tank waiting to respawn
                   does, so the owner saw one attacker vanish and nine stay
                   out for the rest of the round. */
            for (i = 0; i < MAX_TANKS && killed < 0; i++) {
                if (slrTeam(sim, i) != SLR_WAVE_TEAM) continue;
                if (!sim->lobbyPlayers[i].fielded) continue;
                if (sim->sim.tanks[i] == NULL) continue;
                tankKillNow(&sim->sim, &sim->sim.tanks[i], (BYTE)i, 0);
                killed = i;
            }
            UT_ASSERT_MSG(killed >= 0, "no attacker on the field to kill");
            for (i = 0; i < 300; i++) serverSimTick(sim);

            /* Past the wave limit, the mute lead and ten one-second
               departures: nobody of the horde is on the field. */
            while (sim->tick < graceFrom + SLR_GRACE_TICKS + SLR_WAVE_TICKS + 200 +
                                 10 * 100 + 500) {
                serverSimTick(sim);
            }
            fielded = 0;
            for (i = 0; i < MAX_TANKS; i++) {
                if (slrTeam(sim, i) == SLR_WAVE_TEAM &&
                    sim->lobbyPlayers[i].fielded) fielded++;
            }
            UT_ASSERT_MSG(fielded == 0,
                          "%d attacker(s) are still on the field after the "
                          "wave's end and its departures", fielded);

            /* Out to the far side of the wave, its staggered departures and
               the breather: the wave limit, ten seconds of vanishing and the
               30 s breather, with room to spare. */
            seen.waveTick = SLR_NO_TICK;
            while (seen.waveTick == SLR_NO_TICK &&
                   sim->tick < graceFrom + SLR_GRACE_TICKS + SLR_WAVE_TICKS + 10000) {
                serverSimTick(sim);
            }
            UT_ASSERT_MSG(seen.waveTick != SLR_NO_TICK,
                          "wave 2 never arrived by tick %u",
                          (unsigned)sim->tick);
            w2From     = seen.waveTick;
            buildsAtW2 = botManagerRunnerBuildCount(sim);
            UT_ASSERT_MSG(buildsAtW2 == buildsAfterW1,
                          "%u runner(s) were built between the waves; the "
                          "breather should build none",
                          (unsigned)(buildsAtW2 - buildsAfterW1));
            while (sim->tick < w2From + 200) {
                serverSimTick(sim);
            }
            fielded = 0;
            for (i = 0; i < MAX_TANKS; i++) {
                if (slrTeam(sim, i) == SLR_WAVE_TEAM &&
                    sim->lobbyPlayers[i].fielded) fielded++;
            }
            UT_ASSERT_MSG(fielded == SLR_HORDE_SEATS,
                          "wave 2 fielded %d attacker(s), expected all %d "
                          "seats back", fielded, SLR_HORDE_SEATS);
            UT_ASSERT_MSG(botManagerRunnerBuildCount(sim) == buildsAtW2,
                          "wave 2 built %u fresh runner(s); every one of its "
                          "spawns should have resumed the one its seat was "
                          "parked on",
                          (unsigned)(botManagerRunnerBuildCount(sim) -
                                     buildsAtW2));
        }

        /* (i) THE WAVE'S OWN ORDERS LANDED. The spawn carries no table — that
               is what makes it a resume — so the wave's values reach the bot
               through game.bot_init, queued right behind it. What that leaves
               in the seat is the LIVE table, and it must be the wave's, not
               the team block's: the template names blitz 2/4 and no flags,
               while wave 1 is a noclaimdead wave.

               This is the script's half of the flag contract. The table is
               the whole statement of the bot's orders, so the brain can
               clear the bare flags it owns before applying one and a flag
               left out is a flag turned off rather than one left standing
               from the wave before. */
        for (i = 0; i < MAX_TANKS; i++) {
            const ScnTable *live;
            if (slrTeam(sim, i) != SLR_WAVE_TEAM) continue;
            if (!sim->lobbyPlayers[i].fielded) continue;
            live = &sim->botMgr.bots[i].initTable;
            UT_ASSERT_MSG(scnTableGet(live, "noclaimdead") != NULL,
                          "attacker %d was not told wave 1's noclaimdead", i);
            UT_ASSERT_MSG(scnTableGet(live, "noblitz") == NULL,
                          "attacker %d was told noblitz, which is wave 3's", i);
            UT_ASSERT_MSG(scnTableGet(live, "mode") == NULL,
                          "attacker %d's wave table carries mode=, which is "
                          "refused at runtime and rides the seat config", i);
        }

        for (i = 0; i < MAX_TANKS; i++) {
            if (slrTeam(sim, i) == SLR_WAVE_TEAM) horde |= (uint16_t)(1u << i);
        }
        UT_ASSERT_MSG(horde != 0, "no horde seat is in the roster");

        for (i = 0; i < MAX_TANKS; i++) {
            uint16_t srv, own;
            if (slrTeam(sim, i) != SLR_WAVE_TEAM) continue;
            if (!sim->lobbyPlayers[i].fielded) continue;
            ashore++;

            /* The server's row: this attacker allied with every horde seat. */
            srv = (uint16_t)playersGetAlliesBitMap(&sim->sim.plyrs, (BYTE)i);
            UT_ASSERT_MSG((srv & horde) == horde,
                          "the server has attacker %d allied with 0x%04x, "
                          "which does not cover the horde's 0x%04x",
                          i, (unsigned)srv, (unsigned)horde);
            UT_ASSERT_MSG((srv & ~horde) == 0,
                          "the server has attacker %d allied with 0x%04x, "
                          "outside the horde's 0x%04x",
                          i, (unsigned)srv, (unsigned)horde);

            /* And the attacker's OWN copy of its own row, which is what its
               brain answers "is that one of mine?" from. */
            own = (uint16_t)botManagerGetClientAllieRow(sim, (BYTE)i, (BYTE)i);
            UT_ASSERT_MSG(own == srv,
                          "attacker %d's own client row is 0x%04x where the "
                          "server says 0x%04x: it does not know its own side",
                          i, (unsigned)own, (unsigned)srv);
        }
        UT_ASSERT_MSG(ashore > 0, "no attacker reached the field");

        /* (h) Nothing of the horde's is lying loose for the defenders to
               drive out and collect: every wave pill is in a tank, or has
               been built, or has been captured by a defender. A dead one
               standing on the ground in horde colours is the shape the
               owner saw. */
        for (n = SLR_CENTER_PILLS; n < (int)(*sim->sim.pb).numPills; n++) {
            const pillbox *p = &(*sim->sim.pb).item[n];
            if (p->inTank) continue;
            if (p->armour > 0) continue;
            UT_ASSERT_MSG(slrTeam(sim, p->owner) != SLR_WAVE_TEAM,
                          "wave pill %d is lying dead on the ground at %d,%d "
                          "in the horde's colours (owner %d)",
                          n + 1, (int)p->x, (int)p->y, (int)p->owner);
        }

        /* (i) The wave's pills are spread evenly over the attackers: a wave
               has more pills than a short roster has attackers, so the
               top-up hands the spares out one to each in turn, and no tank
               may carry two while another carries none. The script stages
               a wave pill on ground and loads it the same tick; an attacker
               still over open water used to find no ground within reach,
               land empty-handed, and have its pill handed to a team-mate.
               The stub brains never build, so the count carried is the
               count made for that seat. */
        {
            int least = -1, most = 0;
            for (i = 0; i < MAX_TANKS; i++) {
                int carried = 0;
                if (slrTeam(sim, i) != SLR_WAVE_TEAM) continue;
                if (!sim->lobbyPlayers[i].fielded) continue;
                for (n = SLR_CENTER_PILLS; n < (int)(*sim->sim.pb).numPills; n++) {
                    const pillbox *p = &(*sim->sim.pb).item[n];
                    if (p->inTank && p->owner == i) carried++;
                }
                if (least < 0 || carried < least) least = carried;
                if (carried > most) most = carried;
            }
            UT_ASSERT_MSG(least >= 1,
                          "an attacker came ashore with no wave pill "
                          "(fewest carried %d, most %d)", least, most);
            UT_ASSERT_MSG(most - least <= 1,
                          "the wave's pills are piled up: one attacker "
                          "carries %d while another carries %d", most, least);
        }

        /* (j) A defender bot's built pill that is shot to nothing stays
               down where it fell. The prebuild's catch-up used to read a
               dead pill beside a bot with nothing standing as a seat still
               owed one, and re-dug it on the ring road at full armour the
               same tick: the owner saw his pillbox vanish at zero and come
               back a tile west, fully built. */
        {
            int seat = -1, pill = -1, px = 0, py = 0, ticks;
            for (i = 0; i < MAX_TANKS && seat < 0; i++) {
                if (slrTeam(sim, i) != SLR_DEF_TEAM) continue;
                if (!sim->lobbyPlayers[i].isBot) continue;
                for (n = 0; n < SLR_CENTER_PILLS; n++) {
                    const pillbox *p = &(*sim->sim.pb).item[n];
                    if (!p->inTank && p->owner == i && p->armour > 0) {
                        seat = i; pill = n; px = p->x; py = p->y;
                        break;
                    }
                }
            }
            UT_ASSERT_MSG(seat >= 0, "no defender bot has a built pill to shoot");
            (*sim->sim.pb).item[pill].armour = 0;
            for (ticks = 0; ticks < 200; ticks++) serverSimTick(sim);
            UT_ASSERT_MSG((*sim->sim.pb).item[pill].armour == 0,
                          "pill %d shot to zero is back at armour %d",
                          pill + 1, (int)(*sim->sim.pb).item[pill].armour);
            UT_ASSERT_MSG((*sim->sim.pb).item[pill].x == px &&
                          (*sim->sim.pb).item[pill].y == py,
                          "pill %d shot to zero moved from %d,%d to %d,%d",
                          pill + 1, px, py, (int)(*sim->sim.pb).item[pill].x,
                          (int)(*sim->sim.pb).item[pill].y);
        }

        /* (l) And wave 2 clears at its end as wave 1 did. The script's
               memory of who had been on the field carried over from wave
               1, so wave 2's queued spawns read as departures and were
               forgotten on arrival; its end then removed nobody. Last,
               because it runs the round out past wave 2's departures. */
        if (seen.waveTick != SLR_NO_TICK) {
            int fielded = 0;
            while (sim->tick < seen.waveTick + SLR_WAVE_TICKS + 200 + 10 * 100 + 500) {
                serverSimTick(sim);
            }
            for (i = 0; i < MAX_TANKS; i++) {
                if (slrTeam(sim, i) == SLR_WAVE_TEAM &&
                    sim->lobbyPlayers[i].fielded) fielded++;
            }
            UT_ASSERT_MSG(fielded == 0,
                          "%d attacker(s) are still on the field after wave "
                          "2's end and its departures", fielded);
        }

        /* (m) The whole round: waves 3, 4 and 5 land and clear the same
               way, and after the fifth's departures the script ends the
               round with the defenders' win. Five waves of four minutes
               plus their breathers is about 23 minutes of game, a few
               seconds here. */
        {
            uint32_t cap = graceFrom + SLR_GRACE_TICKS +
                           5u * (SLR_WAVE_TICKS + 200u + 10u * 100u + 3000u) + 5000u;
            while (sim->tick < cap &&
                   serverSimGetState(sim) == serverStateRunning) {
                serverSimTick(sim);
            }
            /* end_round does not speak on the newswire: it stores the line
               as the pending win message, names the winner and enters the
               game-over state, and the lifecycle says the rest. So the win
               is read where the arm wrote it. */
            UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                          "the round did not end by tick %u (state %d)",
                          (unsigned)sim->tick, (int)serverSimGetState(sim));
            UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_SCENARIO,
                          "the round ended for reason %d, not the script's",
                          (int)sim->returnToLobbyReason);
            UT_ASSERT_MSG(sim->returnToLobbyTeamId == SLR_DEF_TEAM,
                          "the script named team %d the winner, expected the "
                          "defenders' %d", (int)sim->returnToLobbyTeamId,
                          SLR_DEF_TEAM);
            UT_ASSERT_MSG(strstr(sim->pendingWinMessage, "waves survived") != NULL,
                          "the win message is \"%s\"", sim->pendingWinMessage);
            /* And it ended when the fifth wave's departures were done, not
               before: five waves, four breathers. */
            UT_ASSERT_MSG(sim->tick >= graceFrom + SLR_GRACE_TICKS +
                                       5u * SLR_WAVE_TICKS + 4u * 3000u,
                          "the round ended early, at tick %u",
                          (unsigned)sim->tick);
        }
    }

    slrSeen = NULL;
    slrSim  = NULL;
    serverSimDestroy(sim);
    return 0;
}

/* ------------------------------------------------------------------
 * The dedicated server's own seating order, and where the horde lands.
 *
 * A DS boots with nobody in it, so the template is seated FIRST and the
 * ten held horde seats take slots 0..9. The people join above them, and
 * the host then trims the wave down in the lobby. That leaves the horde
 * holding 0..5 — the very numbers the map file gives to the centre
 * puddle — and a placement rule that reads a start off a slot number
 * lands the whole wave inside the keep it is supposed to be attacking.
 *
 * The map's starts: 1..6 the centre puddle, 7..16 the outer ocean ring,
 * one to each 36-degree spoke.
 * ------------------------------------------------------------------ */

#define SLR_MAP_MID     128   /* the island's middle */
#define SLR_OCEAN_MIN_R  18   /* the ring sits at r~25; the puddle inside 8 */
#define SLR_PUDDLE_MAX_R 10
#define SLR_DS_HUMANS     2
#define SLR_DS_KEPT       5   /* horde seats left after the host's trim */

/* How far a fielded seat is from the middle, in map squares. */
static int slrRadius(ServerSim *sim, int slot) {
    WORLD wx = 0, wy = 0;
    int   dx, dy;
    tankGetWorld(&sim->sim.tanks[slot], &wx, &wy);
    dx = (int)(wx >> TANK_SHIFT_MAPSIZE) - SLR_MAP_MID;
    dy = (int)(wy >> TANK_SHIFT_MAPSIZE) - SLR_MAP_MID;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    /* The spokes are diagonal as often as not, so the straight-line
       distance is what the ring is round: an octagon test would pass a
       tank sitting on the puddle's corner. */
    return (int)(SDL_sqrt((double)(dx * dx + dy * dy)) + 0.5);
}

int run_survival_lobby_round_ds_order(void) {
    char          mapPath[512];
    char          err[512];
    ServerSim    *sim;
    ScenarioHost *host;
    SlrSeen       seen;
    int           i;
    int           humanSlot[SLR_DS_HUMANS];
    int           hordeSeen = 0;
    uint32_t      graceFrom;

    memset(&seen, 0, sizeof(seen));
    seen.digInTick = SLR_NO_TICK;
    seen.waveTick  = SLR_NO_TICK;

    snprintf(mapPath, sizeof(mapPath), "%s/Survival.map", WB_DATA_MAPS_DIR);
    sim = serverSimCreate(mapPath, gameOpen, false, 0, -1);
    if (sim == NULL) UT_FAIL("the map is missing or will not load: %s", mapPath);
    slrSim = sim;
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, "brains/GoalHunter_1.7/init.lua");

    err[0] = '\0';
    host = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (host == NULL) {
        serverSimDestroy(sim);
        UT_FAIL("the script beside %s was refused: %s", mapPath, err);
    }

    /* The DS's order: the template goes down into an empty roster. */
    ut_brain_stub_arm(true);
    serverSimScenarioSeatLobby(sim);
    for (i = 0; i < SLR_HORDE_SEATS; i++) {
        UT_ASSERT_MSG(slrTeam(sim, i) == SLR_WAVE_TEAM,
                      "slot %d is on team %d, not the horde's: the template "
                      "did not take the low slots", i, (int)slrTeam(sim, i));
    }

    /* Then the people, above it. */
    for (i = 0; i < SLR_DS_HUMANS; i++) {
        int slot = serverSimFindFreeSlot(sim, false);
        if (slot < 0) UT_FAIL("no free slot for human %d", i);
        serverSimAddPlayer(sim, (BYTE)slot, "Def", false);
        sim->lobbyPlayers[slot].ready      = true;
        sim->lobbyPlayers[slot].teamNumber = SLR_DEF_TEAM;
        humanSlot[i] = slot;
    }

    /* The host's map browse: away to a plain map and straight back. The
       scenario goes with the map and comes back with it, and the template
       re-seats into the slots it just gave up — 0..9 again, under the two
       people. */
    scenarioHostFollowMap(sim, &host);
    /* Nobody is ready while the host is browsing maps — the lobby's own
       auto-unready sees to that on every change, and a lobby that stayed
       ready would start the round out from under him. */
    for (i = 0; i < SLR_DS_HUMANS; i++) {
        sim->lobbyPlayers[humanSlot[i]].ready = false;
    }
    {
        char plain[512];
        snprintf(plain, sizeof(plain), "%s/Everard Island.map",
                 WB_DATA_MAPS_DIR);
        UT_ASSERT_MSG(serverSimReloadMap(sim, plain),
                      "the plain map would not load: %s", plain);
        UT_ASSERT_MSG(serverSimReloadMap(sim, mapPath),
                      "the scenario map would not load back: %s", mapPath);
    }
    UT_ASSERT_MSG(host != NULL,
                  "the scenario did not come back with its map");
    for (i = 0; i < SLR_HORDE_SEATS; i++) {
        UT_ASSERT_MSG(slrTeam(sim, i) == SLR_WAVE_TEAM,
                      "after the map came back slot %d is on team %d, not "
                      "the horde's", i, (int)slrTeam(sim, i));
    }
    for (i = 0; i < SLR_DS_HUMANS; i++) {
        sim->lobbyPlayers[humanSlot[i]].ready      = true;
        sim->lobbyPlayers[humanSlot[i]].teamNumber = SLR_DEF_TEAM;
    }

    /* And the host's trim: four horde seats taken out in the lobby, which
       leaves the wave holding 0..5. */
    for (i = SLR_DS_KEPT; i < SLR_HORDE_SEATS; i++) {
        serverSimRemoveBot(sim, (BYTE)i);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (slrTeam(sim, i) == SLR_WAVE_TEAM) hordeSeen++;
    }
    UT_ASSERT_MSG(hordeSeen == SLR_DS_KEPT,
                  "%d horde seat(s) are in the roster after the trim, "
                  "expected %d", hordeSeen, SLR_DS_KEPT);

    (void)serverSimRegisterSubscriber(sim, slrWatchCb, NULL);
    slrSeen = &seen;

    sim->worldPreLoaded = FALSE;
    serverSimLobbyCheckAllReady(sim);
    while (sim->state == serverStateCountdown) serverSimTick(sim);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the round did not start: state %d", (int)sim->state);
    graceFrom = sim->tick;
    serverSimTick(sim);
    UT_ASSERT_MSG(scenarioHostLastError(host)[0] == '\0',
                  "the round's setup complained: %s",
                  scenarioHostLastError(host));

    /* Out to wave 1, and 200 ticks past its landing so the whole wave is
       ashore and settled. */
    while (sim->tick < graceFrom + SLR_GRACE_TICKS + 400 &&
           seen.waveTick == SLR_NO_TICK) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(seen.waveTick != SLR_NO_TICK,
                  "no wave landed by tick %u", (unsigned)sim->tick);
    for (i = 0; i < 200; i++) serverSimTick(sim);

    /* The people are in the keep. */
    for (i = 0; i < SLR_DS_HUMANS; i++) {
        int r;
        UT_ASSERT_MSG(sim->sim.tanks[humanSlot[i]] != NULL,
                      "the human in slot %d has no tank", humanSlot[i]);
        r = slrRadius(sim, humanSlot[i]);
        UT_ASSERT_MSG(r <= SLR_PUDDLE_MAX_R,
                      "the human in slot %d stands %d square(s) from the "
                      "middle, outside the keep", humanSlot[i], r);
    }

    /* And every attacker is out on the ocean ring, not in the puddle with
       them.

       This is also where "the round said where each of them landed" is now
       checked. The script used to put one "[wave] attacker N ashore at
       (x,y)" line per attacker on the newswire, and this case counted them:
       a recording of the owner's own server carried no position at all, so a
       wave that came ashore in the wrong place could only be argued about.
       That line is an operator-log line now — the panel is what a player
       reads, and a square and a start number is nothing a defender acts on —
       and a unit test cannot see the log. The roster is the better source
       anyway: the loop below reads each attacker's real position and holds
       it against the ring, where counting lines only held the script to
       having mentioned a position. `ashore == SLR_DS_KEPT` closes the other
       half, that no attacker was skipped. */
    {
        int       ashore = 0;
        uint16_t  horde  = 0;
        for (i = 0; i < MAX_TANKS; i++) {
            if (slrTeam(sim, i) == SLR_WAVE_TEAM) horde |= (uint16_t)(1u << i);
        }
        UT_ASSERT_MSG(horde != 0, "no horde seat is in the roster");
        for (i = 0; i < MAX_TANKS; i++) {
            int      r;
            uint16_t srv;
            if (slrTeam(sim, i) != SLR_WAVE_TEAM) continue;
            if (!sim->lobbyPlayers[i].fielded) continue;
            UT_ASSERT_MSG(sim->sim.tanks[i] != NULL,
                          "attacker %d is on the field with no tank", i);
            ashore++;
            r = slrRadius(sim, i);
            UT_ASSERT_MSG(r >= SLR_OCEAN_MIN_R,
                          "attacker %d came ashore %d square(s) from the "
                          "middle: it was put in the keep with the "
                          "defenders, not on the ocean ring", i, r);
            srv = (uint16_t)playersGetAlliesBitMap(&sim->sim.plyrs, (BYTE)i);
            UT_ASSERT_MSG((srv & horde) == horde && (srv & ~horde) == 0,
                          "the server has attacker %d allied with 0x%04x, "
                          "which is not exactly the horde's 0x%04x",
                          i, (unsigned)srv, (unsigned)horde);
        }
        UT_ASSERT_MSG(ashore == SLR_DS_KEPT,
                      "%d attacker(s) reached the field, expected %d",
                      ashore, SLR_DS_KEPT);
    }

    slrSeen = NULL;
    slrSim  = NULL;
    serverSimDestroy(sim);
    return 0;
}

int run_survival_lobby_round(void) {
    return slrRound(2, false);
}

int run_survival_lobby_round_full(void) {
    return slrRound(SLR_MAX_BOTS, true);
}
