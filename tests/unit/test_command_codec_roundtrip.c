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
#include "input_packet.h"           /* PING_KIND_* — CMD_PING's kind byte */
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

    /* CMD_LOBBY_BOT_CONFIG — slot/difficulty/personality/mode + name */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_BOT_CONFIG;
    in.cmdSeq = 3;
    in.u.lobbyBotConfig.slot        = 4;
    in.u.lobbyBotConfig.difficulty  = 2;
    in.u.lobbyBotConfig.personality = 5;
    in.u.lobbyBotConfig.mode        = 1;
    in.u.lobbyBotConfig.nameLen     = 7;
    memcpy(in.u.lobbyBotConfig.name, "BotName", 7);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_BOT_CONFIG");
    UT_ASSERT(out.type == CMD_LOBBY_BOT_CONFIG);
    UT_ASSERT(out.cmdSeq == 3);
    UT_ASSERT(out.u.lobbyBotConfig.slot        == 4);
    UT_ASSERT(out.u.lobbyBotConfig.difficulty  == 2);
    UT_ASSERT(out.u.lobbyBotConfig.personality == 5);
    UT_ASSERT(out.u.lobbyBotConfig.mode        == 1);
    UT_ASSERT(out.u.lobbyBotConfig.nameLen     == 7);
    UT_ASSERT(memcmp(out.u.lobbyBotConfig.name, "BotName", 7) == 0);

    /* CMD_LOBBY_TEAM_META — teamId/color/pool/startSide + variable name */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_TEAM_META;
    in.cmdSeq = 4;
    in.u.lobbyTeamMeta.teamId     = 6;
    in.u.lobbyTeamMeta.color      = 3;
    in.u.lobbyTeamMeta.namingPool = 2;
    in.u.lobbyTeamMeta.startSide  = 4;
    in.u.lobbyTeamMeta.nameLen    = 5;
    memcpy(in.u.lobbyTeamMeta.name, "Reds!", 5);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_TEAM_META");
    UT_ASSERT(out.type == CMD_LOBBY_TEAM_META);
    UT_ASSERT(out.cmdSeq == 4);
    UT_ASSERT(out.u.lobbyTeamMeta.teamId     == 6);
    UT_ASSERT(out.u.lobbyTeamMeta.color      == 3);
    UT_ASSERT(out.u.lobbyTeamMeta.namingPool == 2);
    UT_ASSERT(out.u.lobbyTeamMeta.startSide  == 4);
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

    /* CMD_LOBBY_TRANSFER_HOST — single slot byte */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_TRANSFER_HOST;
    in.cmdSeq = 31;
    in.u.lobbyTransferHost.slot = 5;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_TRANSFER_HOST");
    UT_ASSERT(out.type == CMD_LOBBY_TRANSFER_HOST);
    UT_ASSERT(out.cmdSeq == 31);
    UT_ASSERT(out.u.lobbyTransferHost.slot == 5);

    /* CMD_PLAYER_MUTE — targetPlayer + muted flag */
    memset(&in, 0, sizeof(in));
    in.type = CMD_PLAYER_MUTE;
    in.cmdSeq = 32;
    in.u.playerMute.targetPlayer = 9;
    in.u.playerMute.muted        = 1;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_PLAYER_MUTE");
    UT_ASSERT(out.type == CMD_PLAYER_MUTE);
    UT_ASSERT(out.cmdSeq == 32);
    UT_ASSERT(out.u.playerMute.targetPlayer == 9);
    UT_ASSERT(out.u.playerMute.muted        == 1);

    /* The unmute direction round-trips too. */
    in.cmdSeq = 33;
    in.u.playerMute.muted = 0;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_PLAYER_MUTE unmute");
    UT_ASSERT(out.type == CMD_PLAYER_MUTE);
    UT_ASSERT(out.cmdSeq == 33);
    UT_ASSERT(out.u.playerMute.targetPlayer == 9);
    UT_ASSERT(out.u.playerMute.muted        == 0);

    /* CMD_PLAYER_MUTE decoder rejections — the bytes come off the wire, so
     * a short body and an out-of-range slot must both be refused. */
    {
        const size_t bodyOff = PACKET_HEADER_SIZE + 4;
        uint8_t wire[COMMAND_MAX_WIRE_BYTES];
        size_t wireLen = 0;
        ClientCommand sink;

        memset(&in, 0, sizeof(in));
        in.type = CMD_PLAYER_MUTE;
        in.cmdSeq = 34;
        in.u.playerMute.targetPlayer = 2;
        in.u.playerMute.muted        = 1;
        UT_ASSERT(commandCodecEncode(&in, wire, sizeof(wire), &wireLen) == true);
        UT_ASSERT(wireLen == bodyOff + 2);

        /* One byte short of the two-byte body. */
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT(commandCodecDecode(wire, bodyOff + 1, &sink) == false);

        /* targetPlayer at and past MAX_TANKS. */
        {
            uint8_t bad[COMMAND_MAX_WIRE_BYTES];
            memcpy(bad, wire, wireLen);
            bad[bodyOff] = (uint8_t)MAX_TANKS;
            memset(&sink, 0, sizeof(sink));
            UT_ASSERT(commandCodecDecode(bad, wireLen, &sink) == false);
            bad[bodyOff] = 0xFF;
            memset(&sink, 0, sizeof(sink));
            UT_ASSERT(commandCodecDecode(bad, wireLen, &sink) == false);
        }

        /* Any non-zero muted byte normalises to 1. */
        {
            uint8_t odd[COMMAND_MAX_WIRE_BYTES];
            memcpy(odd, wire, wireLen);
            odd[bodyOff + 1] = 0x7F;
            memset(&sink, 0, sizeof(sink));
            UT_ASSERT(commandCodecDecode(odd, wireLen, &sink) == true);
            UT_ASSERT(sink.u.playerMute.muted == 1);
        }
    }

    /* CMD_VOICE_STATE — hasMic + selfMuted */
    memset(&in, 0, sizeof(in));
    in.type = CMD_VOICE_STATE;
    in.cmdSeq = 35;
    in.u.voiceState.hasMic    = 1;
    in.u.voiceState.selfMuted = 1;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VOICE_STATE");
    UT_ASSERT(out.type == CMD_VOICE_STATE);
    UT_ASSERT(out.cmdSeq == 35);
    UT_ASSERT(out.u.voiceState.hasMic    == 1);
    UT_ASSERT(out.u.voiceState.selfMuted == 1);

    /* Both fields carry their own value — mic open and transmitting. */
    in.cmdSeq = 36;
    in.u.voiceState.hasMic    = 1;
    in.u.voiceState.selfMuted = 0;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VOICE_STATE unmuted");
    UT_ASSERT(out.type == CMD_VOICE_STATE);
    UT_ASSERT(out.cmdSeq == 36);
    UT_ASSERT(out.u.voiceState.hasMic    == 1);
    UT_ASSERT(out.u.voiceState.selfMuted == 0);

    in.cmdSeq = 37;
    in.u.voiceState.hasMic    = 0;
    in.u.voiceState.selfMuted = 0;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VOICE_STATE no mic");
    UT_ASSERT(out.type == CMD_VOICE_STATE);
    UT_ASSERT(out.cmdSeq == 37);
    UT_ASSERT(out.u.voiceState.hasMic    == 0);
    UT_ASSERT(out.u.voiceState.selfMuted == 0);

    /* CMD_VOICE_STATE decoder: a short body is refused, and either byte
     * arriving as some other non-zero value normalises to 1. */
    {
        const size_t bodyOff = PACKET_HEADER_SIZE + 4;
        uint8_t wire[COMMAND_MAX_WIRE_BYTES];
        size_t wireLen = 0;
        ClientCommand sink;

        memset(&in, 0, sizeof(in));
        in.type = CMD_VOICE_STATE;
        in.cmdSeq = 38;
        in.u.voiceState.hasMic    = 1;
        in.u.voiceState.selfMuted = 1;
        UT_ASSERT(commandCodecEncode(&in, wire, sizeof(wire), &wireLen) == true);
        UT_ASSERT(wireLen == bodyOff + 2);

        /* One byte short of the two-byte body. */
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT(commandCodecDecode(wire, bodyOff + 1, &sink) == false);

        {
            uint8_t odd[COMMAND_MAX_WIRE_BYTES];
            memcpy(odd, wire, wireLen);
            odd[bodyOff]     = 0x7F;
            odd[bodyOff + 1] = 0x20;
            memset(&sink, 0, sizeof(sink));
            UT_ASSERT(commandCodecDecode(odd, wireLen, &sink) == true);
            UT_ASSERT(sink.u.voiceState.hasMic    == 1);
            UT_ASSERT(sink.u.voiceState.selfMuted == 1);
        }
    }

    return 0;
}

