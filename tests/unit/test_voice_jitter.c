/*
 * Per-speaker jitter buffer (src/client_frontend/voice_core.c). Frames
 * arrive late, out of order, duplicated, or not at all, and the buffer has
 * to turn that into one 20 ms frame per pop in sequence order, concealing
 * the gaps.
 *
 * Real Opus frames are used rather than crafted bytes: the buffer hands
 * whatever it holds to the decoder, so a payload the decoder rejects would
 * exercise the failure path instead of the ordering being tested. The
 * sequence numbers are the point of the test, not the audio.
 *
 * The exception is the last case, which is about that failure path: a frame
 * can arrive on time and still be unusable, and the buffer has to treat it
 * as a loss rather than pretend it played.
 *
 * Pops carry a clock, because a frame that is missing is not concealed until
 * it is overdue by VOICE_JITTER_LATE_MS. Every case that wants a gap
 * concealed has to move the clock past that deadline first, which is what
 * jtPopOverdue below does.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "voice_core.h"
#include "test_harness.h"

#define TONE_HZ        440.0
#define TONE_AMPLITUDE 0.3

/* One frame of playout, in milliseconds. */
#define JT_FRAME_MS 20

/* Encoded frames, one per sequence number the test uses. Encoding is
 * stateful, so they are produced up front from one continuous tone. */
#define TEST_FRAMES 8

typedef struct {
    uint8_t data[VOICE_MAX_PACKET];
    int     len;
} EncodedFrame;

static int encodeFrames(EncodedFrame *frames, int count) {
    VoiceEncoder *enc;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    double phase = 0.0;
    const double phaseStep = 2.0 * 3.14159265358979323846 * TONE_HZ /
                             (double)VOICE_SAMPLE_RATE;
    int f, i;

    enc = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    if (enc == NULL) {
        return 0;
    }

    for (f = 0; f < count; f++) {
        for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            pcm[i] = (int16_t)(sin(phase) * TONE_AMPLITUDE * 32767.0);
            phase += phaseStep;
        }
        frames[f].len = voiceEncoderEncode(enc, pcm, frames[f].data,
                                           (int)sizeof(frames[f].data));
        if (frames[f].len <= 0) {
            voiceEncoderDestroy(enc);
            return 0;
        }
    }

    voiceEncoderDestroy(enc);
    return 1;
}

/* One pop, then the clock moves on by the frame it was asked for. */
static bool jtPop(VoiceSpeaker *sp, int16_t *pcm, uint32_t *now) {
    bool got = voiceSpeakerPop(sp, pcm, *now);
    *now += JT_FRAME_MS;
    return got;
}

/* A frame that is not there is not concealed on the spot: the buffer waits
 * VOICE_JITTER_LATE_MS for it first. So a concealment takes two pops — one
 * that starts the wait and returns false, and one past the deadline that
 * produces the concealed frame. Returns what that second pop returned, and
 * false if the first pop produced audio, which is not the case under test. */
static bool jtPopOverdue(VoiceSpeaker *sp, int16_t *pcm, uint32_t *now) {
    if (voiceSpeakerPop(sp, pcm, *now)) {
        return false;
    }
    *now += VOICE_JITTER_LATE_MS;
    return voiceSpeakerPop(sp, pcm, *now);
}

