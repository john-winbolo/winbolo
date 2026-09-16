/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * fill_to_caps: a scenario asking for the map's pills and bases to start at
 * the caps its own table sets rather than at the numbers the map file holds.
 *
 * A map file states a number for each pill's armour and each base's stocks
 * and has no way of stating "full", so a scenario that raises a cap gets a
 * map still carrying whatever its author wrote. The key is what a rules-only
 * mod has instead of a setup hook, because a rules-only mod has no hook.
 *
 * run_scenario_fill_caps_raises_bases  — a mod raising base_full_armour with
 *                                        the key set starts every base at the
 *                                        new cap
 * run_scenario_fill_caps_raises_pills  — the same for pill_max_armour
 * run_scenario_fill_caps_off_changes_nothing
 *                                      — the same mod with the key left out
 *                                        raises the cap and moves no stock
 * run_scenario_fill_caps_records       — what the fill moved reaches a
 *                                        recording, so a replay reads the
 *                                        world the round opened on
 *
 * The first three drive the real path: a mod written into a scratch
 * directory, attached through scenarioHostAttachMod, and a round started on
 * it, which is where the host applies the table and asks for the fill. The
 * map file itself is never written — the sims here are built from the
 * built-in map, and a mod is found by name in the scenarios directory rather
 * than beside a map.
 *
 * The fourth calls the sim's own pass, as the clamp's record case does: the
 * fill is a sim call and a recording is what reads it back.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.rules and the two lists */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, StartGame */
#include "server_sim_scenario.h"   /* serverSimScenarioFillWorldToRules */
#include "scenario_defs.h"         /* ScenarioOp, SCN_RULE_* */
#include "game_sim.h"              /* GameSim.rules, pb and bs */
#include "sim_rules.h"             /* SimRules — the caps the fill reads */
#include "pillbox.h"               /* the pill list the fill raises */
#include "bases.h"                 /* the base list the fill raises */
#include "everard_map.h"           /* E_MAP */
#include "scenario_host.h"
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "replay_harness.h"
#include "test_harness.h"

/* Above BASE_FULL_ARMOUR's 90 and inside base_full_armour's own 0..255 row.
 * Every pair that names the field wants something at or below it, so raising
 * it breaks none of them and the value can be set on its own. */
#define FC_BASE_ARMOUR 150

/* Above PILLS_MAX_ARMOUR's 15, and the highest cap the classic table takes on
 * its own: one delivery has to be able to finish a pill, which is
 * lgm_pill_repair_load (4) times pill_repair_amount (4). A bigger cap would
 * need a second rule set with it, and the order a Lua table is walked in is
 * its own business, so this case asks for one rule and nothing else. */
#define FC_PILL_ARMOUR 16

/* ── The scratch scenarios directory ──────────────────────────────── */

static char fcDir[256];
static char fcFile[128];

static bool fcWriteMod(const char *tag, const char *name, const char *text) {
    char  path[512];
    FILE *f;

    snprintf(fcDir, sizeof(fcDir), "wbtest_scn_fill_%s", tag);
    snprintf(fcFile, sizeof(fcFile), "%s", name);
    /* A directory left from a run that was killed is not a failure: the file
       below is written over whatever is in it. */
    (void)SDL_RemovePath(fcDir);
    if (!SDL_CreateDirectory(fcDir)) return false;

    snprintf(path, sizeof(path), "%s/%s", fcDir, name);
    f = fopen(path, "wb");
    if (f == NULL) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

static void fcDropMod(void) {
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", fcDir, fcFile);
    remove(path);
    SDL_RemovePath(fcDir);
}

/* ── The sim ──────────────────────────────────────────────────────── */

static ServerSim *fcSim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, false);
    /* The host writes its lines through the active sim's console callback, as
       the other scenario cases do. */
    serverSimSetActive(sim);
    return sim;
}

/* What the map is holding, read the way the fill reads it. */
typedef struct {
    BYTE numPills;
    BYTE numBases;
    BYTE pillArmour[MAX_PILLS];
    BYTE baseArmour[MAX_BASES];
    BYTE baseShells[MAX_BASES];
    BYTE baseMines[MAX_BASES];
} FcStocks;

