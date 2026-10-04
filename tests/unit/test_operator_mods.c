/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The dedicated server's -mod, -mod-required and -mod-locked: scripts the
 * operator puts on every game's script list, held there with three
 * strengths. serverSimApplyOperatorModArgs parses the flags into
 * serverSimAddOperatorMod (servermain.c prints what it says); the sim holds the rows apart from the list, puts them back on it at every new
 * lobby (serverSimRecordOperatorMods), and the CMD_SET_SCRIPT_LIST arm in
 * server_command_dispatch.c holds the host to them.
 *
 *   operator_mods_resolve  — a name finds its file as given, without its
 *       ending and in another case; an unknown name, a script tied to a map,
 *       a player's upload and a second scenario are each refused with a
 *       reason; a name given twice is one row at the firmer strength; and
 *       the lock bits the rows call for.
 *   operator_mods_required — a required row cannot be taken off by any
 *       list, the empty one included, and refusing it costs neither the tick
 *       gap nor a directory read; a list that keeps it may add, move and
 *       take off anything else, a default-on row included. The script-list
 *       event marks the required row and only it.
 *   operator_mods_locked   — under -mod-locked the list is exactly the
 *       operator's rows and every list is refused, the identical one and the
 *       empty one included; every row is fixed; the lock bits carry
 *       LOBBY_LOCK_SCRIPT_LIST and the MODS lock it implies.
 *   operator_mods_reapply  — a default-on row the host took off is back at
 *       the next return to the lobby and at the empty-lobby reset, with the
 *       host's own picks kept; an operator scenario takes the place of the
 *       host's; a list that already holds the rows is left alone.
 *   operator_mods_wire     — the required flag rides bit 2 of the entry's
 *       flags byte up and back, and a ClientSim answers it through
 *       clientSimGetLobbyScriptRequired.
 *   operator_mods_args     — the command-line parse: comma split, spaces and
 *       blank names, a flag with no value, an over-long value, -mod and
 *       -mod-required merging to the firmer, and -mod-locked beside either
 *       refused whole with SERVER_MOD_ARGS_CONFLICT.
 *   operator_mods_evict    — a full list of ten host picks loses its last
 *       pick to the one operator row.
 *   operator_mods_lock_mask — LOBBY_LOCK_SCRIPT_LIST in the lock mask alone,
 *       with no operator rows, refuses every list.
 *   operator_mods_mods_off — the re-apply switches Mods/Scenario back on
 *       when there are operator rows, and leaves it alone when there are none.
 *   operator_mods_map_yield — a plain -mod scenario gives way to a map's
 *       own scenario (not to a map's own mod) and keeps the map's own row
 *       on the list; a -mod-required one replaces both.
 *   operator_mods_setting_short_name — -setting finds an operator's row by
 *       its short name in any case and by its full name, a directory script
 *       by its short name, and the map's own script for a bare id; each bad
 *       argument is refused with the reason said.
 *   operator_mods_setting_reapply — a -setting value on a -mod row is the
 *       host's to change, and is back at the next return to the lobby and
 *       at the empty-lobby reset; a value on a script that is not the
 *       operator's keeps the host's pick.
 *   operator_mods_setting_locked — on a -mod-required or -mod-locked row
 *       the -setting value is refused to the host (CMD_REJECT_LOCKED), the
 *       row's other settings are not, and the join sync sends a LOCK after
 *       the SETs, with the value even when it is the default.
 *   operator_mods_setting_wire — a LOCK is a SET's body with another op,
 *       decoded with the same bounds; a client holds it until the next
 *       CLEAR, a SET-only stream (an older server) holds nothing, and an
 *       op a client does not know is skipped, as an older client skips a
 *       LOCK.
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"
#include "everard_map.h"
#include "scenario_defs.h"         /* ScnDirEntry, SCN_DIR_SOURCE_* */
#include "scenario_settings.h"     /* ScnSetting, scnSettingsBlobAppend */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->tick, SCENARIO_RELOAD_GAP_TICKS,
                                    * serverSimFillScriptListEvent */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled,
                                    * serverSimReturnToLobby */
#include "server_sim_scenario.h"   /* serverSimSetScenarioLister */
#include "server_lifecycle.h"      /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimResetLobbyToDefaults */
#include "threads.h"
#include "transport_control_codec.h"
#include "wire_limits.h"           /* LOBBY_LOCK_* */
#include "test_harness.h"

/* ── The fake scenarios directory ─────────────────────────────────── */

#define OM_DIR_MAX 20

typedef struct {
    int         count;
    int         calls;
    const char *files[OM_DIR_MAX];
    bool        keepsWin[OM_DIR_MAX];
    bool        bound[OM_DIR_MAX];
    uint8_t     source[OM_DIR_MAX];
} OmDir;

static int omList(void *ctx, const char *dir, ScnDirEntry *out, int max) {
    OmDir *d = (OmDir *)ctx;
    int    n = 0;

    (void)dir;
    d->calls++;
    while (n < d->count && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].file, sizeof(out[n].file), "%s", d->files[n]);
        snprintf(out[n].name, sizeof(out[n].name), "Script %d", n);
        out[n].keepsWinCondition = d->keepsWin[n];
        out[n].bound             = d->bound[n];
        out[n].source            = d->source[n];
        n++;
    }
    return n;
}

/* Three mods, two scenarios, one script tied to its map and one mod a
 * player uploaded. A host in slot 0 and a non-host human in slot 1. */
static ServerSim *omLobby(OmDir *d) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);

    memset(d, 0, sizeof(*d));
    d->count       = 7;
    d->files[0]    = "MacRules.scenario.lua";  /* a mod, with the ending */
    d->keepsWin[0] = true;
    d->files[1]    = "fastreload.lua";         /* a mod */
    d->keepsWin[1] = true;
    d->files[2]    = "nolgm.lua";              /* a mod */
    d->keepsWin[2] = true;
    d->files[3]    = "wave.scenario";          /* a scenario */
    d->files[4]    = "duel.scenario";          /* a second scenario */
    d->files[5]    = "tied.scenario";          /* tied to its map */
    d->bound[5]    = true;
    d->files[6]    = "uploaded.lua";           /* a player's upload */
    d->keepsWin[6] = true;
    d->source[6]   = SCN_DIR_SOURCE_UPLOAD;
    serverSimSetScenarioLister(sim, omList, d);
    return sim;
}

static void omPastCooldown(ServerSim *sim) {
    sim->tick += SCENARIO_RELOAD_GAP_TICKS;
}

