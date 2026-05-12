/*
 * $Id$
 *
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
*Name:          Shells
*Filename:      shells.c
*Author:        John Morrison
*Creation Date: 25/12/98
*Last Modified: 04/04/02
*Purpose:
*  Responsable for Shells tracking/collision detect etc.
*********************************************************/

#include <memory.h>
#include <math.h>   /* atan2f, fmodf — for shellAngleFromTarget */

#include "bolo_map.h"
#include "building.h"
#include "explosions.h"
#include "floodfill.h"
#include "frontend.h"
#include "global.h"
#include "grass.h"
#include "lgm.h"
#include "minesexp.h"
#include "players.h"
#include "pillbox.h"
#include "rubble.h"
#include "brain_data.h"
#include "screenbullet.h"
#include "shells.h"
#include "sounddist.h"
#include "swamp.h"
#include "tank.h"
#include "position_history.h"
#include "tankexp.h"
#include "util.h"
#include "game_sim.h"
#include "client_sim.h"



bool c;
shellsNetHit snh;

/* ---------------------------------------------------------------------- */
/* Debug: ring buffer of recent shell-hit positions in WORLD coords.      */
/* Consumed by the BrainTest debug renderer to draw an orange pixel at    */
/* each hit so we can see when a shell explodes without registering on   */
/* the expected target.                                                    */
/* ---------------------------------------------------------------------- */
#define SHELL_HIT_LOG_SIZE 128
typedef struct { int wx; int wy; uint32_t tick; uint8_t owner; } ShellHitLogEntry;
static ShellHitLogEntry g_shell_hit_log[SHELL_HIT_LOG_SIZE];
static int g_shell_hit_log_head = 0;   /* next write slot */
static int g_shell_hit_log_count = 0;  /* min(writes, SIZE) */

void shellsDebugHitLogClear(void) {
    g_shell_hit_log_head = 0;
    g_shell_hit_log_count = 0;
}

int shellsDebugHitLogCount(void) {
    return g_shell_hit_log_count;
}

/* Read slot i (0-based from the oldest). */
int shellsDebugHitLogGet(int i, int *wx, int *wy, uint32_t *tick, uint8_t *owner) {
    if (i < 0 || i >= g_shell_hit_log_count) return 0;
    int idx;
    if (g_shell_hit_log_count < SHELL_HIT_LOG_SIZE) {
        idx = i;
    } else {
        idx = (g_shell_hit_log_head + i) % SHELL_HIT_LOG_SIZE;
    }
    if (wx)    *wx    = g_shell_hit_log[idx].wx;
    if (wy)    *wy    = g_shell_hit_log[idx].wy;
    if (tick)  *tick  = g_shell_hit_log[idx].tick;
    if (owner) *owner = g_shell_hit_log[idx].owner;
    return 1;
}

static void shellsDebugHitLogAdd(int wx, int wy, uint32_t tick, uint8_t owner) {
    g_shell_hit_log[g_shell_hit_log_head].wx = wx;
    g_shell_hit_log[g_shell_hit_log_head].wy = wy;
    g_shell_hit_log[g_shell_hit_log_head].tick = tick;
    g_shell_hit_log[g_shell_hit_log_head].owner = owner;
    g_shell_hit_log_head = (g_shell_hit_log_head + 1) % SHELL_HIT_LOG_SIZE;
    if (g_shell_hit_log_count < SHELL_HIT_LOG_SIZE) g_shell_hit_log_count++;
}

#undef SHELL_START_ADD
#define SHELL_START_ADD 5

/* ── Pure shell-physics primitives ────────────────────────────────
 * These are the single source of truth for shell motion. Both the
 * live engine (shellsUpdate, shellsAddItem) and the brain's
 * stateless trajectory simulator call into these so the trajectory
 * math can't drift between them. */

void shellApplyStartOffset(WORLD *x, WORLD *y, int xAdd, int yAdd) {
  *x = (WORLD)((int)*x + (SHELL_START_ADD) * xAdd);
  *y = (WORLD)((int)*y + (SHELL_START_ADD) * yAdd);
}

void shellAdvance1Tick(WORLD *x, WORLD *y,
                       int32_t *xAcc, int32_t *yAcc,
                       int32_t xStep, int32_t yStep) {
  *xAcc += xStep;
  *yAcc += yStep;
  int xMove = (int)(*xAcc >> 8);
  int yMove = (int)(*yAcc >> 8);
  *xAcc -= xMove << 8;
  *yAcc -= yMove << 8;
  *x = (WORLD)((int)*x + xMove);
  *y = (WORLD)((int)*y + yMove);
}

int shellLifeTicks(int len) {
  int t = SHELL_LIFE * len - SHELL_START_ADD;
  return t < 0 ? 0 : t;
}

TURNTYPE shellAngleFromTarget(WORLD ox, WORLD oy, WORLD tx, WORLD ty) {
  int dx = (int)tx - (int)ox;
  int dy = (int)ty - (int)oy;
  if (dx == 0 && dy == 0) return 0;
  /* atan2(dx, -dy) → radians cw from north → bradians via *128/π.
   * Returns a FLOAT — the engine's tank.angle is float and
   * shellsAddItem fires at that exact float, so rounding here
   * would inject up to ~0.5 brad of error vs. a real shell. The
   * downstream simulate_shot_walk + utilCalcDistance preserve the
   * fractional brad through the trajectory. */
  float angle_f = atan2f((float)dx, -(float)dy) *
                  (128.0f / 3.14159265358979323846f);
  /* Wrap to [0, 256) keeping the fraction. */
  angle_f = fmodf(fmodf(angle_f, 256.0f) + 256.0f, 256.0f);
  return (TURNTYPE)angle_f;
}