int run_voice_jitter_ordering_and_plc(void) {
    EncodedFrame frames[TEST_FRAMES];
    VoiceSpeaker *sp;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    uint32_t now = 0;
    int i;

    UT_ASSERT(encodeFrames(frames, TEST_FRAMES));

    sp = voiceSpeakerCreate();
    UT_ASSERT(sp != NULL);

    /* --- nothing buffered: nothing to play --- */
    UT_ASSERT(!jtPop(sp, pcm, &now));

    /* --- in order: 0, 1, 2 --- */
    voiceSpeakerPush(sp, 0, 0, frames[0].data, frames[0].len);
    /* One frame is under the target, so playback has not started. */
    UT_ASSERT(!jtPop(sp, pcm, &now));
    voiceSpeakerPush(sp, 1, 0, frames[1].data, frames[1].len);
    voiceSpeakerPush(sp, 2, 0, frames[2].data, frames[2].len);
    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(jtPop(sp, pcm, &now), "no audio for seq %d", i);
    }

    /* --- a gap: 3 never arrives, so its slot is concealed once overdue --- */
    voiceSpeakerPush(sp, 4, 0, frames[4].data, frames[4].len);
    voiceSpeakerPush(sp, 5, 0, frames[5].data, frames[5].len);
    UT_ASSERT(jtPopOverdue(sp, pcm, &now));   /* 3, concealed */
    UT_ASSERT(jtPop(sp, pcm, &now));          /* 4            */
    UT_ASSERT(jtPop(sp, pcm, &now));          /* 5            */

    /* --- out of order: 7 arrives before 6, they play 6 then 7 --- */
    voiceSpeakerPush(sp, 7, 0, frames[7].data, frames[7].len);
    voiceSpeakerPush(sp, 6, 0, frames[6].data, frames[6].len);
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 6 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 7 */
    /* Both are gone; the buffer is empty again. */
    UT_ASSERT(jtPopOverdue(sp, pcm, &now));   /* 8, concealed */

    /* --- a frame already played is dropped, not replayed --- */
    voiceSpeakerPush(sp, 6, 0, frames[6].data, frames[6].len);
    voiceSpeakerPush(sp, 7, 0, frames[7].data, frames[7].len);
    /* If either had been buffered, the pops below would run out early. */

    /* --- five concealed frames with nothing arriving stops playback --- */
    for (i = 0; i < VOICE_JITTER_MAX_PLC - 1; i++) {
        UT_ASSERT_MSG(jtPopOverdue(sp, pcm, &now), "stopped after %d concealed",
                      i);
    }
    /* The run began with the concealed frame after seq 7 above. */
    UT_ASSERT(!jtPop(sp, pcm, &now));
    UT_ASSERT(!jtPop(sp, pcm, &now));

    voiceSpeakerDestroy(sp);

    /* --- sequence numbers wrap at 256 --- */
    sp = voiceSpeakerCreate();
    UT_ASSERT(sp != NULL);
    voiceSpeakerPush(sp, 254, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 255, 0, frames[1].data, frames[1].len);
    voiceSpeakerPush(sp, 0, 0, frames[2].data, frames[2].len);
    voiceSpeakerPush(sp, 1, 0, frames[3].data, frames[3].len);
    /* All four play: the wrap must not read as "0 comes before 254", which
     * would drop the last two as late arrivals and conceal their slots. */
    for (i = 0; i < 4; i++) {
        UT_ASSERT_MSG(jtPop(sp, pcm, &now), "no audio across wrap at %d", i);
    }
    voiceSpeakerDestroy(sp);

    /* --- a new utterance at an unrelated sequence number --- */
    sp = voiceSpeakerCreate();
    UT_ASSERT(sp != NULL);
    voiceSpeakerPush(sp, 10, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 11, 0, frames[1].data, frames[1].len);
    voiceSpeakerPush(sp, 12, 0, frames[2].data, frames[2].len);
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 10 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 11 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 12 */
    /* The talker stops. Concealment covers 13..17, then playback ends. */
    for (i = 0; i < VOICE_JITTER_MAX_PLC; i++) {
        UT_ASSERT(jtPopOverdue(sp, pcm, &now));
    }
    UT_ASSERT(!jtPop(sp, pcm, &now));

    /* They start talking again much later. Playback has to resume from the
     * new sequence number rather than from wherever the cursor was left. */
    voiceSpeakerPush(sp, 100, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 101, 0, frames[1].data, frames[1].len);
    voiceSpeakerPush(sp, 102, 0, frames[2].data, frames[2].len);
    for (i = 0; i < 3; i++) {
        UT_ASSERT_MSG(jtPop(sp, pcm, &now), "no audio after re-prime %d", i);
    }
    /* Exactly three frames were there to play; the fourth is concealed. */
    UT_ASSERT(jtPopOverdue(sp, pcm, &now));

    voiceSpeakerDestroy(sp);

    /* --- the end-of-utterance flag ends playback on the frame it rides --- */
    sp = voiceSpeakerCreate();
    UT_ASSERT(sp != NULL);
    voiceSpeakerPush(sp, 20, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 21, VOICE_FLAG_END_OF_UTTERANCE, frames[1].data,
                     frames[1].len);
    voiceSpeakerPush(sp, 22, 0, frames[2].data, frames[2].len);
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 20 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 21, ends the utterance */
    UT_ASSERT(!jtPop(sp, pcm, &now));  /* 22 was dropped with it */

    voiceSpeakerDestroy(sp);

    /* Destroying nothing is a no-op, and a NULL speaker takes no frames. */
    voiceSpeakerDestroy(NULL);
    voiceSpeakerPush(NULL, 0, 0, frames[0].data, frames[0].len);
    UT_ASSERT(!voiceSpeakerPop(NULL, pcm, now));

    return 0;
}

