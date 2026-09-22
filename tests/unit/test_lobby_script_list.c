/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The lobby's ordered script list: one scenario deciding the round and mods
 * behind it changing how it plays. It travels up as CMD_SET_SCRIPT_LIST, one
 * command carrying the whole list, and back down as CTRL_LOBBY_SCRIPT_LIST,
 * chunked at LOBBY_SCRIPT_LIST_CHUNK entries.
 *
 *   script_list_control_codec  — one chunk encoded from a filled event and
 *       compared against bytes written out by hand, then decoded from those
 *       same hand-written bytes and checked field by field. The same case
 *       refuses a count past the chunk cap, a body that stops inside an
 *       entry, and a body with a byte left over; checks that the decode
 *       zeroes the entries above the count; and measures a full chunk of
 *       longest-possible entries against the control segment it has to ride
 *       in, which is the ceiling delivery applies.
 *   script_list_command_codec  — the whole list up the wire: a round trip
 *       through the production encoder and decoder, the worst case measured
 *       against COMMAND_MAX_WIRE_BYTES, and a count past the cap refused on
 *       both sides.
 *   script_list_dispatch       — the CMD_SET_SCRIPT_LIST case in
 *       server_command_dispatch.c: who may set a list, what it refuses, and
 *       that a refusal leaves the previous list alone. Including the one
 *       bound name it lets through, which is the committed map's own script
 *       and is how a host says where on the list that script is composed.
 *   script_list_lists_once     — a full list of ten names reads the mod
 *       directories once, not once per name, and a refused list reads them
 *       once too.
 *   script_list_client_apply   — a two-chunk list reassembled into a
 *       ClientSim and read back through the accessors the lobby chooser
 *       uses, including a run that would overrun the cap.
 *
 * The segment and not MAX_CONTROL_PACKET is the ceiling that matters for the
 * chunk: one control event is one channel segment, and the encoder is handed
 * CHANNEL_CONTROL_SEG less the channel frame's type(1) and bodyLen(2). A
 * chunk measured against the 1400-byte datagram instead would pass here and
 * be dropped at delivery with nothing visible to the client.
 *
 * Reads the ClientSim struct directly; the unittests profile permits it.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "channel_mux.h"           /* CHANNEL_CONTROL_SEG — the real ceiling */
#include "client_command.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"
#include "everard_map.h"
#include "netpacks.h"              /* PACKET_SET_SCRIPT_LIST */
#include "scenario_defs.h"         /* ScnDirEntry — what the lister fills */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->tick, SCENARIO_RELOAD_GAP_TICKS,
                                    * serverSimGetScriptCount */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"   /* serverSimSetScenarioLister */
#include "threads.h"
#include "transport_command_codec.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* What the encoder is really handed at delivery: a control segment less the
 * channel frame's type(1) and bodyLen(2). */
#define SL_BODY_CAP (CHANNEL_CONTROL_SEG - 3)

/* ── 1. One chunk, byte for byte and back ─────────────────────────── */

/* Two entries: a scenario that is bound to its map, and a mod that is not.
 * Bit 0 of the flags byte keeps the win condition, bit 1 is bound. */
static const uint8_t kSlBody[] = {
    0x01, 0x02,                                     /* final, two entries */
    0x02, 0x04, 'w', 'a', 'v', 'e', 0x04, 'W', 'a', 'v', 'e',
    0x01, 0x07, 'f', 'a', 's', 't', '.', 'l', 'u', 0x04, 'F', 'a', 's', 't'
};

static bool slDecodeBody(const uint8_t *body, size_t len, ControlEvent *out) {
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_LOBBY_SCRIPT_LIST);
    if (dec == NULL) return false;
    return dec(body, len, out);
}

