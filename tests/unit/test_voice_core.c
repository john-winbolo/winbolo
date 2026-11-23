/*
 * Voice codec round-trip (src/client_frontend/voice_core.c). A 440 Hz sine
 * is encoded and decoded frame by frame with no network in between, which
 * pins the three properties the capture path depends on:
 *
 *   - every frame fits the per-tick byte budget. Plain VBR only averages
 *     the target bitrate and individual frames spike well past it, so the
 *     100-byte ceiling here is what catches a missing VBR constraint.
 *   - the decoder returns a full 20 ms frame for every packet.
 *   - the round trip preserves signal level, so a loopback test is audible
 *     at the same loudness it was captured at.
 *
 * The level check skips the first half of the run: Opus has algorithmic
 * lookahead and the encoder needs a few frames to converge, so early frames
 * decode quieter than the input and asserting on them is flaky.
 *
 * Further down: a codec reset has to leave the encoder and decoder exactly
 * as new ones, since each utterance starts with one, and the fade ramp that
 * takes the click out of an utterance's edges has to land on its end gains.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "voice_core.h"
#include "test_harness.h"

#define FRAME_COUNT     50
#define WARMUP_FRAMES   25
#define TONE_HZ         440.0
#define TONE_AMPLITUDE  0.3   /* fraction of full scale */
#define MAX_PACKET_BYTES 100  /* 24 kbps constrained VBR over a 20 ms frame */

/* Sum of squares of a mono S16 block, as a double so the accumulation over
 * many frames cannot overflow. */
static double sumSquares(const int16_t *pcm, int samples) {
    double total = 0.0;
    int i;
    for (i = 0; i < samples; i++) {
        total += (double)pcm[i] * (double)pcm[i];
    }
    return total;
}

int run_voice_core_roundtrip(void) {
    VoiceEncoder *enc;
    VoiceDecoder *dec;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int16_t decodedPcm[VOICE_FRAME_SAMPLES];
    uint8_t packet[VOICE_MAX_PACKET];
    double phase = 0.0;
    const double phaseStep = 2.0 * 3.14159265358979323846 * TONE_HZ /
                             (double)VOICE_SAMPLE_RATE;
    double inSquares = 0.0;
    double outSquares = 0.0;
    double inRms;
    double outRms;
    long scoredSamples = 0;
    int frame;
    int i;

    enc = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    UT_ASSERT(enc != NULL);
    dec = voiceDecoderCreate();
    UT_ASSERT(dec != NULL);

    for (frame = 0; frame < FRAME_COUNT; frame++) {
        int encoded;
        int decoded;

        /* Phase carries across frames so the encoder sees one continuous
         * tone rather than 50 restarts. */
        for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
            pcm[i] = (int16_t)(sin(phase) * TONE_AMPLITUDE * 32767.0);
            phase += phaseStep;
        }

        encoded = voiceEncoderEncode(enc, pcm, packet, (int)sizeof(packet));
        UT_ASSERT_MSG(encoded > 0, "frame %d: encode returned %d", frame,
                      encoded);
        UT_ASSERT_MSG(encoded <= MAX_PACKET_BYTES,
                      "frame %d: %d bytes exceeds the %d byte budget", frame,
                      encoded, MAX_PACKET_BYTES);

        decoded = voiceDecoderDecode(dec, packet, encoded, decodedPcm);
        UT_ASSERT_MSG(decoded == VOICE_FRAME_SAMPLES,
                      "frame %d: decode returned %d", frame, decoded);

        if (frame >= WARMUP_FRAMES) {
            inSquares += sumSquares(pcm, VOICE_FRAME_SAMPLES);
            outSquares += sumSquares(decodedPcm, VOICE_FRAME_SAMPLES);
            scoredSamples += VOICE_FRAME_SAMPLES;
        }
    }

    UT_ASSERT(scoredSamples > 0);
    inRms = sqrt(inSquares / (double)scoredSamples);
    outRms = sqrt(outSquares / (double)scoredSamples);
    UT_ASSERT(inRms > 0.0);
    UT_ASSERT_MSG(outRms <= inRms * 2.0 && outRms >= inRms / 2.0,
                  "level drifted: in %.1f out %.1f", inRms, outRms);

    /* Packet loss concealment still produces a full frame. */
    UT_ASSERT(voiceDecoderDecode(dec, NULL, 0, decodedPcm) ==
              VOICE_FRAME_SAMPLES);

    voiceEncoderDestroy(enc);
    voiceDecoderDestroy(dec);

    /* Destroying nothing is a no-op, so a failed init needs no special case. */
    voiceEncoderDestroy(NULL);
    voiceDecoderDestroy(NULL);

    return 0;
}

