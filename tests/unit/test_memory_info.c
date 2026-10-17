/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* The T1 memory accessors on server_sim.h, which the desktop client's
 * System Info panel reads: serverSimGetMemoryInfo on a hosted sim with and
 * without bots, and serverSimReadProcessMemory on its own.
 *
 * The unit binary has no Lua brain, so the bots here come from the fixture
 * brain in test_stubs.c with a bare lua_State behind each (ut_brain_stub_lua),
 * which is what the accessor's heap read needs: lua_gc counters on a state
 * that is marked running. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim, the bot manager's slots */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "bot_mem_report.h"        /* the T2 counts the accessor must agree with */
#include "server_memreport.h"      /* serverMemReadProcess — the report's reader */
#include "everard_map.h"
#include "test_harness.h"

#define MI_BOT_A 1
#define MI_BOT_B 2

/* A running game on Everard Island with the human in slot 0. */
static ServerSim *miMakeSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, "brains/GoalHunter/init.lua");
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, 0, "Human", false);
    return sim;
}

static bool miAddBot(ServerSim *sim, BYTE slot, const char *name) {
    return serverSimCreateBot(sim, slot, "brains/GoalHunter/init.lua", name,
                              aiFull, gameOpen, false, 0, NULL);
}

static int miListsEqual(const ServerSimListCounts *a,
                        const BotMemListCounts *b, const char *which) {
    UT_ASSERT_MSG(a->shells == b->shells &&
                  a->explosions == b->explosions &&
                  a->tankExplosions == b->tankExplosions &&
                  a->minesExplosions == b->minesExplosions &&
                  a->floodFill == b->floodFill &&
                  a->building == b->building &&
                  a->rubble == b->rubble &&
                  a->swamp == b->swamp &&
                  a->grass == b->grass,
                  "%s list counts differ from botMemCountLists", which);
    return 0;
}

/* The process figures this platform can give have their flags set, and
 * virtual is n/a on macOS on purpose. */
static int miCheckProcess(const ServerSimProcessMemory *pm) {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    UT_ASSERT_MSG(pm->haveRss && pm->rss > 0,
                  "resident should be readable on this platform");
    UT_ASSERT_MSG(pm->havePeakRss && pm->peakRss >= pm->rss,
                  "peak resident should be readable and at least resident");
    UT_ASSERT_MSG(pm->havePriv && pm->priv > 0,
                  "private should be readable on this platform");
#endif
#if defined(__APPLE__)
    UT_ASSERT_MSG(!pm->haveVirt && pm->virt == 0,
                  "virtual is n/a on macOS on purpose");
#elif defined(_WIN32) || defined(__linux__)
    UT_ASSERT_MSG(pm->haveVirt && pm->virt > 0,
                  "virtual should be readable on this platform");
#endif
    return 0;
}

/* A hosted sim with two bots: both slots valid with a heap each, the total
 * their sum, the other slots invalid, and the list counts those of the sim
 * and of the bots' ClientSims. */