static CmdResult omApply(ServerSim *sim, int senderSlot,
                         const char *const *files, int count) {
    ClientCommand cmd;
    CmdResult     r;
    int           i;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_LIST;
    cmd.cmdSeq = 1;
    cmd.u.setScriptList.count = (uint8_t)count;
    for (i = 0; i < count; i++) {
        snprintf(cmd.u.setScriptList.files[i],
                 sizeof(cmd.u.setScriptList.files[i]), "%s", files[i]);
    }
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Whether the list holds file, anywhere on it. */
static bool omListed(const ServerSim *sim, const char *file) {
    int i;

    for (i = 0; i < serverSimGetScriptCount(sim); i++) {
        if (strcmp(serverSimGetScript(sim, i)->file, file) == 0) return true;
    }
    return false;
}

static bool omAdd(ServerSim *sim, const char *name, ServerModStrength s) {
    char err[256];
    return serverSimAddOperatorMod(sim, name, s, err, sizeof(err));
}

/* ── 1. Names to rows ─────────────────────────────────────────────── */

int run_operator_mods_resolve(void) {
    ServerSim *sim;
    OmDir      d;
    char       err[256];

    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimOperatorModLocks(sim) == 0);

    /* Without the ending and in another case: the directory's own spelling
       is what is recorded, so the list names the file the lister knows. */
    UT_ASSERT_MSG(omAdd(sim, "macrules", SERVER_MOD_DEFAULT),
                  "\"macrules\" did not find MacRules.scenario.lua");
    UT_ASSERT(strcmp(serverSimGetOperatorModFile(sim, 0),
                     "MacRules.scenario.lua") == 0);
    UT_ASSERT_MSG(omAdd(sim, "fastreload", SERVER_MOD_DEFAULT),
                  "\"fastreload\" did not find fastreload.lua");
    UT_ASSERT(omAdd(sim, "wave.scenario", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimGetOperatorModCount(sim) == 3);
    /* A plain -mod is the host's to take off, so it locks nothing. */
    UT_ASSERT(serverSimOperatorModLocks(sim) == 0);
    UT_ASSERT(!serverSimOperatorModFixed(sim, "fastreload.lua"));

    /* The same file again, by its full name in capitals and from a firmer
       flag: one row, held as firmly as the firmer flag says. */
    UT_ASSERT(omAdd(sim, "FASTRELOAD.LUA", SERVER_MOD_REQUIRED));
    UT_ASSERT(serverSimGetOperatorModCount(sim) == 3);
    UT_ASSERT(serverSimGetOperatorModStrength(sim, 1) == SERVER_MOD_REQUIRED);
    /* And never weaker by a later, softer flag. */
    UT_ASSERT(omAdd(sim, "fastreload", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimGetOperatorModStrength(sim, 1) == SERVER_MOD_REQUIRED);
    UT_ASSERT(serverSimOperatorModFixed(sim, "fastreload.lua"));
    UT_ASSERT(!serverSimOperatorModFixed(sim, "MacRules.scenario.lua"));
    UT_ASSERT(serverSimOperatorModLocks(sim) == LOBBY_LOCK_MODS);

    /* Refused, each with a reason, none recorded. */
    err[0] = '\0';
    UT_ASSERT(!serverSimAddOperatorMod(sim, "missing", SERVER_MOD_DEFAULT,
                                       err, sizeof(err)));
    UT_ASSERT_MSG(err[0] != '\0', "an unknown name gave no reason");
    err[0] = '\0';
    UT_ASSERT(!serverSimAddOperatorMod(sim, "tied", SERVER_MOD_DEFAULT,
                                       err, sizeof(err)));
    UT_ASSERT_MSG(err[0] != '\0', "a script tied to a map gave no reason");
    err[0] = '\0';
    UT_ASSERT_MSG(!serverSimAddOperatorMod(sim, "duel", SERVER_MOD_DEFAULT,
                                           err, sizeof(err)),
                  "a second scenario was taken beside wave.scenario");
    UT_ASSERT(err[0] != '\0');
    UT_ASSERT_MSG(!omAdd(sim, "uploaded", SERVER_MOD_DEFAULT),
                  "a player's upload was taken for an operator mod");
    UT_ASSERT(!omAdd(sim, "", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimGetOperatorModCount(sim) == 3);
    UT_ASSERT(!serverSimGetOperatorModsLocked(sim));

    /* -mod-locked locks the list even when its name is refused: the
       operator said the list is theirs. */
    UT_ASSERT(!omAdd(sim, "missing", SERVER_MOD_LOCKED));
    UT_ASSERT(serverSimGetOperatorModsLocked(sim));
    UT_ASSERT(serverSimOperatorModLocks(sim) ==
              (LOBBY_LOCK_SCRIPT_LIST | LOBBY_LOCK_MODS));
    /* And every row is fixed once the list is locked, a -mod row included. */
    UT_ASSERT(serverSimOperatorModFixed(sim, "MacRules.scenario.lua"));

    /* The list lock implies the checkbox lock, the way a visibility lock
       implies classicmode. */
    UT_ASSERT(serverSimAddImpliedLocks(LOBBY_LOCK_SCRIPT_LIST) &
              LOBBY_LOCK_MODS);

    serverSimDestroy(sim);
    return 0;
}

/* ── 2. Required rows ─────────────────────────────────────────────── */

int run_operator_mods_required(void) {
    ServerSim   *sim;
    OmDir        d;
    ControlEvent evt;
    const char  *dropDefault[1]  = { "fastreload.lua" };
    const char  *dropRequired[1] = { "nolgm.lua" };
    const char  *addAround[3]    = { "wave.scenario", "MacRules.scenario.lua",
                                     "fastreload.lua" };
    int          i;
    int          seen = 0;

    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "nolgm", SERVER_MOD_DEFAULT));
    UT_ASSERT(omAdd(sim, "fastreload", SERVER_MOD_REQUIRED));
    UT_ASSERT_MSG(serverSimRecordOperatorMods(sim),
                  "putting two rows on an empty list changed nothing");
    UT_ASSERT(serverSimGetScriptCount(sim) == 2);
    UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->file, "nolgm.lua") == 0);
    UT_ASSERT(strcmp(serverSimGetScript(sim, 1)->file, "fastreload.lua") == 0);

    /* The event marks the required row and only it. */
    serverSimFillScriptListEvent(sim, 0, &evt);
    UT_ASSERT(evt.u.lobbyScriptList.count == 2);
    for (i = 0; i < evt.u.lobbyScriptList.count; i++) {
        const LobbyScriptEntry *e = &evt.u.lobbyScriptList.entries[i];
        bool want = strcmp(e->file, "fastreload.lua") == 0;
        UT_ASSERT_MSG(e->required == want,
                      "%s published required=%d", e->file, (int)e->required);
        if (e->required) seen++;
    }
    UT_ASSERT(seen == 1);

    /* A non-host is still a non-host: the operator rule comes after that. */
    UT_ASSERT(omApply(sim, 1, dropDefault, 1) == CMD_REJECT_NOT_HOST);

    /* A list without the required row is refused, the empty one included,
       before the tick gap is stamped or the directory read. */
    {
        int      calls = d.calls;
        uint32_t stamp = sim->scenarioPickTick;

        UT_ASSERT_MSG(omApply(sim, 0, dropRequired, 1) == CMD_REJECT_LOCKED,
                      "a list without the required row was not refused");
        UT_ASSERT_MSG(omApply(sim, 0, NULL, 0) == CMD_REJECT_LOCKED,
                      "a clear took the required row off");
        UT_ASSERT_MSG(d.calls == calls,
                      "a refused list read the directory");
        UT_ASSERT_MSG(sim->scenarioPickTick == stamp,
                      "a refused list stamped the tick gap");
        UT_ASSERT(serverSimGetScriptCount(sim) == 2);
    }

    /* The default-on row is the host's to take off. */
    UT_ASSERT_MSG(omApply(sim, 0, dropDefault, 1) == CMD_OK,
                  "taking a -mod row off kept the required one and was "
                  "refused");
    UT_ASSERT(serverSimGetScriptCount(sim) == 1);
    UT_ASSERT(!omListed(sim, "nolgm.lua"));

    /* And anything may go on around the required row, in any order. */
    omPastCooldown(sim);
    UT_ASSERT(omApply(sim, 0, addAround, 3) == CMD_OK);
    UT_ASSERT(serverSimGetScriptCount(sim) == 3);
    UT_ASSERT(strcmp(serverSimGetScript(sim, 2)->file, "fastreload.lua") == 0);

    serverSimDestroy(sim);
    return 0;
}

