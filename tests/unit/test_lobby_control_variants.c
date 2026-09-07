/*
 * Codec round-trip + apply-path coverage for the lobby ControlEvent
 * variants this branch added or extended:
 *
 *   CTRL_LOBBY_SETTINGS   — extended with lobbyOpenHost,
 *                            lobbyAutoLockOnGameStart, lobbyServerLocks
 *   CTRL_LOBBY_TEAM_META  — new
 *   CTRL_LOBBY_BOT_CONFIG — new
 *   CTRL_LOBBY_BOT_BRAIN  — new
 *   CTRL_LOBBY_BRAIN_LIST — new
 *
 * Each test fills a ControlEvent, runs it through the encoder table
 * (transportControlCodecEncoder), recovers it via the decoder table
 * keyed by the encoded packet's wire type, and asserts every field
 * survives. Then it builds a fresh ClientSim, dispatches the same
 * event through clientSimApplyControl, and asserts the matching
 * ClientSim mirror fields. The two halves test different surfaces:
 * the codec verifies wire layout, the apply verifies the dispatcher
 * in client_sim_control.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "brain_list.h"
#include "transport_control_codec.h"
#include "transport_udp_internal.h"  /* PACKET_HEADER_SIZE */
#include "netpacks.h"                /* PACKET_LOBBY_* IDs */
#include "test_harness.h"

/* Encode `in` via the codec, then locate the decoder by the packet
 * type the encoder stamped into the header (byte 2 of the 8-byte
 * header) and decode the payload back into `out`. Returns 0 on
 * success, negative on failure. */
static int codec_roundtrip(ControlEventType type,
                           const ControlEvent *in,
                           ControlEvent *out) {
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    ControlEncodeFn enc = transportControlCodecEncoder(type);
    if (enc == NULL) return -1;
    EncodeResult r = enc(in, NULL, buf, sizeof(buf), &outLen);
    if (r != ENCODE_OK) return -2;
    if (outLen < PACKET_HEADER_SIZE) return -3;
    /* packHeader writes the packet type at byte 2. */
    uint8_t packetType = buf[2];
    ControlDecodeFn dec = transportControlCodecDecoder(packetType);
    if (dec == NULL) return -4;
    if (!dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, out)) {
        return -5;
    }
    return 0;
}

static ClientSim *fresh_client_sim(void) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);
    return cs;
}

/* ================================================================
 * CTRL_LOBBY_SETTINGS — extended with three trailing Layout A
 * fields. Round-trip every field; verify the apply path drops them
 * onto the matching ClientSim fields.
 * ================================================================ */
