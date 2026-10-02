/*
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
 *Name:          Observation Builder
 *Filename:      obs_builder.c
 *Purpose:
 *  Builds V3 WinBoloObs from BrainInfo for in-game ML
 *  brain inference.
 *
 *  obsBuildMultiView is the one way in: the tank's own view
 *  rect, plus the rect around each pillbox it or an ally
 *  owns, gathered into one observation.
 *
 *  ── The scale these observations are on ──────────────────
 *  An observation is a vector a model was trained against,
 *  so the numbers in it have to mean the same thing on every
 *  tick of every episode. The entries that normalise a stock
 *  against what full means divide by a written-out number —
 *  armour and the tank's other stocks by 40, a pillbox's
 *  armour by 15, a base's by 90 — and those stay written out
 *  on purpose. They are not oversights left behind by the
 *  move to the rules table.
 *
 *  Dividing by the live rule instead would make the vector
 *  rescale itself the moment a scenario changed one, so a
 *  trajectory recorded before the change and one recorded
 *  after would be on two different scales with nothing in
 *  the data saying which. A model trained across that reads
 *  them as one world.
 *
 *  What keeps that honest is the check the builder opens
 *  with: it refuses to build at all on a sim whose rules are
 *  not the classic ones, rather than handing back a vector
 *  on a scale nothing was trained for.
 *********************************************************/

#include <string.h>
#include <math.h>

#include "obs_builder.h"
#include "gametype.h"
#include "input_packet.h"
#include "lgm.h"
#include "pillbox.h"
#include "bases.h"
#include "shells.h"
#include "players.h"
#include "tank.h"
#include "util.h"
#include "client_sim.h"
#include "game_sim.h"
#include "sim_rules.h"           /* simRulesAreClassic — what the scale needs */
#include "../common/wb_log.h"    /* the line a refused build leaves */

/* Helper to determine allegiance from object info flags */
static int8_t obsGetAllegiance(const ObjectInfo *o, BYTE selfPlayer) {
    (void)selfPlayer;
    if (o->info & OBJECT_NEUTRAL) return WBGYM_ALLEG_NEUTRAL;
    if (!(o->info & OBJECT_HOSTILE)) return WBGYM_ALLEG_SELF; /* own or ally */
    return WBGYM_ALLEG_ENEMY;
}

/* Convert an ObjectInfo into a WinBoloEntity and append to entity list.
 * cs is used for tank armour and flags. The NULL test below is what is left
 * of a caller that had no ClientSim to pass; the one caller there is now
 * always has one.
 * Returns true if added, false if skipped (dedup or full). */