/* ── 3. A locked list ─────────────────────────────────────────────── */

int run_operator_mods_locked(void) {
    ServerSim  *sim;
    OmDir       d;
    const char *same[2]  = { "wave.scenario", "nolgm.lua" };
    const char *other[3] = { "wave.scenario", "nolgm.lua", "fastreload.lua" };
    const char *hostPick[1] = { "fastreload.lua" };

    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);

    /* A host pick from before the lock, which the locked list replaces. */
    UT_ASSERT(omApply(sim, 0, hostPick, 1) == CMD_OK);

    UT_ASSERT(omAdd(sim, "wave", SERVER_MOD_LOCKED));
    UT_ASSERT(omAdd(sim, "nolgm", SERVER_MOD_LOCKED));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 2,
                  "the locked list holds %d rows, wanted exactly the 2 "
                  "named", serverSimGetScriptCount(sim));
    UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->file, "wave.scenario") == 0);
    UT_ASSERT(strcmp(serverSimGetScript(sim, 1)->file, "nolgm.lua") == 0);
    UT_ASSERT(!omListed(sim, "fastreload.lua"));
    /* Recorded again, nothing to change. */
    UT_ASSERT(!serverSimRecordOperatorMods(sim));

    /* Every list is refused: the same one, a longer one, the empty one. */
    omPastCooldown(sim);
    UT_ASSERT(omApply(sim, 0, same, 2) == CMD_REJECT_LOCKED);
    UT_ASSERT(omApply(sim, 0, other, 3) == CMD_REJECT_LOCKED);
    UT_ASSERT(omApply(sim, 0, NULL, 0) == CMD_REJECT_LOCKED);
    UT_ASSERT(serverSimGetScriptCount(sim) == 2);

    /* Every row of the locked list is published fixed. */
    UT_ASSERT(serverSimOperatorModFixed(sim, "wave.scenario"));
    UT_ASSERT(serverSimOperatorModFixed(sim, "nolgm.lua"));

    /* And through the operator's lock mask, which is how a client is told:
       the startup ORs these into the -lock mask the instance config sets. */
    {
        ServerInstanceConfig cfg;

        memset(&cfg, 0, sizeof(cfg));
        cfg.serverLocks = serverSimOperatorModLocks(sim);
        serverSimApplyInstanceConfig(sim, &cfg);
        UT_ASSERT(serverSimGetServerLocks(sim) & LOBBY_LOCK_SCRIPT_LIST);
        UT_ASSERT(serverSimGetServerLocks(sim) & LOBBY_LOCK_MODS);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── 4. Put back at every new lobby ───────────────────────────────── */

int run_operator_mods_reapply(void) {
    ServerSim  *sim;
    OmDir       d;
    const char *hostList[2] = { "duel.scenario", "MacRules.scenario.lua" };
    const char *keepAll[3]  = { "nolgm.lua", "wave.scenario",
                                "fastreload.lua" };

    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "nolgm", SERVER_MOD_DEFAULT));
    UT_ASSERT(omAdd(sim, "wave", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT(serverSimGetScriptCount(sim) == 2);

    /* The host takes both -mod rows off and puts their own scenario and a
       mod on instead. Both rows are default-on, so that is allowed. */
    UT_ASSERT(omApply(sim, 0, hostList, 2) == CMD_OK);
    UT_ASSERT(!omListed(sim, "nolgm.lua"));
    UT_ASSERT(!omListed(sim, "wave.scenario"));

    /* The round ends and the lobby comes back with people still in it. The
       -mod rows are back; the operator's scenario takes the place of the
       host's, because a round runs one; the host's mod stays. */
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(omListed(sim, "nolgm.lua"),
                  "a default-on -mod was not back at the next lobby");
    UT_ASSERT(omListed(sim, "wave.scenario"));
    UT_ASSERT_MSG(!omListed(sim, "duel.scenario"),
                  "the host's scenario stayed beside the operator's");
    UT_ASSERT_MSG(omListed(sim, "MacRules.scenario.lua"),
                  "the host's own mod pick was dropped");
    UT_ASSERT(serverSimGetScriptCount(sim) == 3);

    /* A list that already holds every row is left alone, order and all. */
    omPastCooldown(sim);
    UT_ASSERT(omApply(sim, 0, keepAll, 3) == CMD_OK);
    UT_ASSERT(!serverSimRecordOperatorMods(sim));
    UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->file, "nolgm.lua") == 0);

    /* The empty-lobby reset puts them back too. */
    omPastCooldown(sim);
    {
        const char *only[1] = { "fastreload.lua" };
        UT_ASSERT(omApply(sim, 0, only, 1) == CMD_OK);
    }
    UT_ASSERT(!omListed(sim, "nolgm.lua"));
    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(omListed(sim, "nolgm.lua") &&
                      omListed(sim, "wave.scenario"),
                  "the empty-lobby reset did not put the -mod rows back");
    UT_ASSERT(omListed(sim, "fastreload.lua"));

    serverSimDestroy(sim);
    return 0;
}

/* ── 5. The flag on the wire ──────────────────────────────────────── */