int run_lobby_settings_codec_and_apply(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SETTINGS;
    strncpy(in.u.lobbySettings.mapName, "TestMap",
            sizeof(in.u.lobbySettings.mapName) - 1);
    in.u.lobbySettings.lobbyGameType            = gameOpen;
    in.u.lobbySettings.lobbyHiddenMines         = true;
    in.u.lobbySettings.lobbyAiType              = 2;
    in.u.lobbySettings.lobbyTimeLimit           = 90000;
    in.u.lobbySettings.lobbyPillCount           = 11;
    in.u.lobbySettings.lobbyBaseCount           = 7;
    in.u.lobbySettings.lobbyStartCount          = 16;
    in.u.lobbySettings.mapSkipAvailable         = true;
    in.u.lobbySettings.netStat                  = netRunning;
    in.u.lobbySettings.hasLobby                  = false;
    in.u.lobbySettings.lobbyOpenHost            = true;
    in.u.lobbySettings.lobbyAutoLockOnGameStart = true;
    in.u.lobbySettings.lobbyServerLocks         = 0xABCD;
    in.u.lobbySettings.lobbyRanked              = true;
    in.u.lobbySettings.lobbyAllowNewPlayers     = false;
    in.u.lobbySettings.lobbyWbnAvailable        = true;
    in.u.lobbySettings.hostSlot                 = 3;
    in.u.lobbySettings.viewPolicy[viewCategoryPill]    = viewPolicyKey;
    in.u.lobbySettings.viewPolicy[viewCategoryBase]    = viewPolicyDecay;
    in.u.lobbySettings.viewPolicy[viewCategoryAlly]    = viewPolicyOff;
    in.u.lobbySettings.viewDecaySecs[viewCategoryPill] = 45;
    in.u.lobbySettings.viewDecaySecs[viewCategoryBase] = 600;
    in.u.lobbySettings.viewDecaySecs[viewCategoryAlly] = 5;
    in.u.lobbySettings.lobbyClassicMode                = true;
    in.u.lobbySettings.lobbyAlliesInTrees              = true;

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_SETTINGS, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.type == CTRL_LOBBY_SETTINGS);
    UT_ASSERT(strcmp(out.u.lobbySettings.mapName, "TestMap") == 0);
    UT_ASSERT(out.u.lobbySettings.lobbyGameType            == gameOpen);
    UT_ASSERT(out.u.lobbySettings.lobbyHiddenMines         == true);
    UT_ASSERT(out.u.lobbySettings.lobbyAiType              == 2);
    UT_ASSERT(out.u.lobbySettings.lobbyTimeLimit           == 90000);
    UT_ASSERT(out.u.lobbySettings.lobbyPillCount           == 11);
    UT_ASSERT(out.u.lobbySettings.lobbyBaseCount           == 7);
    UT_ASSERT(out.u.lobbySettings.lobbyStartCount          == 16);
    UT_ASSERT(out.u.lobbySettings.mapSkipAvailable         == true);
    UT_ASSERT(out.u.lobbySettings.netStat                  == netRunning);
    UT_ASSERT(out.u.lobbySettings.hasLobby                  == false);
    UT_ASSERT(out.u.lobbySettings.lobbyOpenHost            == true);
    UT_ASSERT(out.u.lobbySettings.lobbyAutoLockOnGameStart == true);
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyServerLocks == 0xABCD,
                  "serverLocks got 0x%04X want 0xABCD",
                  (unsigned)out.u.lobbySettings.lobbyServerLocks);
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyRanked == true,
                  "lobbyRanked did not survive codec round-trip");
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyAllowNewPlayers == false,
                  "lobbyAllowNewPlayers did not survive codec round-trip");
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyWbnAvailable == true,
                  "lobbyWbnAvailable did not survive codec round-trip");
    UT_ASSERT_MSG(out.u.lobbySettings.hostSlot == 3,
                  "hostSlot did not survive codec round-trip");
    UT_ASSERT_MSG(out.u.lobbySettings.viewPolicy[viewCategoryPill] == viewPolicyKey &&
                  out.u.lobbySettings.viewPolicy[viewCategoryBase] == viewPolicyDecay &&
                  out.u.lobbySettings.viewPolicy[viewCategoryAlly] == viewPolicyOff,
                  "view policies did not survive codec round-trip (got %d/%d/%d)",
                  (int)out.u.lobbySettings.viewPolicy[viewCategoryPill],
                  (int)out.u.lobbySettings.viewPolicy[viewCategoryBase],
                  (int)out.u.lobbySettings.viewPolicy[viewCategoryAlly]);
    UT_ASSERT_MSG(out.u.lobbySettings.viewDecaySecs[viewCategoryPill] == 45 &&
                  out.u.lobbySettings.viewDecaySecs[viewCategoryBase] == 600 &&
                  out.u.lobbySettings.viewDecaySecs[viewCategoryAlly] == 5,
                  "view decay seconds did not survive codec round-trip (got %u/%u/%u)",
                  (unsigned)out.u.lobbySettings.viewDecaySecs[viewCategoryPill],
                  (unsigned)out.u.lobbySettings.viewDecaySecs[viewCategoryBase],
                  (unsigned)out.u.lobbySettings.viewDecaySecs[viewCategoryAlly]);
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyClassicMode == true,
                  "lobbyClassicMode did not survive codec round-trip");
    UT_ASSERT_MSG(out.u.lobbySettings.lobbyAlliesInTrees == true,
                  "lobbyAlliesInTrees did not survive codec round-trip");

    /* A sender that stops before the view tail (the payload shape from
     * before these fields existed) must still decode, leaving the view
     * fields at their zero-init values rather than reading past the
     * buffer. Encode a full event, then hand the decoder a body length
     * that is eleven bytes shorter (3 policies + 3 u16 decay values +
     * classic mode + allies in trees). */
    {
        uint8_t buf[MAX_CONTROL_PACKET];
        size_t encLen = 0;
        ControlEncodeFn enc = transportControlCodecEncoder(CTRL_LOBBY_SETTINGS);
        UT_ASSERT(enc != NULL);
        UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &encLen) == ENCODE_OK);
        ControlDecodeFn dec = transportControlCodecDecoder(buf[2]);
        UT_ASSERT(dec != NULL);

        ControlEvent shortOut;
        size_t shortBody = encLen - PACKET_HEADER_SIZE - 11;
        UT_ASSERT_MSG(dec(buf + PACKET_HEADER_SIZE, shortBody, &shortOut),
                      "short lobby-settings payload failed to decode");
        UT_ASSERT_MSG(shortOut.u.lobbySettings.hostSlot == 3,
                      "short payload lost a field that was still present");
        for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
            UT_ASSERT_MSG(shortOut.u.lobbySettings.viewPolicy[vc] == viewPolicyAlways,
                          "short payload view policy %d = %d, want 0",
                          vc, (int)shortOut.u.lobbySettings.viewPolicy[vc]);
            UT_ASSERT_MSG(shortOut.u.lobbySettings.viewDecaySecs[vc] == 0,
                          "short payload view decay %d = %u, want 0",
                          vc, (unsigned)shortOut.u.lobbySettings.viewDecaySecs[vc]);
        }
        UT_ASSERT_MSG(shortOut.u.lobbySettings.lobbyClassicMode == false,
                      "short payload must leave classic mode off");
        UT_ASSERT_MSG(shortOut.u.lobbySettings.lobbyAlliesInTrees == false,
                      "short payload must leave allies in trees off");
    }

    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(strcmp(cs->mapName, "TestMap") == 0);
    UT_ASSERT(cs->lobbyGameType            == gameOpen);
    UT_ASSERT(cs->lobbyHiddenMines         == true);
    UT_ASSERT(cs->lobbyAiType              == 2);
    UT_ASSERT(cs->lobbyTimeLimit           == 90000);
    UT_ASSERT(cs->lobbyPillCount           == 11);
    UT_ASSERT(cs->lobbyBaseCount           == 7);
    UT_ASSERT(cs->lobbyStartCount          == 16);
    UT_ASSERT(cs->mapSkipAvailable         == true);
    UT_ASSERT(cs->netStat                  == netRunning);
    UT_ASSERT(cs->inLobby                  == false);
    UT_ASSERT(cs->lobbyOpenHost            == true);
    UT_ASSERT(cs->lobbyAutoLockOnGameStart == true);
    UT_ASSERT(cs->lobbyServerLocks         == 0xABCD);
    UT_ASSERT(cs->lobbyRanked              == true);
    UT_ASSERT(cs->lobbyAllowNewPlayers     == false);
    UT_ASSERT(cs->lobbyWbnAvailable        == true);
    UT_ASSERT(cs->lobbyHostSlot            == 3);
    UT_ASSERT(clientSimGetViewPolicy(cs, viewCategoryPill) == viewPolicyKey);
    UT_ASSERT(clientSimGetViewPolicy(cs, viewCategoryBase) == viewPolicyDecay);
    UT_ASSERT(clientSimGetViewPolicy(cs, viewCategoryAlly) == viewPolicyOff);
    UT_ASSERT(clientSimGetViewDecaySecs(cs, viewCategoryPill) == 45);
    UT_ASSERT(clientSimGetViewDecaySecs(cs, viewCategoryBase) == 600);
    UT_ASSERT(clientSimGetViewDecaySecs(cs, viewCategoryAlly) == 5);
    UT_ASSERT_MSG(clientSimGetClassicMode(cs) == true,
                  "classic mode did not reach the client mirror");
    UT_ASSERT_MSG(clientSimGetAlliesInTrees(cs) == true,
                  "allies in trees did not reach the client mirror");
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_TEAM_META — `in_use` is not on the wire; the decoder
 * reconstructs it from (nameLen>0 || color!=0 || pool!=0). Two
 * sub-cases exercise both sides of that branch.
 * ================================================================ */
