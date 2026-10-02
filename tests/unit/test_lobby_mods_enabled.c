/*
 * Mods enabled: the LST_MODS_OFF path, from the lobby checkbox down to
 * the byte on the wire and back.
 *
 * The setting decides whether the round composes the scripts on the
 * lobby's pick list, mods and picked scenarios alike; the lobby labels it
 * Mods/Scenario. It does not touch the list; off, nothing composes, the map's
 * own script included. scnDecideScenario
 * (src/scenario/scenario_host.c) is the one reader; what is pinned here is
 * everything under it, because a value that never reaches that function
 * correctly is a checkbox that does nothing.
 *
 * Four things are pinned:
 *
 *   1. The apply path. A fresh sim composes its mods, the case takes
 *      exactly one byte, and any non-zero byte reads as off — the same
 *      shape LST_SMART_PINGS_OFF has. The value rides the
 *      originalLobbySettings snapshot, so a visiting host's change is
 *      undone when serverSimResetLobbyToDefaults restores the operator's
 *      startup configuration.
 *
 *   2. The lock. LST_MODS_OFF maps to LOBBY_LOCK_MODS and to nothing
 *      else, and the CMD_LOBBY_SETTING arm refuses the edit with
 *      CMD_REJECT_LOCKED while the operator holds it. Locking mods drags
 *      no other lock in with it: it writes no other setting's value, so
 *      it has nothing to imply.
 *
 *   3. The codec, both ways, and the byte's polarity. The field is
 *      carried in the NEGATIVE sense: the byte says the mods are OFF, so
 *      the zero a decoder leaves for a sender that never wrote it reads
 *      as the mods running. That is what every build before the setting
 *      existed did, and it is the whole reason the field is stored the
 *      way round it is.
 *
 *   4. The client. A ClientSim that has been sent no settings event at
 *      all answers "mods enabled", and one that has been sent the event
 *      answers what the server said. The accessor is positive where the
 *      field is negative, so a sign lost anywhere between the two shows
 *      up here.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"               /* CMD_LOBBY_SETTING, CmdResult */
#include "client_sim.h"
#include "client_sim_control.h"           /* clientSimApplyControl */
#include "control_event.h"
#include "transport_control_codec.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"         /* serverSimApplyLobbySetting */
#include "server_lifecycle.h"             /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimResetLobbyToDefaults */
#include "threads.h"
#include "wire_limits.h"
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_mods_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* The command the lobby checkbox sends, driven through the dispatcher the
 * way a real client reaches it. The dispatcher asserts the threads mutex,
 * so it is held across the call. */