/* CMD_LOBBY_CLAIM_START — {targetSlot, startIdx} two-byte body, with
 * the 0xFF release sentinel exercised so the codec carries it intact. */
int run_command_codec_lobby_claim_start(void) {
    ClientCommand in, out;

    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_CLAIM_START;
    in.cmdSeq = 40;
    in.u.lobbyClaimStart.targetSlot = 3;
    in.u.lobbyClaimStart.startIdx   = 7;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_LOBBY_CLAIM_START");
    UT_ASSERT(out.type == CMD_LOBBY_CLAIM_START);
    UT_ASSERT(out.cmdSeq == 40);
    UT_ASSERT(out.u.lobbyClaimStart.targetSlot == 3);
    UT_ASSERT(out.u.lobbyClaimStart.startIdx   == 7);

    /* Release sentinel round-trips unchanged. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_LOBBY_CLAIM_START;
    in.cmdSeq = 41;
    in.u.lobbyClaimStart.targetSlot = 11;
    in.u.lobbyClaimStart.startIdx   = 0xFF;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0,
                  "CMD_LOBBY_CLAIM_START release");
    UT_ASSERT(out.type == CMD_LOBBY_CLAIM_START);
    UT_ASSERT(out.cmdSeq == 41);
    UT_ASSERT(out.u.lobbyClaimStart.targetSlot == 11);
    UT_ASSERT(out.u.lobbyClaimStart.startIdx   == 0xFF);

    return 0;
}

/* CMD_RATING_POSTED — a fixed-length key body. The key must survive the
 * round trip whether or not it fills the field, and the decoder must refuse
 * a packet that is not exactly the fixed length. */
int run_command_codec_rating_posted(void) {
    ClientCommand in, out;
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t outLen = 0;

    /* Full-width key. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_RATING_POSTED;
    in.cmdSeq = 50;
    {
        int i;
        for (i = 0; i < ROUND_STATS_LOGKEY_LEN - 1; i++) {
            in.u.ratingPosted.key[i] = (char)('a' + (i % 26));
        }
        in.u.ratingPosted.key[ROUND_STATS_LOGKEY_LEN - 1] = '\0';
    }
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_RATING_POSTED");
    UT_ASSERT(out.type == CMD_RATING_POSTED);
    UT_ASSERT(out.cmdSeq == 50);
    UT_ASSERT(strcmp(out.u.ratingPosted.key, in.u.ratingPosted.key) == 0);

    /* Short key: NUL-padded onto the wire, NUL-terminated on the way back. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_RATING_POSTED;
    in.cmdSeq = 51;
    memcpy(in.u.ratingPosted.key, "abc123", 6);
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0,
                  "CMD_RATING_POSTED short key");
    UT_ASSERT(out.cmdSeq == 51);
    UT_ASSERT(strcmp(out.u.ratingPosted.key, "abc123") == 0);

    /* Fixed length: one byte short and one byte long are both refused. */
    UT_ASSERT(commandCodecEncode(&in, buf, sizeof(buf), &outLen) == true);
    UT_ASSERT_MSG(outLen == PACKET_HEADER_SIZE + 4 + (ROUND_STATS_LOGKEY_LEN - 1),
                  "wire len = %zu", outLen);
    {
        ClientCommand sink;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen - 1, &sink) == false,
                      "decoder must reject a short RATING_POSTED packet");
        buf[outLen] = 0;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen + 1, &sink) == false,
                      "decoder must reject an over-long RATING_POSTED packet");
    }

    return 0;
}