int run_lobby_team_meta_codec_and_apply(void) {
    /* Populated team: name + color + pool all set → in_use reconstructs to 1 */
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_TEAM_META;
    in.u.lobbyTeamMeta.teamId     = 3;
    in.u.lobbyTeamMeta.in_use     = 1;
    in.u.lobbyTeamMeta.color      = 5;
    in.u.lobbyTeamMeta.namingPool = 2;
    strncpy(in.u.lobbyTeamMeta.name, "Phoenix",
            sizeof(in.u.lobbyTeamMeta.name) - 1);

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_TEAM_META, &in, &out) == 0,
                  "codec_roundtrip failed (populated)");
    UT_ASSERT(out.type == CTRL_LOBBY_TEAM_META);
    UT_ASSERT(out.u.lobbyTeamMeta.teamId     == 3);
    UT_ASSERT(out.u.lobbyTeamMeta.in_use     == 1);
    UT_ASSERT(out.u.lobbyTeamMeta.color      == 5);
    UT_ASSERT(out.u.lobbyTeamMeta.namingPool == 2);
    UT_ASSERT(strcmp(out.u.lobbyTeamMeta.name, "Phoenix") == 0);

    /* Empty team: name="", color=0, pool=0 → in_use reconstructs to 0
     * even if the caller had set in_use=1 (it's recomputed). */
    ControlEvent empty_in, empty_out;
    memset(&empty_in, 0, sizeof(empty_in));
    empty_in.type = CTRL_LOBBY_TEAM_META;
    empty_in.u.lobbyTeamMeta.teamId = 4;
    empty_in.u.lobbyTeamMeta.in_use = 1; /* will be overwritten by decoder */
    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_TEAM_META,
                                  &empty_in, &empty_out) == 0,
                  "codec_roundtrip failed (empty)");
    UT_ASSERT(empty_out.u.lobbyTeamMeta.teamId == 4);
    UT_ASSERT_MSG(empty_out.u.lobbyTeamMeta.in_use == 0,
                  "decoder should clear in_use when name/color/pool are zero, got %u",
                  (unsigned)empty_out.u.lobbyTeamMeta.in_use);
    UT_ASSERT(empty_out.u.lobbyTeamMeta.name[0] == '\0');

    /* Apply: the populated event should land on cs->lobbyTeam* arrays. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(cs->lobbyTeamInUse[3] == 1);
    UT_ASSERT(cs->lobbyTeamColor[3] == 5);
    UT_ASSERT(cs->lobbyTeamPool[3]  == 2);
    UT_ASSERT(strcmp(cs->lobbyTeamName[3], "Phoenix") == 0);
    /* Untouched neighbour stays zero. */
    UT_ASSERT(cs->lobbyTeamInUse[4] == 0);
    UT_ASSERT(cs->lobbyTeamName[4][0] == '\0');
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_BOT_CONFIG — the wire carries name but the apply path
 * intentionally ignores it (players.c is source of truth via the
 * lobbySlot path). Test both halves.
 * ================================================================ */