void shellSpawnPos(WORLD tank_x, WORLD tank_y, TURNTYPE angle,
                   WORLD *out_x, WORLD *out_y) {
  int xAdd, yAdd;
  utilCalcDistance(&xAdd, &yAdd, angle, SHELL_SPEED);
  *out_x = tank_x;
  *out_y = tank_y;
  shellApplyStartOffset(out_x, out_y, xAdd, yAdd);
}


/*********************************************************
*NAME:          shellsCreate
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 25/12/98
*PURPOSE:
*  Sets up the shells data structure
*
*ARGUMENTS:
*
*********************************************************/
shells shellsCreate(void) {
	return NULL;
}


/*********************************************************
*NAME:          shellsDestroy
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 25/12/98
*PURPOSE:
*  Destroys and frees memory for the shells data structure
*
*ARGUMENTS:
*  value - Pointer to the shells data structure
*********************************************************/
void shellsDestroy(shells *value) {
  shells q;

  while (!IsEmpty(*value)) {
    q = *value;
    *value = ShellsTail(q);
    Dispose(q);
  }
  c = TRUE;
}

/*********************************************************
*NAME:          shellsAddItem
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 6/3/99
*PURPOSE:
*  Adds an item to the shells data structure. Function
*  also calls the sound playing function
*
*ARGUMENTS:
*  value  - Pointer to the shells data structure
*  x      - X co-ord of the start position
*  y      - Y co-ord of the start position
*  angle  - angle of the shot
*  len    - Length in map units of the item
*  owner  - Who fired the shell
*  onBoat - Was the shell launched from a boat
*********************************************************/
void shellsAddItem(GameSim *sim, shells *value, WORLD x, WORLD y, TURNTYPE angle, TURNTYPE len, BYTE owner, bool onBoat) {
  shells q;
  int xAdd;
  int yAdd;
  BYTE soundMX = (BYTE)(x >> TANK_SHIFT_MAPSIZE);  /* Tank position before shell offset */
  BYTE soundMY = (BYTE)(y >> TANK_SHIFT_MAPSIZE);

  utilCalcDistance(&xAdd, &yAdd, angle, SHELL_SPEED);
  shellApplyStartOffset(&x, &y, xAdd, yAdd);
/*
  if (xAdd >= 0) {
    x += 22;
  } else {
    x -= 22;
  }
  if (yAdd >= 0) {
    y += 22;
  } else {
    y -= 22;
  }
*/
  New (q);
  q->x = x;
  q->y = y;
  q->angle = angle;
  q->length = (BYTE) shellLifeTicks((int)len);
  q->onBoat = onBoat;
  q->creator = sim->viewPlayer;
  q->owner = owner;
  q->packSent = FALSE;
  q->shellDead = FALSE;
  q->compensationTicks = sim->lagCompTicks;
  utilCalcDistanceHP(&q->xStep, &q->yStep, angle, SHELL_SPEED);
  q->xAcc = 0;
  q->yAcc = 0;

  q->next = *value;
  q->prev = NULL;
  if (NonEmpty(*value)) {
    (*value)->prev = q;
  }
  *value = q;

  /* Play shoot sound at tank position (not offset shell position) */
  sim->callbacks.soundDistShoot(sim->callbacks.ctx, soundMX, soundMY, owner);
}