/* One frame of the tone, carrying the phase on so consecutive calls make one
 * continuous signal. */
static void toneFrame(int16_t *pcm, double *phase) {
    const double phaseStep = 2.0 * 3.14159265358979323846 * TONE_HZ /
                             (double)VOICE_SAMPLE_RATE;
    int i;

    for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)(sin(*phase) * TONE_AMPLITUDE * 32767.0);
        *phase += phaseStep;
    }
}

#define RESET_LEAD_FRAMES 10

/* A reset leaves each side exactly as a newly created one would be.  Opus is
 * deterministic for identical state, so the comparison is byte for byte and
 * sample for sample rather than within a tolerance: anything the reset
 * missed shows as a difference here. */
int run_voice_core_reset_matches_fresh(void) {
    VoiceEncoder *enc;
    VoiceEncoder *fresh;
    VoiceDecoder *dec;
    VoiceDecoder *freshDec;
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int16_t outReset[VOICE_FRAME_SAMPLES];
    int16_t outFresh[VOICE_FRAME_SAMPLES];
    uint8_t packet[VOICE_MAX_PACKET];
    uint8_t afterReset[VOICE_MAX_PACKET];
    uint8_t fromFresh[VOICE_MAX_PACKET];
    int lenReset;
    int lenFresh;
    double phase = 0.0;
    int frame;
    int i;

    /* --- encoder --- */
    enc = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    UT_ASSERT(enc != NULL);
    fresh = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    UT_ASSERT(fresh != NULL);

    for (frame = 0; frame < RESET_LEAD_FRAMES; frame++) {
        toneFrame(pcm, &phase);
        UT_ASSERT(voiceEncoderEncode(enc, pcm, packet, (int)sizeof(packet)) >
                  0);
    }
    voiceEncoderReset(enc);

    toneFrame(pcm, &phase);
    lenReset = voiceEncoderEncode(enc, pcm, afterReset, (int)sizeof(afterReset));
    lenFresh = voiceEncoderEncode(fresh, pcm, fromFresh, (int)sizeof(fromFresh));
    UT_ASSERT(lenReset > 0 && lenFresh > 0);
    UT_ASSERT_MSG(lenReset == lenFresh,
                  "reset encoder wrote %d bytes, fresh one %d", lenReset,
                  lenFresh);
    UT_ASSERT_MSG(memcmp(afterReset, fromFresh, (size_t)lenReset) == 0,
                  "reset encoder's bytes differ from a fresh encoder's");

    voiceEncoderDestroy(enc);
    voiceEncoderDestroy(fresh);

    /* --- decoder --- */
    enc = voiceEncoderCreate(VOICE_DEFAULT_BITRATE, VOICE_DEFAULT_COMPLEXITY);
    UT_ASSERT(enc != NULL);
    dec = voiceDecoderCreate();
    UT_ASSERT(dec != NULL);
    freshDec = voiceDecoderCreate();
    UT_ASSERT(freshDec != NULL);

    phase = 0.0;
    for (frame = 0; frame < RESET_LEAD_FRAMES; frame++) {
        int len;
        toneFrame(pcm, &phase);
        len = voiceEncoderEncode(enc, pcm, packet, (int)sizeof(packet));
        UT_ASSERT(len > 0);
        UT_ASSERT(voiceDecoderDecode(dec, packet, len, outReset) ==
                  VOICE_FRAME_SAMPLES);
    }
    voiceDecoderReset(dec);

    toneFrame(pcm, &phase);
    lenReset = voiceEncoderEncode(enc, pcm, packet, (int)sizeof(packet));
    UT_ASSERT(lenReset > 0);
    UT_ASSERT(voiceDecoderDecode(dec, packet, lenReset, outReset) ==
              VOICE_FRAME_SAMPLES);
    UT_ASSERT(voiceDecoderDecode(freshDec, packet, lenReset, outFresh) ==
              VOICE_FRAME_SAMPLES);
    for (i = 0; i < VOICE_FRAME_SAMPLES; i++) {
        UT_ASSERT_MSG(outReset[i] == outFresh[i],
                      "sample %d: reset decoder %d, fresh decoder %d", i,
                      outReset[i], outFresh[i]);
    }

    voiceEncoderDestroy(enc);
    voiceDecoderDestroy(dec);
    voiceDecoderDestroy(freshDec);

    /* Resetting nothing is a no-op, like destroying nothing. */
    voiceEncoderReset(NULL);
    voiceDecoderReset(NULL);

    return 0;
}