int run_lobby_bot_config_codec_and_apply(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_BOT_CONFIG;
    in.u.lobbyBotConfig.slot        = 5;
    in.u.lobbyBotConfig.difficulty  = 3;
    in.u.lobbyBotConfig.personality = 7;
    strncpy(in.u.lobbyBotConfig.name, "BotOnFive",
            sizeof(in.u.lobbyBotConfig.name) - 1);

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BOT_CONFIG, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.type == CTRL_LOBBY_BOT_CONFIG);
    UT_ASSERT(out.u.lobbyBotConfig.slot        == 5);
    UT_ASSERT(out.u.lobbyBotConfig.difficulty  == 3);
    UT_ASSERT(out.u.lobbyBotConfig.personality == 7);
    UT_ASSERT(strcmp(out.u.lobbyBotConfig.name, "BotOnFive") == 0);

    /* Apply: difficulty + personality land on the cs arrays. The name
     * field is informational on this event and must NOT stomp the
     * slot's playerName — the lobbySlot path owns that. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    /* Seed slot 5 with a known playerName so we can prove apply
     * didn't touch it. */
    strncpy(cs->lobbySlots[5].playerName, "ExistingSlotName",
            sizeof(cs->lobbySlots[5].playerName) - 1);
    clientSimApplyControl(cs, &in);
    UT_ASSERT(cs->lobbyBotDifficulty[5]  == 3);
    UT_ASSERT(cs->lobbyBotPersonality[5] == 7);
    UT_ASSERT_MSG(
        strcmp(cs->lobbySlots[5].playerName, "ExistingSlotName") == 0,
        "lobbyBotConfig.name should not stomp lobbySlots[].playerName, got '%s'",
        cs->lobbySlots[5].playerName);
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_BOT_BRAIN — index round-trip and 0xFF "server default"
 * sentinel. The apply path resolves the index through the client's
 * brain-list mirror, so the test seeds that catalogue first.
 * ================================================================ */