/*********************************************************
*NAME:          shellsUpdate
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 04/04/02
*PURPOSE:
*  Updates each shells position and checks for colisions
*
*ARGUMENTS:
*  value    - Pointer to the shells data structure
*  mp       - Pointer to the map Structure
*  pb       - Pointer to the pillbox Structure
*  tk       - Pointer to an array of tank structures
*  numTanks - Number of tanks in the array
*  isServer - TRUE if we are a server
*********************************************************/
void shellsUpdate(GameSim *sim, tank *tk, BYTE numTanks, lgm **lgms, starts *sts) {
	shells *value = &sim->shs;
	map *mp = &sim->mp;
	WORLD newX;      /* world x-coord of new shell location */
	WORLD newY;      /* world y-coord of new shell location */
	shells position; /* The position in the stack of items */
	bool needUpdate; /* Does an update need to occur? */
	BYTE bmx;        /* Shell map x-coord */
	BYTE bmy;        /* Shell map y-coord */
	BYTE sx;         /* Screen - TANK_SUBTRACT Map X and Y Positions */
	BYTE sy;
	BYTE spx;        /* Screen - TANK_SUBTRACT  Pixel X and Y Positions */
	BYTE spy;
	WORLD conv;      /* Used for bit shifting */
	BYTE count;      /* Looping variable */

	position = *value;

	/* In the old dual-context architecture, single-player ran game logic
	 * on the client side so isServer was forced FALSE.  In the new
	 * server-authoritative architecture the server sim passes isServer=TRUE
	 * even for single-player, so we must respect the caller's value. */

	/* While there are shells to process.. */
	while (NonEmpty(position)) {
		needUpdate = TRUE;
		if (position->shellDead == TRUE && (position->packSent == TRUE || sim->isServer == TRUE)) {
			needUpdate = FALSE;
			shellsDeleteItem(value, &position);
		} else if (position->shellDead == TRUE && position->packSent == FALSE) {
			needUpdate = TRUE;
		} else if (position->length > SHELL_DEATH) {
			/* Move the shell. The pure math lives in shellAdvance1Tick
			 * so the brain's stateless trajectory simulator
			 * (brainPathfinderSimulateShot) walks an identical path
			 * — drift between the live shell and the brain's
			 * "would my shot hit?" prediction is structurally
			 * impossible. */
			newX = position->x;
			newY = position->y;
			shellAdvance1Tick(&newX, &newY,
			                  &position->xAcc, &position->yAcc,
			                  position->xStep, position->yStep);
			/* Check for colision */
			if ((shellsCalcCollision(sim, tk, &newX, &newY, position->angle, position->owner, position->onBoat, numTanks, position->compensationTicks)) == TRUE)
			{
				/* Get X and Y map co-ords. */
				conv = newX;
				conv >>= TANK_SHIFT_MAPSIZE;
				bmx = (BYTE) conv;
				conv = newY;
				conv >>= TANK_SHIFT_MAPSIZE;
				bmy = (BYTE) conv;
				/* Get Screen - TANK_SUBTRACT co-ords */
				conv = newX - TANK_SUBTRACT;
				conv >>= TANK_SHIFT_MAPSIZE;
				sx = (BYTE) conv;
				conv = newY - TANK_SUBTRACT;
				conv >>= TANK_SHIFT_MAPSIZE;
				sy = (BYTE) conv;
				conv = newX - TANK_SUBTRACT;
				conv <<= TANK_SHIFT_MAPSIZE;
				conv >>= TANK_SHIFT_PIXELSIZE;
				spx = (BYTE) conv;
				conv = newY - TANK_SUBTRACT;
				conv <<= TANK_SHIFT_MAPSIZE;
				conv >>= TANK_SHIFT_PIXELSIZE;
				spy = (BYTE) conv;
				explosionsAddItem(&sim->expl, sx,sy,spx,spy,EXPLOSION_START);
				/* Debug: record the exact world-pixel hit for BrainTest viz */
				shellsDebugHitLogAdd((int)newX, (int)newY, 0, position->owner);
				if (sim->callbacks.explosion) sim->callbacks.explosion(sim->callbacks.ctx, sx, sy, spx, spy);
				minesExpAddItem(&sim->minesExplosions, mp, bmx, bmy);
				count = 0;
				while (count < numTanks) {
					if (lgms && lgms[count] != NULL) {
						BYTE lgmOwner = (*lgms[count])->playerNum;
						uint8_t rewindTicks;
						WORLD lgmHitX = 0, lgmHitY = 0;
						bool useRewound = FALSE;

						if (position->compensationTicks > 0) {
							rewindTicks = position->compensationTicks;
						} else if (sim->isServer && position->owner == NEUTRAL) {
							rewindTicks = sim->perPlayerCompTicks[lgmOwner];
						} else {
							rewindTicks = 0;
						}

						if (rewindTicks > 0 && sim->lgmPosHistoryPtr != NULL &&
						    lgmOwner != NEUTRAL &&
						    posHistoryGet(&sim->lgmPosHistoryPtr[lgmOwner],
						                  rewindTicks, &lgmHitX, &lgmHitY)) {
							useRewound = TRUE;
						}

						if (useRewound) {
							lgmDeathCheckAtPosition(sim, lgms[count],
							                        lgmHitX, lgmHitY,
							                        newX, newY,
							                        position->owner, &tk[count]);
						} else {
							lgmDeathCheck(sim, lgms[count],
							              newX, newY,
							              position->owner, &tk[count]);
						}
					}
					count++;
				}
				if (position->packSent == TRUE) {
					needUpdate = FALSE;
					shellsDeleteItem(value, &position);
				} else {
					position->shellDead = TRUE;
				}
			} else {
				position->length--;
				position->x = newX;
				position->y = newY;
			}
		} else { /* Update Position */
			/* Add to explosion Data structure and remove from shells data structure */
			needUpdate = FALSE;
			/* Get X and Y map co-ords. */
			conv = position->x;
			conv >>= TANK_SHIFT_MAPSIZE;
			bmx = (BYTE) conv;
			conv = position->y;
			conv >>= TANK_SHIFT_MAPSIZE;
			bmy = (BYTE) conv;
			/* Get Screen - TANK_SUBTRACT co-ords */
			conv = position->x - TANK_SUBTRACT;
			conv >>= TANK_SHIFT_MAPSIZE;
			sx = (BYTE) conv;
			conv = position->y - TANK_SUBTRACT;
			conv >>= TANK_SHIFT_MAPSIZE;
			sy = (BYTE) conv;
			conv = position->x - TANK_SUBTRACT;
			conv <<= TANK_SHIFT_MAPSIZE;
			conv >>= TANK_SHIFT_PIXELSIZE;
			spx = (BYTE) conv;
			conv = position->y - TANK_SUBTRACT;
			conv <<= TANK_SHIFT_MAPSIZE;
			conv >>= TANK_SHIFT_PIXELSIZE;
			spy = (BYTE) conv;
			explosionsAddItem(&sim->expl, sx,sy,spx,spy,EXPLOSION_START);
			if (sim->callbacks.explosion) sim->callbacks.explosion(sim->callbacks.ctx, sx, sy, spx, spy);
			minesExpAddItem(&sim->minesExplosions, mp, bmx, bmy);
			count = 0;
			while (count < numTanks) {
				if (lgms && lgms[count] != NULL) {
					BYTE lgmOwner = (*lgms[count])->playerNum;
					uint8_t rewindTicks;
					WORLD lgmHitX = 0, lgmHitY = 0;
					bool useRewound = FALSE;

					if (position->compensationTicks > 0) {
						rewindTicks = position->compensationTicks;
					} else if (sim->isServer && position->owner == NEUTRAL) {
						rewindTicks = sim->perPlayerCompTicks[lgmOwner];
					} else {
						rewindTicks = 0;
					}

					if (rewindTicks > 0 && sim->lgmPosHistoryPtr != NULL &&
					    lgmOwner != NEUTRAL &&
					    posHistoryGet(&sim->lgmPosHistoryPtr[lgmOwner],
					                  rewindTicks, &lgmHitX, &lgmHitY)) {
						useRewound = TRUE;
					}

					if (useRewound) {
						lgmDeathCheckAtPosition(sim, lgms[count],
						                        lgmHitX, lgmHitY,
						                        position->x, position->y,
						                        position->owner, &tk[count]);
					} else {
						lgmDeathCheck(sim, lgms[count],
						              position->x, position->y,
						              position->owner, &tk[count]);
					}
				}
				count++;
			}
			if (position->packSent == TRUE) {
				shellsDeleteItem(value, &position);
			} else if (position->shellDead == FALSE) {
				position->shellDead = TRUE;
			}
		}

		/* Get the next Item */
		if (*value != NULL && needUpdate == TRUE) {
			position = ShellsTail(position);
		}
	}
}