/* A payload the decoder refuses: one byte whose table-of-contents asks for
 * frame-count code 3, which needs a second byte to say how many frames
 * follow.  Opus rejects the packet outright rather than decoding part of it,
 * so a frame carrying this arrives intact and is still unusable — corruption
 * that survived the transport's own checks. */
static const uint8_t s_undecodable[] = { 0x03 };

int run_voice_jitter_undecodable_run(void) {
    EncodedFrame frames[TEST_FRAMES];
    VoiceSpeaker *sp;
    VoiceSpeakerStats st;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    uint32_t now = 0;
    int i;

    UT_ASSERT(encodeFrames(frames, TEST_FRAMES));

    /* --- a run of frames that arrive but do not decode ends playback --- */
    sp = voiceSpeakerCreate();
    UT_ASSERT(sp != NULL);

    voiceSpeakerPush(sp, 0, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 1, 0, frames[1].data, frames[1].len);
    voiceSpeakerPush(sp, 2, 0, frames[2].data, frames[2].len);
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 0 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 1 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 2 */
    voiceSpeakerGetStats(sp, &st);
    UT_ASSERT_MSG(st.played == 3, "played %u before the run, expected 3",
                  st.played);

    /* Nothing is missing now — every slot in the sequence arrives on time —
     * but none of them decodes, so each one is concealed instead. The
     * playout deadline is not involved: these frames are all present, and a
     * present frame is spent on the pop that finds it. */
    for (i = 0; i < VOICE_JITTER_MAX_PLC; i++) {
        voiceSpeakerPush(sp, (uint8_t)(3 + i), 0, s_undecodable,
                         (int)sizeof(s_undecodable));
    }
    for (i = 0; i < VOICE_JITTER_MAX_PLC; i++) {
        UT_ASSERT_MSG(jtPop(sp, pcm, &now), "stopped after %d undecodable", i);
    }

    /* Each was concealed and consumed: none decoded, and none was offered a
     * second time. */
    voiceSpeakerGetStats(sp, &st);
    UT_ASSERT_MSG(st.played == 3, "played %u after the run, expected 3",
                  st.played);
    UT_ASSERT_MSG(st.concealed == (uint32_t)VOICE_JITTER_MAX_PLC,
                  "concealed %u after the run, expected %d", st.concealed,
                  VOICE_JITTER_MAX_PLC);

    /* The run has reached the concealment limit, so playback stops here
     * rather than concealing for as long as the frames keep coming. */
    UT_ASSERT(!jtPop(sp, pcm, &now));
    UT_ASSERT(!jtPop(sp, pcm, &now));

    /* Frames that do decode start it again from where they say. */
    voiceSpeakerPush(sp, 40, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 41, 0, frames[1].data, frames[1].len);
    voiceSpeakerPush(sp, 42, 0, frames[2].data, frames[2].len);
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 40 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 41 */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 42 */
    voiceSpeakerGetStats(sp, &st);
    UT_ASSERT_MSG(st.played == 6, "played %u after re-priming, expected 6",
                  st.played);

    voiceSpeakerDestroy(sp);

    /* --- end of utterance is honoured on a frame that does not decode --- */
    sp = voiceSpeakerCreate();
    UT_ASSERT(sp != NULL);
    voiceSpeakerPush(sp, 50, 0, frames[0].data, frames[0].len);
    voiceSpeakerPush(sp, 51, VOICE_FLAG_END_OF_UTTERANCE, s_undecodable,
                     (int)sizeof(s_undecodable));
    voiceSpeakerPush(sp, 52, 0, frames[2].data, frames[2].len);
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 50                        */
    UT_ASSERT(jtPop(sp, pcm, &now));   /* 51, concealed, and it ends
                                        * the utterance all the same */
    UT_ASSERT(!jtPop(sp, pcm, &now));  /* 52 was dropped with it     */

    voiceSpeakerGetStats(sp, &st);
    UT_ASSERT_MSG(st.played == 1 && st.concealed == 1,
                  "played %u concealed %u, expected 1 and 1", st.played,
                  st.concealed);

    voiceSpeakerDestroy(sp);

    return 0;
}