int run_script_list_control_codec(void) {
    ControlEncodeBodyFn enc;
    ControlEvent        evt;
    ControlEvent        back;
    uint8_t             buf[SL_BODY_CAP];
    size_t              len = 0;
    size_t              i;

    enc = transportControlCodecBodyEncoder(CTRL_LOBBY_SCRIPT_LIST);
    UT_ASSERT_MSG(enc != NULL, "no body encoder for CTRL_LOBBY_SCRIPT_LIST");

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_LIST;
    evt.u.lobbyScriptList.final = 1;
    evt.u.lobbyScriptList.count = 2;
    snprintf(evt.u.lobbyScriptList.entries[0].file,
             sizeof(evt.u.lobbyScriptList.entries[0].file), "%s", "wave");
    snprintf(evt.u.lobbyScriptList.entries[0].name,
             sizeof(evt.u.lobbyScriptList.entries[0].name), "%s", "Wave");
    evt.u.lobbyScriptList.entries[0].keepsWinCondition = false;
    evt.u.lobbyScriptList.entries[0].bound             = true;
    snprintf(evt.u.lobbyScriptList.entries[1].file,
             sizeof(evt.u.lobbyScriptList.entries[1].file), "%s", "fast.lu");
    snprintf(evt.u.lobbyScriptList.entries[1].name,
             sizeof(evt.u.lobbyScriptList.entries[1].name), "%s", "Fast");
    evt.u.lobbyScriptList.entries[1].keepsWinCondition = true;
    evt.u.lobbyScriptList.entries[1].bound             = false;

    UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT_MSG(len == sizeof(kSlBody),
                  "the chunk encoded to %d bytes, the hand-written body is %d",
                  (int)len, (int)sizeof(kSlBody));
    for (i = 0; i < sizeof(kSlBody); i++) {
        UT_ASSERT_MSG(buf[i] == kSlBody[i],
                      "byte %d encoded as 0x%02X, the hand-written body has "
                      "0x%02X", (int)i, buf[i], kSlBody[i]);
    }

    /* And back out of the hand-written bytes, so a symmetric drift in both
       halves of the codec still fails here. */
    memset(&back, 0, sizeof(back));
    UT_ASSERT(slDecodeBody(kSlBody, sizeof(kSlBody), &back));
    UT_ASSERT(back.type == CTRL_LOBBY_SCRIPT_LIST);
    UT_ASSERT(back.u.lobbyScriptList.final == 1);
    UT_ASSERT(back.u.lobbyScriptList.count == 2);
    UT_ASSERT(strcmp(back.u.lobbyScriptList.entries[0].file, "wave") == 0);
    UT_ASSERT(strcmp(back.u.lobbyScriptList.entries[0].name, "Wave") == 0);
    UT_ASSERT(!back.u.lobbyScriptList.entries[0].keepsWinCondition);
    UT_ASSERT(back.u.lobbyScriptList.entries[0].bound);
    UT_ASSERT(strcmp(back.u.lobbyScriptList.entries[1].file, "fast.lu") == 0);
    UT_ASSERT(strcmp(back.u.lobbyScriptList.entries[1].name, "Fast") == 0);
    UT_ASSERT(back.u.lobbyScriptList.entries[1].keepsWinCondition);
    UT_ASSERT(!back.u.lobbyScriptList.entries[1].bound);
    /* The entries above the count are the decode's to clear, or a shorter
       chunk would leave a row of the last one behind it. */
    for (i = 2; i < LOBBY_SCRIPT_LIST_CHUNK; i++) {
        UT_ASSERT_MSG(back.u.lobbyScriptList.entries[i].file[0] == '\0',
                      "entry %d survived a two-entry chunk", (int)i);
    }

    /* An empty list is a chunk and not silence: count 0, final 1. */
    {
        const uint8_t empty[] = { 0x01, 0x00 };
        memset(&back, 0, sizeof(back));
        UT_ASSERT(slDecodeBody(empty, sizeof(empty), &back));
        UT_ASSERT(back.u.lobbyScriptList.count == 0);
        UT_ASSERT(back.u.lobbyScriptList.final == 1);
    }

    /* A count past the chunk cap, on both sides. */
    {
        uint8_t over[2];
        over[0] = 1;
        over[1] = (uint8_t)(LOBBY_SCRIPT_LIST_CHUNK + 1);
        UT_ASSERT(!slDecodeBody(over, sizeof(over), &back));
        evt.u.lobbyScriptList.count = (uint8_t)(LOBBY_SCRIPT_LIST_CHUNK + 1);
        UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &len) != ENCODE_OK);
        evt.u.lobbyScriptList.count = 2;
    }

    /* A body that stops inside an entry, and one with a byte left over. */
    UT_ASSERT(!slDecodeBody(kSlBody, sizeof(kSlBody) - 1, &back));
    {
        uint8_t longer[sizeof(kSlBody) + 1];
        memcpy(longer, kSlBody, sizeof(kSlBody));
        longer[sizeof(kSlBody)] = 0x00;
        UT_ASSERT_MSG(!slDecodeBody(longer, sizeof(longer), &back),
                      "a body with a byte left over decoded, so the sender "
                      "and this reader disagree about the entry shape and "
                      "nothing says so");
    }

    /* And the measurement the chunk size was chosen by: a full chunk of the
       longest entries the fields can hold, against the segment. */
    {
        size_t widest;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_SCRIPT_LIST;
        evt.u.lobbyScriptList.final = 1;
        evt.u.lobbyScriptList.count = LOBBY_SCRIPT_LIST_CHUNK;
        for (i = 0; i < LOBBY_SCRIPT_LIST_CHUNK; i++) {
            memset(evt.u.lobbyScriptList.entries[i].file, 'f',
                   LOBBY_SCENARIO_FILE_LEN - 1);
            memset(evt.u.lobbyScriptList.entries[i].name, 'n',
                   LOBBY_SCENARIO_NAME_LEN - 1);
        }
        UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &widest) == ENCODE_OK);
        UT_ASSERT_MSG(widest == 2 + (size_t)LOBBY_SCRIPT_LIST_CHUNK * 193,
                      "a full chunk measured %d bytes, the sizing in "
                      "control_event.h says %d", (int)widest,
                      (int)(2 + LOBBY_SCRIPT_LIST_CHUNK * 193));
        UT_ASSERT_MSG(widest <= (size_t)SL_BODY_CAP,
                      "a full chunk is %d bytes and a control segment carries "
                      "%d: it would be dropped at delivery", (int)widest,
                      (int)SL_BODY_CAP);
    }
    return 0;
}