int run_operator_mods_wire(void) {
    ControlEncodeBodyFn enc;
    ControlDecodeBodyFn dec;
    ControlEvent        evt;
    ControlEvent        back;
    uint8_t             buf[512];
    size_t              len = 0;
    ClientSim          *cs;

    enc = transportControlCodecBodyEncoder(CTRL_LOBBY_SCRIPT_LIST);
    dec = transportControlCodecBodyDecoder(CTRL_LOBBY_SCRIPT_LIST);
    UT_ASSERT(enc != NULL && dec != NULL);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_LIST;
    evt.u.lobbyScriptList.final = 1;
    evt.u.lobbyScriptList.count = 2;
    snprintf(evt.u.lobbyScriptList.entries[0].file,
             sizeof(evt.u.lobbyScriptList.entries[0].file), "%s", "a.lua");
    evt.u.lobbyScriptList.entries[0].keepsWinCondition = true;
    evt.u.lobbyScriptList.entries[0].required          = true;
    snprintf(evt.u.lobbyScriptList.entries[1].file,
             sizeof(evt.u.lobbyScriptList.entries[1].file), "%s", "b.lua");
    evt.u.lobbyScriptList.entries[1].keepsWinCondition = true;

    UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    /* final, count, then the first entry's flags byte: keeps-win (bit 0)
       and required (bit 2), and bound (bit 1) clear. */
    UT_ASSERT(len > 3);
    UT_ASSERT_MSG(buf[2] == 0x05,
                  "the required entry's flags byte is 0x%02X, wanted 0x05",
                  buf[2]);

    memset(&back, 0, sizeof(back));
    UT_ASSERT(dec(buf, len, &back));
    UT_ASSERT(back.u.lobbyScriptList.count == 2);
    UT_ASSERT(back.u.lobbyScriptList.entries[0].required);
    UT_ASSERT(back.u.lobbyScriptList.entries[0].keepsWinCondition);
    UT_ASSERT(!back.u.lobbyScriptList.entries[0].bound);
    UT_ASSERT(!back.u.lobbyScriptList.entries[1].required);

    /* And into a client, where the chooser reads it. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    back.type = CTRL_LOBBY_SCRIPT_LIST;
    clientSimApplyControl(cs, &back);
    UT_ASSERT(clientSimGetLobbyScriptCount(cs) == 2);
    UT_ASSERT(clientSimGetLobbyScriptRequired(cs, 0));
    UT_ASSERT(!clientSimGetLobbyScriptRequired(cs, 1));
    UT_ASSERT(!clientSimGetLobbyScriptRequired(cs, 2));
    clientSimDestroy(cs);
    return 0;
}

/* ── 6. The command line ──────────────────────────────────────────── */

/* What serverSimApplyOperatorModArgs said, kept for the asserts. */
typedef struct {
    int  n;
    char lines[8][700];
} OmSaid;

static void omSay(void *ctx, const char *line) {
    OmSaid *said = (OmSaid *)ctx;

    if (said->n < 8) {
        snprintf(said->lines[said->n], sizeof(said->lines[0]), "%s", line);
    }
    said->n++;
}

static bool omSaidHas(const OmSaid *said, const char *needle) {
    int i;

    for (i = 0; i < said->n && i < 8; i++) {
        if (strstr(said->lines[i], needle) != NULL) return true;
    }
    return false;
}

int run_operator_mods_args(void) {
    ServerSim *sim;
    OmDir      d;
    OmSaid     said;

    /* A comma list with spaces and blank names, the flag in capitals, a
       second flag naming the same file firmer, and a name that is not
       there. */
    {
        const char *argv[] = { "WinBoloDS", "-map", "x.map",
                               "-MOD", " nolgm , ,fastreload,",
                               "-mod-required", "FASTRELOAD",
                               "-mod", "missing" };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));

        sim = omLobby(&d);
        UT_ASSERT(sim != NULL);
        UT_ASSERT(serverSimOperatorModArgsGiven(argc, argv));
        UT_ASSERT(!serverSimOperatorModArgsConflict(argc, argv));
        memset(&said, 0, sizeof(said));
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, argc, argv, omSay,
                                                &said) == SERVER_MOD_ARGS_OK);
        UT_ASSERT_MSG(serverSimGetOperatorModCount(sim) == 2,
                      "%d rows, wanted nolgm and fastreload",
                      serverSimGetOperatorModCount(sim));
        UT_ASSERT(strcmp(serverSimGetOperatorModFile(sim, 0), "nolgm.lua") == 0);
        UT_ASSERT(serverSimGetOperatorModStrength(sim, 0) == SERVER_MOD_DEFAULT);
        UT_ASSERT(strcmp(serverSimGetOperatorModFile(sim, 1),
                         "fastreload.lua") == 0);
        UT_ASSERT_MSG(serverSimGetOperatorModStrength(sim, 1) ==
                          SERVER_MOD_REQUIRED,
                      "-mod and -mod-required on one name did not keep the "
                      "firmer");
        /* Blank names say nothing; only the unknown name is warned about. */
        UT_ASSERT_MSG(said.n == 1, "%d lines said, wanted 1", said.n);
        UT_ASSERT(omSaidHas(&said, "-mod missing"));
        UT_ASSERT(!serverSimGetOperatorModsLocked(sim));
        serverSimDestroy(sim);
    }

    /* A flag with no value: the last argument, or followed by another flag.
       Warned about, nothing recorded. */
    {
        const char *argv[] = { "WinBoloDS", "-mod", "-nolobby",
                               "-mod-required" };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));

        sim = omLobby(&d);
        UT_ASSERT(sim != NULL);
        memset(&said, 0, sizeof(said));
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, argc, argv, omSay,
                                                &said) == SERVER_MOD_ARGS_OK);
        UT_ASSERT(serverSimGetOperatorModCount(sim) == 0);
        UT_ASSERT(said.n == 2);
        UT_ASSERT(omSaidHas(&said, "-mod needs a mod name"));
        UT_ASSERT(omSaidHas(&said, "-mod-required needs a mod name"));
        serverSimDestroy(sim);
    }

    /* A value longer than the parse buffer is warned about and skipped
       whole, rather than cut into a name that is not the one meant. */
    {
        static char big[1200];
        const char *argv[] = { "WinBoloDS", "-mod", big };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        memcpy(big, "nolgm,", 6);
        sim = omLobby(&d);
        UT_ASSERT(sim != NULL);
        memset(&said, 0, sizeof(said));
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, argc, argv, omSay,
                                                &said) == SERVER_MOD_ARGS_OK);
        UT_ASSERT_MSG(serverSimGetOperatorModCount(sim) == 0,
                      "an over-long value was cut and read");
        UT_ASSERT(said.n == 1);
        UT_ASSERT(omSaidHas(&said, "longer than 1023 characters"));
        serverSimDestroy(sim);
    }

    /* -mod-locked with nothing usable still locks the list, empty. */
    {
        const char *argv[] = { "WinBoloDS", "-mod-locked", ",missing" };
        int argc = (int)(sizeof(argv) / sizeof(argv[0]));

        sim = omLobby(&d);
        UT_ASSERT(sim != NULL);
        memset(&said, 0, sizeof(said));
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, argc, argv, omSay,
                                                &said) == SERVER_MOD_ARGS_OK);
        UT_ASSERT(serverSimGetOperatorModsLocked(sim));
        UT_ASSERT(serverSimGetOperatorModCount(sim) == 0);
        UT_ASSERT(omSaidHas(&said, "-mod-locked missing"));
        UT_ASSERT(omSaidHas(&said, "locked empty"));
        serverSimDestroy(sim);
    }

    /* -mod-locked beside -mod, or beside -mod-required, in either order:
       refused whole, nothing recorded, not even the lock. */
    {
        const char *withMod[] = { "WinBoloDS", "-mod-locked", "wave",
                                  "-mod", "nolgm" };
        const char *withReq[] = { "WinBoloDS", "-Mod-Required", "nolgm",
                                  "-mod-locked", "wave" };
        const char *twoLocked[] = { "WinBoloDS", "-mod-locked", "wave",
                                    "-mod-locked", "nolgm" };

        UT_ASSERT(serverSimOperatorModArgsConflict(5, withMod));
        UT_ASSERT(serverSimOperatorModArgsConflict(5, withReq));
        UT_ASSERT(!serverSimOperatorModArgsConflict(5, twoLocked));

        sim = omLobby(&d);
        UT_ASSERT(sim != NULL);
        memset(&said, 0, sizeof(said));
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, 5, withMod, omSay,
                                                &said) ==
                  SERVER_MOD_ARGS_CONFLICT);
        UT_ASSERT(said.n == 1);
        UT_ASSERT(strcmp(said.lines[0], SERVER_MOD_ARGS_CONFLICT_TEXT) == 0);
        UT_ASSERT(serverSimGetOperatorModCount(sim) == 0);
        UT_ASSERT_MSG(!serverSimGetOperatorModsLocked(sim),
                      "a refused command line still locked the list");
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, 5, withReq, NULL, NULL) ==
                  SERVER_MOD_ARGS_CONFLICT);
        UT_ASSERT(serverSimGetOperatorModCount(sim) == 0);

        /* Two -mod-lockeds are one locked list. */
        UT_ASSERT(serverSimApplyOperatorModArgs(sim, 5, twoLocked, NULL,
                                                NULL) == SERVER_MOD_ARGS_OK);
        UT_ASSERT(serverSimGetOperatorModsLocked(sim));
        UT_ASSERT(serverSimGetOperatorModCount(sim) == 2);
        serverSimDestroy(sim);
    }

    /* None of the three, and a flag that only starts like one. */
    {
        const char *argv[] = { "WinBoloDS", "-modx", "a", "-moddir", "b",
                               "-nolobby" };

        UT_ASSERT(!serverSimOperatorModArgsGiven(6, argv));
        UT_ASSERT(!serverSimOperatorModArgsConflict(6, argv));
    }
    return 0;
}