static void fcRead(ServerSim *sim, FcStocks *s) {
    GameSim *gs = serverSimGetGameSim(sim);
    BYTE     i;

    memset(s, 0, sizeof(*s));
    s->numPills = pillsGetNumPills(&gs->pb);
    s->numBases = basesGetNumBases(&gs->bs);
    for (i = 0; i < s->numPills; i++) {
        pillbox pill;
        memset(&pill, 0, sizeof(pill));
        pillsGetPill(&gs->pb, &pill, (BYTE)(i + 1));
        s->pillArmour[i] = pill.armour;
    }
    for (i = 0; i < s->numBases; i++) {
        base item;
        memset(&item, 0, sizeof(item));
        basesGetBase(&gs->bs, &item, (BYTE)(i + 1));
        s->baseArmour[i] = item.armour;
        s->baseShells[i] = item.shells;
        s->baseMines[i]  = item.mines;
    }
}

/* ── 1. A raised base cap, filled ─────────────────────────────────── */

/* The mod raises one rule and asks for the fill. Nothing else: no game type,
   no teams, no hooks — which is the shape the key exists for, because a
   rules-only mod has no on_setup to fill the map from. */
static const char kFcBases[] =
    "scenario = {\n"
    "  name = \"Heavy Bases\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  fill_to_caps = true,\n"
    "  rules = { base_full_armour = 150 },\n"
    "}\n";