/* ── 2. The whole list up the wire ────────────────────────────────── */

int run_script_list_command_codec(void) {
    ClientCommand cmd;
    ClientCommand back;
    uint8_t       buf[COMMAND_MAX_WIRE_BYTES];
    size_t        len = 0;
    int           i;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_SET_SCRIPT_LIST;
    cmd.cmdSeq = 7;
    cmd.u.setScriptList.count = 3;
    snprintf(cmd.u.setScriptList.files[0],
             sizeof(cmd.u.setScriptList.files[0]), "%s", "wave.scenario");
    snprintf(cmd.u.setScriptList.files[1],
             sizeof(cmd.u.setScriptList.files[1]), "%s", "fastreload.lua");
    snprintf(cmd.u.setScriptList.files[2],
             sizeof(cmd.u.setScriptList.files[2]), "%s", "nolgm.lua");

    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    UT_ASSERT_MSG(buf[2] == PACKET_SET_SCRIPT_LIST,
                  "the packet type byte is %d, wanted PACKET_SET_SCRIPT_LIST",
                  (int)buf[2]);
    memset(&back, 0, sizeof(back));
    UT_ASSERT(commandCodecDecode(buf, len, &back));
    UT_ASSERT(back.type == CMD_SET_SCRIPT_LIST);
    UT_ASSERT(back.cmdSeq == 7);
    UT_ASSERT(back.u.setScriptList.count == 3);
    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(strcmp(back.u.setScriptList.files[i],
                             cmd.u.setScriptList.files[i]) == 0,
                      "entry %d came back as \"%s\"", i,
                      back.u.setScriptList.files[i]);
    }
    /* The names above the count are the decode's to clear. */
    UT_ASSERT(back.u.setScriptList.files[3][0] == '\0');

    /* An empty list is a message and not a mistake: it is how a host clears
       the list, so it goes on the wire. */
    memset(&cmd.u.setScriptList, 0, sizeof(cmd.u.setScriptList));
    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    UT_ASSERT(commandCodecDecode(buf, len, &back));
    UT_ASSERT(back.u.setScriptList.count == 0);

    /* The worst case, which is what COMMAND_MAX_WIRE_BYTES is sized by: a
       full list of names one short of the field. */
    memset(&cmd.u.setScriptList, 0, sizeof(cmd.u.setScriptList));
    cmd.u.setScriptList.count = CMD_SCRIPT_LIST_MAX;
    for (i = 0; i < CMD_SCRIPT_LIST_MAX; i++) {
        memset(cmd.u.setScriptList.files[i], 'f', CMD_SCRIPT_LIST_FILE_LEN - 1);
    }
    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    UT_ASSERT_MSG(len == 13 + (size_t)CMD_SCRIPT_LIST_MAX *
                               (size_t)CMD_SCRIPT_LIST_FILE_LEN,
                  "the worst case measured %d bytes, the sizing in "
                  "transport_command_codec.h says %d", (int)len,
                  (int)(13 + CMD_SCRIPT_LIST_MAX * CMD_SCRIPT_LIST_FILE_LEN));
    UT_ASSERT_MSG(len <= (size_t)COMMAND_MAX_WIRE_BYTES,
                  "the worst case is %d bytes and the stack bound is %d",
                  (int)len, (int)COMMAND_MAX_WIRE_BYTES);
    UT_ASSERT(commandCodecDecode(buf, len, &back));
    UT_ASSERT(back.u.setScriptList.count == CMD_SCRIPT_LIST_MAX);

    /* A count past the cap is refused rather than trimmed, on both sides. */
    cmd.u.setScriptList.count = CMD_SCRIPT_LIST_MAX + 1;
    UT_ASSERT(!commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    cmd.u.setScriptList.count = 1;
    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), &len));
    buf[PACKET_HEADER_SIZE + 4] = CMD_SCRIPT_LIST_MAX + 1;
    UT_ASSERT(!commandCodecDecode(buf, len, &back));
    return 0;
}