/* CMD_VIEW_STATE — kind + target, a fixed two-byte body. Both bytes must
 * survive the round trip, including values the dispatcher will later degrade
 * (the codec carries whatever the client sent), and the decoder must refuse a
 * packet that is not exactly the fixed length. */
int run_command_codec_view_state(void) {
    ClientCommand in, out;
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t outLen = 0;

    memset(&in, 0, sizeof(in));
    in.type = CMD_VIEW_STATE;
    in.cmdSeq = 60;
    in.u.viewState.kind   = VIEW_KIND_PILL;
    in.u.viewState.target = 5;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VIEW_STATE");
    UT_ASSERT(out.type == CMD_VIEW_STATE);
    UT_ASSERT(out.cmdSeq == 60);
    UT_ASSERT(out.u.viewState.kind   == VIEW_KIND_PILL);
    UT_ASSERT(out.u.viewState.target == 5);

    /* Out-of-range values are the dispatcher's business, not the codec's. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_VIEW_STATE;
    in.cmdSeq = 61;
    in.u.viewState.kind   = 200;
    in.u.viewState.target = 0xFF;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VIEW_STATE out of range");
    UT_ASSERT(out.cmdSeq == 61);
    UT_ASSERT(out.u.viewState.kind   == 200);
    UT_ASSERT(out.u.viewState.target == 0xFF);

    /* Fixed length: one byte short and one byte long are both refused. */
    UT_ASSERT(commandCodecEncode(&in, buf, sizeof(buf), &outLen) == true);
    UT_ASSERT_MSG(outLen == PACKET_HEADER_SIZE + 4 + 2, "wire len = %zu", outLen);
    {
        ClientCommand sink;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen - 1, &sink) == false,
                      "decoder must reject a short VIEW_STATE packet");
        buf[outLen] = 0;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen + 1, &sink) == false,
                      "decoder must reject an over-long VIEW_STATE packet");
    }

    return 0;
}

