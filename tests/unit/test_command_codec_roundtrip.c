/*
 * Codec round-trip coverage for ClientCommand variants.
 *
 * Each test fills a ClientCommand, runs it through commandCodecEncode,
 * recovers it via commandCodecDecode, and asserts every field
 * survives. Verifies the wire layout of every CMD_* variant plus the
 * cmdSeq slot the codec wrapper owns.
 */

#include <stdint.h>
#include <string.h>

#include "client_command.h"
#include "transport_command_codec.h"
#include "netpacks.h"               /* BOLO_NEW_MAGIC_*, PACKET_HEADER_SIZE */
#include "test_harness.h"

/* Encode `in` then decode back into `out`. Returns 0 on success;
 * non-zero distinguishes encode-side vs decode-side failure. */
static int roundtrip_command(const ClientCommand *in, ClientCommand *out) {
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t outLen = 0;
    if (!commandCodecEncode(in, buf, sizeof(buf), &outLen)) return -1;
    if (!commandCodecDecode(buf, outLen, out)) return -2;
    return 0;
}

int run_command_codec_roundtrip_variants(void) {
    ClientCommand in, out;

    /* CMD_TEAM_SET — two scalars */
    memset(&in, 0, sizeof(in));
    in.type = CMD_TEAM_SET;
    in.cmdSeq = 1;
    in.u.teamSet.slot = 3;
    in.u.teamSet.team = 7;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_TEAM_SET");
    UT_ASSERT(out.type == CMD_TEAM_SET);
    UT_ASSERT(out.cmdSeq == 1);
    UT_ASSERT(out.u.teamSet.slot == 3);
    UT_ASSERT(out.u.teamSet.team == 7);

    /* CMD_READY — bool ready (legacy playerNum byte on wire, not in struct) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_READY;
    in.cmdSeq = 2;
    in.u.ready.ready = true;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_READY");
    UT_ASSERT(out.type == CMD_READY);
    UT_ASSERT(out.cmdSeq == 2);
    UT_ASSERT(out.u.ready.ready == true);

    /* CMD_LOBBY_BOT_CONFIG — slot/difficulty/personality + variable name */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_BOT_CONFIG;
    in.cmdSeq = 3;
    in.u.lobbyBotConfig.slot        = 4;
    in.u.lobbyBotConfig.difficulty  = 2;
    in.u.lobbyBotConfig.personality = 5;
    in.u.lobbyBotConfig.nameLen     = 7;
    memcpy(in.u.lobbyBotConfig.name, "BotName", 7);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_BOT_CONFIG");
    UT_ASSERT(out.type == CMD_LOBBY_BOT_CONFIG);
    UT_ASSERT(out.cmdSeq == 3);
    UT_ASSERT(out.u.lobbyBotConfig.slot        == 4);
    UT_ASSERT(out.u.lobbyBotConfig.difficulty  == 2);
    UT_ASSERT(out.u.lobbyBotConfig.personality == 5);
    UT_ASSERT(out.u.lobbyBotConfig.nameLen     == 7);
    UT_ASSERT(memcmp(out.u.lobbyBotConfig.name, "BotName", 7) == 0);

    /* CMD_LOBBY_TEAM_META — teamId/color/pool + variable name */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_TEAM_META;
    in.cmdSeq = 4;
    in.u.lobbyTeamMeta.teamId     = 6;
    in.u.lobbyTeamMeta.color      = 3;
    in.u.lobbyTeamMeta.namingPool = 2;
    in.u.lobbyTeamMeta.nameLen    = 5;
    memcpy(in.u.lobbyTeamMeta.name, "Reds!", 5);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_TEAM_META");
    UT_ASSERT(out.type == CMD_LOBBY_TEAM_META);
    UT_ASSERT(out.cmdSeq == 4);
    UT_ASSERT(out.u.lobbyTeamMeta.teamId     == 6);
    UT_ASSERT(out.u.lobbyTeamMeta.color      == 3);
    UT_ASSERT(out.u.lobbyTeamMeta.namingPool == 2);
    UT_ASSERT(out.u.lobbyTeamMeta.nameLen    == 5);
    UT_ASSERT(memcmp(out.u.lobbyTeamMeta.name, "Reds!", 5) == 0);

    /* CMD_LOBBY_TEAM_CLEAR — single byte teamId */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_TEAM_CLEAR;
    in.cmdSeq = 5;
    in.u.lobbyTeamClear.teamId = 9;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_TEAM_CLEAR");
    UT_ASSERT(out.type == CMD_LOBBY_TEAM_CLEAR);
    UT_ASSERT(out.cmdSeq == 5);
    UT_ASSERT(out.u.lobbyTeamClear.teamId == 9);

    /* CMD_LOBBY_SETTING — settingType + variable value buffer (max 32) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_SETTING;
    in.cmdSeq = 6;
    in.u.lobbySetting.settingType = 11;
    in.u.lobbySetting.valueLen    = 4;
    in.u.lobbySetting.value[0]    = 0xDE;
    in.u.lobbySetting.value[1]    = 0xAD;
    in.u.lobbySetting.value[2]    = 0xBE;
    in.u.lobbySetting.value[3]    = 0xEF;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_SETTING");
    UT_ASSERT(out.type == CMD_LOBBY_SETTING);
    UT_ASSERT(out.cmdSeq == 6);
    UT_ASSERT(out.u.lobbySetting.settingType == 11);
    UT_ASSERT(out.u.lobbySetting.valueLen    == 4);
    UT_ASSERT(memcmp(out.u.lobbySetting.value,
                     in.u.lobbySetting.value, 4) == 0);

    /* CMD_CHAT — destPlayer + variable body (length implied by packet length) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_CHAT;
    in.cmdSeq = 7;
    in.u.chat.destPlayer = 0xFF;  /* broadcast */
    in.u.chat.bodyLen    = 11;
    memcpy(in.u.chat.body, "hello world", 11);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_CHAT");
    UT_ASSERT(out.type == CMD_CHAT);
    UT_ASSERT(out.cmdSeq == 7);
    UT_ASSERT(out.u.chat.destPlayer == 0xFF);
    UT_ASSERT(out.u.chat.bodyLen    == 11);
    UT_ASSERT(memcmp(out.u.chat.body, "hello world", 11) == 0);

    /* CMD_ALLIANCE_REQUEST — toPlayer (legacy fromPlayer byte zeroed on wire) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_ALLIANCE_REQUEST;
    in.cmdSeq = 8;
    in.u.allianceRequest.toPlayer = 5;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_ALLIANCE_REQUEST");
    UT_ASSERT(out.type == CMD_ALLIANCE_REQUEST);
    UT_ASSERT(out.cmdSeq == 8);
    UT_ASSERT(out.u.allianceRequest.toPlayer == 5);

    /* CMD_ALLIANCE_ACCEPT — newMember (legacy fromPlayer byte zeroed on wire) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_ALLIANCE_ACCEPT;
    in.cmdSeq = 9;
    in.u.allianceAccept.newMember = 6;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_ALLIANCE_ACCEPT");
    UT_ASSERT(out.type == CMD_ALLIANCE_ACCEPT);
    UT_ASSERT(out.cmdSeq == 9);
    UT_ASSERT(out.u.allianceAccept.newMember == 6);

    /* CMD_ALLIANCE_LEAVE — no real payload; only type + cmdSeq survive */
    memset(&in, 0, sizeof(in));
    in.type = CMD_ALLIANCE_LEAVE;
    in.cmdSeq = 10;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_ALLIANCE_LEAVE");
    UT_ASSERT(out.type == CMD_ALLIANCE_LEAVE);
    UT_ASSERT(out.cmdSeq == 10);

    /* CMD_GAME_VOTE_TOGGLE — kind + toggleMode */
    memset(&in, 0, sizeof(in));
    in.type = CMD_GAME_VOTE_TOGGLE;
    in.cmdSeq = 11;
    in.u.gameVoteToggle.kind       = 1;
    in.u.gameVoteToggle.toggleMode = 2;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_GAME_VOTE_TOGGLE");
    UT_ASSERT(out.type == CMD_GAME_VOTE_TOGGLE);
    UT_ASSERT(out.cmdSeq == 11);
    UT_ASSERT(out.u.gameVoteToggle.kind       == 1);
    UT_ASSERT(out.u.gameVoteToggle.toggleMode == 2);

    /* CMD_LOBBY_REMOVE_BOT — single slot byte */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_REMOVE_BOT;
    in.cmdSeq = 12;
    in.u.lobbyRemoveBot.slot = 8;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_REMOVE_BOT");
    UT_ASSERT(out.type == CMD_LOBBY_REMOVE_BOT);
    UT_ASSERT(out.cmdSeq == 12);
    UT_ASSERT(out.u.lobbyRemoveBot.slot == 8);

    /* CMD_LOBBY_SET_BOT_BRAIN — slot + brainIdx */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_SET_BOT_BRAIN;
    in.cmdSeq = 13;
    in.u.lobbySetBotBrain.slot     = 4;
    in.u.lobbySetBotBrain.brainIdx = 0xFF;  /* server-default sentinel */
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_SET_BOT_BRAIN");
    UT_ASSERT(out.type == CMD_LOBBY_SET_BOT_BRAIN);
    UT_ASSERT(out.cmdSeq == 13);
    UT_ASSERT(out.u.lobbySetBotBrain.slot     == 4);
    UT_ASSERT(out.u.lobbySetBotBrain.brainIdx == 0xFF);

    /* CMD_LOBBY_OPEN_HOST — bool openHost */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_OPEN_HOST;
    in.cmdSeq = 14;
    in.u.lobbyOpenHost.openHost = true;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_OPEN_HOST");
    UT_ASSERT(out.type == CMD_LOBBY_OPEN_HOST);
    UT_ASSERT(out.cmdSeq == 14);
    UT_ASSERT(out.u.lobbyOpenHost.openHost == true);

    /* CMD_MAP_SKIP_VOTE — no payload; only type + cmdSeq survive */
    memset(&in, 0, sizeof(in));
    in.type = CMD_MAP_SKIP_VOTE;
    in.cmdSeq = 15;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_MAP_SKIP_VOTE");
    UT_ASSERT(out.type == CMD_MAP_SKIP_VOTE);
    UT_ASSERT(out.cmdSeq == 15);

    /* CMD_NAME_CHANGE — newName[PACKET_MAX_PLAYER_NAME], NUL-padded */
    memset(&in, 0, sizeof(in));
    in.type = CMD_NAME_CHANGE;
    in.cmdSeq = 16;
    memcpy(in.u.nameChange.newName, "NewName", 7);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_NAME_CHANGE");
    UT_ASSERT(out.type == CMD_NAME_CHANGE);
    UT_ASSERT(out.cmdSeq == 16);
    UT_ASSERT(strcmp(out.u.nameChange.newName, "NewName") == 0);

    /* CMD_LOCK_TOGGLE — bool allow */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOCK_TOGGLE;
    in.cmdSeq = 17;
    in.u.lockToggle.allow = true;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOCK_TOGGLE");
    UT_ASSERT(out.type == CMD_LOCK_TOGGLE);
    UT_ASSERT(out.cmdSeq == 17);
    UT_ASSERT(out.u.lockToggle.allow == true);

    /* CMD_LOBBY_ADD_BOT — teamNumber + variable name (path byte forced to 0) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_ADD_BOT;
    in.cmdSeq = 18;
    in.u.lobbyAddBot.teamNumber = 2;
    in.u.lobbyAddBot.nameLen    = 5;
    memcpy(in.u.lobbyAddBot.name, "Botty", 5);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_ADD_BOT");
    UT_ASSERT(out.type == CMD_LOBBY_ADD_BOT);
    UT_ASSERT(out.cmdSeq == 18);
    UT_ASSERT(out.u.lobbyAddBot.teamNumber == 2);
    UT_ASSERT(out.u.lobbyAddBot.nameLen    == 5);
    UT_ASSERT(memcmp(out.u.lobbyAddBot.name, "Botty", 5) == 0);

    /* CMD_LOBBY_SET_MAP — pathLen + relPath (decoder rejects pathLen=0) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_SET_MAP;
    in.cmdSeq = 19;
    in.u.lobbySetMap.relPathLen = 8;
    memcpy(in.u.lobbySetMap.relPath, "test.map", 8);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_SET_MAP");
    UT_ASSERT(out.type == CMD_LOBBY_SET_MAP);
    UT_ASSERT(out.cmdSeq == 19);
    UT_ASSERT(out.u.lobbySetMap.relPathLen == 8);
    UT_ASSERT(memcmp(out.u.lobbySetMap.relPath, "test.map", 8) == 0);

    /* CMD_LOBBY_PREVIEW_CANCEL — empty body */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_PREVIEW_CANCEL;
    in.cmdSeq = 20;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_PREVIEW_CANCEL");
    UT_ASSERT(out.type == CMD_LOBBY_PREVIEW_CANCEL);
    UT_ASSERT(out.cmdSeq == 20);

    /* CMD_LOBBY_PREVIEW_COMMIT — empty body */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_PREVIEW_COMMIT;
    in.cmdSeq = 21;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_PREVIEW_COMMIT");
    UT_ASSERT(out.type == CMD_LOBBY_PREVIEW_COMMIT);
    UT_ASSERT(out.cmdSeq == 21);

    /* CMD_LOBBY_PREVIEW_RANDOM — seedLen + seed (clamped to 63) */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_PREVIEW_RANDOM;
    in.cmdSeq = 22;
    in.u.lobbyPreviewRandom.seedLen = 6;
    memcpy(in.u.lobbyPreviewRandom.seed, "abc123", 6);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_PREVIEW_RANDOM");
    UT_ASSERT(out.type == CMD_LOBBY_PREVIEW_RANDOM);
    UT_ASSERT(out.cmdSeq == 22);
    UT_ASSERT(out.u.lobbyPreviewRandom.seedLen == 6);
    UT_ASSERT(memcmp(out.u.lobbyPreviewRandom.seed, "abc123", 6) == 0);

    /* CMD_LOBBY_KICK — single slot byte */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_KICK;
    in.cmdSeq = 23;
    in.u.lobbyKick.slot = 11;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_KICK");
    UT_ASSERT(out.type == CMD_LOBBY_KICK);
    UT_ASSERT(out.cmdSeq == 23);
    UT_ASSERT(out.u.lobbyKick.slot == 11);

    /* CMD_LOBBY_SET_PASSWORD — pwLen + password */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_SET_PASSWORD;
    in.cmdSeq = 24;
    in.u.lobbySetPassword.pwLen = 9;
    memcpy(in.u.lobbySetPassword.password, "swordfish", 9);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_SET_PASSWORD");
    UT_ASSERT(out.type == CMD_LOBBY_SET_PASSWORD);
    UT_ASSERT(out.cmdSeq == 24);
    UT_ASSERT(out.u.lobbySetPassword.pwLen == 9);
    UT_ASSERT(memcmp(out.u.lobbySetPassword.password, "swordfish", 9) == 0);

    /* CMD_BALANCE_REQUEST — teamSize + includeBots bool */
    memset(&in, 0, sizeof(in));
    in.type = CMD_BALANCE_REQUEST;
    in.cmdSeq = 25;
    in.u.balanceRequest.teamSize    = 4;
    in.u.balanceRequest.includeBots = true;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_BALANCE_REQUEST");
    UT_ASSERT(out.type == CMD_BALANCE_REQUEST);
    UT_ASSERT(out.cmdSeq == 25);
    UT_ASSERT(out.u.balanceRequest.teamSize    == 4);
    UT_ASSERT(out.u.balanceRequest.includeBots == true);

    /* CMD_BALANCE_APPLY — empty body */
    memset(&in, 0, sizeof(in));
    in.type = CMD_BALANCE_APPLY;
    in.cmdSeq = 26;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_BALANCE_APPLY");
    UT_ASSERT(out.type == CMD_BALANCE_APPLY);
    UT_ASSERT(out.cmdSeq == 26);

    /* CMD_BALANCE_DISMISS — empty body */
    memset(&in, 0, sizeof(in));
    in.type = CMD_BALANCE_DISMISS;
    in.cmdSeq = 27;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_BALANCE_DISMISS");
    UT_ASSERT(out.type == CMD_BALANCE_DISMISS);
    UT_ASSERT(out.cmdSeq == 27);

    /* CMD_WBN_REAUTH — fixed-size token envelope; decoder forces last
     * byte to NUL, so we leave token[64]=0 to keep the round-trip exact. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_WBN_REAUTH;
    in.cmdSeq = 28;
    {
        int i;
        for (i = 0; i < (int)sizeof(in.u.wbnReauth.token) - 1; i++) {
            in.u.wbnReauth.token[i] = (char)('A' + (i % 26));
        }
        in.u.wbnReauth.token[sizeof(in.u.wbnReauth.token) - 1] = '\0';
    }
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_WBN_REAUTH");
    UT_ASSERT(out.type == CMD_WBN_REAUTH);
    UT_ASSERT(out.cmdSeq == 28);
    UT_ASSERT(memcmp(out.u.wbnReauth.token, in.u.wbnReauth.token,
                     sizeof(in.u.wbnReauth.token)) == 0);

    return 0;
}

/* Focused check on the 4-byte cmdSeq slot the codec wrapper owns,
 * independent of any variant body. Catches off-by-four errors in the
 * wire-offset arithmetic that the per-variant suite would mask if
 * every variant's body still round-tripped under a stuck cmdSeq. */
int run_command_codec_cmdseq_slot(void) {
    const uint32_t seqs[] = { 0u, 1u, 0x12345678u, UINT32_MAX };
    size_t i;
    for (i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        ClientCommand in, out;
        memset(&in, 0, sizeof(in));
        in.type = CMD_LOCK_TOGGLE;
        in.cmdSeq = seqs[i];
        in.u.lockToggle.allow = true;
        memset(&out, 0, sizeof(out));
        UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0,
                      "cmdSeq=0x%08x roundtrip", (unsigned)seqs[i]);
        UT_ASSERT_MSG(out.cmdSeq == seqs[i],
                      "cmdSeq slot mangled: got 0x%08x want 0x%08x",
                      (unsigned)out.cmdSeq, (unsigned)seqs[i]);
        UT_ASSERT(out.type == CMD_LOCK_TOGGLE);
        UT_ASSERT(out.u.lockToggle.allow == true);
    }
    return 0;
}