#define FADE_LEVEL 10000

static void fillConstant(int16_t *pcm, int count) {
    int i;
    for (i = 0; i < count; i++) {
        pcm[i] = FADE_LEVEL;
    }
}

int run_voice_core_fade_apply(void) {
    int16_t pcm[VOICE_FRAME_SAMPLES];
    int i;

    /* --- 0 to 1 over the whole frame: in from silence --- */
    fillConstant(pcm, VOICE_FRAME_SAMPLES);
    voiceFadeApply(pcm, VOICE_FRAME_SAMPLES, 0.0f, 1.0f);
    UT_ASSERT_MSG(pcm[0] == 0, "fade in starts at %d", pcm[0]);
    UT_ASSERT_MSG(pcm[VOICE_FRAME_SAMPLES - 1] == FADE_LEVEL,
                  "fade in ends at %d", pcm[VOICE_FRAME_SAMPLES - 1]);
    for (i = 1; i < VOICE_FRAME_SAMPLES; i++) {
        UT_ASSERT_MSG(pcm[i] >= pcm[i - 1], "fade in falls at sample %d", i);
    }

    /* --- 1 to 0: out to silence --- */
    fillConstant(pcm, VOICE_FRAME_SAMPLES);
    voiceFadeApply(pcm, VOICE_FRAME_SAMPLES, 1.0f, 0.0f);
    UT_ASSERT_MSG(pcm[0] == FADE_LEVEL, "fade out starts at %d", pcm[0]);
    UT_ASSERT_MSG(pcm[VOICE_FRAME_SAMPLES - 1] == 0, "fade out ends at %d",
                  pcm[VOICE_FRAME_SAMPLES - 1]);

    /* --- a ramp shorter than the frame leaves the rest alone --- */
    fillConstant(pcm, VOICE_FRAME_SAMPLES);
    voiceFadeApply(pcm, VOICE_FADE_SAMPLES, 0.0f, 1.0f);
    UT_ASSERT(pcm[0] == 0);
    UT_ASSERT(pcm[VOICE_FADE_SAMPLES - 1] == FADE_LEVEL);
    for (i = VOICE_FADE_SAMPLES; i < VOICE_FRAME_SAMPLES; i++) {
        UT_ASSERT_MSG(pcm[i] == FADE_LEVEL, "sample %d past the ramp is %d",
                      i, pcm[i]);
    }

    /* --- a half-way ramp, the shape the push-to-talk release uses --- */
    fillConstant(pcm, VOICE_FRAME_SAMPLES);
    voiceFadeApply(pcm, VOICE_FRAME_SAMPLES, 1.0f, 0.5f);
    UT_ASSERT(pcm[0] == FADE_LEVEL);
    UT_ASSERT_MSG(pcm[VOICE_FRAME_SAMPLES - 1] == FADE_LEVEL / 2,
                  "half fade ends at %d", pcm[VOICE_FRAME_SAMPLES - 1]);

    /* --- degenerate counts --- */
    fillConstant(pcm, VOICE_FRAME_SAMPLES);
    voiceFadeApply(pcm, 0, 0.0f, 1.0f);
    UT_ASSERT(pcm[0] == FADE_LEVEL);
    voiceFadeApply(pcm, -1, 0.0f, 1.0f);
    UT_ASSERT(pcm[0] == FADE_LEVEL);
    /* One sample takes the gain the ramp arrives at, and only that sample
     * is touched. */
    voiceFadeApply(pcm, 1, 1.0f, 0.0f);
    UT_ASSERT_MSG(pcm[0] == 0, "single sample faded to %d", pcm[0]);
    UT_ASSERT(pcm[1] == FADE_LEVEL);
    voiceFadeApply(NULL, VOICE_FRAME_SAMPLES, 0.0f, 1.0f);

    return 0;
}