/* CMD_VIEW_CYCLE — kind + direction + from, a fixed three-byte body. All
 * three bytes must survive the round trip, including the 0xFF "watching
 * nothing" from and values the dispatcher will later reject (the codec
 * carries whatever the client sent), and the decoder must refuse a packet
 * that is not exactly the fixed length. */
int run_command_codec_view_cycle(void) {
    ClientCommand in, out;
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t outLen = 0;

    memset(&in, 0, sizeof(in));
    in.type = CMD_VIEW_CYCLE;
    in.cmdSeq = 70;
    in.u.viewCycle.kind      = VIEW_KIND_ALLY;
    in.u.viewCycle.direction = VIEW_CYCLE_RIGHT;
    in.u.viewCycle.from      = 3;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VIEW_CYCLE");
    UT_ASSERT(out.type == CMD_VIEW_CYCLE);
    UT_ASSERT(out.cmdSeq == 70);
    UT_ASSERT(out.u.viewCycle.kind      == VIEW_KIND_ALLY);
    UT_ASSERT(out.u.viewCycle.direction == VIEW_CYCLE_RIGHT);
    UT_ASSERT(out.u.viewCycle.from      == 3);

    /* Out-of-range values are the dispatcher's business, not the codec's;
     * from == 0xFF is the sender watching nothing yet. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_VIEW_CYCLE;
    in.cmdSeq = 71;
    in.u.viewCycle.kind      = 200;
    in.u.viewCycle.direction = 99;
    in.u.viewCycle.from      = VIEW_CYCLE_FROM_NONE;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_VIEW_CYCLE out of range");
    UT_ASSERT(out.cmdSeq == 71);
    UT_ASSERT(out.u.viewCycle.kind      == 200);
    UT_ASSERT(out.u.viewCycle.direction == 99);
    UT_ASSERT(out.u.viewCycle.from      == VIEW_CYCLE_FROM_NONE);

    /* Fixed length: one byte short and one byte long are both refused. */
    UT_ASSERT(commandCodecEncode(&in, buf, sizeof(buf), &outLen) == true);
    UT_ASSERT_MSG(outLen == PACKET_HEADER_SIZE + 4 + 3, "wire len = %zu", outLen);
    {
        ClientCommand sink;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen - 1, &sink) == false,
                      "decoder must reject a short VIEW_CYCLE packet");
        buf[outLen] = 0;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen + 1, &sink) == false,
                      "decoder must reject an over-long VIEW_CYCLE packet");
    }

    return 0;
}

