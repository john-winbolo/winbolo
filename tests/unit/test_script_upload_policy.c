/*
 * The policy a server holds for scripts players send it.
 *
 * ScriptUploadPolicy is set from a word — -scriptuploads on the dedicated
 * server, --scriptuploads on the headless runner, "Script Upload Policy" in
 * the desktop's preferences — and is allow when no word is given. It goes out
 * to every client on the lobby-settings event, one byte directly after the
 * map upload policy.
 *
 * The codec case compares the byte against a value written out here by hand
 * and never read back through the codec: a round trip alone would pass
 * whenever the encoder and the decoder agreed with each other.
 *
 * run_script_upload_policy_resolve   — the three words in any case, an
 *                                      unknown word, and no word
 * run_script_upload_policy_word      — the preference spelling of each value
 * run_script_upload_policy_codec     — the byte at its offset, both policies
 *                                      back out, and a body that stops before
 *                                      the byte reading as allow
 * run_script_upload_policy_event     — the sim's policy on the event it
 *                                      publishes, and the client's copy of it
 * run_script_sharing_codec           — the sharing byte behind the policy,
 *                                      both values back out, the start delay
 *                                      behind it, and a short body reading as
 *                                      sharing
 * run_script_sharing_sim             — a new sim shares, a NULL one answers
 *                                      yes, and the event carries off
 * run_script_sharing_client          — the client reads sharing before any
 *                                      event, and what each event says after
 *
 * Whether an uploaded map's script runs under each policy is in
 * test_scenario_packed_map.c, beside the fixtures that case needs.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"
#include "transport_control_codec.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "upload_policy.h"
#include "everard_map.h"
#include "test_harness.h"

/* Where the byte sits in the lobby-settings body, written out rather than
 * taken from the codec:
 *
 *   mapName MAP_STR_SIZE(36)
 * + gameType 1 + hiddenMines 1 + aiType 1 + timeLimit 4
 * + pill 1 + base 1 + start 1 + mapSkip 1 + netStat 1 + hasLobby 1
 * + openHost 1 + autoLock 1 + serverLocks 4                     = 55
 * + ranked 1 + allowNew 1 + wbn 1 + uploadPolicy 1               = 59
 */
#define SUP_UPLOAD_POLICY_OFFSET        58
#define SUP_SCRIPT_UPLOAD_POLICY_OFFSET 59
/* + scriptUploadPolicy 1                                         = 60 */
#define SUP_SCRIPT_SHARING_OFFSET       60
/* + scriptSharing 1, then lobbyStartDelay 4 big-endian           = 61 */
#define SUP_START_DELAY_OFFSET          61

/* ── 1. The word ──────────────────────────────────────────────────── */

int run_script_upload_policy_resolve(void) {
    /* The three words, in the case each is least likely to be typed in. */
    UT_ASSERT_MSG(scriptUploadPolicyResolve("off") == SCRIPT_UPLOAD_OFF,
                  "\"off\" did not resolve to off");
    UT_ASSERT_MSG(scriptUploadPolicyResolve("ALLOW") == SCRIPT_UPLOAD_ALLOW,
                  "\"ALLOW\" did not resolve to allow");
    UT_ASSERT_MSG(scriptUploadPolicyResolve("Persist") == SCRIPT_UPLOAD_PERSIST,
                  "\"Persist\" did not resolve to persist");

    /* A word nobody knows is allow, which is what a server with no word at
       all does, rather than a refusal the operator did not ask for. */
    UT_ASSERT_MSG(scriptUploadPolicyResolve("sometimes") == SCRIPT_UPLOAD_ALLOW,
                  "an unknown word did not resolve to allow");

    /* No word is allow. */
    UT_ASSERT_MSG(scriptUploadPolicyResolve(NULL) == SCRIPT_UPLOAD_ALLOW,
                  "no word did not resolve to allow");
    return 0;
}