/* Bounds-check + sentinel-failure coverage on the public API. */
int run_command_codec_bounds_checks(void) {
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t outLen = 0;
    ClientCommand cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_TEAM_SET;
    cmd.cmdSeq = 42;
    cmd.u.teamSet.slot = 1;
    cmd.u.teamSet.team = 2;

    /* Encode: NULL guards. */
    UT_ASSERT(commandCodecEncode(NULL, buf, sizeof(buf), &outLen) == false);
    UT_ASSERT(commandCodecEncode(&cmd, NULL, sizeof(buf), &outLen) == false);
    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), NULL) == false);

    /* Encode: bufCap too small. */
    UT_ASSERT(commandCodecEncode(&cmd, buf, 0, &outLen) == false);

    /* Encode: CMD_NONE is unsupported. */
    {
        ClientCommand none;
        memset(&none, 0, sizeof(none));
        none.type = CMD_NONE;
        UT_ASSERT(commandCodecEncode(&none, buf, sizeof(buf), &outLen) == false);
    }

    /* Encode: out-of-range type falls through the switch default. */
    {
        ClientCommand bad;
        memset(&bad, 0, sizeof(bad));
        bad.type = (ClientCommandType)0xFFFF;
        UT_ASSERT(commandCodecEncode(&bad, buf, sizeof(buf), &outLen) == false);
    }

    /* Decode: NULL guards. */
    UT_ASSERT(commandCodecDecode(NULL, sizeof(buf), &cmd) == false);
    {
        uint8_t junk[PACKET_HEADER_SIZE + 4 + 2] = {0};
        UT_ASSERT(commandCodecDecode(junk, sizeof(junk), NULL) == false);
    }

    /* Decode: too short for header + cmdSeq slot. */
    {
        uint8_t empty[1] = {0};
        UT_ASSERT(commandCodecDecode(empty, 0, &cmd) == false);
    }
    {
        uint8_t header_only[PACKET_HEADER_SIZE] = {0};
        UT_ASSERT(commandCodecDecode(header_only, PACKET_HEADER_SIZE, &cmd)
                  == false);
    }

    /* Successful encode used as the basis for the corruption tests. */
    UT_ASSERT(commandCodecEncode(&cmd, buf, sizeof(buf), &outLen) == true);
    UT_ASSERT(outLen > PACKET_HEADER_SIZE + 4);

    /* Sanity: an untouched buffer decodes cleanly. */
    {
        ClientCommand ok;
        memset(&ok, 0, sizeof(ok));
        UT_ASSERT(commandCodecDecode(buf, outLen, &ok) == true);
        UT_ASSERT(ok.type == CMD_TEAM_SET);
        UT_ASSERT(ok.cmdSeq == 42);
    }

    /* Corrupt magic byte 0 → decode fails the BOLO_NEW_MAGIC check. */
    {
        uint8_t corrupt[COMMAND_MAX_WIRE_BYTES];
        memcpy(corrupt, buf, outLen);
        corrupt[0] = 0xFF;
        ClientCommand sink;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT(commandCodecDecode(corrupt, outLen, &sink) == false);
    }

    /* Corrupt packet-type byte → decoder's switch default rejects. */
    {
        uint8_t corrupt[COMMAND_MAX_WIRE_BYTES];
        memcpy(corrupt, buf, outLen);
        corrupt[2] = 0xFF;
        ClientCommand sink;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT(commandCodecDecode(corrupt, outLen, &sink) == false);
    }

    return 0;
}
