/*
 * Regression: a remote tank that respawns must not flash at its death spot
 * for one frame (interpolation.c, interpGetPosition and the render-clock path
 * interpGetRenderPosition).
 *
 * The snapshot pipeline holds prev/curr/pending.  When a player dies and
 * respawns, the pipeline reaches a state where prev is the dead sample (at
 * the death position) and curr is the alive respawn sample (at the respawn
 * position, a teleport).  Tweening prev->curr would render the tank at/near
 * the death spot on the first frame.  interpGetPosition must instead snap to
 * curr across a respawn (prev.alive == FALSE), and a normal alive->alive
 * pair must still interpolate.
 */

#include <stdint.h>
#include <string.h>

#include "interpolation.h"
#include "test_harness.h"

/* Snapshot at a world position with an explicit alive flag, plus a distinct
 * angle / onBoat so the test can confirm those outputs follow curr (not the
 * stale prev) across the teleport. */
static InterpSnapshot mkSnapAt(WORLD x, WORLD y, TURNTYPE angle, bool onBoat,
                               bool alive) {
  InterpSnapshot s;
  memset(&s, 0, sizeof(s));
  s.worldX = x;
  s.worldY = y;
  s.angle = angle;
  s.speed = 0;
  s.onBoat = onBoat;
  s.alive = alive;
  return s;
}

