/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
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

/* ── Pure shell-physics primitives ────────────────────────────────
 * These are the single source of truth for shell motion. Both the
 * live engine (shellsUpdate, shellsAddItem) and the brain's
 * stateless trajectory simulator call into these so the trajectory
 * math can't drift between them. Each takes the shell numbers it
 * needs: the engine reads them off sim->rules, the brain off the
 * pathfinder the bot manager pushed to. */

void shellApplyStartOffset(WORLD *x, WORLD *y, int xAdd, int yAdd,
                           int startAdd) {
  *x = (WORLD)((int)*x + startAdd * xAdd);
  *y = (WORLD)((int)*y + startAdd * yAdd);
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

int shellLifeTicks(float len, int shellLife, int startAdd) {
  int t = 1 + (int)(shellLife * len) - startAdd;
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
                   int shellSpeed, int startAdd,
                   WORLD *out_x, WORLD *out_y) {
  int xAdd, yAdd;
  utilCalcDistance(&xAdd, &yAdd, angle, shellSpeed);
  *out_x = tank_x;
  *out_y = tank_y;
  shellApplyStartOffset(out_x, out_y, xAdd, yAdd, startAdd);
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
*  target - The player a pillbox is firing at, NEUTRAL
*           for a tank's shell
*  pill   - The pill index of the pillbox firing it,
*           DMG_NO_PILL for a tank's shell
*  onBoat - Was the shell launched from a boat
*********************************************************/
void shellsAddItem(GameSim *sim, shells *value, WORLD x, WORLD y, TURNTYPE angle, TURNTYPE len, BYTE owner, BYTE target, BYTE pill, bool onBoat) {
  shells q;
  int xAdd;
  int yAdd;
  BYTE soundMX = (BYTE)(x >> TANK_SHIFT_MAPSIZE);  /* Tank position before shell offset */
  BYTE soundMY = (BYTE)(y >> TANK_SHIFT_MAPSIZE);

  utilCalcDistance(&xAdd, &yAdd, angle, sim->rules.shell_speed);
  shellApplyStartOffset(&x, &y, xAdd, yAdd, sim->rules.shell_start_add);
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
  q->length = (BYTE) shellLifeTicks(len, sim->rules.shell_life,
                                    sim->rules.shell_start_add);
  q->onBoat = onBoat;
  q->creator = sim->viewPlayer;
  q->owner = owner;
  q->target = target;
  q->pill = pill;
  q->passedTanks = 0;
  q->passedPills = 0;
  q->fireTick = sim->fireInputTick;  /* originating input tick, 0 when not a player fire */
  q->serverFireTick = 0;             /* filled from the shellFired callback below */
  q->packSent = FALSE;
  q->shellDead = FALSE;
  q->compensationTicks = sim->lagCompTicks;
  utilCalcDistanceHP(&q->xStep, &q->yStep, angle, sim->rules.shell_speed);
  q->xAcc = 0;
  q->yAcc = 0;

  q->next = *value;
  q->prev = NULL;
  if (NonEmpty(*value)) {
    (*value)->prev = q;
  }
  *value = q;

  /* Tell the server the trigger was pulled, now rather than when the shell
   * dies, and take back the SERVER tick it was recorded on.
   *
   * HOW LONG A SHELL IS IN THE AIR, measured rather than guessed. A tank
   * fires with len = sightLen / 2, and sightLen tops out at GUNSIGHT_MAX
   * (14, tank.h), so len is at most 7 map squares. q->length above is
   * shellLifeTicks(7, shell_life 8, shell_start_add 5) = 1 + 8*7 - 5 = 52,
   * and shellsUpdate runs once per GAME tick, which is every second server
   * tick (server_sim_tick.c simRunHalfStep). A full-range shell is
   * therefore 52 * 2 = 104 SERVER ticks in the air — longer than the
   * detector's 100-tick quiet second, which is why a fire log fed only by
   * shellDeath could not see a fourth shot at all.
   *
   * The returned tick is stamped on the shell so the death-time call can
   * name the same number; the client's own fireTick above never takes part
   * in a server-side rule. Server-only (NULL on the client), and inside the
   * sim tick, so it stays deterministic. */
  if (sim->callbacks.shellFired) {
    q->serverFireTick = sim->callbacks.shellFired(sim->callbacks.ctx, owner);
  }

  /* Play shoot sound at tank position (not offset shell position) */
  sim->callbacks.soundDistShoot(sim->callbacks.ctx, soundMX, soundMY, owner);

  if (sim->callbacks.recordPlayerAction) {
    sim->callbacks.recordPlayerAction(sim->callbacks.ctx, owner, PLAYER_ACTION_SHELL, soundMX, soundMY);
  }
}

/* A player's shell rewinds its targets to the view that player fired at.
 * The rewind is measured from the second-newest snapshot his client has
 * applied, which is the one it draws tanks at. Builders are not drawn at
 * that one: his client draws every builder, his own and other players',
 * from the newest snapshot it has applied, which is a snapshot (one game
 * tick) later. So against any builder the rewind is this much shorter. The
 * tank test in shellsCalcCollision keeps the full rewind.
 *
 * The one tick assumes the newest and second-newest applied snapshots are
 * one game tick apart. A lost snapshot widens that gap, and the builder
 * rewind is then one tick too far. That is what every builder rewind was
 * before this rule, so a lost snapshot makes it no worse than it was.
 *
 * It is taken off only a rewind longer than one tick, so it never takes a
 * rewind down to nothing. Single player sends no view tick and has no ping,
 * so its rewind is always one and single player is unchanged. A bot is the
 * same: view tick 0 and ping 0 give (0 + 20) / 20 = 1, so every bot shell is
 * rewound one tick against tanks and builders, although the bot reads the
 * server's present state. That is older than this rule and not changed
 * here. A rewind measured as exactly one, a client on a LAN, also keeps its
 * one against builders.
 *
 * Known small gap: a networked client that has not yet applied two snapshots
 * sends no view tick the server can use, and the server falls back to its
 * ping estimate (serverSimComputeLagCompTicks), which is two or more from
 * 40 ms of ping. The tick is taken off that estimate too, though it was not
 * measured from a snapshot. This happens when a client joins and again at
 * the start of each round, because a new round starts the server's tick
 * count from zero. It lasts until the client has applied two snapshots.
 *
 * A pill's shell is not rewound at all. It carries no rewind (owner NEUTRAL,
 * compensationTicks 0) and tests tanks and builders where they are now. */
#define SHELL_LGM_REWIND_LESS 1

/* Tests one builder against a shell that has just exploded, and kills him if
 * the blast caught him. Both places a shell explodes in shellsUpdate, a
 * collision and the end of its life, come through here, and every builder in
 * the game is tested, whoever owns him.
 *
 * THE RULE. For a shell fired from a player's tank, the shooter's screen
 * decides whether a builder dies. It makes no difference what the shell hit:
 * a tank, a pillbox, a wall, the ground, or nothing at the end of its range.
 *   - The blast caught the builder on the shooter's screen: he dies, even if
 *     the positions the server holds now say it missed, and even if he has
 *     got back into his tank since.
 *   - The blast missed him on the shooter's screen: he does not die, even if
 *     the positions the server holds now say it caught him.
 *   - The shooter's screen showed no builder (he was inside his tank, or
 *     dead): he does not die, even if he has left the tank since.
 * A pillbox's shell is not rewound: it tests the builder where he is now.
 *
 * The blast point is handed in twice:
 *   (nowX, nowY)   where the explosion is drawn and heard. It is what
 *                  shellsUpdate gives the explosion, the shell-death event
 *                  and the mine check.
 *   (seenX, seenY) the blast as the shooter's screen had it. It is kept as
 *                  its own argument because the shooter's-screen rule
 *                  names it.
 * Today (seenX, seenY) is always equal to (nowX, nowY), a tank hit included:
 * a shell that hit a tank explodes at its own place on its path, rewound or
 * not (shellsCalcCollision).
 *
 * What the shooter's screen showed of the builder is in the builder position
 * history, at the shell's rewind (SHELL_LGM_REWIND_LESS above):
 *   - the record of that frame says he was not out: no test, no kill;
 *   - the record of that frame has a position: he is tested at that position
 *     against (seenX, seenY). Both are the shooter's screen. A blast that
 *     caught him kills him even if he is back in his tank now: he dies on the
 *     tank, where it is now (lgmDeathCheckSeenOut). A tank that is itself
 *     dead, as it is when this same shell has just killed it, keeps the
 *     builder inside it: he went down with the tank and is not killed again;
 *   - the shell carries no rewind (a pill's shell), or there is no record
 *     that old yet (the start of a round, a player who has just joined): the
 *     shooter's screen is not known, so he is tested where he is now against
 *     (nowX, nowY). Both are the present. In his tank he is not out and
 *     lives. */
static void shellsBlastTestBuilder(GameSim *sim, shells shell, lgm *lgman, tank *tnk, WORLD nowX, WORLD nowY, WORLD seenX, WORLD seenY) {
	BYTE lgmOwner = (*lgman)->playerNum;
	uint8_t rewindTicks;
	WORLD lgmHitX = 0, lgmHitY = 0;
	PosHistoryLookup seen = POS_HISTORY_NO_RECORD;

	if (shell->compensationTicks > 0) {
		rewindTicks = shell->compensationTicks;
		/* Any builder: the shooter's client draws builders from the newest
		 * snapshot, one newer than the one the rewind was measured from. */
		if (rewindTicks > SHELL_LGM_REWIND_LESS) {
			rewindTicks -= SHELL_LGM_REWIND_LESS;
		}
	} else {
		/* A pill's shell (owner NEUTRAL, compensationTicks 0) tests the
		 * builder where he is now. */
		rewindTicks = 0;
	}

	if (rewindTicks > 0 && sim->lgmPosHistoryPtr != NULL &&
	    lgmOwner != NEUTRAL) {
		seen = posHistoryLookup(&sim->lgmPosHistoryPtr[lgmOwner],
		                        rewindTicks, &lgmHitX, &lgmHitY);
	}

	if (seen == POS_HISTORY_ABSENT) {
		/* The shooter's screen showed no builder: no test, no kill. */
	} else if (seen == POS_HISTORY_PRESENT) {
		/* The shooter's screen showed him out, here: against the blast as
		 * the shooter's screen had it. */
		lgmDeathCheckSeenOut(sim, lgman,
		                     lgmHitX, lgmHitY,
		                     seenX, seenY,
		                     shell->owner, shell->owner,
		                     DMG_SRC_SHELL, shell->pill,
		                     tnk);
	} else {
		/* No rewind, or no record as old as the shooter's screen: where he
		 * is now, against the blast where it is now. */
		lgmDeathCheck(sim, lgman,
		              nowX, nowY,
		              shell->owner, shell->owner,
		              DMG_SRC_SHELL, shell->pill, tnk);
	}
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

	/* --- near-shell memory, observation only -------------------------------
	 * Age every tank's "a shell was near me" countdown by one game tick.
	 * shellsUpdate is called exactly once per game tick from
	 * serverSimTick's world-systems block, so this is the tick clock -- no
	 * wall clock, no allocation, and the loop order is the caller's compacted
	 * tank array, so it is order-independent (each tank only touches its own
	 * counter).  Server only: the client never reads the counter, and its
	 * prediction re-runs ticks, which would age it more than once.
	 *
	 * The drowning site in tankUpdate reads this to split
	 * DEATH_CAUSE_DROWNED from DEATH_CAUSE_DROWNED_UNFORCED.  Nothing else
	 * in the sim reads it. */
	if (sim->isServer) {
		for (count = 0; count < numTanks; count++) {
			if (tk[count] != NULL && tk[count]->shellNearFrames > 0) {
				tk[count]->shellNearFrames--;
			}
		}
	}

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
			/* Observation only: re-arm the near-shell memory of every tank
			 * this shell passed close to.  Done before the collision test so
			 * a shell that is about to impact still counts.  A tank's OWN
			 * shell is skipped -- a bot fires constantly and its outgoing
			 * shell sits inside the ring for the first few frames, which
			 * would mark every self-inflicted drowning as "forced".  An
			 * ally's shell is left counting: erring towards "forced" makes
			 * DEATH_CAUSE_DROWNED_UNFORCED a conservative lower bound on
			 * drownings the bot caused itself. */
			if (sim->isServer) {
				for (count = 0; count < numTanks; count++) {
					int32_t nearDX;
					int32_t nearDY;
					if (tk[count] == NULL) {
						continue;
					}
					if (gameSimGetTankPlayer(sim, &tk[count]) == position->owner) {
						continue;
					}
					nearDX = (int32_t)newX - (int32_t)tk[count]->x;
					nearDY = (int32_t)newY - (int32_t)tk[count]->y;
					if (nearDX > -TANK_SHELL_NEAR_WU && nearDX < TANK_SHELL_NEAR_WU &&
					    nearDY > -TANK_SHELL_NEAR_WU && nearDY < TANK_SHELL_NEAR_WU &&
					    (nearDX * nearDX + nearDY * nearDY) < TANK_SHELL_NEAR_WU_SQUARED) {
						tk[count]->shellNearFrames = TANK_SHELL_NEAR_MEMORY_FRAMES;
					}
				}
			}
			/* Check for colision */
			uint8_t shellOutcome = SHELL_OUTCOME_IMPACT;
			/* The blast as the shooter's screen had it, for the builder test
			 * only. The same point as newX, newY. */
			WORLD seenX = newX;
			WORLD seenY = newY;
			if ((shellsCalcCollision(sim, tk, &newX, &newY, position, numTanks, &shellOutcome, &seenX, &seenY)) == TRUE)
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
				/* newX, newY are where the shell ended: the middle of the
				 * square for a pillbox, base or terrain hit, and for a tank
				 * hit the shell's own place on its path, rewound or not
				 * (see shellsCalcCollision). Everything below is at that one
				 * place. The builder test is also handed seenX, seenY, the
				 * blast as the shooter's screen had it, which is the same
				 * point (shellsBlastTestBuilder). */
				explosionsAddItem(&sim->expl, sx,sy,spx,spy,EXPLOSION_START);
				/* Debug: record the exact world-pixel hit for BrainTest viz */
				shellsDebugHitLogAdd((int)newX, (int)newY, 0, position->owner);
				if (sim->callbacks.explosion) sim->callbacks.explosion(sim->callbacks.ctx, sx, sy, spx, spy);
				/* Server-only: tell the shell's owner their shell ended so the
				 * firing client can cull its predicted ghost and draw the impact
				 * at the true position. Reached once per shell (collision path);
				 * NULL on the client, which is not authoritative over shell death. */
				if (sim->callbacks.shellDeath) sim->callbacks.shellDeath(sim->callbacks.ctx, position->fireTick, position->serverFireTick, position->owner, newX, newY, shellOutcome);
				minesExpAddItem(sim, &sim->minesExplosions, mp, bmx, bmy);
				count = 0;
				while (count < numTanks) {
					/* lgms[count] is the ADDRESS of an lgmen[] slot, so it is never
					 * NULL — what can go away is the slot's CONTENTS, when a player is
					 * removed after the caller snapshotted this array. lgmDeathCheck
					 * guards *lgman itself, but shellsBlastTestBuilder reads
					 * playerNum first and lgmDeathCheckAtPosition has no guard at all. */
					if (lgms && lgms[count] != NULL && *lgms[count] != NULL) {
						shellsBlastTestBuilder(sim, position, lgms[count],
						                       &tk[count], newX, newY,
						                       seenX, seenY);
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
			/* Server-only: shell reached end of life without a collision.
			 * Reached once per shell (expiry path); same owner-closure as the
			 * collision branch but with the shell's own end-of-life position. */
			if (sim->callbacks.shellDeath) sim->callbacks.shellDeath(sim->callbacks.ctx, position->fireTick, position->serverFireTick, position->owner, position->x, position->y, SHELL_OUTCOME_EXPIRED);
			minesExpAddItem(sim, &sim->minesExplosions, mp, bmx, bmy);
			count = 0;
			while (count < numTanks) {
				/* lgms[count] is the ADDRESS of an lgmen[] slot, so it is never
				 * NULL — what can go away is the slot's CONTENTS, when a player is
				 * removed after the caller snapshotted this array. lgmDeathCheck
				 * guards *lgman itself, but shellsBlastTestBuilder reads
				 * playerNum first and lgmDeathCheckAtPosition has no guard at all. */
				if (lgms && lgms[count] != NULL && *lgms[count] != NULL) {
					shellsBlastTestBuilder(sim, position, lgms[count],
					                       &tk[count], position->x, position->y,
					                       position->x, position->y);
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
      screenBulletsAddItem(sBullets, x, y, px, py, (BYTE) (frame + SHELL_START_EXPLODE+1), (BYTE) q->x, (BYTE) q->y); 
    }
    q = ShellsTail(q);
  }
}

/* Whether this shell may hit the tank or pill it has reached, which the host
 * is asked once per shell and target. A target it was let through is noted
 * on the shell and left alone after that, so the question is not put again on
 * every tick the two overlap and a script cannot change its mind halfway
 * through a tank. index is the tank slot or the pill index; a number past the
 * sixteen bits the notes hold is asked every time rather than noted. */
static bool shellsMayHit(GameSim *sim, shells shell, BYTE kind, BYTE index) {
	uint16_t *passed = (kind == HIT_KIND_TANK) ? &shell->passedTanks
	                                           : &shell->passedPills;
	uint16_t bit = (index < 16) ? (uint16_t) (1u << index) : 0;

	if ((*passed & bit) != 0) {
		return FALSE;
	}
	if (gameSimCanHit(sim, shell->owner, kind, index, shell->pill) == FALSE) {
		*passed = (uint16_t) (*passed | bit);
		return FALSE;
	}
	return TRUE;
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
*  shell    - The shell, for its angle, owner, boat flag,
*             lag compensation, firing pill and the
*             targets can_hit has already let it pass
*  numTanks - Number of tanks in the array
*  outSeenX - On a collision, the X of the blast as the
*             shooter's screen had it, for the builder test
*             (shellsBlastTestBuilder). The same as xValue.
*             May be NULL
*  outSeenY - Y, as outSeenX
*********************************************************/
bool shellsCalcCollision(GameSim *sim, tank *tk, WORLD *xValue, WORLD *yValue, shells shell, BYTE numTanks, uint8_t *outOutcome, WORLD *outSeenX, WORLD *outSeenY) {
	map *mp = &sim->mp;
	pillboxes *pb = &sim->pb;
	bases *bs = &sim->bs;
	TURNTYPE angle = shell->angle;       /* The angle the shell is travelling */
	BYTE owner = shell->owner;           /* Who fired the shell */
	bool onBoat = shell->onBoat;         /* Was the shell launched from a boat */
	uint8_t compensationTicks = shell->compensationTicks;
	BYTE pillHit;     /* The pill index on the square, DMG_NO_PILL for none */
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
	/* Default outcome for any collision that isn't a tank hit/kill
	 * (terrain, pillbox, base). The tank-hit switch below overrides it. */
	if (outOutcome != NULL) {
		*outOutcome = SHELL_OUTCOME_IMPACT;
	}
	/* Convert the position to Map co-ords */
	conv = *xValue;
	conv >>= TANK_SHIFT_MAPSIZE;
	mapX = (BYTE) conv;
	conv = *yValue;
	conv >>= TANK_SHIFT_MAPSIZE;
	mapY = (BYTE) conv;

	/* Pill is hit by the shell, unless the host lets the shell pass it, in
	 * which case the pill is not there as far as this shell is concerned and
	 * the tests below carry on as for an empty square. */
	pillHit = pillsHitSlot(pb, mapX, mapY);
	if (pillHit != DMG_NO_PILL && shellsMayHit(sim, shell, HIT_KIND_PILL, pillHit) == TRUE) {
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
			pillsDamagePos(sim, mapX, mapY, TRUE, TRUE, owner, shell->pill);
		} else if (owner == gameSimGetTankPlayer(sim, tk)) {
			pillsDamagePos(sim, mapX, mapY, FALSE, TRUE, owner, shell->pill);
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
				} else {
					/* A pill's shell (owner NEUTRAL, compensationTicks 0) tests
					 * the tank where it is now. */
					rewindTicks = 0;
				}

				/* Unlike the builder, a tank the shooter's screen showed as dead
				 * (posHistoryGet is FALSE for that) is tested where it is now, so
				 * a shell can hit a tank that has just respawned before the
				 * shooter saw it. This is on purpose: the case is rare and the
				 * tank really is there. */
				if (rewindTicks > 0 && sim->posHistoryPtr != NULL &&
				    targetPlayer != NEUTRAL &&
				    posHistoryGet(&sim->posHistoryPtr[targetPlayer],
				                  rewindTicks, &hitCheckX, &hitCheckY)) {
					useRewound = TRUE;
				}

				if (!useRewound && tk[count] != NULL) {
					tankGetWorld(&tk[count], &hitCheckX, &hitCheckY);
				}

				/* A shell the host lets pass a tank carries on as if the tank
				 * were not there: no damage, no knockback, no sound, and the
				 * tanks after this one are still tested. Asked only for a
				 * shell that would otherwise land. */
				if (tankShellInHitZone(sim, &(tk[count]), hitCheckX, hitCheckY,
				                       *xValue, *yValue) == TRUE &&
				    shellsMayHit(sim, shell, HIT_KIND_TANK, targetPlayer) == FALSE) {
					th = TH_MISSED;
				} else if (useRewound) {
					th = tankIsTankHitAtPosition(sim, &(tk[count]),
					                              hitCheckX, hitCheckY,
					                              *xValue, *yValue, angle, owner,
					                              shell->pill);
				} else {
					th = tankIsTankHit(sim, &(tk[count]), *xValue, *yValue, angle,
					                   owner, shell->pill);
				}

				/* A shell that hit a tank explodes at its own place, on its
				 * path, as in the 2.1 release: xValue and yValue are left as
				 * they are. That holds for a rewound hit too. The hit may have
				 * been decided against where the tank was on the shooter's
				 * screen (useRewound); the blast point is still where the
				 * shell is on the server on this tick. It is not moved to
				 * where the tank is now, so the explosion does not follow the
				 * tank. Whether the shell hit is decided above and is not
				 * touched here.
				 *
				 * The hit sound here, the start of a killed tank's explosion
				 * here, and, through xValue and yValue, everything shellsUpdate
				 * draws or sounds at the place the shell ended (the explosion
				 * graphic, the shell-death event, the debug hit log) and its
				 * mine check all use this one point. The builder test uses it
				 * too, as the blast the shooter's screen had (outSeenX). */

				switch (th) {
					case TH_HIT:
						returnValue = TRUE;
						if (outOutcome != NULL) *outOutcome = SHELL_OUTCOME_TANK_HIT;
						sim->callbacks.soundDistTankHit(sim->callbacks.ctx, mapX, mapY, targetPlayer);
						break;
					case TH_KILL_SMALL:
						returnValue = TRUE;
						if (outOutcome != NULL) *outOutcome = SHELL_OUTCOME_TANK_KILL;
						tkExplosionAddItem(sim, *xValue, *yValue, angle, (TURNTYPE) sim->rules.tank_explosion_length, TK_SMALL_EXPLOSION, targetPlayer);
						sim->callbacks.soundDistTankHit(sim->callbacks.ctx, mapX, mapY, targetPlayer);
						sim->callbacks.tankKill(sim->callbacks.ctx, owner, targetPlayer, LAST_DEATH_BY_SHELL, 0);
						break;
					case TH_KILL_BIG:
						returnValue = TRUE;
						if (outOutcome != NULL) *outOutcome = SHELL_OUTCOME_TANK_KILL;
						tkExplosionAddItem(sim, *xValue, *yValue, angle, (TURNTYPE) sim->rules.tank_explosion_length, TK_LARGE_EXPLOSION, targetPlayer);
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
						basesDamagePos(sim, mapX, mapY, owner);
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
					basesDamagePos(sim, mapX, mapY, owner);
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
					mapSetPos(sim, mp, mapX, mapY, (buildingAddItem(sim, &sim->blds, mapX, mapY)), FALSE, FALSE);
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
					mapSetPos(sim, mp, mapX, mapY, (buildingAddItem(sim, &sim->blds, mapX, mapY)), FALSE, FALSE);
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
						newTerrain = grassAddItem(sim, &sim->grs, mapX, mapY);
						mapSetPos(sim, mp, mapX, mapY, newTerrain, FALSE, FALSE);
						if (newTerrain == RIVER) {
							floodAddItem(&sim->ff, mapX, mapY,
                       (BYTE) sim->rules.flood_fill_ticks);
						}
					}
					break;
				case SWAMP:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, SWAMP+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						newTerrain = swampAddItem(sim, &sim->swp, mapX, mapY);
						mapSetPos(sim, mp, mapX, mapY, newTerrain, FALSE, FALSE);
						if (newTerrain == RIVER) {
							floodAddItem(&sim->ff, mapX, mapY,
                       (BYTE) sim->rules.flood_fill_ticks);
						}
					}
					break;
				case RUBBLE:
					if (isMine == TRUE) {
						mapSetPos(sim, mp, mapX, mapY, RUBBLE+MINE_SUBTRACT, FALSE, FALSE);
					} else {
						newTerrain = rubbleAddItem(sim, &sim->rbl, mapX, mapY);
						mapSetPos(sim, mp, mapX, mapY, newTerrain, FALSE, FALSE);
						if (newTerrain == RIVER) {
							floodAddItem(&sim->ff, mapX, mapY,
                       (BYTE) sim->rules.flood_fill_ticks);
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

	/* The blast as the shooter's screen had it: the same point as where
	 * the effects go, for every blast. */
	if (returnValue == TRUE && outSeenX != NULL && outSeenY != NULL) {
		*outSeenX = *xValue;
		*outSeenY = *yValue;
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