/* ── 3. The dispatcher's arm ──────────────────────────────────────── */

/* The directory these cases offer. bound and keepsWinCondition are per entry,
 * as the real lister fills them from each manifest. Room for a full command's
 * worth, so a case can offer as many names as one can carry; slLobby fills
 * the first five and leaves the rest empty. `calls` is how many times the arm
 * asked, which is what the one-listing case reads. */
typedef struct {
    int         calls;
    int         count;
    const char *files[CMD_SCRIPT_LIST_MAX];
    bool        bound[CMD_SCRIPT_LIST_MAX];
    bool        keepsWin[CMD_SCRIPT_LIST_MAX];
} SlDir;

static int slList(void *ctx, const char *dir, ScnDirEntry *out, int max) {
    SlDir *d = (SlDir *)ctx;
    int    n = 0;

    (void)dir;
    d->calls++;
    while (n < d->count && n < max) {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].file, sizeof(out[n].file), "%s", d->files[n]);
        snprintf(out[n].name, sizeof(out[n].name), "Script %d", n);
        out[n].bound            = d->bound[n];
        out[n].keepsWinCondition = d->keepsWin[n];
        n++;
    }
    return n;
}

/* One scenario, two mods and one bound scenario on offer. A host in slot 0 —
 * lobbyClientMayEdit(sim, 0) is true — and a non-host human in slot 1. */