/* CMD_PING — kind + two big-endian u16 coordinates, a fixed five-byte body.
 * The coordinates are WORLD units, so the whole u16 range is legal and the
 * extremes have to survive: a sub-square position is the whole point of not
 * sending map squares. The decoder must refuse a packet that is not exactly
 * the fixed length. */
int run_command_codec_ping(void) {
    ClientCommand in, out;
    uint8_t buf[COMMAND_MAX_WIRE_BYTES];
    size_t outLen = 0;

    memset(&in, 0, sizeof(in));
    in.type = CMD_PING;
    in.cmdSeq = 80;
    in.u.ping.kind   = PING_KIND_ON_MY_WAY;
    in.u.ping.worldX = 0x1234;
    in.u.ping.worldY = 0xABCD;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_PING");
    UT_ASSERT(out.type == CMD_PING);
    UT_ASSERT(out.cmdSeq == 80);
    UT_ASSERT(out.u.ping.kind   == PING_KIND_ON_MY_WAY);
    UT_ASSERT_MSG(out.u.ping.worldX == 0x1234, "worldX = 0x%04X",
                  out.u.ping.worldX);
    UT_ASSERT_MSG(out.u.ping.worldY == 0xABCD, "worldY = 0x%04X",
                  out.u.ping.worldY);

    /* Both ends of the coordinate range, and a kind the dispatcher will
     * later reject — range is the dispatcher's business, not the codec's. */
    memset(&in, 0, sizeof(in));
    in.type = CMD_PING;
    in.cmdSeq = 81;
    in.u.ping.kind   = 200;
    in.u.ping.worldX = 0;
    in.u.ping.worldY = 0xFFFF;
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(roundtrip_command(&in, &out) == 0, "CMD_PING extremes");
    UT_ASSERT(out.cmdSeq == 81);
    UT_ASSERT(out.u.ping.kind   == 200);
    UT_ASSERT(out.u.ping.worldX == 0);
    UT_ASSERT(out.u.ping.worldY == 0xFFFF);

    /* Fixed length: one byte short and one byte long are both refused. */
    UT_ASSERT(commandCodecEncode(&in, buf, sizeof(buf), &outLen) == true);
    UT_ASSERT_MSG(outLen == PACKET_HEADER_SIZE + 4 + 5, "wire len = %zu", outLen);
    {
        ClientCommand sink;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen - 1, &sink) == false,
                      "decoder must reject a short PING packet");
        buf[outLen] = 0;
        memset(&sink, 0, sizeof(sink));
        UT_ASSERT_MSG(commandCodecDecode(buf, outLen + 1, &sink) == false,
                      "decoder must reject an over-long PING packet");
    }

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
