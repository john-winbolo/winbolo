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
                  const InterpSnapshot *snap, uint32_t tick) {
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
    p->missedTicks = 0;
    return;
  }

  if (p->hasPending) {
    /* Shift: curr → prev, pending → curr, new → pending */
    p->prev = p->curr;
    p->hasPrev = TRUE;
    p->curr = p->pending;
  } else {
    /* Second snapshot: curr → prev, new goes to pending */
    p->prev = p->curr;
    p->hasPrev = TRUE;
  }

  p->pending = *snap;
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