static ServerSim *slLobby(SlDir *d) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);

    memset(d, 0, sizeof(*d));
    d->count       = 5;
    d->files[0]    = "wave.scenario";      /* a scenario: decides the round */
    d->files[1]    = "fastreload.lua";     /* a mod: keeps the win condition */
    d->keepsWin[1] = true;
    d->files[2]    = "nolgm.lua";          /* another mod */
    d->keepsWin[2] = true;
    d->files[3]    = "tied.scenario";      /* a scenario bound to its map */
    d->bound[3]    = true;
    d->files[4]    = "duel.scenario";      /* a second unbound scenario */
    serverSimSetScenarioLister(sim, slList, d);
    return sim;
}

static void slPastCooldown(ServerSim *sim) {
    sim->tick += SCENARIO_RELOAD_GAP_TICKS;
}

static CmdResult slApply(ServerSim *sim, int senderSlot,
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

int run_script_list_dispatch(void) {
    ServerSim  *sim;
    SlDir       d;
    const char *good[3]   = { "wave.scenario", "fastreload.lua", "nolgm.lua" };
    const char *dupes[2]  = { "wave.scenario", "wave.scenario" };
    const char *twoScn[2] = { "wave.scenario", "duel.scenario" };
    const char *unknown[1] = { "missing.lua" };
    const char *tied[1]    = { "tied.scenario" };
    const char *shaped[1]  = { "../escape.lua" };

    sim = slLobby(&d);
    UT_ASSERT(sim != NULL);

    /* A non-host is refused before the directory is read. */
    UT_ASSERT(slApply(sim, 1, good, 3) == CMD_REJECT_NOT_HOST);
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 0,
                  "a refused list was recorded anyway");

    /* One scenario and two mods, in order. */
    UT_ASSERT_MSG(slApply(sim, 0, good, 3) == CMD_OK,
                  "the host's list was refused");
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 3,
                  "%d entries recorded, wanted 3", serverSimGetScriptCount(sim));
    UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->file, "wave.scenario") == 0);
    UT_ASSERT(strcmp(serverSimGetScript(sim, 2)->file, "nolgm.lua") == 0);
    /* Entry 0 is what decides the round, which is what the one-name accessor
       has always answered. */
    UT_ASSERT(strcmp(serverSimGetSelectedScenario(sim), "wave.scenario") == 0);
    /* The manifest's name rides with it, so the list event has it without
       reading the directory again. */
    UT_ASSERT(strcmp(serverSimGetScript(sim, 1)->name, "Script 1") == 0);

    /* Inside the tick gap, the next list is refused before the directory is
       read at all. */
    {
        int calls = d.calls;
        UT_ASSERT(slApply(sim, 0, good, 3) == CMD_REJECT_COOLDOWN);
        UT_ASSERT_MSG(d.calls == calls,
                      "a list refused for the cooldown still read the "
                      "directory %d times", d.calls - calls);
    }

    /* The same file twice would load one script twice. */
    slPastCooldown(sim);
    UT_ASSERT(slApply(sim, 0, dupes, 2) == CMD_REJECT_INVALID);
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 3,
                  "a refused list replaced the one that was there");

    /* Two scripts that may each end the round: which one won would come down
       to the order they were called in. */
    slPastCooldown(sim);
    UT_ASSERT_MSG(slApply(sim, 0, twoScn, 2) == CMD_REJECT_INVALID,
                  "a list holding two scenarios was accepted");

    /* A name the directory does not hold, a bound one, and a path rather
       than a name. Each leaves the list alone. */
    slPastCooldown(sim);
    UT_ASSERT(slApply(sim, 0, unknown, 1) == CMD_REJECT_INVALID);
    slPastCooldown(sim);
    UT_ASSERT(slApply(sim, 0, tied, 1) == CMD_REJECT_INVALID);
    slPastCooldown(sim);
    UT_ASSERT(slApply(sim, 0, shaped, 1) == CMD_REJECT_INVALID);
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 3,
                  "a refusal replaced the list that was there");

    /* The committed map's own script is the one bound file a list may name,
       and naming it is how a host says where on the list the map's script
       is composed. The row is not in the directory at all — the file sits
       beside the .map — so the arm has to take it from what the server
       published rather than from a lookup. */
    {
        ScnDirEntry mapOwn;
        const char *placed[2] = { "fastreload.lua", "brought.scenario" };
        const char *twice[2]  = { "brought.scenario", "brought.scenario" };
        const char *withScn[2] = { "wave.scenario", "brought.scenario" };

        memset(&mapOwn, 0, sizeof(mapOwn));
        snprintf(mapOwn.file, sizeof(mapOwn.file), "brought.scenario");
        snprintf(mapOwn.name, sizeof(mapOwn.name), "Brought Along");
        serverSimSetMapScript(sim, &mapOwn);

        slPastCooldown(sim);
        UT_ASSERT_MSG(slApply(sim, 0, placed, 2) == CMD_OK,
                      "a list naming the committed map's own script was "
                      "refused");
        UT_ASSERT(serverSimGetScriptCount(sim) == 2);
        UT_ASSERT_MSG(strcmp(serverSimGetScript(sim, 1)->file,
                             "brought.scenario") == 0,
                      "the map's own row is not where the host put it");
        UT_ASSERT_MSG(serverSimGetScript(sim, 1)->bound,
                      "the map's own row lost its bound flag, which is the "
                      "only thing that tells it from a pick");
        UT_ASSERT_MSG(strcmp(serverSimGetScript(sim, 1)->name,
                             "Brought Along") == 0,
                      "the row was rebuilt from the name rather than taken "
                      "whole from what the server published");

        /* And the lobby is told that list and not a copy of the map's row in
           front of it, because the row is already on it. */
        UT_ASSERT_MSG(serverSimGetLobbyScriptCount(sim) == 2,
                      "the lobby list is %d rows, wanted the host's two",
                      serverSimGetLobbyScriptCount(sim));

        /* Named twice it is a duplicate like any other name. */
        slPastCooldown(sim);
        UT_ASSERT(slApply(sim, 0, twice, 2) == CMD_REJECT_INVALID);

        /* And it counts as a scenario, so a picked scenario beside it is two
           scripts that may each end the round. A host who wants the pick
           takes the map's row off the list. */
        slPastCooldown(sim);
        UT_ASSERT_MSG(slApply(sim, 0, withScn, 2) == CMD_REJECT_INVALID,
                      "the map's own scenario and a picked one were accepted "
                      "together");

        /* A bound file that is not the committed map's is still refused: it
           belongs to a map that is not on. */
        slPastCooldown(sim);
        UT_ASSERT(slApply(sim, 0, tied, 1) == CMD_REJECT_INVALID);
        UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 2,
                      "a refusal replaced the list that was there");

        /* A map with no script of its own takes the row off the list again,
           and the picks behind it close up. */
        serverSimSetMapScript(sim, NULL);
        UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 1,
                      "the list is %d rows after the map's script went, "
                      "wanted the one pick", serverSimGetScriptCount(sim));
        UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->file,
                         "fastreload.lua") == 0);
    }

    /* No scenario at all is a list too: mods over whatever the map brings. */
    {
        const char *mods[2] = { "fastreload.lua", "nolgm.lua" };
        slPastCooldown(sim);
        UT_ASSERT_MSG(slApply(sim, 0, mods, 2) == CMD_OK,
                      "a list of mods with no scenario was refused");
        UT_ASSERT(serverSimGetScriptCount(sim) == 2);
    }

    /* An empty list needs no name checks and no ranked question, but it
       recomposes like any other list, so it waits for the gap the same way. */
    UT_ASSERT_MSG(slApply(sim, 0, NULL, 0) == CMD_REJECT_COOLDOWN,
                  "a clear inside the tick gap recomposed anyway");
    slPastCooldown(sim);
    UT_ASSERT_MSG(slApply(sim, 0, NULL, 0) == CMD_OK,
                  "clearing the list was refused");
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == 0,
                  "the list was not cleared");
    UT_ASSERT(serverSimGetSelectedScenario(sim)[0] == '\0');

    serverSimDestroy(sim);
    return 0;
}