static bool obsAddObjectAsEntity(const ObjectInfo *o, float self_wx, float self_wy,
                                 BYTE selfPlayer, struct ClientSim *cs,
                                 WinBoloObs *obs, uint16_t *ne) {
    if (*ne >= WBGYM_MAX_ENTITIES) return false;

    WinBoloEntity *ent = &obs->entities[*ne];
    float owx = (float)o->x;
    float owy = (float)o->y;

    switch (o->object) {
    case OBJECT_TANK: {
        ent->rx = (owx - self_wx) / 256.0f;
        ent->ry = (owy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_TANK;
        ent->allegiance = obsGetAllegiance(o, selfPlayer);
        ent->direction = (float)o->direction / 256.0f;
        ent->speed = (float)o->speed / 128.0f;
        ent->flags = 0;
        if (!(o->info & OBJECT_HOSTILE) && !(o->info & OBJECT_NEUTRAL))
            ent->flags |= WBGYM_FLAG_IS_SELF;
        ent->id = (uint8_t)o->idnum;

        /* Tank armour and flags from ClientSim (matches gymBuildObs) */
        if (cs != NULL && o->idnum < MAX_TANKS) {
            GameSim *gs = clientSimGetGameSim(cs);
            BYTE armour = tankGetArmour(&gs->tanks[o->idnum]);
            bool tankDead = tankIsDestroyed(&gs->tanks[o->idnum]);
            ent->strength = tankDead ? 0.0f : (float)armour / 40.0f;
            if (tankIsOnBoat(&gs->tanks[o->idnum])) ent->flags |= WBGYM_FLAG_IN_BOAT;
            if (tankDead) ent->flags |= WBGYM_FLAG_DEAD;
            if (tankIsNewTank(&gs->tanks[o->idnum])) ent->flags |= WBGYM_FLAG_DEAD;
            /* Hidden check for non-self tanks */
            if (o->idnum != selfPlayer) {
                WORLD tx_w, ty_w;
                tankGetWorld(&gs->tanks[o->idnum], &tx_w, &ty_w);
                if (utilIsTankInTrees(&gs->mp, &gs->pb,
                                     &gs->bs, tx_w, ty_w))
                    ent->flags |= WBGYM_FLAG_HIDDEN;
            }
        } else {
            ent->strength = 0.0f;
        }

        (*ne)++;
        return true;
    }
    case OBJECT_SHOT:
        ent->rx = (owx - self_wx) / 256.0f;
        ent->ry = (owy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_SHELL;
        ent->allegiance = obsGetAllegiance(o, selfPlayer);
        ent->direction = (float)o->direction / 256.0f;
        ent->speed = 0.0f;
        ent->strength = 0.0f;
        ent->flags = 0;
        ent->id = 0xFF;
        (*ne)++;
        return true;
    case OBJECT_PILLBOX:
        ent->rx = (owx - self_wx) / 256.0f;
        ent->ry = (owy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_PILLBOX;
        ent->allegiance = obsGetAllegiance(o, selfPlayer);
        ent->direction = 0.0f;
        ent->speed = 0.0f;
        ent->strength = (float)o->pillbox_strength / 15.0f;
        ent->flags = 0;
        ent->id = (uint8_t)o->idnum;
        (*ne)++;
        return true;
    case OBJECT_REFBASE:
        ent->rx = (owx - self_wx) / 256.0f;
        ent->ry = (owy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_BASE;
        ent->allegiance = obsGetAllegiance(o, selfPlayer);
        ent->direction = 0.0f;
        ent->speed = 0.0f;
        /* The brain feed gives a friendly base's armour in fifths and any
           other base 1 above the capture threshold or 0 at or below it
           (basesGetBrainBaseInRect). Scaled to what the gym gives. */
        if (ent->allegiance == WBGYM_ALLEG_SELF) {
            ent->strength = (float)o->refbase_strength * 5.0f / 90.0f;
        } else {
            ent->strength = o->refbase_strength > 0 ? 1.0f : 0.0f;
        }
        ent->flags = 0;
        ent->id = (uint8_t)o->idnum;
        (*ne)++;
        return true;
    case OBJECT_BUILDMAN:
        ent->rx = (owx - self_wx) / 256.0f;
        ent->ry = (owy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_LGM;
        ent->allegiance = obsGetAllegiance(o, selfPlayer);
        ent->direction = (float)o->direction / 256.0f;
        ent->speed = 0.0f;
        ent->strength = 0.0f;
        ent->flags = 0;
        if (!(o->info & OBJECT_HOSTILE) && !(o->info & OBJECT_NEUTRAL))
            ent->flags |= WBGYM_FLAG_IS_SELF;
        ent->id = (uint8_t)o->idnum;
        (*ne)++;
        return true;
    case OBJECT_PARACHUTE:
        ent->rx = (owx - self_wx) / 256.0f;
        ent->ry = (owy - self_wy) / 256.0f;
        ent->type = WBGYM_ENT_PARACHUTE;
        ent->allegiance = obsGetAllegiance(o, selfPlayer);
        ent->direction = 0.0f;
        ent->speed = 0.0f;
        ent->strength = 0.0f;
        ent->flags = WBGYM_FLAG_DEAD;
        if (!(o->info & OBJECT_HOSTILE) && !(o->info & OBJECT_NEUTRAL))
            ent->flags |= WBGYM_FLAG_IS_SELF;
        ent->id = (uint8_t)o->idnum;
        (*ne)++;
        return true;
    default:
        return false;
    }
}

/* Helper: determine allegiance of a player relative to self (matches gymGetAllegiance) */
static int8_t obsGetPlayerAllegiance(BYTE playerNum, BYTE selfPlayer, PlayerBitMap alliesBits) {
    if (playerNum == selfPlayer) return WBGYM_ALLEG_SELF;
    if (playerNum == 0xFF) return WBGYM_ALLEG_NEUTRAL;
    if (alliesBits & (1u << playerNum)) return WBGYM_ALLEG_ALLY;
    return WBGYM_ALLEG_ENEMY;
}

ObsEventUse obsEventIsRead(uint8_t type) {
    switch (type) {
    case EVENT_SOUND:
    case EVENT_SOUND_SHOOT:
    case EVENT_SOUND_TANK_HIT:
    case EVENT_TANK_KILLED:
    case EVENT_TANK_HIT:
    case EVENT_PILL_CAPTURED:
    case EVENT_BASE_CAPTURED:
    case EVENT_PILL_KILLED:
    case EVENT_LGM_LOST:
    case EVENT_ASSISTANT_MSG:
        return OBS_EVENT_READ;

    /* Never raised by anything. */
    case EVENT_SHELL_FIRED:
    /* Server-only, and the sound of a builder laying a mine already arrives
     * as EVENT_SOUND manLayingMineNear. Hearing this one would tell the agent
     * where every hidden mine went in, which the game never tells a player. */
    case EVENT_MINE_PLACED:
    /* Shell impacts; the in-game brain's event filter drops them, so the gym
     * must not hear them either. Mine and tank explosions arrive as EVENT_SOUND
     * and are heard through that. */
    case EVENT_EXPLOSION:
    case EVENT_MINE_EXPLODED:
    case EVENT_TK_EXPLOSION:
    /* State the observation reads from the sim directly, or has no use for. */
    case EVENT_MAP_CHANGE:
    case EVENT_SERVER_MSG:
    case EVENT_PILL_UPDATE:
    case EVENT_BASE_UPDATE:
    case EVENT_PLAYER_LEAVE:
    case EVENT_MINE_VISIBLE:
    case EVENT_BASE_STOCK:
    case EVENT_PING:
    case EVENT_TANK_SPAWNED:
    case EVENT_LGM_LANDED:
    case EVENT_PILL_PLACED:
    case EVENT_PILL_PICKED_UP:
    case EVENT_BUILT:
        return OBS_EVENT_IGNORED;

    default:
        return OBS_EVENT_UNKNOWN;
    }
}

/* EVENT_TANK_HIT's pill byte for a shell no pillbox fired (DMG_NO_PILL in
 * the scenario surface, which this file does not include). */
#define OBS_NO_PILL 0xFF

static void obsAddEvent(WinBoloObs *obs, uint8_t ev) {
    if (obs->num_events < WBGYM_MAX_EVENTS) {
        obs->events[obs->num_events++] = ev;
    }
}

/* The observation's sound type for an EVENT_SOUND sound id. Ids with no type
 * of their own are generic; the id itself still travels in sound_id. */
static uint8_t obsSoundTypeFor(uint8_t soundId) {
    switch (soundId) {
    case manDyingNear:
    case manDyingFar:
        return WBGYM_SND_LGM_LOST;
    case mineExplosionNear:
    case mineExplosionFar:
    case bigExplosionNear:
    case bigExplosionFar:
        return WBGYM_SND_EXPLOSION;
    case manLayingMineNear:
        return WBGYM_SND_MINE_PLACE;
    default:
        return WBGYM_SND_GENERIC;
    }
}

/* Appends a positional sound when it is within 40 squares, or always when
 * always is set. */
static void obsAddSound(WinBoloObs *obs, const GameEvent *e, int tankTx,
                        int tankTy, uint8_t type, int8_t allegiance,
                        bool always) {
    float sx = (float)e->data[1] - (float)tankTx;
    float sy = (float)e->data[2] - (float)tankTy;

    if (obs->num_sounds >= WBGYM_MAX_SOUNDS) {
        return;
    }
    if (!always && (fabsf(sx) >= 40.0f || fabsf(sy) >= 40.0f)) {
        return;
    }
    WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
    snd->rx = sx;
    snd->ry = sy;
    snd->type = type;
    snd->allegiance = allegiance;
    snd->sound_id = e->data[0];
}

void obsBuildEventsFrom(const GameEvent *events, int count, bool fromServer,
                        BYTE selfPlayer, PlayerBitMap allies, int tankTx,
                        int tankTy, WinBoloObs *obs) {
    for (int i = 0; i < count; i++) {
        const GameEvent *e = &events[i];
        switch (e->type) {
        case EVENT_SOUND_TANK_HIT:
            /* [soundId, mx, my, hitPlayer]. It names the tank hit and nobody
               else, so it cannot say who dealt a hit; that comes from
               EVENT_TANK_HIT, and so does a hit received when the list has
               it (it also covers mines, which make no hit sound). */
            if (!fromServer && e->data[3] == selfPlayer) {
                obsAddEvent(obs, WBGYM_EVENT_HIT_RECEIVED);
            }
            obsAddSound(obs, e, tankTx, tankTy, WBGYM_SND_HIT_TANK,
                        obsGetPlayerAllegiance(e->data[3], selfPlayer, allies),
                        e->data[3] == selfPlayer);
            break;
        case EVENT_TANK_HIT:
            /* [victim, attacker, cause, amount, pill]. A hit dealt is one of
               the agent's own shells: the shot-accuracy reward divides these
               by the shots it fired. A mine it laid names it as the attacker
               too, and a pillbox shell carries a pill index; neither is a
               shot. (Pillbox shells name NEUTRAL today, and the pill check
               keeps that true if a pill's owner is ever credited.) */
            if (e->data[0] == selfPlayer) {
                obsAddEvent(obs, WBGYM_EVENT_HIT_RECEIVED);
            } else if (e->data[1] == selfPlayer &&
                       e->data[2] == LAST_DEATH_BY_SHELL &&
                       e->data[4] == OBS_NO_PILL) {
                obsAddEvent(obs, WBGYM_EVENT_HIT_DEALT);
            }
            break;
        case EVENT_TANK_KILLED:
            /* [killer, killed, deathCause, carriedPills]. A death nobody
               caused, drowning or the tank's own mine, names the dying tank
               as its killer; that is a death and not a kill. */
            if (e->data[1] == selfPlayer) {
                obsAddEvent(obs, WBGYM_EVENT_DEATH);
            } else if (e->data[0] == selfPlayer) {
                if (obsGetPlayerAllegiance(e->data[1], selfPlayer, allies) ==
                    WBGYM_ALLEG_ALLY) {
                    obsAddEvent(obs, WBGYM_EVENT_ALLY_KILLED);
                } else {
                    obsAddEvent(obs, WBGYM_EVENT_KILL);
                }
            }
            break;
        case EVENT_PILL_CAPTURED:
            if (e->data[0] == selfPlayer)
                obsAddEvent(obs, WBGYM_EVENT_PILL_CAPTURED);
            if (e->data[1] == selfPlayer)
                obsAddEvent(obs, WBGYM_EVENT_PILL_LOST);
            break;
        case EVENT_BASE_CAPTURED:
            if (e->data[0] == selfPlayer)
                obsAddEvent(obs, WBGYM_EVENT_BASE_CAPTURED);
            if (e->data[1] == selfPlayer)
                obsAddEvent(obs, WBGYM_EVENT_BASE_LOST);
            break;
        case EVENT_PILL_KILLED:
            /* [index, attacker] */
            if (e->data[1] == selfPlayer)
                obsAddEvent(obs, WBGYM_EVENT_PILL_KILLED);
            break;
        case EVENT_LGM_LOST:
            /* [victim, killer, quiet]; killer is NEUTRAL when nobody did it. */
            if (e->data[0] == selfPlayer) {
                obsAddEvent(obs, WBGYM_EVENT_LGM_LOST);
            } else if (e->data[1] == selfPlayer &&
                       obsGetPlayerAllegiance(e->data[0], selfPlayer, allies) ==
                           WBGYM_ALLEG_ENEMY) {
                obsAddEvent(obs, WBGYM_EVENT_ENEMY_LGM_KILLED);
            }
            break;
        case EVENT_SOUND_SHOOT:
            if (e->data[3] == selfPlayer) break;
            obsAddSound(obs, e, tankTx, tankTy, WBGYM_SND_SHOOT,
                        obsGetPlayerAllegiance(e->data[3], selfPlayer, allies),
                        false);
            break;
        case EVENT_SOUND:
            /* data[3] is whichever player's tick raised it, not who the sound
               belongs to, so it says nothing about allegiance. */
            obsAddSound(obs, e, tankTx, tankTy, obsSoundTypeFor(e->data[0]),
                        WBGYM_ALLEG_NEUTRAL, false);
            break;
        case EVENT_ASSISTANT_MSG:
            if (e->data[0] == selfPlayer) {
                obs->assistant_msg = e->data[1];
                switch (e->data[1]) {
                case ASSIST_MSG_MAN_DEAD:
                    obsAddEvent(obs, WBGYM_EVENT_ASSIST_MAN_DEAD);
                    break;
                case ASSIST_MSG_NO_TREE:
                case ASSIST_MSG_INSUFFICIENT_TREES:
                    obsAddEvent(obs, WBGYM_EVENT_ASSIST_NO_TREE);
                    break;
                case ASSIST_MSG_BUILDTANK:
                    obsAddEvent(obs, WBGYM_EVENT_ASSIST_BUILDTANK);
                    break;
                }
            }
            break;
        default:
            break;
        }
    }
}

/* Build events and sound events from BrainInfo events list */
static void obsBuildEvents(const BrainInfo *bi, WinBoloObs *obs) {
    PlayerBitMap alliesBits = bi->allies ? *(bi->allies) : 0;

    obsBuildEventsFrom(bi->events, bi->num_events, false,
                       (BYTE)bi->player_number, alliesBits, bi->tankx >> 8,
                       bi->tanky >> 8, obs);

    if (bi->assistant_msg != 0 && obs->assistant_msg == 0) {
        obs->assistant_msg = bi->assistant_msg;
    }
}

/* Build terrain grid from BrainInfo */
static void obsBuildTerrain(const BrainInfo *bi, WinBoloObs *obs) {
    int tank_tx = bi->tankx >> 8;
    int tank_ty = bi->tanky >> 8;
    const TERRAIN *world = bi->theWorld;

    for (int row = 0; row < WBGYM_SPATIAL_SIZE; row++) {
        for (int col = 0; col < WBGYM_SPATIAL_SIZE; col++) {
            int mx = tank_tx - 14 + col;
            int my = tank_ty - 14 + row;
            if (mx >= 0 && mx < 256 && my >= 0 && my < 256 && world != NULL) {
                BYTE raw = world[my * 256 + mx];
                obs->terrain[row][col] = (float)(raw & TERRAIN_MASK) / 15.0f;
                if (raw & TERRAIN_MINE) {
                    obs->mines_map[row][col] = 1.0f;
                }
            }
        }
    }
}

/* Build scalars with ClientSim data for accurate pill/base fracs, LGM, death_wait */
static void obsBuildScalarsCS(const BrainInfo *bi, struct ClientSim *cs, WinBoloObs *obs) {
    bool dead = bi->destroyed != 0;
    int tank_tx = bi->tankx >> 8;
    int tank_ty = bi->tanky >> 8;
    GameSim *gs = clientSimGetGameSim(cs);
    BYTE selfPlayer = clientSimGetMyPlayerNum(cs);
    PlayerBitMap alliesBits = bi->allies ? *(bi->allies) : 0;

    /* Scalars 0-11 */
    {
        unsigned armor = dead ? 0 : (unsigned)bi->armour;
        float dir_rad = (float)bi->direction * (2.0f * 3.14159265f / 256.0f);
        int32_t reload_ticks = gs->rules.tank_reload_ticks;

        obs->scalar[0]  = (float)armor / 40.0f;
        obs->scalar[1]  = (float)bi->shells / 40.0f;
        obs->scalar[2]  = (float)bi->mines / 40.0f;
        obs->scalar[3]  = (float)bi->trees / 40.0f;
        obs->scalar[4]  = (float)bi->speed / 128.0f;
        obs->scalar[5]  = sinf(dir_rad);
        obs->scalar[6]  = cosf(dir_rad);
        /* Ticks left to wait, over the rule that set them, the way the gym
           and the headless scale it. A reload rule of 0 fires every tick, so
           the counter is never set and there is never anything outstanding —
           0 there is the value the numerator gives anyway, and it keeps an
           infinity out of the vector. */
        obs->scalar[7]  = reload_ticks > 0
                              ? (float)bi->reload / (float)reload_ticks : 0.0f;
        obs->scalar[8]  = bi->inboat ? 1.0f : 0.0f;
        obs->scalar[9]  = bi->carriedpills > 0 ? 1.0f : 0.0f;
        obs->scalar[10] = (float)bi->carriedpills / 16.0f;
        obs->scalar[11] = dead ? 1.0f : 0.0f;
    }

    /* Scalars 12-16: pill/base fracs from ClientSim (matches gymBuildObs) */
    {
        int self_pills = 0, enemy_pills = 0, ally_pills = 0;
        int self_bases = 0, ally_bases = 0;
        BYTE np = pillsGetNumPills(&gs->pb);
        int total_pills = 0;
        for (BYTE pi = 1; pi <= np; pi++) {
            pillbox p;
            pillsGetPill(&gs->pb, &p, pi);
            if (pillsIsActive(&gs->pb, pi) == FALSE) continue;
            total_pills++;
            if (p.owner == 0xFF) continue;
            if (p.owner == selfPlayer) self_pills++;
            else if (alliesBits & (1u << p.owner)) ally_pills++;
            else enemy_pills++;
        }
        BYTE nb = basesGetNumBases(&gs->bs);
        int total_bases = 0;
        for (BYTE bsi = 1; bsi <= nb; bsi++) {
            base b;
            basesGetBase(&gs->bs, &b, bsi);
            if (basesIsActive(&gs->bs, bsi) == FALSE) continue;
            total_bases++;
            if (b.owner == 0xFF) continue;
            if (b.owner == selfPlayer) self_bases++;
            else if (alliesBits & (1u << b.owner)) ally_bases++;
        }
        float tp = total_pills > 0 ? (float)total_pills : 1.0f;
        float tb = total_bases > 0 ? (float)total_bases : 1.0f;
        obs->scalar[12] = (float)self_pills / tp;
        obs->scalar[13] = (float)enemy_pills / tp;
        obs->scalar[14] = (float)ally_pills / tp;
        obs->scalar[15] = (float)self_bases / tb;
        obs->scalar[16] = (float)ally_bases / tb;
    }

    /* Scalars 17-18 */
    obs->scalar[17] = (float)tank_tx / 256.0f;
    obs->scalar[18] = (float)tank_ty / 256.0f;

    /* Scalars 19-22 */
    obs->scalar[19] = (float)bi->gunrange / 14.0f;
    obs->scalar[20] = bi->hidden ? 1.0f : 0.0f;
    obs->scalar[21] = bi->newtank ? 1.0f : 0.0f;
    obs->scalar[22] = bi->tankobstructed ? 1.0f : 0.0f;

    /* Scalar 23: LGM status with parachute distinction (matches gymBuildObs) */
    {
        BYTE ms = bi->man_status;
        if (ms == LGM_BRAIN_INTANK) {
            obs->scalar[23] = 0.0f;
        } else if (ms == LGM_BRAIN_DEAD) {
            lgm *lg = &gs->lgmen[selfPlayer];
            if ((*lg)->isDead && !tankIsDestroyed(&gs->tanks[selfPlayer])) {
                obs->scalar[23] = 0.66f; /* parachuting */
            } else {
                obs->scalar[23] = 1.0f; /* actually dead */
            }
        } else {
            obs->scalar[23] = 0.33f; /* outside working */
        }
    }

    obs->scalar[24] = (float)bi->manobstructed / 2.0f;

    /* Scalar 25: death_wait from ClientSim (matches gymBuildObs) */
    obs->scalar[25] = (float)tankGetDeathWait(&gs->tanks[selfPlayer]) /
                      (float)gs->rules.tank_death_ticks;
}

/* Build pill/base lists and metadata from ClientSim (matches gymBuildObs) */
static void obsBuildMetaCS(const BrainInfo *bi, struct ClientSim *cs, WinBoloObs *obs) {
    float self_wx = (float)bi->tankx;
    float self_wy = (float)bi->tanky;
    bool dead = bi->destroyed != 0;
    GameSim *gs = clientSimGetGameSim(cs);
    BYTE selfPlayer = clientSimGetMyPlayerNum(cs);
    PlayerBitMap alliesBits = bi->allies ? *(bi->allies) : 0;

    /* LGM state */
    obs->man_rx = ((float)bi->man_x - self_wx) / 256.0f;
    obs->man_ry = ((float)bi->man_y - self_wy) / 256.0f;
    obs->man_direction = (float)bi->man_direction / 256.0f;

    /* Full-precision tank position */
    obs->tank_x = (float)bi->tankx / 256.0f;
    obs->tank_y = (float)bi->tanky / 256.0f;

    /* Pill list from ClientSim (matches gymBuildObs) */
    {
        BYTE np = pillsGetNumPills(&gs->pb);
        for (BYTE pi = 1; pi <= np && obs->num_pillboxes < WBGYM_MAX_PILLBOXES; pi++) {
            pillbox p;
            pillsGetPill(&gs->pb, &p, pi);
            if (pillsIsActive(&gs->pb, pi) == FALSE) continue;
            WinBoloPillObs *po = &obs->pillboxes[obs->num_pillboxes];
            po->tx = p.x;
            po->ty = p.y;
            po->armor = p.armour;
            po->owner = (p.owner == 0xFF) ? WBGYM_OWNER_NEUTRAL
                      : (p.owner == selfPlayer) ? WBGYM_OWNER_SELF
                      : (alliesBits & (1u << p.owner)) ? WBGYM_OWNER_ALLY
                      : WBGYM_OWNER_ENEMY;
            obs->num_pillboxes++;
        }
    }

    /* Base list from ClientSim (matches gymBuildObs) */
    {
        BYTE nb = basesGetNumBases(&gs->bs);
        for (BYTE bsi = 1; bsi <= nb && obs->num_bases < WBGYM_MAX_BASES; bsi++) {
            base b;
            basesGetBase(&gs->bs, &b, bsi);
            if (basesIsActive(&gs->bs, bsi) == FALSE) continue;
            WinBoloBaseObs *bo = &obs->bases[obs->num_bases];
            bo->tx = b.x;
            bo->ty = b.y;
            bo->owner = (b.owner == 0xFF) ? WBGYM_OWNER_NEUTRAL
                      : (b.owner == selfPlayer) ? WBGYM_OWNER_SELF
                      : (alliesBits & (1u << b.owner)) ? WBGYM_OWNER_ALLY
                      : WBGYM_OWNER_ENEMY;
            /* Expose stocks for friendly bases (matches gymBuildObs) */
            if (bo->owner == WBGYM_OWNER_SELF || bo->owner == WBGYM_OWNER_ALLY) {
                BYTE shellsAmt, minesAmt, armourAmt;
                basesGetStats(&gs->bs, bsi, &shellsAmt, &minesAmt, &armourAmt);
                bo->shells = shellsAmt;
                bo->mines = minesAmt;
                bo->armour = armourAmt;
            }
            obs->num_bases++;
        }
    }

    obs->dead = dead ? 1 : 0;
    obs->tick = bi->server_tick;
}

/* ------------------------------------------------------------------ */
/* Multi-view obs builder                                              */
/* ------------------------------------------------------------------ */

/* Check if a pill's 15x15 rect is fully inside the tank's 29x29 rect.
 * If so, all objects in the pill rect are already gathered. */
static bool obsPillCoveredByTankView(int pill_x, int pill_y, int tank_tx, int tank_ty) {
    int pill_left   = pill_x - 7;
    int pill_right  = pill_x + 7;
    int pill_top    = pill_y - 7;
    int pill_bottom = pill_y + 7;
    int tank_left   = tank_tx - 14;
    int tank_right  = tank_tx + 14;
    int tank_top    = tank_ty - 14;
    int tank_bottom = tank_ty + 14;
    return (pill_left >= tank_left && pill_right <= tank_right &&
            pill_top >= tank_top && pill_bottom <= tank_bottom);
}

/* Check if a tank entity with given idnum already exists in the entity list */
static bool obsTankAlreadySeen(const WinBoloObs *obs, uint16_t ne, uint8_t idnum) {
    for (uint16_t i = 0; i < ne; i++) {
        if (obs->entities[i].type == WBGYM_ENT_TANK && obs->entities[i].id == idnum)
            return true;
    }
    return false;
}

/* Check if an LGM/parachute entity with given idnum already exists */
static bool obsLgmAlreadySeen(const WinBoloObs *obs, uint16_t ne, uint8_t idnum) {
    for (uint16_t i = 0; i < ne; i++) {
        if ((obs->entities[i].type == WBGYM_ENT_LGM ||
             obs->entities[i].type == WBGYM_ENT_PARACHUTE) &&
            obs->entities[i].id == idnum)
            return true;
    }
    return false;
}

/* What an operator sees when a build is refused. Said once a run: the check
 * below runs on every build, and a rule a scenario has moved stays moved for
 * the rest of the round, so saying it every tick would bury everything else
 * in the log and tell nobody anything new. */
static void obsWarnNotClassic(const GameSim *gs) {
    static bool said = false;

    if (said) {
        return;
    }
    said = true;
    if (gs == NULL) {
        WB_LOG_ERROR(WB_LOG_CAT_SIM,
                     "observation refused: no simulation to read the rules "
                     "from. The ML brain will not act.");
        return;
    }
    WB_LOG_ERROR(WB_LOG_CAT_SIM,
                 "observation refused: this simulation's rules are not the "
                 "classic ones — scenario rule index %d is the first that "
                 "differs — and the observation scale is the classic game's. "
                 "The ML brain will not act.",
                 simRulesFirstDifference(&gs->rules));
}

bool obsBuildMultiView(struct ClientSim *cs, const BrainInfo *tankBi, WinBoloObs *obs) {
    memset(obs, 0, sizeof(*obs));

    /* The scale this vector is on is the classic game's, written out above.
       A sim running anything else would fill the same entries against
       different meanings of full, so there is nothing useful to hand back
       and this says so instead of handing back a vector that looks fine.
       Asked on every build rather than once: a scenario can change a rule
       mid-round, and an answer given at the first tick would not have heard
       about it. It costs one comparison of the table against a classic one,
       against the tens of thousands of floats filled below. */
    GameSim *gs = clientSimGetGameSim(cs);
    if (gs == NULL || !simRulesAreClassic(&gs->rules)) {
        obsWarnNotClassic(gs);
        return false;
    }

    BYTE selfPlayer = (BYTE)tankBi->player_number;
    PlayerBitMap alliesBits = tankBi->allies ? *(tankBi->allies) : 0;
    float self_wx = (float)tankBi->tankx;
    float self_wy = (float)tankBi->tanky;
    int tank_tx = tankBi->tankx >> 8;
    int tank_ty = tankBi->tanky >> 8;

    /* Build terrain, scalars, events, pill/base lists from tank-view BrainInfo */
    obsBuildEvents(tankBi, obs);
    obsBuildTerrain(tankBi, obs);

    /* Entity list: start with tank-view objects */
    uint16_t ne = 0;
    for (int i = 0; i < tankBi->num_objects && ne < WBGYM_MAX_ENTITIES; i++) {
        obsAddObjectAsEntity(&tankBi->objects[i], self_wx, self_wy, selfPlayer, cs, obs, &ne);
    }

    /* ---- Multi-view: gather from owned alive pill rects ---- */
    /* Identify owned alive pills and gather dynamic objects from their 15x15 rects.
     * Pills/bases are already in the entity list (gathered globally by aiYes mode or
     * from visible objects). We only need shells, tanks, and LGMs from pill rects. */
    BYTE np = pillsGetNumPills(&gs->pb);
    for (BYTE pi = 1; pi <= np; pi++) {
        pillbox p;
        pillsGetPill(&gs->pb, &p, pi);

        /* Must be: live, owned by self or ally, not in tank, armour > 0 */
        if (pillsIsActive(&gs->pb, pi) == FALSE) continue;
        if (p.inTank) continue;
        if (p.armour == 0) continue;
        if (p.owner == 0xFF) continue;
        if (p.owner != selfPlayer && !(alliesBits & (1u << p.owner))) continue;

        /* Skip if pill's 15x15 rect is fully inside tank's 29x29 rect */
        if (obsPillCoveredByTankView((int)p.x, (int)p.y, tank_tx, tank_ty)) continue;

        /* Gather dynamic objects from this pill's 15x15 view rect.
         * Use the engine's rect-gathering functions which write to the
         * brain-object storage (clientSimGetBrainObjects). */
        unsigned short *numObj = clientSimGetBrainsNumObjects(cs);
        *numObj = 0;

        BYTE left   = (BYTE)((int)p.x - 7);
        BYTE right  = (BYTE)((int)p.x + 7);
        BYTE top    = (BYTE)((int)p.y - 7);
        BYTE bottom = (BYTE)((int)p.y + 7);

        shellsGetBrainShellsInRect(cs, gs, &gs->shs,
                                   left, right, top, bottom);
        playersGetBrainTanksInRect(cs, &gs->plyrs,
                                   left, right, top, bottom,
                                   tankBi->tankx, tankBi->tanky);
        playersGetBrainLgmsInRect(cs, &gs->plyrs,
                                  left, right, top, bottom);

        /* Merge new objects into entity list with dedup */
        ObjectInfo *objects = clientSimGetBrainObjects(cs);
        unsigned short count = *numObj;
        for (unsigned short j = 0; j < count && ne < WBGYM_MAX_ENTITIES; j++) {
            ObjectInfo *o = &objects[j];

            /* Dedup: tanks by idnum, LGMs/parachutes by idnum */
            if (o->object == OBJECT_TANK) {
                if (obsTankAlreadySeen(obs, ne, (uint8_t)o->idnum)) continue;
            } else if (o->object == OBJECT_BUILDMAN || o->object == OBJECT_PARACHUTE) {
                if (obsLgmAlreadySeen(obs, ne, (uint8_t)o->idnum)) continue;
            }
            /* Shells don't need dedup — minor duplicates are acceptable,
             * and shells have no stable ID to dedup against anyway. */

            obsAddObjectAsEntity(o, self_wx, self_wy, selfPlayer, cs, obs, &ne);
        }

        *numObj = 0;
    }

    obs->num_entities = ne;

    obsBuildScalarsCS(tankBi, cs, obs);
    obsBuildMetaCS(tankBi, cs, obs);
    return true;
}