static CmdResult apply_mods_off(ServerSim *sim, int senderSlot, uint8_t off) {
    ClientCommand cmd;
    CmdResult     r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_SETTING;
    cmd.cmdSeq = 1;
    cmd.u.lobbySetting.settingType = LST_MODS_OFF;
    cmd.u.lobbySetting.valueLen    = 1;
    cmd.u.lobbySetting.value[0]    = off;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* ── 1. The apply path and the startup snapshot ───────────────────── */

int run_lobby_mods_enabled_defaults(void) {
    ServerSim           *sim = make_mods_sim();
    ServerInstanceConfig cfg;
    uint8_t              value[3];
    ControlEvent         evt;
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!serverSimGetModsOff(sim),
                  "a fresh sim must compose the mods it is given");

    /* NULL-safe read matches the off default, and a NULL sim answers the
       same thing a sim nobody has configured does. */
    UT_ASSERT(!serverSimGetModsOff(NULL));
    serverSimSetModsOff(NULL, true);  /* NULL-safe */

    /* Off, then on again, through the lobby apply path. */
    value[0] = 1;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 1),
                  "a valid mods-off payload was refused");
    UT_ASSERT(serverSimGetModsOff(sim));

    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 1));
    UT_ASSERT(!serverSimGetModsOff(sim));

    /* Any non-zero byte reads as off, matching the other bool settings. */
    value[0] = 2;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 1));
    UT_ASSERT_MSG(serverSimGetModsOff(sim),
                  "a non-zero byte other than 1 must read as off");

    /* A payload of the wrong length is refused and changes nothing. */
    value[0] = 0;
    value[1] = 0;
    value[2] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 0));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 2));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 3));
    UT_ASSERT_MSG(serverSimGetModsOff(sim),
                  "a refused payload still turned the mods back on");

    /* What the lobby sends every viewer says the same thing the sim
       holds. The event is the only way the value reaches a client, so a
       publish that did not carry it would leave every checkbox in the
       field reading the default no matter what the host set. */
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.lobbyModsOff,
                  "the published settings did not carry mods off");
    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 1));
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(!evt.u.lobbySettings.lobbyModsOff,
                  "the published settings did not follow the toggle back on");

    serverSimDestroy(sim);

    /* The operator starts the server with the mods off; a visiting host
       turns them on; the reset restores the startup value. */
    sim = make_mods_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetModsOff(sim, true);

    /* serverSimApplyInstanceConfig is what server startup runs, and its
       tail captures the snapshot serverSimResetLobbyToDefaults restores
       from. A zeroed config leaves every other startup field alone. */
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 1));
    UT_ASSERT(!serverSimGetModsOff(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(serverSimGetModsOff(sim),
                  "mods off was not restored from the startup snapshot");

    serverSimDestroy(sim);

    /* The other direction: a server that starts composing its mods gets
       that back after a host turned them off. */
    sim = make_mods_sim();
    UT_ASSERT(sim != NULL);
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);
    UT_ASSERT(!serverSimGetModsOff(sim));

    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_MODS_OFF, value, 1));
    UT_ASSERT(serverSimGetModsOff(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(!serverSimGetModsOff(sim),
                  "the reset must restore the mods on, not leave them off");

    serverSimDestroy(sim);
    return 0;
}

/* ── 2. The dispatch arm and the lock ─────────────────────────────── */

int run_lobby_mods_enabled_dispatch(void) {
    ServerSim *sim = make_mods_sim();
    UT_ASSERT(sim != NULL);

    /* The lock bit is the setting's own, distinct from every other. */
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_MODS_OFF) == LOBBY_LOCK_MODS,
                  "LST_MODS_OFF lock bit = 0x%08X, want 0x%08X",
                  (unsigned)serverSimGetSettingLockBit(LST_MODS_OFF),
                  (unsigned)LOBBY_LOCK_MODS);

    /* Host in slot 0, so lobbyClientMayEdit(sim, 0) answers true. */
    serverSimAddPlayer(sim, 0, "Host", false);

    UT_ASSERT_MSG(apply_mods_off(sim, 0, 1) == CMD_OK,
                  "the host's mods-off edit was refused");
    UT_ASSERT(serverSimGetModsOff(sim));
    UT_ASSERT_MSG(apply_mods_off(sim, 0, 0) == CMD_OK,
                  "the host's mods-on edit was refused");
    UT_ASSERT(!serverSimGetModsOff(sim));

    /* A player who is not the host and has no open-host grant cannot set
       it, the same as every other setting on this arm. */
    serverSimAddPlayer(sim, 1, "Joiner", false);
    UT_ASSERT_MSG(apply_mods_off(sim, 1, 1) == CMD_REJECT_NOT_HOST,
                  "a non-host's mods edit must be refused as NOT_HOST");
    UT_ASSERT_MSG(!serverSimGetModsOff(sim),
                  "a refused non-host edit still changed the value");

    /* And the operator's lock refuses even the host. */
    serverSimSetServerLocks(sim, LOBBY_LOCK_MODS);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_MODS_OFF));
    UT_ASSERT_MSG(apply_mods_off(sim, 0, 1) == CMD_REJECT_LOCKED,
                  "a locked mods setting must be refused as LOCKED");
    UT_ASSERT_MSG(!serverSimGetModsOff(sim),
                  "a locked setting was written anyway");

    /* Locking the mods implies no other lock. The setting writes no other
       setting's value, so there is nothing for it to drag in — unlike the
       six that classic mode writes. */
    UT_ASSERT_MSG(serverSimGetServerLocks(sim) == LOBBY_LOCK_MODS,
                  "locking mods pulled in other locks: 0x%08X",
                  (unsigned)serverSimGetServerLocks(sim));

    serverSimDestroy(sim);
    return 0;
}

/* ── 3 and 4. The codec both ways, and what a client with no byte reads ── */

int run_lobby_mods_enabled_codec(void) {
    ControlEncodeBodyFn benc =
        transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    ControlDecodeBodyFn bdec =
        transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
    uint8_t      body[MAX_CONTROL_PACKET];
    ControlEvent in, out;
    size_t       len = 0;
    ClientSim   *cs;

    UT_ASSERT(benc != NULL && bdec != NULL);

    /* Off, out and back. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyModsOff = true;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT(bdec(body, len, &out));
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyModsOff,
                  "mods off did not survive the codec");

    /* On, out and back. Both directions rather than one, because a
       decoder that never read the byte would pass the case above only if
       the encoder happened to leave the field set. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyModsOff = false;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    out.u.lobbySettings.lobbyModsOff = true;  /* so a no-op decode shows */
    UT_ASSERT(bdec(body, len, &out));
    UT_ASSERT_MSG(!out.u.lobbySettings.lobbyModsOff,
                  "mods on did not survive the codec");

    /* The polarity pin. A body that stops just before the mods byte came
       from a sender that predates the setting, and every such server
       composed the mods it was given — so the field has to read as off,
       meaning the mods run. If it is ever flipped to a positive sense this
       is the assertion that fails. The positional-sound byte follows the
       mods byte, so the cut is two bytes. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyModsOff = true;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(bdec(body, len - 2, &out),
                  "a body with no mods byte failed to decode");
    UT_ASSERT_MSG(!out.u.lobbySettings.lobbyModsOff,
                  "a body with no mods byte must read as mods ON");

    /* And the client's own answer, which is the positive one. A sim that
       has been sent nothing reads as composing them. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT_MSG(clientSimGetLobbyModsEnabled(cs),
                  "a client with no settings event must read mods as on");
    UT_ASSERT_MSG(clientSimGetLobbyModsEnabled(NULL),
                  "a NULL client must read mods as on");

    /* The event the server sends turns it off, and sends it back on. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyModsOff = true;
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(!clientSimGetLobbyModsEnabled(cs),
                  "the client did not follow the server's mods-off");

    in.u.lobbySettings.lobbyModsOff = false;
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(clientSimGetLobbyModsEnabled(cs),
                  "the client did not follow the server's mods-on");

    clientSimDestroy(cs);
    return 0;
}
