/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
*Name:          Interpolation
*Filename:      interpolation.c
*Purpose:
*  Per-player interpolation of other tanks' positions
*  between server snapshots for smooth rendering.
*********************************************************/

#include <math.h>
#include "global.h"
#include "interpolation.h"

/* After this many consecutive ticks with no snapshot,
 * stop extrapolating and freeze the player */
#define INTERP_MAX_MISSED_TICKS 3

/* BRadians are 0-256 (stored as float TURNTYPE).
 * Full circle = 256.0 */
#define BRADIANS_FULL_CIRCLE 256.0f
#define BRADIANS_HALF_CIRCLE 128.0f

/*********************************************************
*NAME:          interpAngleLerp
*PURPOSE:
*  Interpolates between two angles in BRadians (0-256),
*  taking the shortest path around the circle.
*
*ARGUMENTS:
*  from - Start angle
*  to   - End angle
*  t    - Interpolation factor [0, 1]
*********************************************************/
static TURNTYPE interpAngleLerp(TURNTYPE from, TURNTYPE to, float t) {
  float diff = to - from;

  /* Wrap to [-128, 128) for shortest path */
  while (diff > BRADIANS_HALF_CIRCLE) {
    diff -= BRADIANS_FULL_CIRCLE;
  }
  while (diff < -BRADIANS_HALF_CIRCLE) {
    diff += BRADIANS_FULL_CIRCLE;
  }

  float result = from + diff * t;

  /* Wrap result to [0, 256) */
  while (result < 0.0f) {
    result += BRADIANS_FULL_CIRCLE;
  }
  while (result >= BRADIANS_FULL_CIRCLE) {
    result -= BRADIANS_FULL_CIRCLE;
  }

  return result;
}