int run_script_upload_policy_word(void) {
    UT_ASSERT(strcmp(scriptUploadPolicyWord(SCRIPT_UPLOAD_OFF), "Off") == 0);
    UT_ASSERT(strcmp(scriptUploadPolicyWord(SCRIPT_UPLOAD_ALLOW), "Allow") == 0);
    UT_ASSERT(strcmp(scriptUploadPolicyWord(SCRIPT_UPLOAD_PERSIST),
                     "Persist") == 0);
    UT_ASSERT_MSG(strcmp(scriptUploadPolicyWord((ScriptUploadPolicy)7),
                         "Allow") == 0,
                  "an out-of-range policy did not read as Allow");

    /* And each word goes back to the value it came from, which is what a
       preference written and read again depends on. */
    UT_ASSERT(scriptUploadPolicyResolve(scriptUploadPolicyWord(SCRIPT_UPLOAD_OFF)) ==
              SCRIPT_UPLOAD_OFF);
    UT_ASSERT(scriptUploadPolicyResolve(scriptUploadPolicyWord(SCRIPT_UPLOAD_ALLOW)) ==
              SCRIPT_UPLOAD_ALLOW);
    UT_ASSERT(scriptUploadPolicyResolve(scriptUploadPolicyWord(SCRIPT_UPLOAD_PERSIST)) ==
              SCRIPT_UPLOAD_PERSIST);
    return 0;
}

/* ── 2. The byte on the wire ──────────────────────────────────────── */

int run_script_upload_policy_codec(void) {
    ControlEncodeBodyFn benc =
        transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    ControlDecodeBodyFn bdec =
        transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
    ControlEvent in;
    ControlEvent out;
    uint8_t      body[MAX_CONTROL_PACKET];
    size_t       len = 0;

    UT_ASSERT(benc != NULL && bdec != NULL);

    /* Two different non-zero values, so the two bytes swapped would show. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.uploadPolicy       = UPLOAD_POLICY_OFF;
    in.u.lobbySettings.scriptUploadPolicy = SCRIPT_UPLOAD_PERSIST;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    UT_ASSERT(len > SUP_SCRIPT_UPLOAD_POLICY_OFFSET);

    UT_ASSERT_MSG(body[SUP_UPLOAD_POLICY_OFFSET] == 0x01,
                  "byte %d (the map upload policy) is 0x%02X, expected 0x01",
                  SUP_UPLOAD_POLICY_OFFSET,
                  (unsigned)body[SUP_UPLOAD_POLICY_OFFSET]);
    UT_ASSERT_MSG(body[SUP_SCRIPT_UPLOAD_POLICY_OFFSET] == 0x02,
                  "byte %d (the script upload policy) is 0x%02X, expected 0x02",
                  SUP_SCRIPT_UPLOAD_POLICY_OFFSET,
                  (unsigned)body[SUP_SCRIPT_UPLOAD_POLICY_OFFSET]);

    memset(&out, 0, sizeof(out));
    UT_ASSERT(bdec(body, len, &out));
    UT_ASSERT_MSG(out.u.lobbySettings.uploadPolicy == UPLOAD_POLICY_OFF,
                  "the map upload policy came back as %d",
                  (int)out.u.lobbySettings.uploadPolicy);
    UT_ASSERT_MSG(out.u.lobbySettings.scriptUploadPolicy == SCRIPT_UPLOAD_PERSIST,
                  "the script upload policy came back as %d",
                  (int)out.u.lobbySettings.scriptUploadPolicy);

    /* A body cut off right before the byte still decodes, keeps the map
       policy ahead of it, and reads the script policy as allow. The event is
       filled with persist first so a decoder that left the field alone
       would show. */
    memset(&out, 0, sizeof(out));
    out.u.lobbySettings.scriptUploadPolicy = SCRIPT_UPLOAD_PERSIST;
    UT_ASSERT_MSG(bdec(body, SUP_SCRIPT_UPLOAD_POLICY_OFFSET, &out),
                  "a body that stops before the script policy did not decode");
    UT_ASSERT_MSG(out.u.lobbySettings.uploadPolicy == UPLOAD_POLICY_OFF,
                  "a short body lost the map upload policy ahead of the cut");
    UT_ASSERT_MSG(out.u.lobbySettings.scriptUploadPolicy == SCRIPT_UPLOAD_ALLOW,
                  "a body with no script policy byte read as %d, not allow",
                  (int)out.u.lobbySettings.scriptUploadPolicy);
    return 0;
}

/* ── 3. The sim's copy and the client's ───────────────────────────── */