int run_lobby_bot_brain_codec_and_apply(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_BOT_BRAIN;
    in.u.lobbyBotBrain.slot     = 2;
    in.u.lobbyBotBrain.brainIdx = 3;

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BOT_BRAIN, &in, &out) == 0,
                  "codec_roundtrip failed (in-range index)");
    UT_ASSERT(out.type == CTRL_LOBBY_BOT_BRAIN);
    UT_ASSERT(out.u.lobbyBotBrain.slot == 2);
    UT_ASSERT(out.u.lobbyBotBrain.brainIdx == 3);

    /* 0xFF "use server default" sentinel round-trip. */
    ControlEvent default_in, default_out;
    memset(&default_in, 0, sizeof(default_in));
    default_in.type = CTRL_LOBBY_BOT_BRAIN;
    default_in.u.lobbyBotBrain.slot     = 6;
    default_in.u.lobbyBotBrain.brainIdx = 0xFF;
    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BOT_BRAIN,
                                  &default_in, &default_out) == 0,
                  "codec_roundtrip failed (0xFF sentinel)");
    UT_ASSERT(default_out.u.lobbyBotBrain.slot == 6);
    UT_ASSERT(default_out.u.lobbyBotBrain.brainIdx == 0xFF);

    /* Apply writes the index straight onto cs->lobbyBotBrainIdx, but
     * clamps any in-range-looking value that lies past the brain
     * catalogue to the 0xFF sentinel — so we still have to seed the
     * count. 0xFF passes the clamp untouched. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    cs->lobbyBrainList.count = 4;
    clientSimApplyControl(cs, &in);
    UT_ASSERT(cs->lobbyBotBrainIdx[2] == 3);
    clientSimApplyControl(cs, &default_in);
    UT_ASSERT(cs->lobbyBotBrainIdx[6] == 0xFF);
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_BRAIN_LIST — name/version round-trip. Tests a
 * single-entry list and a BRAIN_LIST_MAX-entry list to exercise
 * the worst-case encode path (~897 bytes payload, comfortably
 * inside MAX_CONTROL_PACKET's single-datagram budget). Disk paths
 * are server-private and never appear on the wire.
 * ================================================================ */
int run_lobby_brain_list_codec_and_apply(void) {
    /* Single entry. */
    {
        ControlEvent in, out;
        memset(&in, 0, sizeof(in));
        in.type = CTRL_LOBBY_BRAIN_LIST;
        in.u.lobbyBrainList.list.count = 1;
        strncpy(in.u.lobbyBrainList.list.entries[0].name,    "GoalHunter",
                BRAIN_LIST_NAME_LEN - 1);
        strncpy(in.u.lobbyBrainList.list.entries[0].version, "2026-05-11 12:30",
                BRAIN_LIST_VER_LEN - 1);

        UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BRAIN_LIST, &in, &out) == 0,
                      "codec_roundtrip failed (single entry)");
        UT_ASSERT(out.type == CTRL_LOBBY_BRAIN_LIST);
        UT_ASSERT(out.u.lobbyBrainList.list.count == 1);
        UT_ASSERT(strcmp(out.u.lobbyBrainList.list.entries[0].name,
                         "GoalHunter") == 0);
        UT_ASSERT(strcmp(out.u.lobbyBrainList.list.entries[0].version,
                         "2026-05-11 12:30") == 0);
    }

    /* Full BRAIN_LIST_MAX entries — worst-case wire size. Each entry
     * gets a unique name/version so we can spot misordering. */
    ControlEvent full_in, full_out;
    memset(&full_in, 0, sizeof(full_in));
    full_in.type = CTRL_LOBBY_BRAIN_LIST;
    full_in.u.lobbyBrainList.list.count = BRAIN_LIST_MAX;
    int i;
    for (i = 0; i < BRAIN_LIST_MAX; i++) {
        snprintf(full_in.u.lobbyBrainList.list.entries[i].name,
                 BRAIN_LIST_NAME_LEN, "Brain%02d", i);
        snprintf(full_in.u.lobbyBrainList.list.entries[i].version,
                 BRAIN_LIST_VER_LEN, "v%d", i);
    }

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_BRAIN_LIST,
                                  &full_in, &full_out) == 0,
                  "codec_roundtrip failed (BRAIN_LIST_MAX entries)");
    UT_ASSERT(full_out.u.lobbyBrainList.list.count == BRAIN_LIST_MAX);
    for (i = 0; i < BRAIN_LIST_MAX; i++) {
        char wantName[BRAIN_LIST_NAME_LEN];
        char wantVer [BRAIN_LIST_VER_LEN];
        snprintf(wantName, sizeof(wantName), "Brain%02d", i);
        snprintf(wantVer,  sizeof(wantVer),  "v%d", i);
        UT_ASSERT_MSG(strcmp(full_out.u.lobbyBrainList.list.entries[i].name,
                             wantName) == 0,
                      "entry %d name mismatch: got '%s' want '%s'",
                      i, full_out.u.lobbyBrainList.list.entries[i].name, wantName);
        UT_ASSERT(strcmp(full_out.u.lobbyBrainList.list.entries[i].version,
                         wantVer) == 0);
    }

    /* Apply: cs->lobbyBrainList is a straight struct copy. */
    ClientSim *cs = fresh_client_sim();
    UT_ASSERT(cs != NULL);
    clientSimApplyControl(cs, &full_in);
    UT_ASSERT(cs->lobbyBrainList.count == BRAIN_LIST_MAX);
    UT_ASSERT(strcmp(cs->lobbyBrainList.entries[0].name,  "Brain00") == 0);
    UT_ASSERT(strcmp(cs->lobbyBrainList.entries[BRAIN_LIST_MAX - 1].name,
                     "Brain15") == 0);
    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_SYNC_COMPLETE — header-only marker the server delivers
 * as the final event of the join sync replay. No payload, so the
 * round-trip just confirms the type survives encode/decode.
 * ================================================================ */
