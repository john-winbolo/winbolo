/*
 * The level meter's held peak (src/client_frontend/voice_core.c). A single
 * captured frame's RMS is only true for the 20 ms it was measured over, so the
 * drawn value holds the loudest reading and then falls back to what is live.
 * voicePeakUpdate takes the clock as an argument, so this drives it directly
 * and covers the hold, the decay, where the decay stops, a new maximum
 * arriving mid-fall, and readings from outside 0..1 - with no audio device and
 * no real clock anywhere near it.
 *
 * voiceMeterScale, which turns the captured amplitude into the height the same
 * meter draws it at, is covered here too: the two are the two halves of one
 * reading and both are pure arithmetic over a float.  So is voiceFrameRms,
 * which is where the reading they are fed comes from.
 */
#include <math.h>
#include <string.h>

#include "voice_core.h"
#include "test_harness.h"

/* Decayed floats are not exact, so everything is compared with a tolerance. */
#define PEAK_EPS 0.001f

/* Samples in the frames the RMS cases below build.  Shorter than a real
 * VOICE_FRAME_SAMPLES frame on purpose: the count is an argument, so nothing
 * here depends on the frame size the codec happens to run at. */
#define RMS_FRAME 64

static int peakNear(float a, float b) {
    float d = a - b;
    if (d < 0.0f) {
        d = -d;
    }
    return d < PEAK_EPS;
}

/* One reading, with the invariant that has to hold on every one of them
 * checked where it happens rather than at the end: the peak is never under
 * the live level it is drawn over, or it would read as the meter falling
 * through its own bar. */
#define FEED(p, lvl, ms, out)                                               \
    do {                                                                    \
        float feedLevel_ = (lvl);                                           \
        if (feedLevel_ < 0.0f) {                                            \
            feedLevel_ = 0.0f;                                              \
        } else if (feedLevel_ > 1.0f) {                                     \
            feedLevel_ = 1.0f;                                              \
        }                                                                   \
        (out) = voicePeakUpdate(&(p), (lvl), (ms));                         \
        UT_ASSERT((out) >= feedLevel_ - PEAK_EPS);                          \
    } while (0)