int run_interp_respawn_no_death_flash(void) {
  const BYTE PN = 1; /* remote player (localPlayer is 0) */

  /* ---- respawn snaps to curr, not the stale death position ----
   * Drive alive -> alive -> dead -> respawn(alive) -> alive with strictly
   * increasing ticks.  After five updates the pipeline holds
   * prev = the dead sample (death position) and curr = the respawn sample
   * (respawn position), the exact state that flashed. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 1000, 0, FALSE, TRUE);
    InterpSnapshot a2 = mkSnapAt(1100, 1000, 0, FALSE, TRUE);
    InterpSnapshot dead = mkSnapAt(1200, 1000, 64, FALSE, FALSE); /* death spot */
    InterpSnapshot respawn = mkSnapAt(5000, 5000, 32, TRUE, TRUE); /* teleport */
    InterpSnapshot a5 = mkSnapAt(5010, 5000, 32, TRUE, TRUE);
    const InterpPlayer *p;
    WORLD x, y;
    TURNTYPE ang;
    bool boat;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &dead, 104, 1040);
    interpUpdate(&ctx, PN, &respawn, 106, 1060);
    interpUpdate(&ctx, PN, &a5, 108, 1080);

    p = &ctx.players[PN];
    UT_ASSERT_MSG(p->prev.alive == FALSE, "prev should be the dead sample");
    UT_ASSERT_MSG(p->curr.alive == TRUE, "curr should be the respawn sample");
    UT_ASSERT_MSG(p->curr.worldX == 5000, "curr at respawn pos %u",
                  (unsigned)p->curr.worldX);

    /* At t = 0.0 a prev->curr tween would return prev exactly (the death
     * spot).  The fix must output curr's respawn position instead — and the
     * angle / onBoat must follow curr too, not the stale prev. */
    UT_ASSERT(interpGetPosition(&ctx, PN, 0.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 5000, "respawn x, not death x %u", (unsigned)x);
    UT_ASSERT_MSG(y == 5000, "respawn y, not death y %u", (unsigned)y);
    UT_ASSERT_MSG(ang == 32, "angle follows curr %u", (unsigned)ang);
    UT_ASSERT_MSG(boat == TRUE, "onBoat follows curr");
  }

  /* ---- positive control: a live alive->alive pair still interpolates ----
   * After three alive snapshots prev=a1@1000, curr=a2@2000; the fix must not
   * have disabled the normal tween. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 0, 0, FALSE, TRUE);
    InterpSnapshot a2 = mkSnapAt(2000, 0, 0, FALSE, TRUE);
    InterpSnapshot a3 = mkSnapAt(3000, 0, 0, FALSE, TRUE);
    WORLD x, y;
    TURNTYPE ang;
    bool boat;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &a3, 104, 1040);

    UT_ASSERT(interpGetPosition(&ctx, PN, 0.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 1000, "t=0 returns prev %u", (unsigned)x);
    UT_ASSERT(interpGetPosition(&ctx, PN, 0.5f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 1500, "t=0.5 blends prev->curr %u", (unsigned)x);
  }

  /* ---- render clock: curr->pending across a respawn snaps to pending ----
   * Four updates leave curr = the dead sample and pending = the respawn
   * sample.  With nominal snapshot 20ms and extraDelay 0, renderNow 1060 maps
   * to target 1040 = currArrivalMs (fromCurr 0): the unguarded code lerps from
   * the dead curr at t=0 and returns the death position.  The guard must
   * output pending (the respawn) instead, with angle / onBoat following it. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 1000, 0, FALSE, TRUE);
    InterpSnapshot a2 = mkSnapAt(1100, 1000, 0, FALSE, TRUE);
    InterpSnapshot dead = mkSnapAt(1200, 1000, 64, FALSE, FALSE); /* death spot */
    InterpSnapshot respawn = mkSnapAt(5000, 5000, 32, TRUE, TRUE); /* teleport */
    const InterpPlayer *p;
    WORLD x, y;
    TURNTYPE ang;
    bool boat;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &dead, 104, 1040);
    interpUpdate(&ctx, PN, &respawn, 106, 1060);

    p = &ctx.players[PN];
    UT_ASSERT_MSG(p->curr.alive == FALSE, "curr should be the dead sample");
    UT_ASSERT_MSG(p->pending.alive == TRUE, "pending should be the respawn sample");

    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1060, 0.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 5000, "respawn x, not death x %u", (unsigned)x);
    UT_ASSERT_MSG(y == 5000, "respawn y, not death y %u", (unsigned)y);
    UT_ASSERT_MSG(ang == 32, "angle follows pending %u", (unsigned)ang);
    UT_ASSERT_MSG(boat == TRUE, "onBoat follows pending");
  }

  /* ---- render clock: prev->curr across a respawn snaps to curr (depth 2) ----
   * Five updates leave prev = the dead sample and curr = the respawn sample.
   * With extraDelay 20 the target drops a full snapshot into the prev->curr
   * segment.  renderNow 1080 maps to target 1040 = prevArrivalMs (fromPrev 0),
   * the clamp-to-prev sub-case the unguarded code returns as the death spot;
   * renderNow 1090 maps to target 1050, a mid-segment lerp out of the death
   * spot.  Both must output curr (the respawn). */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 1000, 0, FALSE, TRUE);
    InterpSnapshot a2 = mkSnapAt(1100, 1000, 0, FALSE, TRUE);
    InterpSnapshot dead = mkSnapAt(1200, 1000, 64, FALSE, FALSE); /* death spot */
    InterpSnapshot respawn = mkSnapAt(5000, 5000, 32, TRUE, TRUE); /* teleport */
    InterpSnapshot a5 = mkSnapAt(5010, 5000, 32, TRUE, TRUE);
    const InterpPlayer *p;
    WORLD x, y;
    TURNTYPE ang;
    bool boat;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &dead, 104, 1040);
    interpUpdate(&ctx, PN, &respawn, 106, 1060);
    interpUpdate(&ctx, PN, &a5, 108, 1080);

    p = &ctx.players[PN];
    UT_ASSERT_MSG(p->prev.alive == FALSE, "prev should be the dead sample");
    UT_ASSERT_MSG(p->curr.alive == TRUE, "curr should be the respawn sample");

    /* Clamp-to-prev sub-case (fromPrev == 0). */
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1080, 20.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 5000, "respawn x at clamp-to-prev, not death x %u",
                  (unsigned)x);
    UT_ASSERT_MSG(y == 5000, "respawn y at clamp-to-prev, not death y %u",
                  (unsigned)y);
    UT_ASSERT_MSG(ang == 32, "angle follows curr %u", (unsigned)ang);
    UT_ASSERT_MSG(boat == TRUE, "onBoat follows curr");

    /* Mid prev->curr segment (fromPrev > 0). */
    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1090, 20.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 5000, "respawn x mid-segment, not death x %u", (unsigned)x);
    UT_ASSERT_MSG(y == 5000, "respawn y mid-segment, not death y %u", (unsigned)y);
  }

  /* ---- render clock: multi-tick death, prev->curr both dead (depth 2) ----
   * A death that spans two snapshots leaves prev AND curr dead, with pending
   * the alive respawn.  Four updates alive -> dead -> dead -> respawn shift the
   * pipeline to prev=dead1, curr=dead2, pending=respawn.  With extraDelay 20 the
   * target drops a full snapshot into the prev->curr segment: renderNow 1070
   * maps to target 1030, between prevArrival 1020 and currArrival 1040
   * (fromCurr -10, fromPrev +10).  The prev-dead-only guard would treat this as
   * an ordinary respawn snap and output curr -- but curr is the second death
   * sample, so that is the death spot.  Snapping to pending (the respawn) is the
   * only correct output. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 1000, 0, FALSE, TRUE);
    InterpSnapshot dead1 = mkSnapAt(1200, 1000, 64, FALSE, FALSE); /* death spot */
    InterpSnapshot dead2 = mkSnapAt(1200, 1000, 64, FALSE, FALSE); /* still dead */
    InterpSnapshot respawn = mkSnapAt(5000, 5000, 32, TRUE, TRUE); /* teleport */
    const InterpPlayer *p;
    WORLD x, y;
    TURNTYPE ang;
    bool boat;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &dead1, 102, 1020);
    interpUpdate(&ctx, PN, &dead2, 104, 1040);
    interpUpdate(&ctx, PN, &respawn, 106, 1060);

    p = &ctx.players[PN];
    UT_ASSERT_MSG(p->prev.alive == FALSE, "prev should be the first dead sample");
    UT_ASSERT_MSG(p->curr.alive == FALSE, "curr should be the second dead sample");
    UT_ASSERT_MSG(p->pending.alive == TRUE, "pending should be the respawn sample");

    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1070, 20.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 5000, "respawn x, not death x %u", (unsigned)x);
    UT_ASSERT_MSG(y == 5000, "respawn y, not death y %u", (unsigned)y);
    UT_ASSERT_MSG(ang == 32, "angle follows respawn %u", (unsigned)ang);
    UT_ASSERT_MSG(boat == TRUE, "onBoat follows respawn");
  }

  /* ---- render clock positive control: a live curr->pending pair still
   * interpolates, proving the guards didn't blanket-disable the tween ----
   * prev=a1@1000, curr=a2@2000-pos, pending=a3.  renderNow 1050 / extraDelay 0
   * maps to target 1030 (fromCurr 10, interval 20, t=0.5): the midpoint. */
  {
    InterpContext ctx;
    InterpSnapshot a1 = mkSnapAt(1000, 0, 0, FALSE, TRUE);
    InterpSnapshot a2 = mkSnapAt(2000, 0, 0, FALSE, TRUE);
    InterpSnapshot a3 = mkSnapAt(3000, 0, 0, FALSE, TRUE);
    WORLD x, y;
    TURNTYPE ang;
    bool boat;

    interpCreate(&ctx, 0);
    interpUpdate(&ctx, PN, &a1, 100, 1000);
    interpUpdate(&ctx, PN, &a2, 102, 1020);
    interpUpdate(&ctx, PN, &a3, 104, 1040);

    UT_ASSERT(interpGetRenderPosition(&ctx, PN, 1050, 0.0f, &x, &y, &ang, &boat));
    UT_ASSERT_MSG(x == 2500, "mid curr->pending still tweens %u", (unsigned)x);
  }

  return 0;
}