/*********************************************************
*NAME:          shellsDeleteItem
*AUTHOR:        John Morrison
*CREATION DATE: 25/12/98
*LAST MODIFIED: 25/12/98
*PURPOSE:
*  Deletes the value from the master list
*
*ARGUMENTS:
*  master  - The master list of all shells
*  value   - Pointer to the shells to delete. Also puts
*            next shell its position
*********************************************************/
void shellsDeleteItem(shells *master, shells *value) {
  shells del;  /* The item to delete */

  del = *value;
  (*value) = ShellsTail(del);
  if (del->prev != NULL) {
    del->prev->next = del->next;
  } else {
    /* Must be the first item - Move the master position along one */
    (*master) = ShellsTail(*master);
    if (NonEmpty(*master)) {
      (*master)->prev = NULL;
    }
  }

  if (del->next != NULL) {
    del->next->prev = del->prev;
  }
  Dispose(del);
  c= TRUE;
}

/*********************************************************
*NAME:          shellsCalcScreenBullets
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 26/12/98
*PURPOSE:
*  Adds items to the sceenBullets data structure if they
*  are on screen
*
*ARGUMENTS:
*  value   - Pointer to the shells data structure
*  sBullet  - The screenBullets Data structure
*  leftPos  - X Map offset start
*  rightPos - X Map offset end
*  top      - Y Map offset end
*  bottom   - Y Map offset end
*********************************************************/
void shellsCalcScreenBullets(shells *value, screenBullets *sBullets, BYTE leftPos, BYTE rightPos, BYTE top, BYTE bottom) {
  shells q;   /* Temp Pointer */
  WORLD conv; /* Used for Bit shifting */
  BYTE x;     /* Map X and Y Positions (relative to screen) */
  BYTE y;
  BYTE px;    /* Pixel X and Y Positions */
  BYTE py;
  BYTE frame; /* Animation Frame to draw */


  q = *value;
  c = FALSE;
  while (NonEmpty(q) && c == FALSE) {
    conv = q->x;
    conv >>= TANK_SHIFT_MAPSIZE;
    x = (BYTE) conv;
    conv = q->y;
    conv >>= TANK_SHIFT_MAPSIZE;
    y = (BYTE) conv;
    if (x >= leftPos && x < rightPos && y >= top && y < bottom) {
      frame = utilGetDir(q->angle);
      x -= (BYTE) leftPos;
      y -= (BYTE) top;
      conv = q->x;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      px = (BYTE) conv;
      conv = q->y;
      conv <<= TANK_SHIFT_MAPSIZE;
      conv >>= TANK_SHIFT_PIXELSIZE;
      py = (BYTE) conv;
      screenBulletsAddItem(sBullets, x, y, px, py, (BYTE) (frame + SHELL_START_EXPLODE+1)); 
    }
    q = ShellsTail(q);
  }
}