int run_voice_peak(void) {
    VoicePeak p;
    float v;
    float decayed;
    float floorLevel;
    float expected;
    float previous;
    int step;
    int16_t frame[RMS_FRAME];

    memset(&p, 0, sizeof(p));

    /* A spike sets the peak.  A zeroed struct has to do this on its first
     * call, with nothing having primed it. */
    FEED(p, 0.8f, 1000, v);
    UT_ASSERT(peakNear(v, 0.8f));

    /* It holds while the clock stands still... */
    FEED(p, 0.05f, 1000, v);
    UT_ASSERT(peakNear(v, 0.8f));

    /* ...and up to the last millisecond inside the hold. */
    FEED(p, 0.05f, 1000 + VOICE_PEAK_HOLD_MS - 1, v);
    UT_ASSERT(peakNear(v, 0.8f));

    /* Past the hold it starts falling, but is still above the live level. */
    FEED(p, 0.05f, 1700, v);
    UT_ASSERT(v < 0.8f - PEAK_EPS);
    UT_ASSERT(v > 0.05f + PEAK_EPS);
    decayed = v;

    /* A louder reading while it is falling replaces it outright... */
    FEED(p, 0.95f, 1750, v);
    UT_ASSERT(v > decayed);
    UT_ASSERT(peakNear(v, 0.95f));

    /* ...and re-arms the hold, so it stands again over a quiet frame. */
    FEED(p, 0.0f, 1750 + VOICE_PEAK_HOLD_MS - 50, v);
    UT_ASSERT(peakNear(v, 0.95f));

    /* Far enough past the hold that a linear fall would overshoot: it lands on
     * the live level instead of dropping through it. */
    FEED(p, 0.1f, 4000, v);
    UT_ASSERT(peakNear(v, 0.1f));

    /* And stays there while the level does, rather than carrying on down. */
    FEED(p, 0.1f, 6000, v);
    UT_ASSERT(peakNear(v, 0.1f));

    /* Over-range readings are clamped before they reach the state: 1.5 is a
     * peak of 1.0, not of 1.5. */
    FEED(p, 1.5f, 6100, v);
    UT_ASSERT(peakNear(v, 1.0f));

    /* A negative one is a level of zero.  It leaves the held peak alone, since
     * this is inside the hold. */
    FEED(p, -0.5f, 6200, v);
    UT_ASSERT(peakNear(v, 1.0f));

    /* Held over from the negative reading, the decay still reaches zero and
     * stops there rather than going under it. */
    FEED(p, -0.5f, 9000, v);
    UT_ASSERT(peakNear(v, 0.0f));
    UT_ASSERT(v >= 0.0f);

    /* Neither out-of-range reading left the struct in a state a later call
     * misbehaves from. */
    FEED(p, 0.5f, 9100, v);
    UT_ASSERT(peakNear(v, 0.5f));
    FEED(p, 0.2f, 9150, v);
    UT_ASSERT(peakNear(v, 0.5f));

    /* No state at all is answered rather than dereferenced. */
    UT_ASSERT(peakNear(voicePeakUpdate(NULL, 0.5f, 9200), 0.0f));

    /* Silence draws empty, and so does anything at or under the floor.  The
     * amplitude the floor sits at is what the dB definition makes it. */
    floorLevel = powf(10.0f, VOICE_METER_FLOOR_DB / 20.0f);
    UT_ASSERT(peakNear(voiceMeterScale(0.0f), 0.0f));
    UT_ASSERT(peakNear(voiceMeterScale(floorLevel), 0.0f));
    UT_ASSERT(peakNear(voiceMeterScale(floorLevel * 0.5f), 0.0f));

    /* Full scale draws full. */
    UT_ASSERT(peakNear(voiceMeterScale(1.0f), 1.0f));

    /* A known mid-point, derived from the floor rather than written out, so
     * moving the floor moves the expectation with it instead of leaving this
     * asserting something that is no longer true. */
    expected = (20.0f * log10f(0.1f) - VOICE_METER_FLOOR_DB) /
               (0.0f - VOICE_METER_FLOOR_DB);
    UT_ASSERT(fabsf(voiceMeterScale(0.1f) - expected) < 0.01f);

    /* The whole point of the conversion: ordinary speech lands well up the
     * bar rather than in the few percent a linear amplitude leaves it in. */
    UT_ASSERT(voiceMeterScale(0.03f) > 0.2f);
    UT_ASSERT(voiceMeterScale(0.15f) > 0.5f);

    /* Monotonic, and inside 0..1 the whole way up - including the two ways of
     * being out of range, which are clamped rather than extrapolated. */
    UT_ASSERT(peakNear(voiceMeterScale(-0.5f), 0.0f));
    UT_ASSERT(peakNear(voiceMeterScale(1.5f), 1.0f));
    previous = voiceMeterScale(-0.5f);
    for (step = 0; step <= 200; step++) {
        float scaled = voiceMeterScale((float)step / 100.0f);
        UT_ASSERT(scaled >= 0.0f);
        UT_ASSERT(scaled <= 1.0f);
        UT_ASSERT(scaled >= previous);
        previous = scaled;
    }

    /* The reading the two above are fed from: one frame's RMS. */
    for (step = 0; step < RMS_FRAME; step++) {
        frame[step] = 0;
    }
    UT_ASSERT(peakNear(voiceFrameRms(frame, RMS_FRAME), 0.0f));

    /* A full-scale square wave is the loudest a frame can be, and every
     * sample is at the extreme, so the mean square is the extreme too.  Not
     * exactly 1: the samples are +32767 against the 32768 the scale is
     * defined on. */
    for (step = 0; step < RMS_FRAME; step++) {
        frame[step] = (step & 1) ? 32767 : -32767;
    }
    UT_ASSERT(voiceFrameRms(frame, RMS_FRAME) > 0.999f);
    UT_ASSERT(voiceFrameRms(frame, RMS_FRAME) <= 1.0f);

    /* Half the amplitude is half the reading - the measurement is linear in
     * amplitude, which is why voiceMeterScale exists to draw it. */
    for (step = 0; step < RMS_FRAME; step++) {
        frame[step] = (step & 1) ? 16384 : -16384;
    }
    UT_ASSERT(fabsf(voiceFrameRms(frame, RMS_FRAME) - 0.5f) < 0.01f);

    /* Full scale negative on every sample, one count further out than the
     * positive extreme, is exactly the value the scale is defined on and so
     * reads exactly full - the top of the range, not over it. */
    for (step = 0; step < RMS_FRAME; step++) {
        frame[step] = -32768;
    }
    UT_ASSERT(peakNear(voiceFrameRms(frame, RMS_FRAME), 1.0f));

    /* No frame, and a frame of nothing, are silence rather than a crash or a
     * division by zero. */
    UT_ASSERT(peakNear(voiceFrameRms(NULL, RMS_FRAME), 0.0f));
    UT_ASSERT(peakNear(voiceFrameRms(frame, 0), 0.0f));
    UT_ASSERT(peakNear(voiceFrameRms(frame, -1), 0.0f));

    return 0;
}