int run_script_upload_policy_event(void) {
    BYTE         emap[6000] = E_MAP;
    ServerSim   *sim;
    ClientSim   *cs;
    ControlEvent evt;

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);

    /* A new sim is allow, as is a NULL one. */
    UT_ASSERT_MSG(serverSimGetScriptUploadPolicy(sim) == SCRIPT_UPLOAD_ALLOW,
                  "a new sim's script upload policy is %d, not allow",
                  (int)serverSimGetScriptUploadPolicy(sim));
    UT_ASSERT(serverSimGetScriptUploadPolicy(NULL) == SCRIPT_UPLOAD_ALLOW);
    serverSimSetScriptUploadPolicy(NULL, SCRIPT_UPLOAD_OFF);  /* NULL-safe */

    serverSimSetScriptUploadPolicy(sim, SCRIPT_UPLOAD_PERSIST);
    UT_ASSERT(serverSimGetScriptUploadPolicy(sim) == SCRIPT_UPLOAD_PERSIST);
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.scriptUploadPolicy == SCRIPT_UPLOAD_PERSIST,
                  "the published settings carry script upload policy %d, "
                  "not persist",
                  (int)evt.u.lobbySettings.scriptUploadPolicy);

    /* The client reads allow before any event, and what the event says after
       one. A zeroed event carrying only the policy, as the mods case in
       test_lobby_mods_enabled.c sends, so nothing else in it moves the
       client. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT_MSG(clientSimGetScriptUploadPolicy(cs) == SCRIPT_UPLOAD_ALLOW,
                  "a client with no settings event reads script upload policy "
                  "%d, not allow", (int)clientSimGetScriptUploadPolicy(cs));
    UT_ASSERT(clientSimGetScriptUploadPolicy(NULL) == SCRIPT_UPLOAD_ALLOW);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SETTINGS;
    evt.u.lobbySettings.scriptUploadPolicy = SCRIPT_UPLOAD_PERSIST;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScriptUploadPolicy(cs) == SCRIPT_UPLOAD_PERSIST,
                  "the client did not follow the server's persist");

    evt.u.lobbySettings.scriptUploadPolicy = SCRIPT_UPLOAD_OFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScriptUploadPolicy(cs) == SCRIPT_UPLOAD_OFF,
                  "the client did not follow the server's off");

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

/* ── 4. Whether players may copy the server's scripts ─────────────── */

