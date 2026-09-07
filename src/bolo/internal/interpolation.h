/*
 * Copyright (c) 1998-2026 John Morrison.
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
*Filename:      interpolation.h
*Purpose:
*  Per-player interpolation of other tanks' positions
*  between server snapshots for smooth rendering.
*********************************************************/

#ifndef INTERPOLATION_H
#define INTERPOLATION_H

#include "global.h"

#ifndef INTERPCONTEXT_TYPEDEF
#define INTERPCONTEXT_TYPEDEF
typedef struct InterpContext InterpContext;
#endif

/* A snapshot of one player's tank state at a point in time.
 * Named InterpSnapshot to distinguish from the wire-format
 * TankSnapshot in input_packet.h. */
typedef struct {
  WORLD worldX;
  WORLD worldY;
  TURNTYPE angle;
  SPEEDTYPE speed;
  bool onBoat;
  bool alive;       /* TRUE if armour <= TANK_FULL_ARMOUR */
  bool tankHidden;  /* TRUE when the server withheld this tank's position and
                     * the entry arrived only to carry the LGM below — worldX,
                     * worldY, angle and speed hold nothing to draw. */
  BYTE lgmMX;
  BYTE lgmMY;
  BYTE lgmPX;
  BYTE lgmPY;
  BYTE lgmFrame;
} InterpSnapshot;

/* One game tick of display delay for jitter absorption */
#define INTERP_BUFFER_MS 20

/* Per-player interpolation state */
typedef struct {
  InterpSnapshot prev;
  InterpSnapshot curr;
  InterpSnapshot pending;   /* newest arrival, not yet displayed */
  uint32_t snapshotTick;   /* Tick when curr was received */
  bool hasData;            /* At least one snapshot received */
  bool hasPrev;            /* Two snapshots received (can interpolate) */
  bool hasPending;         /* true when pending holds a newer snapshot than curr */
  uint32_t missedTicks;    /* Consecutive ticks with no update */
  uint32_t currArrivalMs;    /* wall-clock ms when curr was received */
  uint32_t prevArrivalMs;    /* wall-clock ms when prev was received */
  uint32_t pendingArrivalMs; /* wall-clock ms when pending was received */
} InterpPlayer;

/* The full interpolation context for all other players */
struct InterpContext {
  InterpPlayer players[MAX_TANKS];
  BYTE localPlayer;        /* Our own player number (skip interpolation) */
};

/*********************************************************
*NAME:          interpCreate
*PURPOSE:
*  Initializes the interpolation context.
*
*ARGUMENTS:
*  ctx         - Pointer to the InterpContext
*  localPlayer - Our own player number
*********************************************************/
void interpCreate(InterpContext *ctx, BYTE localPlayer);

/*********************************************************
*NAME:          interpUpdate
*PURPOSE:
*  Called when a new server snapshot arrives for a player.
*  Shifts current→previous, stores new snapshot as current.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player this snapshot is for
*  snap      - The new snapshot data
*  tick      - Server tick number
*  nowMs     - Wall-clock arrival time (ms), used by the render-time
*              interpolation to advance a fractional t against a render
*              clock.  A snapshot whose tick is not newer than the last
*              applied one is ignored (idempotent on serverTick), so the
*              same snapshot drained from two call sites can't shift twice.
*********************************************************/
void interpUpdate(InterpContext *ctx, BYTE playerNum,
                  const InterpSnapshot *snap, uint32_t tick, uint32_t nowMs);

/*********************************************************
*NAME:          interpMarkMissing
*PURPOSE:
*  Called each tick for players that did NOT receive a
*  snapshot update (e.g., out of view or disconnected).
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player had no update
*********************************************************/
void interpMarkMissing(InterpContext *ctx, BYTE playerNum);

/*********************************************************
*NAME:          interpGetPosition
*PURPOSE:
*  Returns interpolated world position and angle for a
*  player at the given fractional tick offset.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player to interpolate
*  t         - Interpolation factor [0.0, 1.0] where
*              0 = previous snapshot, 1 = current snapshot
*  outX      - Output: interpolated world X
*  outY      - Output: interpolated world Y
*  outAngle  - Output: interpolated angle
*  outOnBoat - Output: on boat status
*
*RETURNS:
*  TRUE if valid interpolated data is available,
*  FALSE if player has no data or is stale
*********************************************************/
bool interpGetPosition(const InterpContext *ctx, BYTE playerNum,
                       float t,
                       WORLD *outX, WORLD *outY,
                       TURNTYPE *outAngle, bool *outOnBoat);

/*********************************************************
*NAME:          interpGetLgm
*PURPOSE:
*  Returns the most recent LGM data for a player.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*  outMX     - Output: LGM map X
*  outMY     - Output: LGM map Y
*  outPX     - Output: LGM pixel X
*  outPY     - Output: LGM pixel Y
*  outFrame  - Output: LGM frame
*
*RETURNS:
*  TRUE if valid LGM data is available
*********************************************************/
bool interpGetLgm(const InterpContext *ctx, BYTE playerNum,
                  BYTE *outMX, BYTE *outMY,
                  BYTE *outPX, BYTE *outPY, BYTE *outFrame);

