/*
 * Regression: a remote tank that respawns must not flash at its death spot
 * for one frame (interpolation.c, interpGetPosition).
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

  return 0;
}
