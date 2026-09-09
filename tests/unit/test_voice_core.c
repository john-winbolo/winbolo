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
 */
#include <math.h>
#include <stdint.h>

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