/*********************************************************
*NAME:          shellsCalcCollision
*AUTHOR:        John Morrison
*CREATION DATE: 29/12/98
*LAST MODIFIED: 29/07/00
*PURPOSE:
*  Returns whether the collision has occured.
*  
*ARGUMENTS:
*  map      - Pointer to the Map Structure
*  pb       - Pointer to the pillboxes structure
*  tk       - Pointer to an array of tank structures
*  bs       - Pointer to the bases structure
*  xValue   - X position
*  yValue   - Y position
*  angle    - The angle the shell is travelling
*  owner    - Who fired the shell
*  onBoat   - Was the shell launched from a boat
*  numTanks - Number of tanks in the array
*  isServer - TRUE if we are a server
*********************************************************/
bool shellsCalcCollision(GameSim *sim, tank *tk, WORLD *xValue, WORLD *yValue, TURNTYPE angle, BYTE owner, bool onBoat, BYTE numTanks, uint8_t compensationTicks) {
	map *mp = &sim->mp;
	pillboxes *pb = &sim->pb;
	bases *bs = &sim->bs;
	bool returnValue; /* Value to return */
	tankHit th;       /* Used to store whether the tank has been hit */
	WORLD conv;       /* Used in the conversion */
	BYTE mapX;        /* Map Co-ordinates of where the item is going to hit */
	BYTE mapY;
	BYTE terrain;     /* The Terrain Type hit */
	BYTE newTerrain;  /* The new terrain type to replace it with */
	bool isMine;      /* Is the terrain a mine */
	BYTE count;       /* Looping variable */
	bool baseExist;   /* Does a base exist here */


	returnValue =  FALSE;
	isMine = FALSE;
	baseExist = FALSE;
	/* Convert the position to Map co-ords */
	conv = *xValue;
	conv >>= TANK_SHIFT_MAPSIZE;
	mapX = (BYTE) conv;
	conv = *yValue;
	conv >>= TANK_SHIFT_MAPSIZE;
	mapY = (BYTE) conv;

	/* Pill is hit by the shell */
	if ((pillsIsPillHit(pb, mapX, mapY)) == TRUE) {
		returnValue = TRUE;
		*xValue = mapX;
		*xValue <<= TANK_SHIFT_MAPSIZE;
		*xValue += MAP_SQUARE_MIDDLE;
		*yValue = mapY;
		*yValue <<= TANK_SHIFT_MAPSIZE;
		*yValue += MAP_SQUARE_MIDDLE;
		/* We are the server or are in a single player game */
		if (sim->isServer == TRUE) {
			/* The pill has died */
			pillsDamagePos(sim, mapX, mapY, TRUE, TRUE);
		} else if (owner == gameSimGetTankPlayer(sim, tk)) {
			pillsDamagePos(sim, mapX, mapY, FALSE, TRUE);
		}
		sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
	}


	/* Shell did not hit pillbox */
	if (returnValue == FALSE) {
		count = 0;
		while (count < numTanks && returnValue == FALSE) {
			if (gameSimGetTankPlayer(sim, &tk[count]) != owner) {
				BYTE targetPlayer = gameSimGetTankPlayer(sim, &tk[count]);
				uint8_t rewindTicks;
				WORLD hitCheckX = 0, hitCheckY = 0;
				bool useRewound = FALSE;

				if (compensationTicks > 0) {
					/* Player shell: rewind target by shooter's delay */
					rewindTicks = compensationTicks;
				} else if (sim->isServer && owner == NEUTRAL) {
					/* Pill shell: rewind target by the target's own delay */
					rewindTicks = sim->perPlayerCompTicks[targetPlayer];
				} else {
					rewindTicks = 0;
				}

				if (rewindTicks > 0 && sim->posHistoryPtr != NULL &&
				    targetPlayer != NEUTRAL &&
				    posHistoryGet(&sim->posHistoryPtr[targetPlayer],
				                  rewindTicks, &hitCheckX, &hitCheckY)) {
					useRewound = TRUE;
				}

				if (useRewound) {
					th = tankIsTankHitAtPosition(sim, &(tk[count]),
					                              hitCheckX, hitCheckY,
					                              *xValue, *yValue, angle, owner);
				} else {
					th = tankIsTankHit(sim, &(tk[count]), *xValue, *yValue, angle, owner);
				}

				switch (th) {
					case TH_HIT:
						returnValue = TRUE;
						sim->callbacks.soundDistTankHit(sim->callbacks.ctx, mapX, mapY, targetPlayer);
						break;
					case TH_KILL_SMALL:
						returnValue = TRUE;
						tkExplosionAddItem(sim, *xValue, *yValue, angle, TK_EXPLODE_LENGTH, TK_SMALL_EXPLOSION, targetPlayer);
						sim->callbacks.soundDistTankHit(sim->callbacks.ctx, mapX, mapY, targetPlayer);
						sim->callbacks.tankKill(sim->callbacks.ctx, owner, targetPlayer, LAST_DEATH_BY_SHELL, 0);
						break;
					case TH_KILL_BIG:
						returnValue = TRUE;
						tkExplosionAddItem(sim, *xValue, *yValue, angle, TK_EXPLODE_LENGTH, TK_LARGE_EXPLOSION, targetPlayer);
						sim->callbacks.soundDistTankHit(sim->callbacks.ctx, mapX, mapY, targetPlayer);
						sim->callbacks.tankKill(sim->callbacks.ctx, owner, targetPlayer, LAST_DEATH_BY_SHELL, 0);
						break;
					case TH_MISSED:
					default:
						break;
				}
			}
			count++;
		}
	}

	if (returnValue == FALSE) {
		baseExist = basesExistPos(bs, mapX, mapY);
		/* Check for base */
		if (baseExist == TRUE) {
			if (onBoat == TRUE) {
				returnValue = TRUE;
				*xValue = mapX;
				*xValue <<= TANK_SHIFT_MAPSIZE;
				*xValue += MAP_SQUARE_MIDDLE;
				*yValue = mapY;
				*yValue <<= TANK_SHIFT_MAPSIZE;
				*yValue += MAP_SQUARE_MIDDLE;
				/* Play sound */
				if ((basesCanHit(sim, mapX, mapY, owner)) == TRUE) {
					if (sim->isServer == TRUE) {
						basesDamagePos(sim, mapX, mapY);
		}
					pillsBaseHit(sim, pb, mapX, mapY, (basesGetOwnerPos(bs, mapX, mapY)));
				}
				sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
			} else if ((basesCanHit(sim, mapX, mapY, owner)) == TRUE) { /* Huh? */
				returnValue = TRUE;
				*xValue = mapX;
				*xValue <<= TANK_SHIFT_MAPSIZE;
				*xValue += MAP_SQUARE_MIDDLE;
				*yValue = mapY;
				*yValue <<= TANK_SHIFT_MAPSIZE;
				*yValue += MAP_SQUARE_MIDDLE;
				/* Do damage to base */
				if (sim->isServer == TRUE) {
					basesDamagePos(sim, mapX, mapY);
}
				pillsBaseHit(sim, pb, mapX, mapY, (basesGetOwnerPos(bs, mapX, mapY)));
				/* Play sound */
				sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
			} /*end huh?*/
		}
	}

	if (returnValue == FALSE)
	{
		if ((mapIsPassable(mp, mapX, mapY, onBoat)) == FALSE && baseExist == FALSE)
		{
			returnValue = TRUE;
			*xValue = mapX;
			*xValue <<= TANK_SHIFT_MAPSIZE;
			*xValue += MAP_SQUARE_MIDDLE;
			conv = *xValue;
			conv >>= TANK_SHIFT_MAPSIZE;
			mapX = (BYTE) conv;
			*yValue = mapY;
			*yValue <<= TANK_SHIFT_MAPSIZE;
			*yValue += MAP_SQUARE_MIDDLE;
			conv = *yValue;
			conv >>= TANK_SHIFT_MAPSIZE;
			mapY = (BYTE) conv;

			terrain = mapGetPos(mp, mapX, mapY);
			if (terrain >= MINE_START && terrain <= MINE_END) {
				terrain -= MINE_SUBTRACT;
				isMine = TRUE;
			}

			/* Update the map  & Play the sound */
			switch (terrain)
			{
				case BUILDING:
					mapSetPos(sim, mp, mapX, mapY, (buildingAddItem(&sim->blds, mapX, mapY)), FALSE, FALSE);
					sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
					break;
				case FOREST:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, GRASS+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						mapSetPos(sim, mp, mapX, mapY, GRASS, FALSE, FALSE);
					}
					sim->callbacks.soundDist(sim->callbacks.ctx, shotTreeNear, mapX, mapY);
					break;
				case HALFBUILDING:
					mapSetPos(sim, mp, mapX, mapY, (buildingAddItem(&sim->blds, mapX, mapY)), FALSE, FALSE);
					sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
					break;
				case BOAT:
					mapSetPos(sim, mp, mapX, mapY, RIVER, FALSE, FALSE);
					sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
					break;
				case GRASS:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, GRASS+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						newTerrain = grassAddItem(&sim->grs, mapX, mapY);
						mapSetPos(sim, mp, mapX, mapY, newTerrain, FALSE, FALSE);
						if (newTerrain == RIVER) {
							floodAddItem(&sim->ff, mapX, mapY);
						}
					}
					break;
				case SWAMP:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, SWAMP+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						newTerrain = swampAddItem(&sim->swp, mapX, mapY);
						mapSetPos(sim, mp, mapX, mapY, newTerrain, FALSE, FALSE);
						if (newTerrain == RIVER) {
							floodAddItem(&sim->ff, mapX, mapY);
						}
					}
					break;
				case RUBBLE:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, RUBBLE+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						newTerrain = rubbleAddItem(&sim->rbl, mapX, mapY);
						mapSetPos(sim, mp, mapX, mapY, newTerrain, FALSE, FALSE);
						if (newTerrain == RIVER) {
							floodAddItem(&sim->ff, mapX, mapY);
						}
					}
					break;
				case ROAD:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, ROAD+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						mapSetPos(sim, mp, mapX, mapY, (shellsCheckRoad(sim, mapX, mapY, angle)), FALSE, FALSE);
					}
			}
		}
	}

	if (returnValue == TRUE && !sim->isServer) {
		clientSimRecalc((ClientSim *)sim);
	}

	return returnValue;
}


