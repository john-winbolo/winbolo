/*
 * Codec round-trip coverage for CTRL_GAME_VOTE_STATE — the in-game
 * back-to-lobby / surrender vote that mirrors PACKET_GAME_VOTE_STATE
 * onto remote UDP clients. Wire body is fixed-size 11 bytes; we
 * verify every field survives encode → decode untouched.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "transport_udp_internal.h"  /* PACKET_HEADER_SIZE */
#include "netpacks.h"                /* PACKET_GAME_VOTE_STATE, GAME_VOTE_* */
#include "test_harness.h"

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
    uint8_t packetType = buf[2];
    ControlDecodeFn dec = transportControlCodecDecoder(packetType);
    if (dec == NULL) return -4;
    if (!dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, out)) {
        return -5;
    }
    return 0;
}

/* Active back-to-lobby vote with a mix of yes/no votes and a partial
 * eligible-set bitmask. Round-trip and assert every field. */
int run_game_vote_state_codec_back_to_lobby(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_GAME_VOTE_STATE;
    in.u.gameVoteState.kind             = GAME_VOTE_KIND_BACK_TO_LOBBY;
    in.u.gameVoteState.active           = GAME_VOTE_ACTIVE_RUNNING;
    in.u.gameVoteState.triggerSrc       = GAME_VOTE_TRIGGER_MANUAL;
    in.u.gameVoteState.teamId           = 0;   /* all-teams for BTL */
    in.u.gameVoteState.threshold        = 3;
    in.u.gameVoteState.yesCount         = 2;
    in.u.gameVoteState.noCount          = 1;
    in.u.gameVoteState.eligibleCount    = 4;
    in.u.gameVoteState.secondsRemaining = 42;
    in.u.gameVoteState.votes            = 0xA5C3; /* arbitrary bitmask */

    UT_ASSERT_MSG(codec_roundtrip(CTRL_GAME_VOTE_STATE, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.type == CTRL_GAME_VOTE_STATE);
    UT_ASSERT(out.u.gameVoteState.kind             == GAME_VOTE_KIND_BACK_TO_LOBBY);
    UT_ASSERT(out.u.gameVoteState.active           == GAME_VOTE_ACTIVE_RUNNING);
    UT_ASSERT(out.u.gameVoteState.triggerSrc       == GAME_VOTE_TRIGGER_MANUAL);
    UT_ASSERT(out.u.gameVoteState.teamId           == 0);
    UT_ASSERT(out.u.gameVoteState.threshold        == 3);
    UT_ASSERT(out.u.gameVoteState.yesCount         == 2);
    UT_ASSERT(out.u.gameVoteState.noCount          == 1);
    UT_ASSERT(out.u.gameVoteState.eligibleCount    == 4);
    UT_ASSERT(out.u.gameVoteState.secondsRemaining == 42);
    UT_ASSERT_MSG(out.u.gameVoteState.votes == 0xA5C3,
                  "votes bitmask did not survive round-trip: got 0x%04X",
                  (unsigned)out.u.gameVoteState.votes);
    return 0;
}

/* Surrender vote, just-passed state, targeted at a specific team.
 * Exercises the teamId byte and the maximum 60-second countdown. */
int run_game_vote_state_codec_surrender_passed(void) {
    ControlEvent in, out;
    memset(&in, 0, sizeof(in));
    in.type = CTRL_GAME_VOTE_STATE;
    in.u.gameVoteState.kind             = GAME_VOTE_KIND_SURRENDER;
    in.u.gameVoteState.active           = GAME_VOTE_ACTIVE_PASSED;
    in.u.gameVoteState.triggerSrc       = GAME_VOTE_TRIGGER_BASE_MONOPOLY;
    in.u.gameVoteState.teamId           = 7;
    in.u.gameVoteState.threshold        = 2;
    in.u.gameVoteState.yesCount         = 2;
    in.u.gameVoteState.noCount          = 0;
    in.u.gameVoteState.eligibleCount    = 2;
    in.u.gameVoteState.secondsRemaining = 60;
    in.u.gameVoteState.votes            = 0xFFFF;

    UT_ASSERT_MSG(codec_roundtrip(CTRL_GAME_VOTE_STATE, &in, &out) == 0,
                  "codec_roundtrip failed");
    UT_ASSERT(out.u.gameVoteState.kind             == GAME_VOTE_KIND_SURRENDER);
    UT_ASSERT(out.u.gameVoteState.active           == GAME_VOTE_ACTIVE_PASSED);
    UT_ASSERT(out.u.gameVoteState.triggerSrc       == GAME_VOTE_TRIGGER_BASE_MONOPOLY);
    UT_ASSERT(out.u.gameVoteState.teamId           == 7);
    UT_ASSERT(out.u.gameVoteState.threshold        == 2);
    UT_ASSERT(out.u.gameVoteState.yesCount         == 2);
    UT_ASSERT(out.u.gameVoteState.noCount          == 0);
    UT_ASSERT(out.u.gameVoteState.eligibleCount    == 2);
    UT_ASSERT(out.u.gameVoteState.secondsRemaining == 60);
    UT_ASSERT(out.u.gameVoteState.votes            == 0xFFFF);
    return 0;
}

/* Decoder must reject a short body (anything < 11 bytes). */
int run_game_vote_state_decoder_rejects_short(void) {
    uint8_t shortBuf[10] = {0};
    ControlEvent out;
    ControlDecodeFn dec = transportControlCodecDecoder(PACKET_GAME_VOTE_STATE);
    UT_ASSERT(dec != NULL);
    UT_ASSERT_MSG(dec(shortBuf, sizeof(shortBuf), &out) == false,
                  "decoder should reject a body shorter than 11 bytes");
    return 0;
}

/* CTRL_SERVER_TEXT encodes onto the wire as PACKET_CHAT_BROADCAST with
 * fromPlayer=0xFE.  Decoding via the chat decoder reconstructs a
 * CTRL_CHAT event (not CTRL_SERVER_TEXT) — the codec deliberately
 * collapses the two on the wire so the existing UDP client chat path
 * keeps working without a new packet type.  Verify both halves. */
int run_server_text_codec_via_chat_wire(void) {
    ControlEvent in;
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;

    memset(&in, 0, sizeof(in));
    in.type = CTRL_SERVER_TEXT;
    const char *msg = "Vote passed: returning to lobby.";
    strncpy(in.u.serverText.text, msg, sizeof(in.u.serverText.text) - 1);

    ControlEncodeFn enc = transportControlCodecEncoder(CTRL_SERVER_TEXT);
    UT_ASSERT(enc != NULL);
    EncodeResult r = enc(&in, NULL, buf, sizeof(buf), &outLen);
    UT_ASSERT(r == ENCODE_OK);
    UT_ASSERT(outLen == PACKET_HEADER_SIZE + 2 + strlen(msg));

    /* Wire packet type is PACKET_CHAT_BROADCAST (NOT a dedicated type). */
    UT_ASSERT_MSG(buf[2] == PACKET_CHAT_BROADCAST,
                  "expected CTRL_SERVER_TEXT to encode as PACKET_CHAT_BROADCAST, got %u",
                  (unsigned)buf[2]);
    UT_ASSERT(buf[PACKET_HEADER_SIZE]     == 0xFE);  /* fromPlayer */
    UT_ASSERT(buf[PACKET_HEADER_SIZE + 1] == 0xFF);  /* destPlayer broadcast */
    UT_ASSERT(memcmp(buf + PACKET_HEADER_SIZE + 2, msg, strlen(msg)) == 0);

    /* Decode via the chat decoder — recovers a CTRL_CHAT, not CTRL_SERVER_TEXT. */
    ControlDecodeFn dec = transportControlCodecDecoder(PACKET_CHAT_BROADCAST);
    UT_ASSERT(dec != NULL);
    ControlEvent out;
    UT_ASSERT(dec(buf + PACKET_HEADER_SIZE, outLen - PACKET_HEADER_SIZE, &out));
    UT_ASSERT(out.type == CTRL_CHAT);
    UT_ASSERT(out.u.chat.fromPlayer == 0xFE);
    UT_ASSERT(out.u.chat.destPlayer == 0xFF);
    UT_ASSERT(out.u.chat.bodyLen == strlen(msg));
    UT_ASSERT(memcmp(out.u.chat.body, msg, strlen(msg)) == 0);
    return 0;
}