/* ── 7. A full list ───────────────────────────────────────────────── */

int run_operator_mods_evict(void) {
    static const char *const kMore[10] = {
        "m0.lua", "m1.lua", "m2.lua", "m3.lua", "m4.lua",
        "m5.lua", "m6.lua", "m7.lua", "m8.lua", "m9.lua"
    };
    ServerSim *sim;
    OmDir      d;
    int        i;

    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    for (i = 0; i < 10; i++) {
        d.files[d.count]    = kMore[i];
        d.keepsWin[d.count] = true;
        d.count++;
    }

    /* The host fills the list with ten mods of their own. */
    UT_ASSERT(omApply(sim, 0, (const char *const *)kMore, 10) == CMD_OK);
    UT_ASSERT(serverSimGetScriptCount(sim) == 10);

    /* One operator row: the host's last pick makes room for it, and only
       that one. */
    UT_ASSERT(omAdd(sim, "nolgm", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT(serverSimGetScriptCount(sim) == 10);
    UT_ASSERT_MSG(!omListed(sim, "m9.lua"),
                  "the host's last pick was kept on a full list");
    for (i = 0; i < 9; i++) {
        UT_ASSERT_MSG(omListed(sim, kMore[i]), "%s was dropped", kMore[i]);
    }
    UT_ASSERT(strcmp(serverSimGetScript(sim, 9)->file, "nolgm.lua") == 0);

    serverSimDestroy(sim);
    return 0;
}

/* ── 8. The list lock on its own ──────────────────────────────────── */

int run_operator_mods_lock_mask(void) {
    ServerSim           *sim;
    OmDir                d;
    ServerInstanceConfig cfg;
    const char          *one[1] = { "nolgm.lua" };

    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);

    /* No operator rows at all: only the bit in the mask. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.serverLocks = LOBBY_LOCK_SCRIPT_LIST;
    serverSimApplyInstanceConfig(sim, &cfg);
    UT_ASSERT(serverSimGetOperatorModCount(sim) == 0);
    UT_ASSERT(!serverSimGetOperatorModsLocked(sim));

    UT_ASSERT_MSG(omApply(sim, 0, one, 1) == CMD_REJECT_LOCKED,
                  "LOBBY_LOCK_SCRIPT_LIST alone did not refuse a list");
    UT_ASSERT(omApply(sim, 0, NULL, 0) == CMD_REJECT_LOCKED);
    UT_ASSERT(serverSimGetScriptCount(sim) == 0);

    serverSimDestroy(sim);
    return 0;
}

/* ── 9. Mods/Scenario back on ─────────────────────────────────────── */

int run_operator_mods_mods_off(void) {
    ServerSim *sim;
    OmDir      d;

    /* With an operator row, the host's "off" lasts one lobby. */
    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "nolgm", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT(!serverSimRecordOperatorMods(sim));

    serverSimSetModsOff(sim, true);
    /* The list already holds the row, and that is still a change. */
    UT_ASSERT_MSG(serverSimRecordOperatorMods(sim),
                  "switching Mods/Scenario back on was not a change");
    UT_ASSERT(!serverSimGetModsOff(sim));

    serverSimSetModsOff(sim, true);
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(!serverSimGetModsOff(sim),
                  "the -mod row came back listed with Mods/Scenario off");
    UT_ASSERT(omListed(sim, "nolgm.lua"));
    serverSimDestroy(sim);

    /* With none, the setting is the host's and stays as left. */
    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    serverSimSetModsOff(sim, true);
    UT_ASSERT(!serverSimRecordOperatorMods(sim));
    serverSimReturnToLobby(sim);
    UT_ASSERT(serverSimGetModsOff(sim));
    serverSimDestroy(sim);
    return 0;
}

/* ── 10. A scripted map ───────────────────────────────────────────── */

static const ScnDirEntry *omFind(const ServerSim *sim, const char *file) {
    int i;

    for (i = 0; i < serverSimGetScriptCount(sim); i++) {
        if (strcmp(serverSimGetScript(sim, i)->file, file) == 0) {
            return serverSimGetScript(sim, i);
        }
    }
    return NULL;
}

static bool omHasBoundRow(const ServerSim *sim) {
    int i;

    for (i = 0; i < serverSimGetScriptCount(sim); i++) {
        if (serverSimGetScript(sim, i)->bound) return true;
    }
    return false;
}

/* The map's own row where a host placed it, and one mod behind it. */
static void omListMapRow(ServerSim *sim) {
    ScnDirEntry rows[2];

    memset(rows, 0, sizeof(rows));
    snprintf(rows[0].file, sizeof(rows[0].file), "%s", "own.scenario.lua");
    rows[0].bound = true;
    snprintf(rows[1].file, sizeof(rows[1].file), "%s",
             "MacRules.scenario.lua");
    rows[1].keepsWinCondition = true;
    serverSimSetScriptList(sim, rows, 2);
}

int run_operator_mods_map_yield(void) {
    ServerSim         *sim;
    OmDir              d;
    const ScnDirEntry *wave;
    const ScnDirEntry *mod;

    /* A plain -mod scenario: the map's own row stays on the list, and the
       -mod row gives way when the map's own script is a scenario. */
    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    omListMapRow(sim);
    UT_ASSERT(omAdd(sim, "wave", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT_MSG(omHasBoundRow(sim),
                  "a plain -mod scenario took the map's own row off");
    wave = omFind(sim, "wave.scenario");
    mod  = omFind(sim, "MacRules.scenario.lua");
    UT_ASSERT(wave != NULL && mod != NULL);
    UT_ASSERT_MSG(serverSimOperatorModYieldsToMap(sim, wave->file, true),
                  "a plain -mod scenario did not give way to a map's scenario");
    /* Not beside a map's own mod or a plain map, and never a mod. */
    UT_ASSERT_MSG(!serverSimOperatorModYieldsToMap(sim, wave->file, false),
                  "a plain -mod scenario gave way to a map's own mod");
    UT_ASSERT(!serverSimOperatorModYieldsToMap(sim, mod->file, true));
    /* Nor the map's own row itself. */
    UT_ASSERT(serverSimGetScript(sim, 0)->bound);
    UT_ASSERT(!serverSimOperatorModYieldsToMap(
        sim, serverSimGetScript(sim, 0)->file, true));
    serverSimDestroy(sim);

    /* A -mod-required scenario replaces the map's own, row and script. */
    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    omListMapRow(sim);
    UT_ASSERT(omAdd(sim, "wave", SERVER_MOD_REQUIRED));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT_MSG(!omHasBoundRow(sim),
                  "a -mod-required scenario left the map's own row on");
    wave = omFind(sim, "wave.scenario");
    UT_ASSERT(wave != NULL);
    UT_ASSERT(!serverSimOperatorModYieldsToMap(sim, wave->file, true));
    UT_ASSERT(omListed(sim, "MacRules.scenario.lua"));
    serverSimDestroy(sim);

    /* And a -mod-locked one too. */
    sim = omLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "wave", SERVER_MOD_LOCKED));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    wave = omFind(sim, "wave.scenario");
    UT_ASSERT(wave != NULL);
    UT_ASSERT(!serverSimOperatorModYieldsToMap(sim, wave->file, true));
    serverSimDestroy(sim);
    return 0;
}

/* ── 11. -setting on the operator's rows ──────────────────────────── */

static ScnSetting omIntSetting(const char *id, int32_t min, int32_t max,
                               int32_t step, int32_t def) {
    ScnSetting s;

    memset(&s, 0, sizeof(s));
    snprintf(s.id, sizeof(s.id), "%s", id);
    snprintf(s.label, sizeof(s.label), "%s", id);
    s.type = SCN_SETTING_TYPE_INT;
    s.min  = min;
    s.max  = max;
    s.step = step;
    s.def  = def;
    return s;
}

static ScnSetting omBoolSetting(const char *id, bool def) {
    ScnSetting s = omIntSetting(id, 0, 1, 1, def ? 1 : 0);

    s.type = SCN_SETTING_TYPE_BOOL;
    return s;
}

/* The declarations of omLobby's files: MacRules.scenario.lua declares the
 * bool pushback, default on, and armour 100..200 step 10 default 120;
 * nolgm.lua the bool fog, default off; wave.scenario rounds 1..5 default 5.
 * The rest declare nothing. */
static int omSettingsReader(void *ctx, const char *dir, const char *file,
                            uint8_t *out, size_t cap) {
    ScnSetting push   = omBoolSetting("pushback", true);
    ScnSetting armour = omIntSetting("armour", 100, 200, 10, 120);
    ScnSetting fog    = omBoolSetting("fog", false);
    ScnSetting rounds = omIntSetting("rounds", 1, 5, 1, 5);
    size_t     len    = 0;

    (void)ctx;
    (void)dir;
    if (strcmp(file, "MacRules.scenario.lua") == 0) {
        if (!scnSettingsBlobAppend(out, cap, &len, &push) ||
            !scnSettingsBlobAppend(out, cap, &len, &armour)) {
            return -1;
        }
    } else if (strcmp(file, "nolgm.lua") == 0) {
        if (!scnSettingsBlobAppend(out, cap, &len, &fog)) return -1;
    } else if (strcmp(file, "wave.scenario") == 0) {
        if (!scnSettingsBlobAppend(out, cap, &len, &rounds)) return -1;
    } else {
        return -1;
    }
    return (int)len;
}

static ServerSim *omSettingsLobby(OmDir *d) {
    ServerSim *sim = omLobby(d);

    if (sim != NULL) {
        serverSimSetScenarioSettingsReader(sim, omSettingsReader, NULL);
    }
    return sim;
}

static CmdResult omSetSetting(ServerSim *sim, int slot, const char *file,
                              const char *id, int32_t value) {
    ClientCommand cmd;
    CmdResult     r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_SETTING;
    cmd.cmdSeq = 1;
    snprintf(cmd.u.setScriptSetting.file,
             sizeof(cmd.u.setScriptSetting.file), "%s", file);
    snprintf(cmd.u.setScriptSetting.id, sizeof(cmd.u.setScriptSetting.id),
             "%s", id);
    cmd.u.setScriptSetting.value = value;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, slot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* file's value for id: the kept one, or def when none is kept. */
static int32_t omSettingValue(const ServerSim *sim, const char *file,
                              const char *id, int32_t def) {
    int32_t v = def;

    (void)serverSimGetScriptSetting(sim, file, id, &v);
    return v;
}

int run_operator_mods_setting_short_name(void) {
    ServerSim *sim;
    OmDir      d;
    OmSaid     said;

    sim = omSettingsLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "macrules", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));

    /* The short name, in another case, finds the operator's row. */
    memset(&said, 0, sizeof(said));
    UT_ASSERT_MSG(serverSimApplySettingArg(sim, "MACRULES:pushback=false",
                                           "", omSay, &said),
                  "a short name did not reach MacRules.scenario.lua: %s",
                  said.lines[0]);
    UT_ASSERT(omSettingValue(sim, "MacRules.scenario.lua", "pushback", 1) ==
              0);
    UT_ASSERT_MSG(omSaidHas(&said, "MacRules.scenario.lua:pushback = 0") &&
                      omSaidHas(&said, "each new lobby"),
                  "said: %s", said.lines[0]);
    /* And the full name still works. */
    UT_ASSERT(serverSimApplySettingArg(sim,
                                       "MacRules.scenario.lua:armour=150", "",
                                       NULL, NULL));
    UT_ASSERT(omSettingValue(sim, "MacRules.scenario.lua", "armour", 120) ==
              150);
    UT_ASSERT(sim->operatorSettingCount == 2);

    /* A script that is not the operator's: found by its short name in the
       directory and set, but not kept for the next lobby. */
    UT_ASSERT(serverSimApplySettingArg(sim, "wave:rounds=2", "", NULL, NULL));
    UT_ASSERT(omSettingValue(sim, "wave.scenario", "rounds", 5) == 2);
    UT_ASSERT(sim->operatorSettingCount == 2);
    UT_ASSERT(!serverSimOperatorSettingLocked(sim, "wave.scenario",
                                              "rounds"));

    /* A bare id is the map's own script, and only that. */
    UT_ASSERT(serverSimApplySettingArg(sim, "rounds=3", "wave.scenario", NULL,
                                       NULL));
    UT_ASSERT(omSettingValue(sim, "wave.scenario", "rounds", 5) == 3);
    memset(&said, 0, sizeof(said));
    UT_ASSERT(!serverSimApplySettingArg(sim, "pushback=false", "", omSay,
                                        &said));
    UT_ASSERT(omSaidHas(&said, "the map has no script"));

    /* Each refusal says why and changes nothing. */
    memset(&said, 0, sizeof(said));
    UT_ASSERT(!serverSimApplySettingArg(sim, "missing:pushback=false", "",
                                        omSay, &said));
    UT_ASSERT(omSaidHas(&said, "declares no setting 'pushback'"));
    memset(&said, 0, sizeof(said));
    UT_ASSERT(!serverSimApplySettingArg(sim, "macrules:nope=1", "", omSay,
                                        &said));
    UT_ASSERT(omSaidHas(&said, "declares no setting 'nope'"));
    memset(&said, 0, sizeof(said));
    UT_ASSERT(!serverSimApplySettingArg(sim, "macrules:pushback=maybe", "",
                                        omSay, &said));
    UT_ASSERT(omSaidHas(&said, "is not a value of"));
    memset(&said, 0, sizeof(said));
    UT_ASSERT(!serverSimApplySettingArg(sim, "macrules", "", omSay, &said));
    UT_ASSERT(omSaidHas(&said, "wanted [file:]id=value"));
    memset(&said, 0, sizeof(said));
    UT_ASSERT(!serverSimApplySettingArg(sim, ":pushback=1", "", omSay,
                                        &said));
    UT_ASSERT(omSaidHas(&said, "bad file name"));
    UT_ASSERT(omSettingValue(sim, "MacRules.scenario.lua", "pushback", 1) ==
              0);
    UT_ASSERT(sim->operatorSettingCount == 2);

    serverSimDestroy(sim);
    return 0;
}

int run_operator_mods_setting_reapply(void) {
    ServerSim *sim;
    OmDir      d;

    sim = omSettingsLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "macrules", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT(serverSimApplySettingArg(sim, "macrules:pushback=off", "", NULL,
                                       NULL));
    UT_ASSERT(serverSimApplySettingArg(sim, "macrules:armour=150", "", NULL,
                                       NULL));
    UT_ASSERT(serverSimApplySettingArg(sim, "wave:rounds=2", "", NULL, NULL));

    /* On a -mod row the value is a default: the host may change it. */
    UT_ASSERT(!serverSimOperatorSettingLocked(sim, "MacRules.scenario.lua",
                                              "pushback"));
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "pushback", 1) ==
              CMD_OK);
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "armour", 120) ==
              CMD_OK);
    UT_ASSERT(omSetSetting(sim, 0, "wave.scenario", "rounds", 4) == CMD_OK);
    UT_ASSERT(omSettingValue(sim, "MacRules.scenario.lua", "pushback", 1) ==
              1);
    UT_ASSERT(!serverSimGetScriptSetting(sim, "MacRules.scenario.lua",
                                         "armour", NULL));

    /* The next lobby has the operator's values again. A script that is not
       the operator's keeps the host's pick, as it always has. */
    serverSimReturnToLobby(sim);
    UT_ASSERT_MSG(omSettingValue(sim, "MacRules.scenario.lua", "pushback",
                                 1) == 0,
                  "the operator's pushback was not back at the next lobby");
    UT_ASSERT(omSettingValue(sim, "MacRules.scenario.lua", "armour", 120) ==
              150);
    UT_ASSERT(omSettingValue(sim, "wave.scenario", "rounds", 5) == 4);

    /* And at the empty-lobby reset. */
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "pushback", 1) ==
              CMD_OK);
    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(omSettingValue(sim, "MacRules.scenario.lua", "pushback",
                                 1) == 0,
                  "the empty-lobby reset did not put the operator's value "
                  "back");

    /* A value already in force is not published again. */
    {
        int before = sim->scriptSettingValueCount;
        UT_ASSERT(!serverSimRecordOperatorMods(sim));
        UT_ASSERT(sim->scriptSettingValueCount == before);
    }

    serverSimDestroy(sim);
    return 0;
}