/*********************************************************
*NAME:          shellsCheckRoad
*AUTHOR:        John Morrison
*CREATION DATE: 6/1/99
*LAST MODIFIED: 6/1/99
*PURPOSE:
*  A shell has been fired from a boat and has hit a road.
*  This function returns whether the road should be kept
*  as road or destroyed and replaced as river
*  
*ARGUMENTS:
*  mp    - Pointer to the Map Data structure
*  pb    - Pointer to the pillbox structure
*  bs    - Pointer to the bases structure
*  mapX  - X position of the hit
*  mapY  - Y position of the hit
*  angle - The angle the shell is travelling
*********************************************************/
BYTE shellsCheckRoad(GameSim *sim, BYTE mapX, BYTE mapY, TURNTYPE dir) {
  map *mp = &sim->mp;
  pillboxes *pb = &sim->pb;
  bases *bs = &sim->bs;
  BYTE returnValue; /* Value to return */
  BYTE dir16;       /* Angle converted to 16 direction */

  returnValue = ROAD;
  dir16 = utilGet16Dir(dir);

  if (dir16 < BRADIANS_NEAST || dir16 >= BRADIANS_NWEST) {
    if (mapIsLand(mp, pb, bs, mapX, (BYTE) (mapY-1)) == FALSE) {
      returnValue = RIVER;
    }
  } else if (dir16 >= BRADIANS_NEAST && dir16 < BRADIANS_SEAST) {
    if (mapIsLand(mp, pb, bs, (BYTE) (mapX+1), mapY) == FALSE) {
      returnValue = RIVER;
    }
  } else if (dir16 >= BRADIANS_SEAST && dir16 < BRADIANS_SWEST) {
    if (mapIsLand(mp, pb, bs, mapX, (BYTE) (mapY+1)) == FALSE) {
      returnValue = RIVER;
    }
  } else {
    if (mapIsLand(mp, pb, bs, (BYTE) (mapX-1), mapY) == FALSE) {
      returnValue = RIVER;
    }
  }

  /* Play the sound if required */
  if (returnValue == RIVER) {
    sim->callbacks.soundDist(sim->callbacks.ctx, shotBuildingNear, mapX, mapY);
  }

   return returnValue;
}

