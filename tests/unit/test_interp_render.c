/*
 * Pure tests for the render-time interpolation pieces (interpolation.{c,h}):
 * the snapshot-arrival pipeline's idempotency seq-guard, the render-clock
 * entity interpolation (interpGetRenderPosition), and the adaptive
 * display-delay controller (interpRenderControl).  None of these touch a
 * socket or SDL — they run on synthetic deterministic inputs.
 *
 * Pipeline recap: interpUpdate holds prev/curr/pending and stamps each one's
 * wall-clock arrival.  A real prev→curr interval (so a render-time t) only
 * exists once a third snapshot has promoted pending into curr.
 */

#include <stdint.h>
#include <string.h>

#include "interpolation.h"
#include "test_harness.h"

/* Build a minimal alive snapshot at a world position. */
static InterpSnapshot mkSnap(WORLD x, WORLD y) {
  InterpSnapshot s;
  memset(&s, 0, sizeof(s));
  s.worldX = x;
  s.worldY = y;
  s.angle = 0;
  s.speed = 0;
  s.onBoat = FALSE;
  s.alive = TRUE;
  return s;
}

/* Run the controller for n frames at a fixed frame gap and jitter. */
static InterpRenderDecision runFrames(InterpRenderCtl *ctl, int n,
                                      float jitterMs, uint32_t dtMs,
                                      uint32_t *nowMs) {
  InterpRenderDecision d;
  int i;
  memset(&d, 0, sizeof(d));
  for (i = 0; i < n; i++) {
    *nowMs += dtMs;
    d = interpRenderControl(ctl, jitterMs, *nowMs);
  }
  return d;
}