/* What a join sync delivered, in order, for the asserts. */
typedef struct {
    int     n;
    uint8_t op[16];
    char    id[16][SCN_SETTING_ID_LEN];
    int32_t value[16];
} OmSettingSeen;

static void omSettingWatch(void *ctx, const ControlEvent *evt) {
    OmSettingSeen *s = (OmSettingSeen *)ctx;

    if (evt->type != CTRL_LOBBY_SCRIPT_SETTING || s->n >= 16) return;
    s->op[s->n] = evt->u.lobbyScriptSetting.op;
    snprintf(s->id[s->n], sizeof(s->id[0]), "%s",
             evt->u.lobbyScriptSetting.id);
    s->value[s->n] = evt->u.lobbyScriptSetting.value;
    s->n++;
}

/* Where id's event of kind op is in seen, or -1. */
static int omSeenAt(const OmSettingSeen *s, uint8_t op, const char *id) {
    int i;

    for (i = 0; i < s->n; i++) {
        if (s->op[i] == op && strcmp(s->id[i], id) == 0) return i;
    }
    return -1;
}

int run_operator_mods_setting_locked(void) {
    ServerSim     *sim;
    OmDir          d;
    OmSaid         said;
    OmSettingSeen  seen;
    SubscriberHandle h;
    int            lockAt;
    int            setAt;

    sim = omSettingsLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "macrules", SERVER_MOD_REQUIRED));
    UT_ASSERT(omAdd(sim, "nolgm", SERVER_MOD_DEFAULT));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    memset(&said, 0, sizeof(said));
    UT_ASSERT(serverSimApplySettingArg(sim, "macrules:pushback=false", "",
                                       omSay, &said));
    UT_ASSERT_MSG(omSaidHas(&said, "the host cannot change it"),
                  "said: %s", said.lines[0]);
    UT_ASSERT(serverSimApplySettingArg(sim, "nolgm:fog=true", "", NULL,
                                       NULL));

    /* Only the -setting value on the required row is held. */
    UT_ASSERT(serverSimOperatorSettingLocked(sim, "MacRules.scenario.lua",
                                             "pushback"));
    UT_ASSERT(!serverSimOperatorSettingLocked(sim, "MacRules.scenario.lua",
                                              "armour"));
    UT_ASSERT(!serverSimOperatorSettingLocked(sim, "nolgm.lua", "fog"));

    /* The server refuses the host's change to it, the same value
       included. A player who is not the host is refused before the lock
       is asked (CMD_REJECT_NOT_HOST). */
    UT_ASSERT_MSG(omSetSetting(sim, 0, "MacRules.scenario.lua", "pushback",
                               1) == CMD_REJECT_LOCKED,
                  "the host changed a locked setting");
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "pushback", 0) ==
              CMD_REJECT_LOCKED);
    UT_ASSERT(omSettingValue(sim, "MacRules.scenario.lua", "pushback", 1) ==
              0);
    /* The row's other setting and the -mod row's setting stay the host's. */
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "armour", 150) ==
              CMD_OK);
    UT_ASSERT(omSetSetting(sim, 0, "nolgm.lua", "fog", 0) == CMD_OK);

    /* The join sync: the SETs, then a LOCK for the held value only. */
    memset(&seen, 0, sizeof(seen));
    h = serverSimRegisterSubscriber(sim, omSettingWatch, &seen);
    (void)h;
    UT_ASSERT(seen.n >= 1 && seen.op[0] == LOBBY_SCRIPT_SETTING_CLEAR);
    lockAt = omSeenAt(&seen, LOBBY_SCRIPT_SETTING_LOCK, "pushback");
    setAt  = omSeenAt(&seen, LOBBY_SCRIPT_SETTING_SET, "pushback");
    UT_ASSERT_MSG(lockAt > 0, "the join sync sent no LOCK for pushback");
    UT_ASSERT(seen.value[lockAt] == 0);
    UT_ASSERT_MSG(setAt > 0 && setAt < lockAt,
                  "the LOCK did not come after pushback's SET");
    UT_ASSERT(omSeenAt(&seen, LOBBY_SCRIPT_SETTING_LOCK, "fog") < 0);
    UT_ASSERT(omSeenAt(&seen, LOBBY_SCRIPT_SETTING_LOCK, "armour") < 0);
    serverSimDestroy(sim);

    /* -mod-locked: the same rule. A held value equal to its default has no
       SET in the sync, so its LOCK carries the value. */
    sim = omSettingsLobby(&d);
    UT_ASSERT(sim != NULL);
    UT_ASSERT(omAdd(sim, "macrules", SERVER_MOD_LOCKED));
    UT_ASSERT(serverSimRecordOperatorMods(sim));
    UT_ASSERT(serverSimApplySettingArg(sim, "macrules:armour=120", "", NULL,
                                       NULL));
    UT_ASSERT(serverSimOperatorSettingLocked(sim, "MacRules.scenario.lua",
                                             "armour"));
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "armour", 150) ==
              CMD_REJECT_LOCKED);
    UT_ASSERT(omSetSetting(sim, 0, "MacRules.scenario.lua", "pushback", 0) ==
              CMD_OK);
    memset(&seen, 0, sizeof(seen));
    (void)serverSimRegisterSubscriber(sim, omSettingWatch, &seen);
    UT_ASSERT(omSeenAt(&seen, LOBBY_SCRIPT_SETTING_SET, "armour") < 0);
    lockAt = omSeenAt(&seen, LOBBY_SCRIPT_SETTING_LOCK, "armour");
    UT_ASSERT(lockAt > 0 && seen.value[lockAt] == 120);
    serverSimDestroy(sim);
    return 0;
}