/*********************************************************
*NAME:          shellsNetMake
*AUTHOR:        John Morrison
*CREATION DATE: 6/3/99
*LAST MODIFIED: 8/9/00
*PURPOSE:
*  When we have the token we inform all the players of
*  shells we have fired since last time we had the token.
*  Returns the length of the data created
*  
*ARGUMENTS:
*  value       - Pointer to shells structure
*  buff        - Pointer to a buffer to hold the shells 
*                net data
*  noPlayerNum - If the shells ->creator equals this
*                do not send it
*  sentState   - What to set the send state to
*********************************************************/
BYTE shellsNetMake(shells *value, BYTE *buff, BYTE noPlayerNum, bool sentState) {
  BYTE returnValue;  /* Value to return */
  unsigned int ttsz; /* Size of turntype type */
  unsigned int wsz;  /* Size of world type */
  BYTE *pnt;         /* Pointer to offset in the buffer */
  shells q;          /* Temp pointer to the shells structure */

  ttsz = sizeof(TURNTYPE);
  wsz = sizeof(WORLD);
  returnValue = 0;
  pnt = buff;
  q = *value;

  while (NonEmpty(q)) {
    if (q->packSent == FALSE && q->creator != noPlayerNum) {
      /* Need to add */
      /* Check range from things */
      if (TRUE) { /* screenTankInView: client always TRUE; server never calls shellsNetMake */
        memcpy(pnt, &(q->x), wsz); /* X */
        pnt += wsz;
        returnValue = (BYTE) (returnValue + wsz);
        memcpy(pnt, &(q->y), wsz); /* Y */
        pnt += wsz;
        returnValue = (BYTE) (returnValue + wsz);
        memcpy(pnt, &(q->angle), ttsz); /* Angle */
        pnt += ttsz;
        returnValue = (BYTE) (returnValue + ttsz);
        *pnt = q->length; /* Length */
        pnt++;
        returnValue++;
        *pnt = q->owner; /* Owner */
        pnt++;
        returnValue++;
        *pnt = q->onBoat; /* On Boat */
        pnt++;
        returnValue++;
        *pnt = q->creator; /* Creator */
        pnt++;
        returnValue++;
      }
      /* We have no sent it */
      q->packSent = sentState;
    }
    q = ShellsTail(q);
  }
  return returnValue;
}