int run_interp_render(void) {
  const BYTE PN = 1; /* remote player (localPlayer is 0) */

  /* ---- (0) interpUpdate idempotency on serverTick ---- */
  {
    InterpContext ctx;
    InterpSnapshot s1 = mkSnap(1000, 2000);
    InterpSnapshot s2 = mkSnap(1100, 2100);
    InterpSnapshot s3 = mkSnap(1200, 2200);
    InterpSnapshot dup = mkSnap(9999, 9999); /* same tick as s3, different pos */
    const InterpPlayer *p;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &s1, 100, 1000);
    interpUpdate(&ctx, PN, &s2, 102, 1020);
    interpUpdate(&ctx, PN, &s3, 104, 1040);

    p = &ctx.players[PN];
    /* After three snapshots: prev=s1@1000, curr=s2@1020, pending=s3@1040. */
    UT_ASSERT_MSG(p->snapshotTick == 104, "snapshotTick %u", p->snapshotTick);
    UT_ASSERT(p->prevArrivalMs == 1000);
    UT_ASSERT(p->currArrivalMs == 1020);
    UT_ASSERT(p->pendingArrivalMs == 1040);
    UT_ASSERT(p->curr.worldX == 1100);    /* s2 */
    UT_ASSERT(p->pending.worldX == 1200); /* s3 */

    /* Re-deliver the same serverTick (104) with a different position and a
     * later arrival — must be ignored: no pipeline shift, no restamp. */
    interpUpdate(&ctx, PN, &dup, 104, 1060);
    UT_ASSERT_MSG(p->snapshotTick == 104, "tick after dup %u", p->snapshotTick);
    UT_ASSERT(p->prevArrivalMs == 1000);
    UT_ASSERT(p->currArrivalMs == 1020);
    UT_ASSERT(p->pendingArrivalMs == 1040);
    UT_ASSERT(p->curr.worldX == 1100);
    UT_ASSERT(p->pending.worldX == 1200);

    /* An older serverTick (reorder) is likewise ignored. */
    interpUpdate(&ctx, PN, &dup, 102, 1080);
    UT_ASSERT(p->snapshotTick == 104);
    UT_ASSERT(p->pending.worldX == 1200);

    /* A strictly newer tick advances the pipeline normally. */
    InterpSnapshot s4 = mkSnap(1300, 2300);
    interpUpdate(&ctx, PN, &s4, 106, 1060);
    UT_ASSERT(p->snapshotTick == 106);
    UT_ASSERT(p->curr.worldX == 1200);    /* s3 promoted */
    UT_ASSERT(p->pending.worldX == 1300);
    UT_ASSERT(p->currArrivalMs == 1040);
    UT_ASSERT(p->prevArrivalMs == 1020);
  }

  /* ---- (1) interpGetRenderPosition bracketing + mapping ----
   * Distinct worldX per snapshot so the lerp is observable.  Nominal
   * snapshot = 20ms, so the depth-1 target (now-20) lands in the
   * curr→pending segment, ~1 snapshot behind the newest arrival. */
  {
    InterpContext ctx;
    InterpSnapshot s1 = mkSnap(1000, 0);
    InterpSnapshot s2 = mkSnap(2000, 0);
    InterpSnapshot s3 = mkSnap(3000, 0);
    WORLD x, y;
    TURNTYPE a;
    bool boat;
    interpCreate(&ctx, 0);

    /* One snapshot: no forward buffer → smooth path bows out. */
    interpUpdate(&ctx, PN, &s1, 100, 1000);
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1000, 0.0f, &x, &y, &a, &boat)
              == FALSE);

    /* prev=s1@1000, curr=s2@2000-pos@1020, pending=s3@1040. */
    interpUpdate(&ctx, PN, &s2, 102, 1020);
    interpUpdate(&ctx, PN, &s3, 104, 1040);

    /* Depth 1 (extraDelay 0): target = now-20 sweeps curr(2000)→pending(3000). */
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1040, 0.0f, &x, &y, &a, &boat));
    UT_ASSERT_MSG(x == 2000, "curr at segment start %u", (unsigned)x);
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1050, 0.0f, &x, &y, &a, &boat));
    UT_ASSERT_MSG(x == 2500, "mid curr->pending %u", (unsigned)x);
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1060, 0.0f, &x, &y, &a, &boat));
    UT_ASSERT_MSG(x == 3000, "clamp pending at newest %u", (unsigned)x);
    /* Far future with no newer snapshot: clamp to newest (no extrapolation,
     * no snap-back). */
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 5000, 0.0f, &x, &y, &a, &boat));
    UT_ASSERT_MSG(x == 3000, "clamp pending in future %u", (unsigned)x);

    /* Depth 2 (extraDelay 20): target = now-40 drops into prev(1000)→curr(2000). */
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1050, 20.0f, &x, &y, &a, &boat));
    UT_ASSERT_MSG(x == 1500, "mid prev->curr at depth 2 %u", (unsigned)x);
  }

  /* ---- (1b) dead player and stale interval bow out to the discrete path ---- */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnap(10, 0);
    InterpSnapshot a2 = mkSnap(20, 0);
    InterpSnapshot dead = mkSnap(30, 0);
    WORLD x, y;
    TURNTYPE a;
    bool boat;

    /* Dead newest sample → FALSE (caller runs the dead-player branch). */
    interpCreate(&ctx, 0);
    dead.alive = FALSE;
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &dead, 104, 1040);
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1050, 0.0f, &x, &y, &a, &boat)
              == FALSE);

    /* Stale interval (arrivals far past the cap) → FALSE. */
    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1300);
    interpUpdate(&ctx, PN, &a2, 104, 1600); /* interval 300ms */
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1600, 0.0f, &x, &y, &a, &boat)
              == FALSE);
  }

  /* ---- (1c) a long stall (missedTicks past the cap) bows out to discrete ---- */
  {
    InterpContext ctx;
    InterpSnapshot s1 = mkSnap(1000, 0);
    InterpSnapshot s2 = mkSnap(2000, 0);
    InterpSnapshot s3 = mkSnap(3000, 0);
    WORLD x, y;
    TURNTYPE a;
    bool boat;
    int i;
    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &s1, 100, 1000);
    interpUpdate(&ctx, PN, &s2, 102, 1020);
    interpUpdate(&ctx, PN, &s3, 104, 1040);
    /* Smooth path live at the boundary (<= INTERP_MAX_MISSED_TICKS). */
    for (i = 0; i < 3; i++) interpMarkMissing(&ctx, PN);
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1050, 0.0f, &x, &y, &a, &boat));
    /* One past the cap → bow out (caller freezes). */
    interpMarkMissing(&ctx, PN);
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1050, 0.0f, &x, &y, &a, &boat)
              == FALSE);
  }

  /* ---- (2) interpRenderControl: first frame is discrete ---- */
  {
    InterpRenderCtl ctl;
    InterpRenderDecision d;
    memset(&ctl, 0, sizeof(ctl));
    d = interpRenderControl(&ctl, 80.0f, 1000);
    UT_ASSERT(d.discrete == TRUE);          /* lastRenderMs was 0 */
    UT_ASSERT(d.extraDelayMs == 0.0f);      /* depth untouched on a stressed frame */
  }

  /* ---- (3) Clean link (jitter under the floor) holds depth 1 ---- */
  {
    InterpRenderCtl ctl;
    InterpRenderDecision d;
    uint32_t now = 5000;
    memset(&ctl, 0, sizeof(ctl));
    /* jitter 25ms < 30ms floor → target 0 → extra delay stays 0. */
    d = runFrames(&ctl, 60, 25.0f, 16, &now);
    UT_ASSERT(d.discrete == FALSE);
    UT_ASSERT_MSG(d.extraDelayMs == 0.0f, "clean extra delay %f", d.extraDelayMs);
  }

  /* ---- (4) High jitter grows slowly, bounded to one extra snapshot ---- */
  {
    InterpRenderCtl ctl;
    InterpRenderDecision d;
    uint32_t now = 5000;
    float afterOne;
    memset(&ctl, 0, sizeof(ctl));

    /* Prime the controller (the very first call is always discrete because
     * lastRenderMs is 0, so it can't grow). */
    now += 16;
    interpRenderControl(&ctl, 80.0f, now);

    /* One more 16ms frame at jitter 80 (target = 80-30 = 50, capped to 20):
     * grow is rate-limited to 20ms/s, so a single frame moves well under 1ms. */
    now += 16;
    d = interpRenderControl(&ctl, 80.0f, now);
    afterOne = d.extraDelayMs;
    UT_ASSERT_MSG(afterOne > 0.0f && afterOne < 1.0f,
                  "grow after one frame %f", afterOne);

    /* Many frames later it settles just shy of the one-snapshot cap (the
     * hysteresis deadband stops it within INTERP_DEPTH_HYST_MS of target) and
     * never exceeds it. */
    d = runFrames(&ctl, 400, 80.0f, 16, &now);
    UT_ASSERT_MSG(d.extraDelayMs <= (float)INTERP_MAX_EXTRA_DELAY_MS,
                  "exceeded one-snapshot cap %f", d.extraDelayMs);
    UT_ASSERT_MSG(d.extraDelayMs >=
                      (float)INTERP_MAX_EXTRA_DELAY_MS - INTERP_DEPTH_HYST_MS,
                  "did not approach cap %f", d.extraDelayMs);
  }

  /* ---- (5) Shrink toward today's behaviour is faster than grow ---- */
  {
    InterpRenderCtl grow, shrink;
    InterpRenderDecision dg, ds;
    uint32_t now = 5000;
    float growDelta, shrinkDelta;

    /* Grow one 16ms frame from 0 (prime first so the measured frame grows). */
    memset(&grow, 0, sizeof(grow));
    now += 16;
    interpRenderControl(&grow, 80.0f, now);   /* prime */
    now += 16;
    dg = interpRenderControl(&grow, 80.0f, now);
    growDelta = dg.extraDelayMs - 0.0f;

    /* Pre-load a controller at the cap, then shrink one 16ms frame at jitter 0. */
    memset(&shrink, 0, sizeof(shrink));
    shrink.currentDelayMs = (float)INTERP_MAX_EXTRA_DELAY_MS;
    shrink.lastRenderMs = now;          /* so the next frame is not "first" */
    ds = interpRenderControl(&shrink, 0.0f, now + 16);
    shrinkDelta = (float)INTERP_MAX_EXTRA_DELAY_MS - ds.extraDelayMs;

    UT_ASSERT_MSG(shrinkDelta > growDelta,
                  "shrinkDelta %f not > growDelta %f", shrinkDelta, growDelta);
  }

  /* ---- (6) A frame-time spike degrades to discrete and freezes depth ---- */
  {
    InterpRenderCtl ctl;
    InterpRenderDecision d;
    uint32_t now = 5000;
    float held;
    memset(&ctl, 0, sizeof(ctl));
    /* Build up some depth first. */
    d = runFrames(&ctl, 100, 80.0f, 16, &now);
    held = d.extraDelayMs;
    UT_ASSERT(held > 0.0f);
    /* A long gap (> stress threshold) → discrete this frame, depth unchanged. */
    now += (uint32_t)INTERP_FRAME_STRESS_MS + 20;
    d = interpRenderControl(&ctl, 80.0f, now);
    UT_ASSERT(d.discrete == TRUE);
    UT_ASSERT_MSG(d.extraDelayMs == held, "depth moved on spike %f vs %f",
                  d.extraDelayMs, held);
  }

  return 0;
}