int run_operator_mods_setting_wire(void) {
    ControlEncodeBodyFn enc;
    ControlDecodeBodyFn dec;
    ControlEvent        evt;
    ControlEvent        back;
    uint8_t             setBuf[256];
    uint8_t             lockBuf[256];
    size_t              setLen  = 0;
    size_t              lockLen = 0;
    ClientSim          *cs;
    int32_t             v = -1;

    enc = transportControlCodecBodyEncoder(CTRL_LOBBY_SCRIPT_SETTING);
    dec = transportControlCodecBodyDecoder(CTRL_LOBBY_SCRIPT_SETTING);
    UT_ASSERT(enc != NULL && dec != NULL);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_SET;
    snprintf(evt.u.lobbyScriptSetting.file,
             sizeof(evt.u.lobbyScriptSetting.file), "%s",
             "MacRules.scenario.lua");
    snprintf(evt.u.lobbyScriptSetting.id, sizeof(evt.u.lobbyScriptSetting.id),
             "%s", "pushback");
    evt.u.lobbyScriptSetting.value = 0;
    UT_ASSERT(enc(&evt, NULL, setBuf, sizeof(setBuf), &setLen) == ENCODE_OK);
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_LOCK;
    UT_ASSERT(enc(&evt, NULL, lockBuf, sizeof(lockBuf), &lockLen) ==
              ENCODE_OK);

    /* A LOCK is a SET's body with another op byte: same length, so the
       exact-length check an older decoder makes still passes, and that
       decoder hands the op to a client that skips it. */
    UT_ASSERT(lockLen == setLen);
    UT_ASSERT(lockBuf[0] == LOBBY_SCRIPT_SETTING_LOCK);
    UT_ASSERT(memcmp(lockBuf + 1, setBuf + 1, setLen - 1) == 0);
    memset(&back, 0, sizeof(back));
    UT_ASSERT(dec(lockBuf, lockLen, &back));
    UT_ASSERT(back.u.lobbyScriptSetting.op == LOBBY_SCRIPT_SETTING_LOCK);
    UT_ASSERT(strcmp(back.u.lobbyScriptSetting.file,
                     "MacRules.scenario.lua") == 0);
    UT_ASSERT(strcmp(back.u.lobbyScriptSetting.id, "pushback") == 0);
    UT_ASSERT(back.u.lobbyScriptSetting.value == 0);
    /* Bounded as before: a trailing byte or a cut body is refused. */
    UT_ASSERT(!dec(lockBuf, lockLen - 1, &back));
    lockBuf[lockLen] = 0;
    UT_ASSERT(!dec(lockBuf, lockLen + 1, &back));

    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    /* An older server: a CLEAR and a SET, and nothing is held. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_CLEAR;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(dec(setBuf, setLen, &back));
    back.type = CTRL_LOBBY_SCRIPT_SETTING;
    clientSimApplyControl(cs, &back);
    UT_ASSERT(clientSimGetLobbyScriptSetting(cs, "MacRules.scenario.lua",
                                             "pushback", &v) && v == 0);
    UT_ASSERT(!clientSimGetLobbyScriptSettingLocked(
        cs, "MacRules.scenario.lua", "pushback"));

    /* This server: the LOCK holds it, and a later SET (the value put back
       at a new lobby) leaves it held. */
    UT_ASSERT(dec(lockBuf, lockLen, &back));
    back.type = CTRL_LOBBY_SCRIPT_SETTING;
    clientSimApplyControl(cs, &back);
    UT_ASSERT(clientSimGetLobbyScriptSettingLocked(
        cs, "MacRules.scenario.lua", "pushback"));
    UT_ASSERT(dec(setBuf, setLen, &back));
    back.type = CTRL_LOBBY_SCRIPT_SETTING;
    clientSimApplyControl(cs, &back);
    UT_ASSERT(clientSimGetLobbyScriptSettingLocked(
        cs, "MacRules.scenario.lua", "pushback"));
    UT_ASSERT(!clientSimGetLobbyScriptSettingLocked(
        cs, "MacRules.scenario.lua", "armour"));

    /* A LOCK alone carries its value: a held default has no SET. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_LOCK;
    snprintf(evt.u.lobbyScriptSetting.file,
             sizeof(evt.u.lobbyScriptSetting.file), "%s",
             "MacRules.scenario.lua");
    snprintf(evt.u.lobbyScriptSetting.id, sizeof(evt.u.lobbyScriptSetting.id),
             "%s", "armour");
    evt.u.lobbyScriptSetting.value = 120;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetLobbyScriptSetting(cs, "MacRules.scenario.lua",
                                             "armour", &v) && v == 120);
    UT_ASSERT(clientSimGetLobbyScriptSettingLocked(
        cs, "MacRules.scenario.lua", "armour"));

    /* An op a build does not know is skipped, which is what an older
       client does with a LOCK. */
    evt.u.lobbyScriptSetting.op = 7;
    snprintf(evt.u.lobbyScriptSetting.id, sizeof(evt.u.lobbyScriptSetting.id),
             "%s", "unknown");
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(!clientSimGetLobbyScriptSetting(cs, "MacRules.scenario.lua",
                                              "unknown", NULL));

    /* The next join sync's CLEAR lets every lock go. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_CLEAR;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(!clientSimGetLobbyScriptSettingLocked(
        cs, "MacRules.scenario.lua", "pushback"));
    clientSimDestroy(cs);
    return 0;
}