/*********************************************************
*NAME:          shellsNetExtract
*AUTHOR:        John Morrison
*CREATION DATE:  6/3/99
*LAST MODIFIED: 23/9/00
*PURPOSE:
* Network shells data have arrived. Add them to our 
* shells structure here.
*  
*ARGUMENTS:
*  value    - Pointer to shells structure
*  pb       - Pointer to the pillboxes structure
*  buff     - Pointer to a buffer to hold the shells 
*             net data
*  dataLen  - Length of the data
*  isServer - TRUE if we are the game server.
*********************************************************/
void shellsNetExtract(GameSim *sim, shells *value, pillboxes *pb, BYTE *buff, BYTE dataLen, bool isServer, tank *tanks) {
  BYTE pos;   /* Position through the data we are */
  BYTE *pnt;  /* Pointer to offset in the buffer */
  shells q;   /* Temp pointer to hold additions to the shells structure */
  WORLD conv; /* Used in the conversion */
  BYTE mx;    /* Map X position */
  BYTE my;    /* Map Y position */
  BYTE self;  /* Player Number */
  WORLD wx;   /* An Item */
  WORLD wy;
  WORLD twx, twy;
  TURNTYPE tt;
  BYTE length;
  BYTE owner;
  bool onBoat;
  BYTE creator;
  bool shouldAdd;
  BYTE amount;
  tank *tnk;
  double dummy;
  int xAdd;
  int yAdd;

  self = sim->viewPlayer;
  pos = 0;
  pnt = buff;
  q = NULL;

  while (pos < dataLen) {
    shouldAdd = FALSE;
    /* Get each Data item out */
    memcpy(&wx, pnt, sizeof(WORLD)); /* X */
    pnt += sizeof(WORLD);
    pos += sizeof(WORLD);
    memcpy(&wy, pnt, sizeof(WORLD)); /* Y */
    pnt += sizeof(WORLD);
    pos += sizeof(WORLD);
    memcpy(&tt, pnt, sizeof(TURNTYPE)); /* Angle */
    pnt += sizeof(TURNTYPE);
    pos += sizeof(TURNTYPE);
    length = *pnt; /* Length */ 
    pnt++;
    pos++;
    owner = *pnt; /* Owner */ 
    pnt++;
    pos++;
    onBoat = *pnt; /* On boat */
    pnt++;
    pos++;
    creator = *pnt; /* Who or what created it */
    pnt++;
    pos++;

    /* Check to see if we should add to it */
    if (isServer == TRUE) {
      if (owner != NEUTRAL) {
        tnk = (tanks != NULL) ? &tanks[creator] : NULL;
        tankGetWorld(tnk, &twx, &twy);
        amount = tankGetShells(tnk);
        if (amount > 0) {
          if (utilIsItemInRange(twx, twy, wx, wy, 512, &dummy) == TRUE) {
            amount--;
            tankSetShells(tnk, amount);
            shouldAdd = TRUE;
          }
        }
      } else {
        /* Fired from a pill - Check locality */
        utilCalcDistance(&xAdd, &yAdd, tt, SHELL_SPEED);
        xAdd *=2;
        yAdd *=2;
		// Becuase the pillbox fires from the 'center' of the pill, this doesn't translate perfectly back to the pillbox on the east and south side
		// so we have to add 1 world coordinate to the distance check, so that it properly determines that the pillbox did indeed fire the shells.
		// Ultimately this should be fixed by making winbolo fire a pillbox from the turrets of the pillbox, instead of from the 'center' so this is just a 
		// hack to get the code functioning correctly again. This problem was revealed becuase the new rounding code makes the bullets move 1 world coordinate
		// more and this moved it just enough that this check no longer works properly.
		if (xAdd == 64){
			xAdd++;
		}
		if (yAdd == 64){
			yAdd++;
		}
		/* If a pill exists at the location of ??? and a dead pill does not exist at that same location */
        if ((pillsExistPos(pb, (BYTE) ((WORLD) (wx-xAdd) >> M_W_SHIFT_SIZE), (BYTE) ((WORLD) (wy-yAdd) >> M_W_SHIFT_SIZE)) == TRUE)
			&& (pillsDeadPos(pb, (BYTE) ((WORLD) (wx-xAdd) >> M_W_SHIFT_SIZE), (BYTE) ((WORLD) (wy-yAdd) >> M_W_SHIFT_SIZE)) == FALSE)) {
          shouldAdd = TRUE;
        }
      }
    } else if (creator != self) {
      shouldAdd = TRUE;
    }

    if (length > 68 && owner == NEUTRAL) { /* Added length check to stop cheating */
      shouldAdd = FALSE;
    } else if (length > 52 && owner != NEUTRAL) {
		/* FIXME: Check shells fired from a tank are near the tank that fired them, they have sufficent shells etc */
      /* Tank max length */
      shouldAdd = FALSE;
    }


    /* Add it if required */
    if (shouldAdd == TRUE) {
      New(q);
      if (isServer == TRUE) {
        q->packSent = FALSE;
      } else {
        q->packSent = TRUE;
      }
      q->x = wx;
      q->y = wy;
      q->angle = tt;
      q->shellDead = FALSE;
      q->compensationTicks = 0;
      q->length = length;
      q->owner = owner;
      q->onBoat = onBoat;
      q->creator = creator;
      utilCalcDistanceHP(&q->xStep, &q->yStep, tt, SHELL_SPEED);
      q->xAcc = 0;
      q->yAcc = 0;
      /* Add it to the structure */
      q->next = *value;
      q->prev = NULL;
      if (NonEmpty(*value)) {
        (*value)->prev = q;
      }
      *value = q;
    }
  }
  
  /* Play the last sound event if exist */
  if (q != NULL) {
    conv = q->x;
    conv >>= TANK_SHIFT_MAPSIZE;
    mx = (BYTE) conv;
    conv = q->y;
    conv >>= TANK_SHIFT_MAPSIZE;
    my = (BYTE) conv;
    sim->callbacks.soundDist(sim->callbacks.ctx, shootNear, mx, my);
  }
}

/*********************************************************
*NAME:          shellsGetBrainShellsInRect
*AUTHOR:        John Morrison
*CREATION DATE: 26/11/99
*LAST MODIFIED: 28/11/99
*PURPOSE:
*  Makes the brain shell info for each shell inside the
*  rectangle formed by the function parameters.
*
*ARGUMENTS:
*  value     - Pointer to the shells structure
*  leftPos   - Left position of rectangle
*  rightPos  - Right position of rectangle
*  topPos    - Top position of rectangle
*  bottomPos - Bottom position of rectangle
*********************************************************/
void shellsGetBrainShellsInRect(ClientSim *cs, GameSim *sim, shells *value, BYTE leftPos, BYTE rightPos, BYTE topPos, BYTE bottomPos) {
  shells position; /* The position in the stack of items */
  BYTE owner;      /* Owner of the item */
  WORLD conv;      /* Used in converting items world co-ordinates */
  BYTE mx;         /* Shell X and Y Positions */
  BYTE my; 
  BYTE playerNum;  /* Our player number       */

  playerNum = sim->viewPlayer;
  position = *value;

/* typedef struct
	{
	OBJECT object; = 3
	WORLD_X x;
	WORLD_Y y;
	WORD idnum;
	BYTE direction;
	BYTE info;
	} ObjectInfo;
*/

  while (NonEmpty(position)) {
    conv = position->x;
    conv >>= TANK_SHIFT_MAPSIZE;
    mx = (BYTE) conv;
    conv = position->y;
    conv >>= TANK_SHIFT_MAPSIZE;
    my = (BYTE) conv;
    if (mx >= leftPos && mx <= rightPos && my >= topPos && my <= bottomPos) {
      /* In the rectangle */
      if (position->owner == NEUTRAL) {
        owner = SHELLS_BRAIN_NEUTRAL;
      } else if (playersIsAllie(&sim->plyrs, playerNum, position->owner) == TRUE) {
        owner = SHELLS_BRAIN_FRIENDLY;
      } else {
        owner = SHELLS_BRAIN_HOSTILE;
      }
      brainDataAddObject(cs, SHELLS_BRAIN_OBJECT_TYPE, position->x, position->y, 0, utilGet16Dir(position->angle), owner, 0);
    }
    position = ShellsTail(position);
  }
}