int run_script_sharing_codec(void) {
    ControlEncodeBodyFn benc =
        transportControlCodecBodyEncoder(CTRL_LOBBY_SETTINGS);
    ControlDecodeBodyFn bdec =
        transportControlCodecBodyDecoder(CTRL_LOBBY_SETTINGS);
    ControlEvent in;
    ControlEvent out;
    uint8_t      body[MAX_CONTROL_PACKET];
    size_t       len = 0;
    int          on;

    UT_ASSERT(benc != NULL && bdec != NULL);

    /* Both values, each with a start delay whose four bytes all differ, so a
       byte left out or written twice ahead of it shows as a wrong delay. */
    for (on = 0; on <= 1; on++) {
        memset(&in, 0, sizeof(in));
        in.type = CTRL_LOBBY_SETTINGS;
        in.u.lobbySettings.scriptUploadPolicy = SCRIPT_UPLOAD_PERSIST;
        in.u.lobbySettings.scriptSharing      = on ? true : false;
        in.u.lobbySettings.lobbyStartDelay    = 0x0A0B0C0D;
        UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
        UT_ASSERT(len > SUP_START_DELAY_OFFSET + 3);

        UT_ASSERT_MSG(body[SUP_SCRIPT_UPLOAD_POLICY_OFFSET] == 0x02,
                      "byte %d (the script upload policy) is 0x%02X, "
                      "expected 0x02",
                      SUP_SCRIPT_UPLOAD_POLICY_OFFSET,
                      (unsigned)body[SUP_SCRIPT_UPLOAD_POLICY_OFFSET]);
        UT_ASSERT_MSG(body[SUP_SCRIPT_SHARING_OFFSET] == (uint8_t)on,
                      "byte %d (script sharing) is 0x%02X, expected 0x%02X",
                      SUP_SCRIPT_SHARING_OFFSET,
                      (unsigned)body[SUP_SCRIPT_SHARING_OFFSET], (unsigned)on);
        UT_ASSERT_MSG(body[SUP_START_DELAY_OFFSET]     == 0x0A &&
                      body[SUP_START_DELAY_OFFSET + 1] == 0x0B &&
                      body[SUP_START_DELAY_OFFSET + 2] == 0x0C &&
                      body[SUP_START_DELAY_OFFSET + 3] == 0x0D,
                      "the start delay is not at byte %d behind the sharing "
                      "byte", SUP_START_DELAY_OFFSET);

        memset(&out, 0, sizeof(out));
        UT_ASSERT(bdec(body, len, &out));
        UT_ASSERT_MSG(out.u.lobbySettings.scriptSharing == (on ? true : false),
                      "script sharing %d came back as %d", on,
                      (int)out.u.lobbySettings.scriptSharing);
        UT_ASSERT_MSG(out.u.lobbySettings.scriptUploadPolicy ==
                          SCRIPT_UPLOAD_PERSIST,
                      "the script upload policy ahead of it came back as %d",
                      (int)out.u.lobbySettings.scriptUploadPolicy);
        UT_ASSERT_MSG(out.u.lobbySettings.lobbyStartDelay == 0x0A0B0C0D,
                      "the start delay behind it came back as 0x%08X",
                      (unsigned)out.u.lobbySettings.lobbyStartDelay);
    }

    /* A body cut off right before the byte decodes, keeps the policy ahead
       of it, and reads as sharing. The last body encoded carries sharing on,
       so encode one with it off first: a decoder that read past the cut
       would then show off. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    in.u.lobbySettings.scriptUploadPolicy = SCRIPT_UPLOAD_PERSIST;
    in.u.lobbySettings.scriptSharing      = false;
    UT_ASSERT(benc(&in, NULL, body, sizeof(body), &len) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(bdec(body, SUP_SCRIPT_SHARING_OFFSET, &out),
                  "a body that stops before the sharing byte did not decode");
    UT_ASSERT_MSG(out.u.lobbySettings.scriptUploadPolicy ==
                      SCRIPT_UPLOAD_PERSIST,
                  "a short body lost the script upload policy ahead of the cut");
    UT_ASSERT_MSG(out.u.lobbySettings.scriptSharing == true,
                  "a body with no sharing byte read as not sharing");
    return 0;
}

int run_script_sharing_sim(void) {
    BYTE         emap[6000] = E_MAP;
    ServerSim   *sim;
    ControlEvent evt;

    sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                    gameOpen, false, 0, -1);
    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);

    /* A new sim shares, and a NULL one answers yes. */
    UT_ASSERT_MSG(serverSimGetScriptSharing(sim) == true,
                  "a new sim does not share its scripts");
    UT_ASSERT(serverSimGetScriptSharing(NULL) == true);
    serverSimSetScriptSharing(NULL, false);  /* NULL-safe */

    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.scriptSharing == true,
                  "a new sim's settings do not carry sharing");

    serverSimSetScriptSharing(sim, false);
    UT_ASSERT(serverSimGetScriptSharing(sim) == false);
    memset(&evt, 0, sizeof(evt));
    evt.u.lobbySettings.scriptSharing = true;
    serverSimFillLobbySettingsEvent(sim, &evt);
    UT_ASSERT_MSG(evt.u.lobbySettings.scriptSharing == false,
                  "the published settings carry sharing after it was "
                  "turned off");

    serverSimSetScriptSharing(sim, true);
    UT_ASSERT(serverSimGetScriptSharing(sim) == true);

    serverSimDestroy(sim);
    return 0;
}

int run_script_sharing_client(void) {
    ClientSim   *cs;
    ControlEvent evt;

    /* Sharing before any event, as for a NULL client. */
    cs = clientSimAlloc();
    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT_MSG(clientSimGetScriptSharing(cs) == true,
                  "a client with no settings event reads not sharing");
    UT_ASSERT(clientSimGetScriptSharing(NULL) == true);

    /* A zeroed event carrying only the flag, as the policy case above
       sends. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SETTINGS;
    evt.u.lobbySettings.scriptSharing = false;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScriptSharing(cs) == false,
                  "the client did not follow the server's sharing off");

    evt.u.lobbySettings.scriptSharing = true;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScriptSharing(cs) == true,
                  "the client did not follow the server's sharing on");

    clientSimDestroy(cs);
    return 0;
}