/* The arm reads the mod directories once per command and matches every name
 * against that one reading. It used to ask per name, so a full list of ten
 * read them ten times — on the tick thread and under the sim lock, each one
 * opening every file in the directories and running the top level of every
 * loose script in them. */
int run_script_list_lists_once(void) {
    ServerSim  *sim;
    SlDir       d;
    char        files[CMD_SCRIPT_LIST_MAX][32];
    const char *names[CMD_SCRIPT_LIST_MAX];
    int         i;

    sim = slLobby(&d);
    UT_ASSERT(sim != NULL);

    /* A full command's worth on offer and all of it named. Mods rather than
       scenarios: only one script on a list may end the round, and what is
       being counted here is directory reads, not what the list holds. */
    d.count = CMD_SCRIPT_LIST_MAX;
    for (i = 0; i < CMD_SCRIPT_LIST_MAX; i++) {
        snprintf(files[i], sizeof(files[i]), "mod%d.lua", i);
        d.files[i]    = files[i];
        d.bound[i]    = false;
        d.keepsWin[i] = true;
        names[i]      = files[i];
    }

    /* Past the gap, then zeroed: what the fixture's own set-up read does not
       count against the one reading this command is allowed. */
    slPastCooldown(sim);
    d.calls = 0;
    UT_ASSERT_MSG(slApply(sim, 0, names, CMD_SCRIPT_LIST_MAX) == CMD_OK,
                  "a full list of ten valid names was refused");
    UT_ASSERT_MSG(d.calls == 1,
                  "%d names read the mod directories %d times, wanted one "
                  "reading for the whole command", CMD_SCRIPT_LIST_MAX,
                  d.calls);
    UT_ASSERT_MSG(serverSimGetScriptCount(sim) == CMD_SCRIPT_LIST_MAX,
                  "%d entries recorded, wanted %d",
                  serverSimGetScriptCount(sim), CMD_SCRIPT_LIST_MAX);
    /* The rows came off that one reading and carry what the lister reported,
       not names rebuilt off the wire. */
    UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->file, files[0]) == 0);
    UT_ASSERT(strcmp(serverSimGetScript(sim, 0)->name, "Script 0") == 0);
    UT_ASSERT(strcmp(serverSimGetScript(sim, CMD_SCRIPT_LIST_MAX - 1)->file,
                     files[CMD_SCRIPT_LIST_MAX - 1]) == 0);

    /* A refusal reads once too: the name that is not there is found missing
       in the listing the arm already has. */
    {
        const char *unknown[2] = { "mod0.lua", "absent.lua" };

        slPastCooldown(sim);
        d.calls = 0;
        UT_ASSERT(slApply(sim, 0, unknown, 2) == CMD_REJECT_INVALID);
        UT_ASSERT_MSG(d.calls == 1,
                      "a refused list read the mod directories %d times",
                      d.calls);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── 4. The chunks, reassembled on the client ─────────────────────── */

/* Fill chunk `chunk` of a list of `total` entries, named "s0".."s9". */
static void slFillChunk(ControlEvent *evt, int total, int chunk) {
    int first = chunk * LOBBY_SCRIPT_LIST_CHUNK;
    int n     = total - first;
    int i;

    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_LOBBY_SCRIPT_LIST;
    if (n < 0) n = 0;
    if (n > LOBBY_SCRIPT_LIST_CHUNK) n = LOBBY_SCRIPT_LIST_CHUNK;
    evt->u.lobbyScriptList.count = (uint8_t)n;
    evt->u.lobbyScriptList.final =
        (first + n >= total) ? 1 : 0;
    for (i = 0; i < n; i++) {
        snprintf(evt->u.lobbyScriptList.entries[i].file,
                 sizeof(evt->u.lobbyScriptList.entries[i].file), "s%d",
                 first + i);
        snprintf(evt->u.lobbyScriptList.entries[i].name,
                 sizeof(evt->u.lobbyScriptList.entries[i].name), "Script %d",
                 first + i);
        /* Entry 0 is the scenario; everything behind it is a mod. The last
           one is bound, so the row flags are not all the same value. */
        evt->u.lobbyScriptList.entries[i].keepsWinCondition = (first + i) != 0;
        evt->u.lobbyScriptList.entries[i].bound = (first + i) == total - 1;
    }
}

int run_script_list_client_apply(void) {
    ClientSim   *cs = clientSimAlloc();
    ControlEvent evt;
    int          i;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    /* A full list of LOBBY_SCRIPT_LIST_MAX, which is two chunks. */
    slFillChunk(&evt, LOBBY_SCRIPT_LIST_MAX, 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetLobbyScriptCount(cs) == 0,
                  "the list was installed off a chunk that was not the last "
                  "one, so a chooser can draw half a list");
    slFillChunk(&evt, LOBBY_SCRIPT_LIST_MAX, 1);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetLobbyScriptCount(cs) == LOBBY_SCRIPT_LIST_MAX,
                  "%d entries installed, wanted %d",
                  clientSimGetLobbyScriptCount(cs), LOBBY_SCRIPT_LIST_MAX);
    for (i = 0; i < LOBBY_SCRIPT_LIST_MAX; i++) {
        char want[16];
        snprintf(want, sizeof(want), "s%d", i);
        UT_ASSERT_MSG(strcmp(clientSimGetLobbyScriptFile(cs, i), want) == 0,
                      "entry %d reads \"%s\", wanted \"%s\"", i,
                      clientSimGetLobbyScriptFile(cs, i), want);
    }
    UT_ASSERT(strcmp(clientSimGetLobbyScriptName(cs, 3), "Script 3") == 0);
    UT_ASSERT(!clientSimGetLobbyScriptKeepsWinCondition(cs, 0));
    UT_ASSERT(clientSimGetLobbyScriptKeepsWinCondition(cs, 1));
    UT_ASSERT(!clientSimGetLobbyScriptBound(cs, 0));
    UT_ASSERT(clientSimGetLobbyScriptBound(cs, LOBBY_SCRIPT_LIST_MAX - 1));
    /* Out of range answers rather than reading off the end: a chooser reads
       these a frame at a time while a new list may land between two reads. */
    UT_ASSERT(clientSimGetLobbyScriptFile(cs, LOBBY_SCRIPT_LIST_MAX)[0] == '\0');
    UT_ASSERT(!clientSimGetLobbyScriptBound(cs, -1));

    /* A shorter list replaces the longer one whole. */
    {
        uint32_t seq = clientSimGetLobbyScriptSeq(cs);
        slFillChunk(&evt, 2, 0);
        clientSimApplyControl(cs, &evt);
        UT_ASSERT(clientSimGetLobbyScriptCount(cs) == 2);
        UT_ASSERT_MSG(clientSimGetLobbyScriptSeq(cs) == seq + 1,
                      "the sequence did not move on a new list");
    }

    /* An empty list arrives as one chunk with no entries and clears. */
    slFillChunk(&evt, 0, 0);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetLobbyScriptCount(cs) == 0,
                  "an empty list left %d entries behind",
                  clientSimGetLobbyScriptCount(cs));

    /* A run of chunks that would overrun the cap is thrown away and the last
       whole list stands. */
    slFillChunk(&evt, 4, 0);
    evt.u.lobbyScriptList.final = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetLobbyScriptCount(cs) == 4);
    for (i = 0; i < 4; i++) {
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_SCRIPT_LIST;
        evt.u.lobbyScriptList.count = LOBBY_SCRIPT_LIST_CHUNK;
        evt.u.lobbyScriptList.final = 0;
        clientSimApplyControl(cs, &evt);
    }
    UT_ASSERT_MSG(clientSimGetLobbyScriptCount(cs) == 4,
                  "a run past the cap replaced the list that was there");

    clientSimDestroy(cs);
    return 0;
}