int run_memory_info_hosted_bots(void) {
    ServerSim          *sim;
    ServerSimMemoryInfo info;
    BotMemListCounts    lists;
    int                 i, rc;
    uint64_t            sum = 0;

    ut_brain_stub_arm(true);
    ut_brain_stub_lua(true);
    sim = miMakeSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(miAddBot(sim, MI_BOT_A, "BotA"), "bot A was refused");
    UT_ASSERT_MSG(miAddBot(sim, MI_BOT_B, "BotB"), "bot B was refused");
    UT_ASSERT_MSG(sim->botMgr.bots[MI_BOT_A].brain.L != NULL,
                  "bot A has no Lua state behind it");

    /* A few ticks, so the lists hold whatever a running game puts there. */
    for (i = 0; i < 5; i++) serverSimTick(sim);

    memset(&info, 0xAA, sizeof(info));
    serverSimGetMemoryInfo(sim, &info);

    UT_ASSERT_MSG(info.botCount == 2, "botCount %d, expected 2", info.botCount);
    for (i = 0; i < MAX_TANKS; i++) {
        bool isBot = (i == MI_BOT_A || i == MI_BOT_B);
        UT_ASSERT_MSG(info.botLuaValid[i] == isBot,
                      "slot %d valid=%d, expected %d", i,
                      (int)info.botLuaValid[i], (int)isBot);
        if (isBot) {
            size_t direct = 0;
            UT_ASSERT_MSG(info.botLuaBytes[i] > 0,
                          "slot %d has a running brain but a 0 heap", i);
            UT_ASSERT(botMemLuaHeapBytes(sim, (BYTE)i, &direct));
            UT_ASSERT_MSG(info.botLuaBytes[i] == (uint64_t)direct,
                          "slot %d heap %llu differs from the T2 read %llu",
                          i, (unsigned long long)info.botLuaBytes[i],
                          (unsigned long long)direct);
            sum += info.botLuaBytes[i];
        } else {
            UT_ASSERT_MSG(info.botLuaBytes[i] == 0,
                          "slot %d has no brain but a heap of %llu", i,
                          (unsigned long long)info.botLuaBytes[i]);
        }
    }
    UT_ASSERT_MSG(info.luaTotalBytes == sum,
                  "luaTotalBytes %llu, expected the slots' sum %llu",
                  (unsigned long long)info.luaTotalBytes,
                  (unsigned long long)sum);

    botMemCountLists(&sim->sim, &lists);
    rc = miListsEqual(&info.serverLists, &lists, "server");
    if (rc != 0) return rc;
    UT_ASSERT_MSG(info.botListCount == 2,
                  "botListCount %d, expected 2", info.botListCount);
    UT_ASSERT_MSG(botMemSumBotClientLists(sim, &lists) == 2,
                  "the T2 sum counted a different number of bots");
    rc = miListsEqual(&info.botLists, &lists, "bot");
    if (rc != 0) return rc;

    rc = miCheckProcess(&info.process);

    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    return rc;
}

/* A hosted sim with no bots: nothing valid, nothing summed, and the server's
 * own lists still counted. */
int run_memory_info_no_bots(void) {
    ServerSim          *sim;
    ServerSimMemoryInfo info;
    BotMemListCounts    lists;
    int                 i, rc;

    sim = miMakeSim();
    UT_ASSERT(sim != NULL);
    serverSimTick(sim);

    memset(&info, 0xAA, sizeof(info));
    serverSimGetMemoryInfo(sim, &info);

    UT_ASSERT_MSG(info.botCount == 0, "botCount %d, expected 0", info.botCount);
    UT_ASSERT_MSG(info.luaTotalBytes == 0, "luaTotalBytes should be 0");
    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(!info.botLuaValid[i] && info.botLuaBytes[i] == 0,
                      "slot %d should be invalid with a 0 heap", i);
    }
    UT_ASSERT_MSG(info.botListCount == 0,
                  "botListCount %d, expected 0", info.botListCount);
    UT_ASSERT(info.botLists.shells == 0 && info.botLists.explosions == 0 &&
              info.botLists.tankExplosions == 0);
    botMemCountLists(&sim->sim, &lists);
    rc = miListsEqual(&info.serverLists, &lists, "server");

    serverSimDestroy(sim);
    return rc;
}

/* The process reader on its own, as the client uses it for a game joined on
 * a remote server; a NULL sim gives the same figures and nothing else; and
 * the dedicated server's reader reports the same flags. */
int run_memory_info_process_figures(void) {
    ServerSimProcessMemory pm;
    ServerSimMemoryInfo    info;
    ServerMemProcess       rep;
    int                    i, rc;

    memset(&pm, 0xAA, sizeof(pm));
    UT_ASSERT_MSG(serverSimReadProcessMemory(&pm),
                  "the process read failed on this platform");
    rc = miCheckProcess(&pm);
    if (rc != 0) return rc;

    memset(&info, 0xAA, sizeof(info));
    serverSimGetMemoryInfo(NULL, &info);
    rc = miCheckProcess(&info.process);
    if (rc != 0) return rc;
    UT_ASSERT(info.botCount == 0 && info.luaTotalBytes == 0 &&
              info.botListCount == 0);
    for (i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT(!info.botLuaValid[i] && info.botLuaBytes[i] == 0);
    }
    UT_ASSERT(info.serverLists.shells == 0 && info.botLists.shells == 0);

    memset(&rep, 0xAA, sizeof(rep));
    UT_ASSERT(serverMemReadProcess(&rep));
    UT_ASSERT_MSG(rep.haveRss == pm.haveRss && rep.havePeakRss == pm.havePeakRss &&
                  rep.havePriv == pm.havePriv && rep.haveVirt == pm.haveVirt,
                  "the report's reader and the T1 reader disagree on which "
                  "figures this platform gives");
    return 0;
}
