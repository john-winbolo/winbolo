/*
 * Voice segment framing (src/bolo/voice_segment.c). Two halves:
 *
 *   - the round trip, which pins that every field survives both
 *     directions and that a parse hands back a pointer into the caller's
 *     own buffer rather than a copy.
 *   - the hostile-input pass. These segments arrive from the network on a
 *     best-effort channel, where nothing upstream has validated them, so
 *     every truncation of a valid segment and every out-of-range field
 *     must be refused rather than parsed.
 */
#include <string.h>

#include "voice_segment.h"
#include "global.h"

/* The end-of-utterance bit as the wire framing spells it, captured before
 * the codec header below brings its own copy of the same macro into scope.
 * The codec side cannot see voice_segment.h, so this file - which sees both
 * - is where the two definitions are held to the same value. */
enum { VOICE_WIRE_FLAG_END_OF_UTTERANCE = VOICE_FLAG_END_OF_UTTERANCE };

#include "voice_core.h"
#include "test_harness.h"

BOLO_STATIC_ASSERT(VOICE_WIRE_FLAG_END_OF_UTTERANCE ==
                   VOICE_FLAG_END_OF_UTTERANCE,
                   voice_flag_end_of_utterance_drift);

/* A payload with no byte repeated in a pattern the header could be
 * mistaken for, so a parse that lands one byte off is visible. */
static void fillPayload(uint8_t *buf, int len) {
    int i;
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(0x40 + i);
    }
}

int run_voice_segment_roundtrip(void) {
    uint8_t payload[VOICE_SEG_MAX_OPUS];
    uint8_t seg[CHANNEL_VOICE_SEG];
    const uint8_t *opus;
    uint8_t fromPlayer, seq, flags;
    int opusLen;
    int segLen;

    /* --- client -> server, a typical 60 byte frame --- */
    fillPayload(payload, 60);
    segLen = voiceSegmentPackUp(seg, (int)sizeof(seg), 200, 0, payload, 60);
    UT_ASSERT_MSG(segLen == VOICE_SEG_UP_HEADER + 60, "up length %d", segLen);

    UT_ASSERT(voiceSegmentUnpackUp(seg, segLen, &seq, &flags, &opus,
                                   &opusLen));
    UT_ASSERT(seq == 200);
    UT_ASSERT(flags == 0);
    UT_ASSERT(opusLen == 60);
    UT_ASSERT(memcmp(opus, payload, 60) == 0);
    /* The payload is not copied: it points at the tail of the input. */
    UT_ASSERT(opus == seg + VOICE_SEG_UP_HEADER);

    /* --- server -> client, carrying the end-of-utterance flag ---
     * Packed with the wire spelling and checked against the codec's, so a
     * bit that means one thing on the wire and another to the jitter buffer
     * fails here as well as at compile time. */
    segLen = voiceSegmentPackDown(seg, (int)sizeof(seg), MAX_TANKS - 1, 255,
                                  VOICE_WIRE_FLAG_END_OF_UTTERANCE, payload,
                                  60);
    UT_ASSERT_MSG(segLen == VOICE_SEG_DOWN_HEADER + 60, "down length %d",
                  segLen);

    UT_ASSERT(voiceSegmentUnpackDown(seg, segLen, &fromPlayer, &seq, &flags,
                                     &opus, &opusLen));
    UT_ASSERT(fromPlayer == MAX_TANKS - 1);
    UT_ASSERT(seq == 255);
    UT_ASSERT(flags == VOICE_FLAG_END_OF_UTTERANCE);
    UT_ASSERT(opusLen == 60);
    UT_ASSERT(memcmp(opus, payload, 60) == 0);
    UT_ASSERT(opus == seg + VOICE_SEG_DOWN_HEADER);

    /* --- the largest payload that fits, both directions --- */
    fillPayload(payload, VOICE_SEG_MAX_OPUS);
    segLen = voiceSegmentPackDown(seg, (int)sizeof(seg), 0, 1, 0, payload,
                                  VOICE_SEG_MAX_OPUS);
    UT_ASSERT(segLen == VOICE_SEG_DOWN_HEADER + VOICE_SEG_MAX_OPUS);
    UT_ASSERT(segLen <= CHANNEL_VOICE_SEG);
    UT_ASSERT(voiceSegmentUnpackDown(seg, segLen, &fromPlayer, &seq, &flags,
                                     &opus, &opusLen));
    UT_ASSERT(opusLen == VOICE_SEG_MAX_OPUS);
    UT_ASSERT(memcmp(opus, payload, VOICE_SEG_MAX_OPUS) == 0);

    segLen = voiceSegmentPackUp(seg, (int)sizeof(seg), 1, 0, payload,
                                VOICE_SEG_MAX_OPUS);
    UT_ASSERT(segLen == VOICE_SEG_UP_HEADER + VOICE_SEG_MAX_OPUS);
    UT_ASSERT(voiceSegmentUnpackUp(seg, segLen, &seq, &flags, &opus,
                                   &opusLen));
    UT_ASSERT(opusLen == VOICE_SEG_MAX_OPUS);

    /* --- a one byte payload is still a payload --- */
    payload[0] = 0x99;
    segLen = voiceSegmentPackUp(seg, (int)sizeof(seg), 7, 0, payload, 1);
    UT_ASSERT(segLen == VOICE_SEG_UP_HEADER + 1);
    UT_ASSERT(voiceSegmentUnpackUp(seg, segLen, &seq, &flags, &opus,
                                   &opusLen));
    UT_ASSERT(opusLen == 1 && opus[0] == 0x99);

    return 0;
}

