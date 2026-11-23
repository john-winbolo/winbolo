/*
 * Regression: a remote tank whose snapshots stop for a while (it left our
 * view, or its slot went to a new player) must not flash at its old position
 * when the snapshots come back (interpolation.c, interpUpdate).
 *
 * The pipeline holds prev/curr/pending.  Before the fix the first snapshot
 * after the gap was shifted in as pending behind the pre-gap curr, the render
 * clock bowed out over the long curr->pending interval, and the discrete
 * fallback drew curr: where the tank was before the gap.  The first snapshot
 * after a freeze-length gap must instead restart the pipeline, so the tank is
 * drawn at its new position at once, and a short gap must keep the pipeline.
 */

#include <stdint.h>
#include <string.h>

#include "interpolation.h"
#include "test_harness.h"

static InterpSnapshot mkSnapAt(WORLD x, WORLD y) {
  InterpSnapshot s;
  memset(&s, 0, sizeof(s));
  s.worldX = x;
  s.worldY = y;
  s.alive = TRUE;
  return s;
}

/* The lookup clientSnapshotRenderInterp makes: the render clock first, then
 * the discrete apply at t = 1.0. */
static bool drawPosition(const InterpContext *ctx, BYTE pn, uint32_t nowMs,
                         WORLD *x, WORLD *y) {
  TURNTYPE ang;
  bool boat;
  if (interpGetRenderPosition(ctx, pn, nowMs, 0.0f, x, y, &ang, &boat)) {
    return TRUE;
  }
  return interpGetPosition(ctx, pn, 1.0f, x, y, &ang, &boat);
}

int run_interp_no_stale_flash_after_gap(void) {
  const BYTE PN = 1; /* remote player (localPlayer is 0) */

  /* ---- long gap: first snapshot back is drawn where it says ----
   * Three in-view snapshots, then a second of hidden stubs, then the tank
   * comes back far away. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 1000);
    InterpSnapshot a2 = mkSnapAt(1010, 1000);
    InterpSnapshot a3 = mkSnapAt(1020, 1000);
    InterpSnapshot back1 = mkSnapAt(9000, 9000);
    InterpSnapshot back2 = mkSnapAt(9010, 9000);
    uint32_t tick = 106;
    uint32_t nowMs = 1060;
    WORLD x, y;
    int i;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &a3, 104, 1040);
    for (i = 0; i < 50; i++) {
      interpMarkMissing(&ctx, PN);
      tick += 2;
      nowMs += 20;
    }
    UT_ASSERT_MSG(!drawPosition(&ctx, PN, nowMs, &x, &y),
                  "frozen while out of view");

    interpUpdate(&ctx, PN, &back1, tick, nowMs);
    for (i = 1; i < 20; i += 9) {
      UT_ASSERT(drawPosition(&ctx, PN, nowMs + (uint32_t)i, &x, &y));
      UT_ASSERT_MSG(x == 9000 && y == 9000,
                    "first snapshot back drawn at (%u, %u), not the old spot",
                    (unsigned)x, (unsigned)y);
    }

    /* The next snapshot tweens on from the new position. */
    interpUpdate(&ctx, PN, &back2, tick + 2, nowMs + 20);
    UT_ASSERT(drawPosition(&ctx, PN, nowMs + 30, &x, &y));
    UT_ASSERT_MSG(x >= 9000 && x <= 9010 && y == 9000,
                  "second snapshot back near the new position (%u, %u)",
                  (unsigned)x, (unsigned)y);
  }

  /* ---- short gap: the pipeline is kept and still tweens ----
   * A single missed snapshot does not freeze the player, so the next one
   * shifts in as usual and the render clock interpolates curr->pending. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 0);
    InterpSnapshot a2 = mkSnapAt(2000, 0);
    InterpSnapshot a3 = mkSnapAt(3000, 0);
    WORLD x, y;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpMarkMissing(&ctx, PN);
    interpUpdate(&ctx, PN, &a3, 106, 1060);

    UT_ASSERT(ctx.players[PN].hasPending);
    UT_ASSERT(drawPosition(&ctx, PN, 1060, &x, &y));
    UT_ASSERT_MSG(x == 2500, "short gap still tweens curr->pending %u",
                  (unsigned)x);
  }

  return 0;
}
