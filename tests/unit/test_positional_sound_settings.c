/*
 * Positional sound: the LST_POSITIONAL_SOUND apply path, its interaction
 * with classic mode, the startup snapshot, and the lobby-settings byte.
 *
 * Three things are pinned here:
 *
 *   1. A fresh sim has the setting off — every sound sent centred, with only
 *      its near or far variant chosen. That keeps a server nobody has
 *      configured on the classic behaviour. The apply case takes exactly one
 *      byte and reads any non-zero value as on, matching the other bool
 *      settings. The lock bit is the setting's own, and locking it locks
 *      classic mode too, because classic mode writes the value.
 *
 *   2. Classic mode plays every sound centred, so it owns this value the
 *      same way it owns allies in trees: turning classic mode on forces the
 *      setting off, and an edit is refused while it stays on. The value
 *      rides the originalLobbySettings snapshot, so a visiting host's change
 *      is undone when serverSimResetLobbyToDefaults restores the operator's
 *      startup configuration.
 *
 *   3. The lobby-settings event carries it in the byte after modsOff, both
 *      ways. A body that stops before the byte reads as off, and a lobby with
 *      a scenario still decodes its scenario tail behind the byte.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"           /* clientSimApplyControl */
#include "control_event.h"
#include "transport_control_codec.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"         /* serverSimApplyLobbySetting */
#include "server_lifecycle.h"             /* ServerInstanceConfig */
#include "server/sim/server_sim_shared.h" /* serverSimResetLobbyToDefaults */
#include "wire_limits.h"
#include "everard_map.h"
#include "test_harness.h"

static ServerSim *make_sound_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

int run_positional_sound_defaults(void) {
    ServerSim   *sim = make_sound_sim();
    ControlEvent evt;
    uint8_t      value[3];
    UT_ASSERT(sim != NULL);

    UT_ASSERT_MSG(!serverSimGetPositionalSound(sim),
                  "a fresh sim must send every sound centred");

    /* NULL-safe read matches the off default. */
    UT_ASSERT(!serverSimGetPositionalSound(NULL));
    serverSimSetPositionalSound(NULL, true);  /* NULL-safe */

    /* On, then off, through the lobby apply path. */
    value[0] = 1;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1),
                  "a valid positional-sound payload was refused");
    UT_ASSERT(serverSimGetPositionalSound(sim));

    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1));
    UT_ASSERT(!serverSimGetPositionalSound(sim));

    /* Any non-zero byte reads as on, matching the other bool settings. */
    value[0] = 2;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1));
    UT_ASSERT_MSG(serverSimGetPositionalSound(sim),
                  "a non-zero byte other than 1 must read as on");

    /* A payload of the wrong length is refused and changes nothing. */
    value[0] = 0;
    value[1] = 0;
    value[2] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 0));
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 2));
    UT_ASSERT_MSG(serverSimGetPositionalSound(sim),
                  "a refused payload still turned positional sound off");

    /* What the lobby sends every viewer says the same thing the sim holds. */
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.lobbyPositionalSound,
                  "the published settings did not carry positional sound on");

    /* The lock bit is the setting's own, distinct from every other. */
    UT_ASSERT_MSG(serverSimGetSettingLockBit(LST_POSITIONAL_SOUND)
                      == LOBBY_LOCK_POSITIONAL_SOUND,
                  "LST_POSITIONAL_SOUND lock bit = 0x%08X, want 0x%08X",
                  (unsigned)serverSimGetSettingLockBit(LST_POSITIONAL_SOUND),
                  (unsigned)LOBBY_LOCK_POSITIONAL_SOUND);
    /* Classic mode forces this setting off, so locking the setting has to
     * lock classic mode as well — otherwise the checkbox is a way round the
     * lock. */
    UT_ASSERT_MSG((serverSimAddImpliedLocks(LOBBY_LOCK_POSITIONAL_SOUND) &
                   LOBBY_LOCK_CLASSIC_MODE) != 0u,
                  "the positional-sound lock must also cover classic mode");
    UT_ASSERT(!serverSimIsSettingLocked(sim, LST_POSITIONAL_SOUND));
    serverSimSetServerLocks(sim, LOBBY_LOCK_POSITIONAL_SOUND);
    UT_ASSERT(serverSimIsSettingLocked(sim, LST_POSITIONAL_SOUND));

    serverSimDestroy(sim);
    return 0;
}