int run_lobby_sync_complete_codec_roundtrip(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_LOBBY_SYNC_COMPLETE;

    UT_ASSERT_MSG(codec_roundtrip(CTRL_LOBBY_SYNC_COMPLETE, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.type == CTRL_LOBBY_SYNC_COMPLETE);
    return 0;
}

/* ================================================================
 * CTRL_ROUND_RATING_POSTED — who posted, and the WinBolo.net key of
 * the round they posted against. Delivered body-only on
 * CHANNEL_CONTROL with no full-packet wrapper and no PACKET_* type,
 * so this one resolves the pair through the body tables the live path
 * uses rather than through codec_roundtrip above.
 * ================================================================ */
int run_lobby_rating_posted_codec_roundtrip(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_ROUND_RATING_POSTED);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_ROUND_RATING_POSTED);
    UT_ASSERT_MSG(enc != NULL, "no body encoder for CTRL_ROUND_RATING_POSTED");
    UT_ASSERT_MSG(dec != NULL, "no body decoder for CTRL_ROUND_RATING_POSTED");

    ControlEvent in, out;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;
    const char *key = "0123456789abcdef0123456789abcdef";  /* fills the field */

    memset(&in, 0, sizeof(in));
    in.type = CTRL_ROUND_RATING_POSTED;
    in.u.ratingPosted.fromPlayer = 9;
    memcpy(in.u.ratingPosted.key, key, strlen(key));

    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    UT_ASSERT_MSG(outLen == 1 + (ROUND_STATS_LOGKEY_LEN - 1),
                  "body len = %zu (want %zu)", outLen,
                  (size_t)(1 + (ROUND_STATS_LOGKEY_LEN - 1)));

    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed");
    UT_ASSERT(out.type == CTRL_ROUND_RATING_POSTED);
    UT_ASSERT(out.u.ratingPosted.fromPlayer == 9);
    UT_ASSERT(strcmp(out.u.ratingPosted.key, key) == 0);

    /* A short key is NUL-padded out, so the body is the same size and the
     * key still comes back terminated. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_ROUND_RATING_POSTED;
    in.u.ratingPosted.fromPlayer = 0;
    memcpy(in.u.ratingPosted.key, "abc123", 6);
    outLen = 0;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(dec(buf, outLen, &out), "decode failed (short key)");
    UT_ASSERT(out.u.ratingPosted.fromPlayer == 0);
    UT_ASSERT(strcmp(out.u.ratingPosted.key, "abc123") == 0);

    /* Fixed-length body: a truncated one is refused. */
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(!dec(buf, outLen - 1, &out),
                  "decoder must reject a short body");
    return 0;
}
