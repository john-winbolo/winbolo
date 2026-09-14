/*
 * The two curves behind the smart ping's world marker
 * (test_ping_marker_anim.c).
 *
 * Both are pure functions of a ping's AGE, with no wall clock and no state,
 * which is what lets a recording replay the marker it showed live. That
 * property is exactly what a test can pin and a screenshot cannot:
 *
 *   pingWorldMarkerAlpha (src/gui/ping_kinds.h) — the blink. The marker is
 *   the only one of the four that covers ground the player has to keep
 *   reading, so it flashes three times and then gives the tile back. The
 *   first flash is held twice as long as the rest and starts solid rather
 *   than easing in, because it plays under the arrival ring and a marker
 *   that arrived after the thing pointing at it would be backwards.
 *
 *   ringAnimAt (src/gui/sdl3/ring_band.c) — the arrival ring, closing onto
 *   the marker square and pulsing once as it goes. Shared with the map
 *   overview's respawn ring.
 *
 * The numbers here are deliberately written out rather than derived from
 * the constants: a test that recomputes the implementation agrees with it
 * whatever either says. Where a constant is named it is to say which knob
 * moved when the assert fires.
 */

#include <math.h>

#include "ping_kinds.h"   /* pingWorldMarkerAlpha and the blink constants */
#include "ring_band.h"    /* ringAnimAt and RingAnim */
#include "test_harness.h"

#define NEARLY(a, b) (fabsf((float)(a) - (float)(b)) < 0.001f)

int run_ping_world_marker_alpha(void) {
    /* The shape this test is written against. If one of these moves the
     * timings below are no longer the ones the player sees, and the asserts
     * that follow say which. */
    UT_ASSERT_MSG(PING_BLINK_PERIOD_MS == 1000 && PING_BLINK_ON_MS == 250 &&
                      PING_BLINK_FIRST_ON_MS == 500 &&
                      PING_BLINK_FADE_MS == 80 && PING_BLINK_COUNT == 3,
                  "the blink constants moved: period=%d on=%d first=%d "
                  "fade=%d count=%d",
                  PING_BLINK_PERIOD_MS, PING_BLINK_ON_MS,
                  PING_BLINK_FIRST_ON_MS, PING_BLINK_FADE_MS,
                  PING_BLINK_COUNT);

    /* The instant it lands, fully on. Not a ramp: the arrival ring is
     * already drawing and the marker has to be under it, not behind it. */
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(0), 1.0f),
                  "age 0 = %f, want 1.0 — the first flash must start solid",
                  pingWorldMarkerAlpha(0));

    /* The first flash is held to 500 ms, twice the rest. Solid through the
     * hold, leaving over the last 80, and gone on the 500th. */
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(100), 1.0f),
                  "100 ms into the first flash = %f, want 1.0",
                  pingWorldMarkerAlpha(100));
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(419), 1.0f),
                  "just before the first flash's fade out = %f, want 1.0",
                  pingWorldMarkerAlpha(419));
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(460), 0.5f),
                  "halfway down the first flash's fade = %f, want 0.5",
                  pingWorldMarkerAlpha(460));
    UT_ASSERT_MSG(pingWorldMarkerAlpha(499) > 0.0f,
                  "the first flash ended before 500 ms");
    UT_ASSERT_MSG(pingWorldMarkerAlpha(500) == 0.0f,
                  "the first flash ran past 500 ms: alpha at 500 = %f",
                  pingWorldMarkerAlpha(500));

    /* The gap is a hard zero, not a dim marker: anything drawn at any
     * opacity still covers the tile. */
    UT_ASSERT_MSG(pingWorldMarkerAlpha(501) == 0.0f, "501 ms is not zero");
    UT_ASSERT_MSG(pingWorldMarkerAlpha(750) == 0.0f, "750 ms is not zero");
    UT_ASSERT_MSG(pingWorldMarkerAlpha(999) == 0.0f, "999 ms is not zero");

    /* The second flash: 250 ms of the next 1000, and this one ramps IN as
     * well as out. The ramp is what separates it from the first — at the
     * top of the period it is still at nothing. */
    UT_ASSERT_MSG(pingWorldMarkerAlpha(1000) == 0.0f,
                  "the second flash did not ramp in: alpha at 1000 = %f, "
                  "want 0 (the first flash is the only solid start)",
                  pingWorldMarkerAlpha(1000));
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(1040), 0.5f),
                  "halfway up the second flash's fade in = %f, want 0.5",
                  pingWorldMarkerAlpha(1040));
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(1120), 1.0f),
                  "the second flash's hold = %f, want 1.0",
                  pingWorldMarkerAlpha(1120));
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(1210), 0.5f),
                  "halfway down the second flash's fade out = %f, want 0.5",
                  pingWorldMarkerAlpha(1210));
    UT_ASSERT_MSG(pingWorldMarkerAlpha(1249) > 0.0f,
                  "the second flash ended before 250 ms of its period");
    UT_ASSERT_MSG(pingWorldMarkerAlpha(1250) == 0.0f,
                  "the second flash ran past 250 ms: alpha at 1250 = %f",
                  pingWorldMarkerAlpha(1250));
    UT_ASSERT_MSG(pingWorldMarkerAlpha(1600) == 0.0f, "1600 ms is not zero");

    /* The third is the last, and behaves as the second. */
    UT_ASSERT_MSG(pingWorldMarkerAlpha(2000) == 0.0f,
                  "the third flash did not ramp in");
    UT_ASSERT_MSG(NEARLY(pingWorldMarkerAlpha(2120), 1.0f),
                  "the third flash's hold = %f, want 1.0",
                  pingWorldMarkerAlpha(2120));
    UT_ASSERT_MSG(pingWorldMarkerAlpha(2250) == 0.0f,
                  "the third flash ran past 250 ms of its period");

    /* Three flashes and no more. The ping itself lives PING_DISPLAY_MS, and
     * the steady markers go on drawing it — but the world marker is off the
     * tile from here to the end, so a fourth flash can never appear. */
    UT_ASSERT_MSG(pingWorldMarkerAlpha(3000) == 0.0f,
                  "a fourth flash at 3000 ms: alpha = %f",
                  pingWorldMarkerAlpha(3000));
    UT_ASSERT_MSG(pingWorldMarkerAlpha(3001) == 0.0f, "3001 ms is not zero");
    UT_ASSERT_MSG(pingWorldMarkerAlpha(3120) == 0.0f,
                  "3120 ms — the hold of what would be a fourth flash — is "
                  "not zero");
    UT_ASSERT_MSG(pingWorldMarkerAlpha(PING_DISPLAY_MS) == 0.0f,
                  "the marker is drawing past the ping's own life");

    /* A negative age is a clock that ran backwards, not a ping about to
     * arrive. Nothing is drawn. */
    UT_ASSERT_MSG(pingWorldMarkerAlpha(-1) == 0.0f, "a negative age drew");

    return 0;
}

