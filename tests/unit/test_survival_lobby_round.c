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
 *      * the first wave's line, and its first attacker, land 30 s in and not
 *        before
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
#define SLR_GRACE_TICKS  3000       /* 30 s at the 100-a-second tick */
#define SLR_MAX_BOTS     5

/* Every line the whole game was told, with the tick it was told on. The
   script's own announcements come down this channel (game.message is server
   text), so this is where the round's clock is read off. */
typedef struct {
    int      count;
    uint32_t digInTick;
    uint32_t waveTick;
} SlrText;

static ServerSim *slrSim  = NULL;
static SlrText   *slrSeen = NULL;

#define SLR_NO_TICK 0xFFFFFFFFu

static void slrTextCb(void *ctx, const ControlEvent *evt) {
    (void)ctx;
    if (slrSeen == NULL || evt->type != CTRL_SERVER_TEXT) return;
    slrSeen->count++;
    if (strstr(evt->u.serverText.text, "SURVIVAL: dig in!") != NULL &&
        slrSeen->digInTick == SLR_NO_TICK) {
        slrSeen->digInTick = slrSim != NULL ? slrSim->tick : 0;
    }
    if (strstr(evt->u.serverText.text, "attackers inbound!") != NULL &&
        slrSeen->waveTick == SLR_NO_TICK) {
        slrSeen->waveTick = slrSim != NULL ? slrSim->tick : 0;
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
    SlrText       seen;
    int           i;
    int           hordeLow  = 0;    /* horde seats below the first defender */
    int           humanSlot = 0;
    int           botSlot[SLR_MAX_BOTS];
    int           built     = 0;
    BYTE          fieldedAtStart;
    uint32_t      graceFrom;

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

    (void)serverSimRegisterSubscriber(sim, slrTextCb, NULL);
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
    graceFrom = sim->tick;
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

    /* (d) The grace is a real 30 seconds: no attacker on the field and no
           wave line before it, and both just after. Ticked in two stretches
           so a wave that landed early is caught where it happened rather
           than at the end. */
    fieldedAtStart = serverSimGetNumFielded(sim);
    for (;;) {
        /* The wave is due ON the tick the deadline names, so that tick ends
           the loop rather than being asserted against. Everything before it
           is inside the grace and must be quiet. */
        if (sim->tick >= graceFrom + SLR_GRACE_TICKS) break;
        UT_ASSERT_MSG(seen.waveTick == SLR_NO_TICK,
                      "wave 1 was called on tick %u, inside the 30 s grace "
                      "that began on tick %u",
                      (unsigned)seen.waveTick, (unsigned)graceFrom);
        UT_ASSERT_MSG(serverSimGetNumFielded(sim) == fieldedAtStart,
                      "an attacker took the field by tick %u, inside the "
                      "30 s grace", (unsigned)sim->tick);
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
                  "no wave was called by tick %u, 4 s past the grace",
                  (unsigned)sim->tick);
    UT_ASSERT_MSG(seen.waveTick >= graceFrom + SLR_GRACE_TICKS,
                  "wave 1 was called on tick %u, before the grace that began "
                  "on tick %u was out",
                  (unsigned)seen.waveTick, (unsigned)graceFrom);
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) > fieldedAtStart,
                  "no attacker had taken the field by tick %u, 4 s past the "
                  "grace", (unsigned)sim->tick);

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

        while (sim->tick < graceFrom + SLR_GRACE_TICKS + 2500) {
            serverSimTick(sim);
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