int run_positional_sound_classic_mode(void) {
    ServerSim *sim = make_sound_sim();
    ServerInstanceConfig cfg;
    uint8_t value[1];
    UT_ASSERT(sim != NULL);

    /* Turning classic mode on forces the setting off. */
    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1));
    UT_ASSERT(serverSimGetPositionalSound(sim));
    serverSimSetClassicMode(sim, true);
    UT_ASSERT_MSG(!serverSimGetPositionalSound(sim),
                  "classic mode must turn positional sound off");

    /* While classic mode is on, an edit is refused in both directions. */
    value[0] = 1;
    UT_ASSERT_MSG(!serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1),
                  "a positional-sound edit should be refused under classic mode");
    UT_ASSERT(!serverSimGetPositionalSound(sim));
    value[0] = 0;
    UT_ASSERT(!serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1));

    /* Off. The value stays where classic mode put it, and the setting
     * becomes editable again. */
    serverSimSetClassicMode(sim, false);
    UT_ASSERT_MSG(!serverSimGetPositionalSound(sim),
                  "leaving classic mode must leave positional sound off");
    value[0] = 1;
    UT_ASSERT_MSG(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1),
                  "an edit should be allowed once classic mode is off");
    UT_ASSERT(serverSimGetPositionalSound(sim));

    serverSimDestroy(sim);

    /* The operator starts the server with the setting on; a visiting host
     * turns it off; the reset restores the startup value. */
    sim = make_sound_sim();
    UT_ASSERT(sim != NULL);
    serverSimSetPositionalSound(sim, true);

    /* serverSimApplyInstanceConfig is what server startup runs, and its
     * tail captures the snapshot serverSimResetLobbyToDefaults restores
     * from. A zeroed config leaves every other startup field alone. */
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);

    value[0] = 0;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1));
    UT_ASSERT(!serverSimGetPositionalSound(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(serverSimGetPositionalSound(sim),
                  "positional sound was not restored from the startup snapshot");

    serverSimDestroy(sim);

    /* The other direction: a server that starts with the setting off gets
     * it back off after a host turned it on. */
    sim = make_sound_sim();
    UT_ASSERT(sim != NULL);
    memset(&cfg, 0, sizeof(cfg));
    serverSimApplyInstanceConfig(sim, &cfg);
    UT_ASSERT(!serverSimGetPositionalSound(sim));

    value[0] = 1;
    UT_ASSERT(serverSimApplyLobbySetting(sim, LST_POSITIONAL_SOUND, value, 1));
    UT_ASSERT(serverSimGetPositionalSound(sim));

    serverSimResetLobbyToDefaults(sim);
    UT_ASSERT_MSG(!serverSimGetPositionalSound(sim),
                  "the reset must restore positional sound off, not leave it on");

    serverSimDestroy(sim);
    return 0;
}

int run_positional_sound_lobby_event(void) {
    ControlEncodeBodyFn benc =
        transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    ControlDecodeBodyFn bdec =
        transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
    uint8_t      body[MAX_CONTROL_PACKET];
    ControlEvent in, out;
    size_t       len = 0;
    ClientSim   *cs;

    UT_ASSERT(benc != NULL && bdec != NULL);

    /* On, out and back. modsOff is set too, so the byte ahead of the new
     * one is a 1 and a read one place out shows. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyModsOff = true;
    in.u.lobbySettings.lobbyPositionalSound = true;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT(bdec(body, len, &out));
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyPositionalSound,
                  "positional sound on did not survive the codec");
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyModsOff,
                  "mods off ahead of the byte did not survive the codec");

    /* Off, out and back. The out field starts true so a decoder that never
     * read the byte shows. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyPositionalSound = false;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    out.u.lobbySettings.lobbyPositionalSound = true;
    UT_ASSERT(bdec(body, len, &out));
    UT_ASSERT_MSG(!out.u.lobbySettings.lobbyPositionalSound,
                  "positional sound off did not survive the codec");

    /* A body that stops just before the byte reads as off, and keeps the
     * mods byte ahead of it. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyModsOff = true;
    in.u.lobbySettings.lobbyPositionalSound = true;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(bdec(body, len - 1, &out),
                  "a body with no positional-sound byte failed to decode");
    UT_ASSERT_MSG(!out.u.lobbySettings.lobbyPositionalSound,
                  "a body with no positional-sound byte must read as off");
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyModsOff,
                  "a body with no positional-sound byte lost the mods byte "
                  "ahead of it");

    /* A lobby with a scenario: the tail sits behind the new byte and still
     * decodes whole. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyPositionalSound = true;
    in.u.lobbySettings.scenarioSource = lobbyScenarioMap;
    snprintf(in.u.lobbySettings.scenarioName,
             sizeof(in.u.lobbySettings.scenarioName), "%s", "Wave");
    snprintf(in.u.lobbySettings.scenarioFileName,
             sizeof(in.u.lobbySettings.scenarioFileName), "%s", "wave.lua");
    snprintf(in.u.lobbySettings.scenarioDescription,
             sizeof(in.u.lobbySettings.scenarioDescription), "%s", "Hold out");
    in.u.lobbySettings.scenarioExtraTeams = true;
    in.u.lobbySettings.scenarioBaseGame   = (uint8_t)gameStrictTournament;
    in.u.lobbySettings.scenarioUnsafe     = true;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(bdec(body, len, &out), "the scripted body did not decode");
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyPositionalSound,
                  "positional sound did not survive ahead of a scenario tail");
    UT_ASSERT_MSG(out.u.lobbySettings.scenarioSource == lobbyScenarioMap,
                  "the scenario source came back as %d, wanted %d",
                  (int)out.u.lobbySettings.scenarioSource,
                  (int)lobbyScenarioMap);
    UT_ASSERT_MSG(strcmp(out.u.lobbySettings.scenarioName, "Wave") == 0,
                  "the scenario name came back \"%s\"",
                  out.u.lobbySettings.scenarioName);
    UT_ASSERT_MSG(strcmp(out.u.lobbySettings.scenarioFileName, "wave.lua") == 0,
                  "the scenario file came back \"%s\"",
                  out.u.lobbySettings.scenarioFileName);
    UT_ASSERT_MSG(strcmp(out.u.lobbySettings.scenarioDescription,
                         "Hold out") == 0,
                  "the scenario description came back \"%s\"",
                  out.u.lobbySettings.scenarioDescription);
    UT_ASSERT(out.u.lobbySettings.scenarioExtraTeams);
    UT_ASSERT_MSG(out.u.lobbySettings.scenarioBaseGame ==
                      (uint8_t)gameStrictTournament,
                  "the scenario base game came back as %u",
                  (unsigned)out.u.lobbySettings.scenarioBaseGame);
    UT_ASSERT_MSG(out.u.lobbySettings.scenarioUnsafe,
                  "the last byte of the scenario tail did not survive");

    /* The client mirrors what the event says, and reads off before one
     * arrives. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT_MSG(!clientSimGetPositionalSound(cs),
                  "a client with no settings event must read positional sound "
                  "as off");
    UT_ASSERT(!clientSimGetPositionalSound(NULL));

    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.lobbyPositionalSound = true;
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(clientSimGetPositionalSound(cs),
                  "the client did not follow the server's positional sound on");

    in.u.lobbySettings.lobbyPositionalSound = false;
    clientSimApplyControl(cs, &in);
    UT_ASSERT_MSG(!clientSimGetPositionalSound(cs),
                  "the client did not follow the server's positional sound off");

    clientSimDestroy(cs);
    return 0;
}