/*********************************************************
*NAME:          interpClampF
*PURPOSE:
*  Clamps a float to [0, 1].
*********************************************************/
static float interpClampF(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

void interpCreate(InterpContext *ctx, BYTE localPlayer) {
  BYTE i;
  for (i = 0; i < MAX_TANKS; i++) {
    ctx->players[i].hasData = FALSE;
    ctx->players[i].hasPrev = FALSE;
    ctx->players[i].hasPending = FALSE;
    ctx->players[i].snapshotTick = 0;
    ctx->players[i].missedTicks = 0;
    memset(&ctx->players[i].prev, 0, sizeof(InterpSnapshot));
    memset(&ctx->players[i].curr, 0, sizeof(InterpSnapshot));
    memset(&ctx->players[i].pending, 0, sizeof(InterpSnapshot));
  }
  ctx->localPlayer = localPlayer;
}

void interpUpdate(InterpContext *ctx, BYTE playerNum,
                  const InterpSnapshot *snap, uint32_t tick, uint32_t nowMs) {
  InterpPlayer *p;
  if (playerNum >= MAX_TANKS || playerNum == ctx->localPlayer) {
    return;
  }

  p = &ctx->players[playerNum];

  if (!p->hasData) {
    /* First snapshot ever: put directly into curr so the player
     * is visible immediately (interpGetPosition needs curr.alive) */
    p->curr = *snap;
    p->hasData = TRUE;
    p->snapshotTick = tick;
    p->currArrivalMs = nowMs;
    p->prevArrivalMs = nowMs;
    p->missedTicks = 0;
    return;
  }

  /* Idempotent on serverTick: a snapshot that is not strictly newer than the
   * last applied one is a re-delivery of an already-applied frame (the same
   * packet drained from both the pump and the render seam, or a reorder).
   * Skipping it keeps the prev→curr→pending pipeline from shifting twice. */
  if (tick <= p->snapshotTick) {
    return;
  }

  if (p->hasPending) {
    /* Shift: curr → prev, pending → curr, new → pending */
    p->prev = p->curr;
    p->prevArrivalMs = p->currArrivalMs;
    p->hasPrev = TRUE;
    p->curr = p->pending;
    p->currArrivalMs = p->pendingArrivalMs;
  } else {
    /* Second snapshot: curr → prev, new goes to pending */
    p->prev = p->curr;
    p->prevArrivalMs = p->currArrivalMs;
    p->hasPrev = TRUE;
  }

  p->pending = *snap;
  p->pendingArrivalMs = nowMs;
  p->hasPending = TRUE;
  p->snapshotTick = tick;
  p->missedTicks = 0;
}

void interpMarkMissing(InterpContext *ctx, BYTE playerNum) {
  if (playerNum >= MAX_TANKS || playerNum == ctx->localPlayer) {
    return;
  }
  ctx->players[playerNum].missedTicks++;
}

bool interpGetPosition(const InterpContext *ctx, BYTE playerNum,
                       float t,
                       WORLD *outX, WORLD *outY,
                       TURNTYPE *outAngle, bool *outOnBoat) {
  const InterpPlayer *p;
  float clamped;

  if (playerNum >= MAX_TANKS || playerNum == ctx->localPlayer) {
    return FALSE;
  }

  p = &ctx->players[playerNum];

  if (!p->hasData) {
    return FALSE;
  }

  if (!p->curr.alive) {
    return FALSE;
  }

  /* If too many missed ticks, freeze at last known position */
  if (p->missedTicks > INTERP_MAX_MISSED_TICKS) {
    return FALSE;
  }

  /* If we only have one snapshot, use it directly */
  if (!p->hasPrev) {
    *outX = p->curr.worldX;
    *outY = p->curr.worldY;
    *outAngle = p->curr.angle;
    *outOnBoat = p->curr.onBoat;
    return TRUE;
  }

  /* Respawn snap: curr is alive (gated above) but prev is a death/dead
   * sample, so prev's position is the death spot and is meaningless for the
   * teleport back to the respawn point. Output curr directly instead of
   * tweening from the stale death position (a one-frame flash at the death
   * spot). Placed before the missedTicks block so a missed-tick respawn can't
   * extrapolate a bogus death->respawn velocity either. */
  if (!p->prev.alive) {
    *outX = p->curr.worldX;
    *outY = p->curr.worldY;
    *outAngle = p->curr.angle;
    *outOnBoat = p->curr.onBoat;
    return TRUE;
  }

  /* If missed 1-3 ticks, extrapolate from current position */
  if (p->missedTicks > 0) {
    /* Brief extrapolation: continue at last known velocity */
    float dx = (float)p->curr.worldX - (float)p->prev.worldX;
    float dy = (float)p->curr.worldY - (float)p->prev.worldY;
    float extFactor = interpClampF(t) + (float)p->missedTicks;
    float ex = (float)p->curr.worldX + dx * extFactor;
    float ey = (float)p->curr.worldY + dy * extFactor;

    /* Clamp to valid world range */
    if (ex < 0.0f) ex = 0.0f;
    if (ex > 65535.0f) ex = 65535.0f;
    if (ey < 0.0f) ey = 0.0f;
    if (ey > 65535.0f) ey = 65535.0f;

    *outX = (WORLD)ex;
    *outY = (WORLD)ey;
    *outAngle = p->curr.angle;
    *outOnBoat = p->curr.onBoat;
    return TRUE;
  }

  /* Normal interpolation between prev and curr */
  clamped = interpClampF(t);

  *outX = (WORLD)((float)p->prev.worldX +
                   ((float)p->curr.worldX - (float)p->prev.worldX) * clamped);
  *outY = (WORLD)((float)p->prev.worldY +
                   ((float)p->curr.worldY - (float)p->prev.worldY) * clamped);
  *outAngle = interpAngleLerp(p->prev.angle, p->curr.angle, clamped);
  *outOnBoat = p->curr.onBoat;
  return TRUE;
}

bool interpGetLgm(const InterpContext *ctx, BYTE playerNum,
                  BYTE *outMX, BYTE *outMY,
                  BYTE *outPX, BYTE *outPY, BYTE *outFrame) {
  const InterpPlayer *p;
  const InterpSnapshot *lgmSnap;

  if (playerNum >= MAX_TANKS || playerNum == ctx->localPlayer) {
    return FALSE;
  }

  p = &ctx->players[playerNum];

  if (!p->hasData) {
    return FALSE;
  }

  lgmSnap = p->hasPending ? &p->pending : &p->curr;
  *outMX = lgmSnap->lgmMX;
  *outMY = lgmSnap->lgmMY;
  *outPX = lgmSnap->lgmPX;
  *outPY = lgmSnap->lgmPY;
  *outFrame = lgmSnap->lgmFrame;
  return TRUE;
}

bool interpHasData(const InterpContext *ctx, BYTE playerNum) {
  if (playerNum >= MAX_TANKS) {
    return FALSE;
  }
  return ctx->players[playerNum].hasData;
}

bool interpIsAlive(const InterpContext *ctx, BYTE playerNum) {
  const InterpPlayer *p;
  if (playerNum >= MAX_TANKS) {
    return FALSE;
  }
  p = &ctx->players[playerNum];
  if (!p->hasData) {
    return FALSE;
  }
  if (p->hasPending) {
    return p->pending.alive;
  }
  return p->curr.alive;
}

/* Above this a snapshot inter-arrival interval is treated as garbage/stale
 * and the smooth path bows out (a real interval is ~20ms; loss stretches it,
 * but past this the discrete freeze owns the motion). */
#define INTERP_MAX_INTERVAL_MS 200

/* Lerp one snapshot toward another at t in [0,1] (t clamped by the caller). */
static void interpLerpSnap(const InterpSnapshot *a, const InterpSnapshot *b,
                           float t, WORLD *outX, WORLD *outY,
                           TURNTYPE *outAngle, bool *outOnBoat) {
  *outX = (WORLD)((float)a->worldX + ((float)b->worldX - (float)a->worldX) * t);
  *outY = (WORLD)((float)a->worldY + ((float)b->worldY - (float)a->worldY) * t);
  *outAngle = interpAngleLerp(a->angle, b->angle, t);
  *outOnBoat = (t < 0.5f) ? a->onBoat : b->onBoat;
}

bool interpGetRenderPosition(const InterpContext *ctx, BYTE playerNum,
                             uint32_t renderNowMs, float extraDelayMs,
                             WORLD *outX, WORLD *outY,
                             TURNTYPE *outAngle, bool *outOnBoat) {
  const InterpPlayer *p;
  int32_t target;
  int32_t toNewest;

  if (playerNum >= MAX_TANKS || playerNum == ctx->localPlayer) {
    return FALSE;
  }
  p = &ctx->players[playerNum];

  /* Need a forward buffer (curr + pending) to interpolate toward; a long
   * stall or a dead player bows out to the discrete/dead handling. */
  if (!p->hasData || !p->hasPending) {
    return FALSE;
  }
  if (p->missedTicks > INTERP_MAX_MISSED_TICKS) {
    return FALSE;
  }
  if (!p->pending.alive) {
    return FALSE;
  }

  /* Render one nominal snapshot behind the newest arrival, plus the adaptive
   * extra delay.  Signed arithmetic keeps the SDL tick wraparound defined. */
  target = (int32_t)(renderNowMs -
                     (uint32_t)((float)INTERP_NOMINAL_SNAPSHOT_MS + extraDelayMs));

  /* At/after the newest sample: nothing newer to move toward — clamp to the
   * newest (pending).  This is the brief-loss case; clamping avoids snap-back. */
  toNewest = (int32_t)((uint32_t)target - p->pendingArrivalMs);
  if (toNewest >= 0) {
    *outX = p->pending.worldX;
    *outY = p->pending.worldY;
    *outAngle = p->pending.angle;
    *outOnBoat = p->pending.onBoat;
    return TRUE;
  }

  /* curr → pending segment. */
  {
    int32_t fromCurr = (int32_t)((uint32_t)target - p->currArrivalMs);
    if (fromCurr >= 0) {
      uint32_t interval = p->pendingArrivalMs - p->currArrivalMs;
      float t;
      if (interval == 0 || interval > INTERP_MAX_INTERVAL_MS) {
        return FALSE;
      }
      t = (float)fromCurr / (float)interval;
      if (t > 1.0f) t = 1.0f;
      interpLerpSnap(&p->curr, &p->pending, t, outX, outY, outAngle, outOnBoat);
      return TRUE;
    }
  }

  /* prev → curr segment (reached when the extra delay pushes the target back
   * a full snapshot, i.e. depth 2). */
  if (p->hasPrev) {
    int32_t fromPrev = (int32_t)((uint32_t)target - p->prevArrivalMs);
    uint32_t interval = p->currArrivalMs - p->prevArrivalMs;
    if (interval == 0 || interval > INTERP_MAX_INTERVAL_MS) {
      return FALSE;
    }
    if (fromPrev <= 0) {
      /* Older than prev: clamp to prev (deepest the buffer holds). */
      *outX = p->prev.worldX;
      *outY = p->prev.worldY;
      *outAngle = p->prev.angle;
      *outOnBoat = p->prev.onBoat;
      return TRUE;
    }
    {
      float t = (float)fromPrev / (float)interval;
      if (t > 1.0f) t = 1.0f;
      interpLerpSnap(&p->prev, &p->curr, t, outX, outY, outAngle, outOnBoat);
      return TRUE;
    }
  }

  return FALSE;
}

InterpRenderDecision interpRenderControl(InterpRenderCtl *ctl,
                                         float jitterMs, uint32_t nowMs) {
  InterpRenderDecision d;
  int32_t dt;
  float target;
  float cur;
  float diff;

  /* Frame-pacing / stress gate: the first frame, a zero/negative gap, or a
   * gap spike means the render clock can't smoothly drive fractional t, so
   * this frame uses the discrete apply (today's behaviour). */
  dt = (ctl->lastRenderMs == 0) ? 0 : (int32_t)(nowMs - ctl->lastRenderMs);
  d.discrete = (ctl->lastRenderMs == 0) || dt <= 0 || dt > INTERP_FRAME_STRESS_MS;

  /* Target extra delay: only jitter above the quantization floor is real and
   * allowed to deepen the buffer, bounded to one extra snapshot. */
  target = jitterMs - (float)INTERP_JITTER_FLOOR_MS;
  if (target < 0.0f) target = 0.0f;
  if (target > (float)INTERP_MAX_EXTRA_DELAY_MS) {
    target = (float)INTERP_MAX_EXTRA_DELAY_MS;
  }

  /* Slew currentDelayMs toward target with a hysteresis deadband and an
   * asymmetric rate limit: slow to grow (push past today's depth), fast to
   * shrink (relax toward today's one-snapshot behaviour).  The controller is
   * frozen on a stressed frame so a spike can't ratchet the depth. */
  cur = ctl->currentDelayMs;
  if (!d.discrete) {
    diff = target - cur;
    if (diff > INTERP_DEPTH_HYST_MS) {
      /* Grow only when the target meaningfully exceeds the current depth
       * (deadband suppresses growth on tiny jitter), and only slowly. */
      float step = INTERP_DEPTH_GROW_MS_PER_S * (float)dt / 1000.0f;
      cur += (diff < step) ? diff : step;
    } else if (diff < 0.0f) {
      /* Relax toward today's behaviour with no deadband — quick to shrink,
       * landing exactly on the target so a clean link returns to depth 1. */
      float step = INTERP_DEPTH_SHRINK_MS_PER_S * (float)dt / 1000.0f;
      cur += (-diff < step) ? diff : -step;
    }
    if (cur < 0.0f) cur = 0.0f;
    if (cur > (float)INTERP_MAX_EXTRA_DELAY_MS) {
      cur = (float)INTERP_MAX_EXTRA_DELAY_MS;
    }
    ctl->currentDelayMs = cur;
    ctl->lastRenderMs = nowMs;
  } else {
    /* Anchor the clock so the next frame measures a real gap, but leave the
     * depth untouched. */
    ctl->lastRenderMs = nowMs;
  }

  d.extraDelayMs = ctl->currentDelayMs;
  return d;
}