int run_ring_anim_at(void) {
    /* Round numbers rather than the ping's own, so the arithmetic is
     * readable: in from 10 to 2 over 200 ms, then out to 3 and back over
     * 100 more. */
    RingAnim anim;
    float radius = -1.0f, alpha = -1.0f;

    anim.startRadius = 10.0f;
    anim.endRadius   = 2.0f;
    anim.pulseRadius = 3.0f;
    anim.closeMs     = 200;
    anim.pulseMs     = 100;

    /* At the start the ring is where it began, and solid. */
    UT_ASSERT_MSG(ringAnimAt(&anim, 0, &radius, &alpha),
                  "the ring was not running at 0");
    UT_ASSERT_MSG(NEARLY(radius, 10.0f),
                  "radius at 0 = %f, want the start radius 10.0", radius);
    UT_ASSERT_MSG(NEARLY(alpha, 1.0f), "alpha at 0 = %f, want 1.0", alpha);

    /* Through the close it is on its way in, still solid the whole way —
     * the fade belongs to the pulse. Eased out, so it is already most of
     * the way there at the halfway point. */
    UT_ASSERT_MSG(ringAnimAt(&anim, 100, &radius, &alpha),
                  "the ring stopped inside the close");
    UT_ASSERT_MSG(radius < 10.0f && radius > 2.0f,
                  "radius halfway through the close = %f, want between the "
                  "two radii", radius);
    UT_ASSERT_MSG(radius < 6.0f,
                  "radius halfway through the close = %f — the close is "
                  "eased out and should be past the midpoint by now", radius);
    UT_ASSERT_MSG(NEARLY(alpha, 1.0f),
                  "alpha inside the close = %f, want 1.0 — nothing fades "
                  "until the pulse", alpha);

    /* The close lands exactly on the end radius, and hands over to the
     * pulse still solid. */
    UT_ASSERT_MSG(ringAnimAt(&anim, 200, &radius, &alpha),
                  "the ring stopped at the end of the close");
    UT_ASSERT_MSG(NEARLY(radius, 2.0f),
                  "radius at closeMs = %f, want the end radius 2.0", radius);
    UT_ASSERT_MSG(NEARLY(alpha, 1.0f),
                  "alpha at closeMs = %f, want 1.0", alpha);

    /* The pulse opens back out and fades as it goes: a straight ramp from
     * 1 to 0 across pulseMs, with the radius out past the end on a half
     * sine — at the top of it, the full pulse radius. */
    UT_ASSERT_MSG(ringAnimAt(&anim, 250, &radius, &alpha),
                  "the ring stopped inside the pulse");
    UT_ASSERT_MSG(NEARLY(radius, 3.0f),
                  "radius at the top of the pulse = %f, want 3.0", radius);
    UT_ASSERT_MSG(NEARLY(alpha, 0.5f),
                  "alpha halfway through the pulse = %f, want 0.5", alpha);
    UT_ASSERT_MSG(ringAnimAt(&anim, 299, &radius, &alpha),
                  "the ring stopped before the pulse ended");
    UT_ASSERT_MSG(alpha > 0.0f && alpha < 0.02f,
                  "alpha at the last millisecond of the pulse = %f, want "
                  "almost nothing", alpha);

    /* And it is over. Neither output is written, which is what lets a
     * caller test the return rather than the alpha. */
    radius = -1.0f;
    alpha = -1.0f;
    UT_ASSERT_MSG(!ringAnimAt(&anim, 300, &radius, &alpha),
                  "the ring was still running at closeMs + pulseMs");
    UT_ASSERT_MSG(radius == -1.0f && alpha == -1.0f,
                  "a finished ring wrote its outputs: radius=%f alpha=%f",
                  radius, alpha);
    UT_ASSERT_MSG(!ringAnimAt(&anim, 100000, NULL, NULL),
                  "the ring came back long after it ended");

    /* No ring is not a running ring. Both outputs may be NULL too — the
     * overview asks for the radius alone. */
    UT_ASSERT_MSG(!ringAnimAt(NULL, 0, &radius, &alpha),
                  "a NULL anim reported a running ring");
    UT_ASSERT_MSG(ringAnimAt(&anim, 50, NULL, NULL),
                  "the ring refused to run with both outputs skipped");

    return 0;
}