int run_voice_segment_rejects_malformed(void) {
    uint8_t payload[VOICE_SEG_MAX_OPUS + 1];
    uint8_t seg[CHANNEL_VOICE_SEG];
    uint8_t tooSmall[VOICE_SEG_DOWN_HEADER];
    const uint8_t *opus;
    uint8_t fromPlayer, seq, flags;
    int opusLen;
    int segLen;
    int cut;

    fillPayload(payload, (int)sizeof(payload));

    /* --- every truncation of a real up-segment --- */
    segLen = voiceSegmentPackUp(seg, (int)sizeof(seg), 3, 0, payload, 40);
    UT_ASSERT(segLen == VOICE_SEG_UP_HEADER + 40);
    for (cut = 0; cut <= VOICE_SEG_UP_HEADER; cut++) {
        UT_ASSERT_MSG(!voiceSegmentUnpackUp(seg, cut, &seq, &flags, &opus,
                                            &opusLen),
                      "up parse accepted %d bytes", cut);
    }
    /* One byte past the header is the shortest thing that is a segment. */
    UT_ASSERT(voiceSegmentUnpackUp(seg, VOICE_SEG_UP_HEADER + 1, &seq, &flags,
                                   &opus, &opusLen));
    UT_ASSERT(opusLen == 1);

    /* --- every truncation of a real down-segment --- */
    segLen = voiceSegmentPackDown(seg, (int)sizeof(seg), 2, 3, 0, payload, 40);
    UT_ASSERT(segLen == VOICE_SEG_DOWN_HEADER + 40);
    for (cut = 0; cut <= VOICE_SEG_DOWN_HEADER; cut++) {
        UT_ASSERT_MSG(!voiceSegmentUnpackDown(seg, cut, &fromPlayer, &seq,
                                              &flags, &opus, &opusLen),
                      "down parse accepted %d bytes", cut);
    }
    UT_ASSERT(voiceSegmentUnpackDown(seg, VOICE_SEG_DOWN_HEADER + 1,
                                     &fromPlayer, &seq, &flags, &opus,
                                     &opusLen));
    UT_ASSERT(opusLen == 1);

    /* Nothing in the layout states the payload length - it is whatever is
     * left - so a segment cut anywhere past the header is a shorter valid
     * segment, not a malformed one. Walk every such cut and pin that the
     * payload it reports stays inside the bytes handed in. */
    for (cut = VOICE_SEG_DOWN_HEADER + 1; cut <= segLen; cut++) {
        UT_ASSERT_MSG(voiceSegmentUnpackDown(seg, cut, &fromPlayer, &seq,
                                             &flags, &opus, &opusLen),
                      "down parse refused %d bytes", cut);
        UT_ASSERT(opusLen == cut - VOICE_SEG_DOWN_HEADER);
        UT_ASSERT(opus >= seg && opus + opusLen <= seg + cut);
    }

    /* A negative length is as malformed as a short one. */
    UT_ASSERT(!voiceSegmentUnpackUp(seg, -1, &seq, &flags, &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, -1, &fromPlayer, &seq, &flags,
                                      &opus, &opusLen));

    /* --- a length claiming more payload than a segment can hold --- */
    UT_ASSERT(!voiceSegmentUnpackUp(seg, VOICE_SEG_UP_HEADER +
                                    VOICE_SEG_MAX_OPUS + 1, &seq, &flags,
                                    &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, VOICE_SEG_DOWN_HEADER +
                                      VOICE_SEG_MAX_OPUS + 1, &fromPlayer,
                                      &seq, &flags, &opus, &opusLen));

    /* --- a sender outside the tank slots has no speaker to play it --- */
    segLen = voiceSegmentPackDown(seg, (int)sizeof(seg), 0, 9, 0, payload, 20);
    UT_ASSERT(segLen > 0);
    seg[0] = MAX_TANKS;
    UT_ASSERT(!voiceSegmentUnpackDown(seg, segLen, &fromPlayer, &seq, &flags,
                                      &opus, &opusLen));
    seg[0] = 0xFF;
    UT_ASSERT(!voiceSegmentUnpackDown(seg, segLen, &fromPlayer, &seq, &flags,
                                      &opus, &opusLen));
    seg[0] = MAX_TANKS - 1;
    UT_ASSERT(voiceSegmentUnpackDown(seg, segLen, &fromPlayer, &seq, &flags,
                                     &opus, &opusLen));

    /* Packing rejects it too, so the server can never emit one. */
    UT_ASSERT(voiceSegmentPackDown(seg, (int)sizeof(seg), MAX_TANKS, 0, 0,
                                   payload, 20) == 0);

    /* --- pack refuses what will not fit rather than truncating --- */
    UT_ASSERT(voiceSegmentPackUp(seg, (int)sizeof(seg), 0, 0, payload,
                                 VOICE_SEG_MAX_OPUS + 1) == 0);
    UT_ASSERT(voiceSegmentPackDown(seg, (int)sizeof(seg), 0, 0, 0, payload,
                                   VOICE_SEG_MAX_OPUS + 1) == 0);
    UT_ASSERT(voiceSegmentPackUp(tooSmall, (int)sizeof(tooSmall), 0, 0, payload,
                                 40) == 0);
    UT_ASSERT(voiceSegmentPackDown(tooSmall, (int)sizeof(tooSmall), 0, 0, 0, payload,
                                   40) == 0);
    /* An empty frame carries nothing to play. */
    UT_ASSERT(voiceSegmentPackUp(seg, (int)sizeof(seg), 0, 0, payload, 0) == 0);
    UT_ASSERT(voiceSegmentPackDown(seg, (int)sizeof(seg), 0, 0, 0, payload,
                                   0) == 0);

    /* --- NULL arguments are refused, not dereferenced --- */
    UT_ASSERT(voiceSegmentPackUp(NULL, 64, 0, 0, payload, 40) == 0);
    UT_ASSERT(voiceSegmentPackUp(seg, (int)sizeof(seg), 0, 0, NULL, 40) == 0);
    UT_ASSERT(voiceSegmentPackDown(NULL, 64, 0, 0, 0, payload, 40) == 0);
    UT_ASSERT(voiceSegmentPackDown(seg, (int)sizeof(seg), 0, 0, 0, NULL,
                                   40) == 0);
    UT_ASSERT(!voiceSegmentUnpackUp(NULL, 40, &seq, &flags, &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackUp(seg, 40, NULL, &flags, &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackUp(seg, 40, &seq, NULL, &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackUp(seg, 40, &seq, &flags, NULL, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackUp(seg, 40, &seq, &flags, &opus, NULL));
    UT_ASSERT(!voiceSegmentUnpackDown(NULL, 40, &fromPlayer, &seq, &flags,
                                      &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, 40, NULL, &seq, &flags, &opus,
                                      &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, 40, &fromPlayer, NULL, &flags,
                                      &opus, &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, 40, &fromPlayer, &seq, NULL, &opus,
                                      &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, 40, &fromPlayer, &seq, &flags, NULL,
                                      &opusLen));
    UT_ASSERT(!voiceSegmentUnpackDown(seg, 40, &fromPlayer, &seq, &flags,
                                      &opus, NULL));

    return 0;
}