/*********************************************************
*NAME:          interpHasData
*PURPOSE:
*  Returns whether we have any snapshot data for a player.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*********************************************************/
bool interpHasData(const InterpContext *ctx, BYTE playerNum);

/*********************************************************
*NAME:          interpIsAlive
*PURPOSE:
*  Returns whether the player's most recent snapshot shows
*  them as alive.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*********************************************************/
bool interpIsAlive(const InterpContext *ctx, BYTE playerNum);

/*********************************************************
*NAME:          interpTankHidden
*PURPOSE:
*  Returns whether the player's most recent snapshot withheld the tank's
*  position — it arrived to carry the LGM alone, so there is a man to draw
*  but no tank.
*
*ARGUMENTS:
*  ctx       - Pointer to the InterpContext
*  playerNum - Which player
*********************************************************/
bool interpTankHidden(const InterpContext *ctx, BYTE playerNum);

/* === Render-time interpolation (driven off a render clock) === */

/* Nominal spacing between server snapshots (ms).  The server emits a
 * snapshot every 20ms, so rendering the prev→curr segment over one such
 * interval after curr's arrival places the display one snapshot behind the
 * newest arrival — today's structural buffer depth. */
#define INTERP_NOMINAL_SNAPSHOT_MS 20

/* Adaptive display delay never exceeds two snapshots (one nominal interval
 * of extra delay on top of the one-snapshot base). */
#define INTERP_MAX_EXTRA_DELAY_MS INTERP_NOMINAL_SNAPSHOT_MS

/* A clean loopback link's measured jitter floors at ~20-30ms purely from
 * arrival-timestamp quantization; only jitter above this floor is treated as
 * real and allowed to deepen the buffer, so a clean link stays at depth 1. */
#define INTERP_JITTER_FLOOR_MS 30

/* Render-clock stress threshold: a frame gap larger than this means the
 * render clock is too irregular to drive smooth fractional interpolation, so
 * the render path falls back to today's discrete (t=1.0) apply for that frame. */
#define INTERP_FRAME_STRESS_MS 50

/* Hysteresis deadband and asymmetric slew rates for the adaptive depth: the
 * controller is quick to relax toward today's one-snapshot behaviour (fast
 * shrink) and slow to push past it (rate-limited grow). */
#define INTERP_DEPTH_HYST_MS        3.0f
#define INTERP_DEPTH_GROW_MS_PER_S  20.0f
#define INTERP_DEPTH_SHRINK_MS_PER_S 80.0f

/* Persistent per-client controller state for the adaptive display delay.
 * Zero-initialised (lastRenderMs == 0 => first frame => discrete). */
typedef struct {
  uint32_t lastRenderMs;   /* render clock at the previous control update */
  float    currentDelayMs; /* smoothed extra delay beyond the 1-snapshot base */
} InterpRenderCtl;

/* Output of one control update. */
typedef struct {
  float extraDelayMs; /* extra display delay (ms) on top of the 1-snapshot base */
  bool  discrete;     /* true => render clock stressed, use discrete t=1.0 */
} InterpRenderDecision;

/*********************************************************
*NAME:          interpRenderControl
*PURPOSE:
*  Advances the adaptive display-delay controller one render frame.
*  Derives a target extra delay from measured snapshot jitter (above the
*  quantization floor), bounded to one extra snapshot, then slews
*  currentDelayMs toward it with hysteresis and an asymmetric rate limit
*  (slow grow, fast shrink).  Flags the frame discrete when the render
*  clock is stressed (first frame, zero/negative gap, or a gap spike).
*
*ARGUMENTS:
*  ctl      - Persistent controller state (updated in place)
*  jitterMs - Measured snapshot inter-arrival jitter (ms)
*  nowMs    - Current render clock (ms)
*********************************************************/
InterpRenderDecision interpRenderControl(InterpRenderCtl *ctl,
                                         float jitterMs, uint32_t nowMs);

/*********************************************************
*NAME:          interpGetRenderPosition
*PURPOSE:
*  Render-clock entity interpolation for a player.  Renders at a target time
*  of (renderNowMs - one nominal snapshot - extraDelayMs) and interpolates
*  between whichever two of the prev/curr/pending samples bracket that target
*  by wall-clock arrival time (the clean-link depth-1 case lands in the
*  curr→pending segment, ~1 snapshot behind the newest arrival).  This is the
*  smooth path; it does NOT extrapolate — brief loss clamps to the newest
*  sample (no snap-back) and a long stall returns FALSE.
*
*ARGUMENTS:
*  ctx          - Pointer to the InterpContext
*  playerNum    - Which player to interpolate
*  renderNowMs  - Current render clock (ms)
*  extraDelayMs - Adaptive extra display delay beyond the 1-snapshot base
*  outX/outY/outAngle/outOnBoat - Interpolated outputs
*
*RETURNS:
*  TRUE and writes the outputs when a smooth interpolated position is
*  available; FALSE when the caller should fall back to the discrete apply
*  (no forward buffer yet, player dead/frozen, or a stale/garbage interval).
*********************************************************/
bool interpGetRenderPosition(const InterpContext *ctx, BYTE playerNum,
                             uint32_t renderNowMs, float extraDelayMs,
                             WORLD *outX, WORLD *outY,
                             TURNTYPE *outAngle, bool *outOnBoat);

#endif /* INTERPOLATION_H */