int run_scenario_fill_caps_raises_bases(void) {
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    FcStocks      before;
    FcStocks      after;
    char          err[512];
    BYTE          i;
    int           below = 0;

    UT_ASSERT(fcWriteMod("bases", "heavybases.lua", kFcBases));
    sim = fcSim();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    h = scenarioHostAttachMod(sim, fcDir, fcFile, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the mod was refused: %s", err);

    /* The numbers the map file holds, before any round has been started on
       it. At least one base has to be under the raised cap or the fill would
       have nothing to do and the case would prove nothing. */
    fcRead(sim, &before);
    UT_ASSERT_MSG(before.numBases > 0, "setup: the map carries no bases");
    for (i = 0; i < before.numBases; i++) {
        if (before.baseArmour[i] < FC_BASE_ARMOUR) below++;
    }
    UT_ASSERT_MSG(below > 0,
                  "setup: every base already holds %d armour or more, so the "
                  "fill would move nothing", FC_BASE_ARMOUR);

    serverSimStartGame(sim);

    /* The rule really did reach the table: without this the case would pass
       on a round that applied nothing at all. */
    UT_ASSERT_MSG(gs->rules.base_full_armour == FC_BASE_ARMOUR,
                  "the round is on a base_full_armour of %ld, wanted the "
                  "mod's %d", (long)gs->rules.base_full_armour,
                  FC_BASE_ARMOUR);

    fcRead(sim, &after);
    UT_ASSERT_MSG(after.numBases == before.numBases,
                  "the start left %u bases on a map that had %u",
                  (unsigned)after.numBases, (unsigned)before.numBases);
    for (i = 0; i < after.numBases; i++) {
        UT_ASSERT_MSG(after.baseArmour[i] == FC_BASE_ARMOUR,
                      "base %u held %u armour in the file and holds %u under "
                      "a cap of %d, wanted the cap", (unsigned)i,
                      (unsigned)before.baseArmour[i],
                      (unsigned)after.baseArmour[i], FC_BASE_ARMOUR);
        /* The fill is the whole of the caps in force rather than the one the
           mod raised, so the other two stocks are at theirs as well. */
        UT_ASSERT_MSG(after.baseShells[i] == gs->rules.base_full_shells,
                      "base %u is holding %u shells against a cap of %ld",
                      (unsigned)i, (unsigned)after.baseShells[i],
                      (long)gs->rules.base_full_shells);
        UT_ASSERT_MSG(after.baseMines[i] == gs->rules.base_full_mines,
                      "base %u is holding %u mines against a cap of %ld",
                      (unsigned)i, (unsigned)after.baseMines[i],
                      (long)gs->rules.base_full_mines);
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    fcDropMod();
    return 0;
}

/* ── 2. A raised pill cap, filled ─────────────────────────────────── */

static const char kFcPills[] =
    "scenario = {\n"
    "  name = \"Hard Pills\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  fill_to_caps = true,\n"
    "  rules = { pill_max_armour = 16 },\n"
    "}\n";

int run_scenario_fill_caps_raises_pills(void) {
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    FcStocks      before;
    FcStocks      after;
    char          err[512];
    BYTE          i;
    int           below = 0;

    UT_ASSERT(fcWriteMod("pills", "hardpills.lua", kFcPills));
    sim = fcSim();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    h = scenarioHostAttachMod(sim, fcDir, fcFile, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the mod was refused: %s", err);

    fcRead(sim, &before);
    UT_ASSERT_MSG(before.numPills > 0, "setup: the map carries no pillboxes");
    for (i = 0; i < before.numPills; i++) {
        if (before.pillArmour[i] < FC_PILL_ARMOUR) below++;
    }
    UT_ASSERT_MSG(below > 0,
                  "setup: every pillbox already holds %d armour or more, so "
                  "the fill would move nothing", FC_PILL_ARMOUR);

    serverSimStartGame(sim);

    UT_ASSERT_MSG(gs->rules.pill_max_armour == FC_PILL_ARMOUR,
                  "the round is on a pill_max_armour of %ld, wanted the mod's "
                  "%d", (long)gs->rules.pill_max_armour, FC_PILL_ARMOUR);

    fcRead(sim, &after);
    UT_ASSERT_MSG(after.numPills == before.numPills,
                  "the start left %u pillboxes on a map that had %u",
                  (unsigned)after.numPills, (unsigned)before.numPills);
    for (i = 0; i < after.numPills; i++) {
        UT_ASSERT_MSG(after.pillArmour[i] == FC_PILL_ARMOUR,
                      "pillbox %u held %u armour in the file and holds %u "
                      "under a cap of %d, wanted the cap", (unsigned)i,
                      (unsigned)before.pillArmour[i],
                      (unsigned)after.pillArmour[i], FC_PILL_ARMOUR);
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    fcDropMod();
    return 0;
}

/* ── 3. The same mod without the key ──────────────────────────────── */

/* Word for word the base mod above with the one line removed, so what the
   case reads is the key and not some other difference between two files. */
static const char kFcBasesNoKey[] =
    "scenario = {\n"
    "  name = \"Heavy Bases\",\n"
    "  api = 1,\n"
    "  bound = false,\n"
    "  rules = { base_full_armour = 150 },\n"
    "}\n";

int run_scenario_fill_caps_off_changes_nothing(void) {
    ServerSim    *sim;
    ScenarioHost *h;
    GameSim      *gs;
    FcStocks      before;
    FcStocks      after;
    char          err[512];
    BYTE          i;

    UT_ASSERT(fcWriteMod("nokey", "heavybases.lua", kFcBasesNoKey));
    sim = fcSim();
    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    h = scenarioHostAttachMod(sim, fcDir, fcFile, err, sizeof(err));
    UT_ASSERT_MSG(h != NULL, "the mod was refused: %s", err);

    fcRead(sim, &before);
    UT_ASSERT(before.numPills > 0 && before.numBases > 0);

    serverSimStartGame(sim);

    /* The same raised cap as the case above, so the one thing that differs
       between them is whether the map was filled to it. */
    UT_ASSERT_MSG(gs->rules.base_full_armour == FC_BASE_ARMOUR,
                  "the round is on a base_full_armour of %ld, wanted the "
                  "mod's %d", (long)gs->rules.base_full_armour,
                  FC_BASE_ARMOUR);

    fcRead(sim, &after);
    UT_ASSERT(after.numPills == before.numPills &&
              after.numBases == before.numBases);
    for (i = 0; i < after.numPills; i++) {
        UT_ASSERT_MSG(after.pillArmour[i] == before.pillArmour[i],
                      "pillbox %u held %u armour in the file and holds %u "
                      "after a round with no fill_to_caps", (unsigned)i,
                      (unsigned)before.pillArmour[i],
                      (unsigned)after.pillArmour[i]);
    }
    for (i = 0; i < after.numBases; i++) {
        UT_ASSERT_MSG(after.baseArmour[i] == before.baseArmour[i],
                      "base %u held %u armour in the file and holds %u after "
                      "a round with no fill_to_caps", (unsigned)i,
                      (unsigned)before.baseArmour[i],
                      (unsigned)after.baseArmour[i]);
        UT_ASSERT_MSG(after.baseShells[i] == before.baseShells[i],
                      "base %u held %u shells in the file and holds %u",
                      (unsigned)i, (unsigned)before.baseShells[i],
                      (unsigned)after.baseShells[i]);
        UT_ASSERT_MSG(after.baseMines[i] == before.baseMines[i],
                      "base %u held %u mines in the file and holds %u",
                      (unsigned)i, (unsigned)before.baseMines[i],
                      (unsigned)after.baseMines[i]);
    }

    scenarioHostDetach(h);
    serverSimDestroy(sim);
    fcDropMod();
    return 0;
}

/* ── 4. What the fill moved, in the recording ─────────────────────── */

/* The pillbox is the half of this only the fill's own record can settle.
 * Nothing else in a round of idle ticks writes a pill's armour, and a .wbv
 * carries one snapshot, at its head, which states the armour the map gave the
 * pill — so a viewer holding the filled armour was told about the fill. A
 * base is here for the round trip rather than for the record: the periodic
 * stock update restates every base within a regen period whatever the fill
 * writes. The clamp's own record case makes the same two points the other way
 * round.
 *
 * Driven through the sim call rather than through a script: the fill is the
 * sim's pass and the three cases above are what say a manifest reaches it. */

#define FC_RECORD_TAG    "scnFillCaps"
#define FC_RECORD_PLAYER "FillCaps"

static ScnOpResult fcSetRule(ServerSim *sim, uint16_t rule, double value) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_SET_RULE;
    op.u.setRule.rule  = rule;
    op.u.setRule.value = value;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

int run_scenario_fill_caps_records(void) {
    ReplayHarness h;
    GameSim      *gs;
    BYTE          numPills, i;
    int           under = -1;
    BYTE          underWas = 0;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, FC_RECORD_TAG,
                                              FC_RECORD_PLAYER),
                  "could not start recording");

    /* Let the round settle after the opening snapshot, which is what states
       the armour the fill is about to put up. */
    replayHarnessTick(&h, 4);

    gs = serverSimGetGameSim(h.sim);
    UT_ASSERT_MSG(gs != NULL, "the round has no GameSim");
    numPills = pillsGetNumPills(&gs->pb);
    for (i = 0; i < numPills; i++) {
        pillbox pill;
        memset(&pill, 0, sizeof(pill));
        pillsGetPill(&gs->pb, &pill, (BYTE)(i + 1));
        if (pill.armour < FC_PILL_ARMOUR) {
            under    = (int)i;
            underWas = pill.armour;
            break;
        }
    }
    UT_ASSERT_MSG(under >= 0,
                  "setup: every pillbox on the map already holds %d armour, "
                  "so the fill would move nothing", FC_PILL_ARMOUR);

    UT_ASSERT(fcSetRule(h.sim, SCN_RULE_pill_max_armour,
                        (double)FC_PILL_ARMOUR) == SCN_OP_OK);
    UT_ASSERT(fcSetRule(h.sim, SCN_RULE_base_full_armour,
                        (double)FC_BASE_ARMOUR) == SCN_OP_OK);
    serverSimScenarioFillWorldToRules(h.sim);

    {
        pillbox pill;
        memset(&pill, 0, sizeof(pill));
        pillsGetPill(&gs->pb, &pill, (BYTE)(under + 1));
        UT_ASSERT_MSG(pill.armour == FC_PILL_ARMOUR,
                      "setup: pillbox %d was at %u armour and the fill left "
                      "it at %u under a cap of %d, so there is nothing for "
                      "the record to carry", under, (unsigned)underWas,
                      (unsigned)pill.armour, FC_PILL_ARMOUR);
    }

    /* Two recorded ticks, so the records reach the file. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");
    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode the recording");
    UT_ASSERT_MSG(h.replayed != NULL, "the decode captured no world");

    UT_ASSERT_MSG(h.replayed->pills[under].armour == FC_PILL_ARMOUR,
                  "the viewer holds %u armour for pillbox %d and the sim "
                  "filled it to %d — the fill moved the record and wrote "
                  "nothing to say so",
                  (unsigned)h.replayed->pills[under].armour, under,
                  FC_PILL_ARMOUR);

    UT_ASSERT_MSG(replayHarnessCompare(&h),
                  "the replay disagrees with the sim: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}
