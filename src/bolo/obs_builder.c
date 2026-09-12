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
 *Name:          Observation Builder
 *Filename:      obs_builder.c
 *Purpose:
 *  Builds V3 WinBoloObs from BrainInfo for in-game ML
 *  brain inference.
 *
 *  obsBuildFromBrainInfo: single-view (tank view rect)
 *  obsBuildMultiView:     multi-view (tank + owned pills)
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

/* Helper to determine allegiance from object info flags */
static int8_t obsGetAllegiance(const ObjectInfo *o, BYTE selfPlayer) {
    (void)selfPlayer;
    if (o->info & OBJECT_NEUTRAL) return WBGYM_ALLEG_NEUTRAL;
    if (!(o->info & OBJECT_HOSTILE)) return WBGYM_ALLEG_SELF; /* own or ally */
    return WBGYM_ALLEG_ENEMY;
}

/* Helper to determine owner constant from object info flags */
static uint8_t obsGetOwner(const ObjectInfo *o) {
    if (o->info & OBJECT_NEUTRAL) return WBGYM_OWNER_NEUTRAL;
    if (!(o->info & OBJECT_HOSTILE)) return WBGYM_OWNER_SELF;
    return WBGYM_OWNER_ENEMY;
}

/* Convert an ObjectInfo into a WinBoloEntity and append to entity list.
 * cs may be NULL (single-view path); when non-NULL, used for tank armour/flags.
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
        ent->strength = (float)o->refbase_strength / 90.0f;
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

/* Build events and sound events from BrainInfo events list */
static void obsBuildEvents(const BrainInfo *bi, WinBoloObs *obs) {
    BYTE selfPlayer = (BYTE)bi->player_number;
    PlayerBitMap alliesBits = bi->allies ? *(bi->allies) : 0;
    int tank_tx = bi->tankx >> 8;
    int tank_ty = bi->tanky >> 8;

    for (int i = 0; i < bi->num_events; i++) {
        GameEvent *e = &bi->events[i];
        switch (e->type) {
        case EVENT_SOUND_TANK_HIT:
            if (e->data[3] != selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_HIT_DEALT;
            if (e->data[3] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_HIT_RECEIVED;
            if (obs->num_sounds < WBGYM_MAX_SOUNDS) {
                float sx = (float)e->data[1] - (float)tank_tx;
                float sy = (float)e->data[2] - (float)tank_ty;
                bool isSelfHit = (e->data[3] == selfPlayer);
                if (isSelfHit || (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f)) {
                    WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                    snd->rx = sx;
                    snd->ry = sy;
                    snd->type = WBGYM_SND_HIT_TANK;
                    snd->allegiance = obsGetPlayerAllegiance(e->data[3], selfPlayer, alliesBits);
                }
            }
            break;
        case EVENT_TANK_KILLED:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_KILL;
            if (e->data[1] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_DEATH;
            break;
        case EVENT_PILL_CAPTURED:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_PILL_CAPTURED;
            if (e->data[1] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_PILL_LOST;
            break;
        case EVENT_BASE_CAPTURED:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_BASE_CAPTURED;
            if (e->data[1] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_BASE_LOST;
            break;
        case EVENT_LGM_LOST:
            if (e->data[0] == selfPlayer && obs->num_events < WBGYM_MAX_EVENTS)
                obs->events[obs->num_events++] = WBGYM_EVENT_LGM_LOST;
            break;
        case EVENT_SOUND_SHOOT: {
            if (e->data[3] == selfPlayer) break;
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_SHOOT;
                snd->allegiance = obsGetPlayerAllegiance(e->data[3], selfPlayer, alliesBits);
            }
            break;
        }
        case EVENT_SOUND: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_GENERIC;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_EXPLOSION: {
            float sx = (float)e->data[0] - (float)tank_tx;
            float sy = (float)e->data[1] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_EXPLOSION;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_MINE_PLACED: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_MINE_PLACE;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_SHELL_FIRED: {
            float sx = (float)e->data[1] - (float)tank_tx;
            float sy = (float)e->data[2] - (float)tank_ty;
            if (fabsf(sx) < 40.0f && fabsf(sy) < 40.0f && obs->num_sounds < WBGYM_MAX_SOUNDS) {
                WinBoloSoundEvent *snd = &obs->sounds[obs->num_sounds++];
                snd->rx = sx;
                snd->ry = sy;
                snd->type = WBGYM_SND_SHELL_FIRED;
                snd->allegiance = WBGYM_ALLEG_NEUTRAL;
            }
            break;
        }
        case EVENT_ASSISTANT_MSG:
            if (e->data[0] == selfPlayer) {
                obs->assistant_msg = e->data[1];
                switch (e->data[1]) {
                case 1:
                    if (obs->num_events < WBGYM_MAX_EVENTS)
                        obs->events[obs->num_events++] = WBGYM_EVENT_ASSIST_MAN_DEAD;
                    break;
                case 2: case 5:
                    if (obs->num_events < WBGYM_MAX_EVENTS)
                        obs->events[obs->num_events++] = WBGYM_EVENT_ASSIST_NO_TREE;
                    break;
                case 6:
                    if (obs->num_events < WBGYM_MAX_EVENTS)
                        obs->events[obs->num_events++] = WBGYM_EVENT_ASSIST_BUILDTANK;
                    break;
                }
            }
            break;
        default:
            break;
        }
    }

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

/* Build scalars from BrainInfo */
static void obsBuildScalars(const BrainInfo *bi, WinBoloObs *obs) {
    bool dead = bi->destroyed != 0;
    int tank_tx = bi->tankx >> 8;
    int tank_ty = bi->tanky >> 8;

    /* Scalars 0-11 */
    {
        unsigned armor = dead ? 0 : (unsigned)bi->armour;
        float dir_rad = (float)bi->direction * (2.0f * 3.14159265f / 256.0f);

        obs->scalar[0]  = (float)armor / 40.0f;
        obs->scalar[1]  = (float)bi->shells / 40.0f;
        obs->scalar[2]  = (float)bi->mines / 40.0f;
        obs->scalar[3]  = (float)bi->trees / 40.0f;
        obs->scalar[4]  = (float)bi->speed / 128.0f;
        obs->scalar[5]  = sinf(dir_rad);
        obs->scalar[6]  = cosf(dir_rad);
        obs->scalar[7]  = (float)bi->reload / 15.0f;
        obs->scalar[8]  = bi->inboat ? 1.0f : 0.0f;
        obs->scalar[9]  = bi->carriedpills > 0 ? 1.0f : 0.0f;
        obs->scalar[10] = (float)bi->carriedpills / 16.0f;
        obs->scalar[11] = dead ? 1.0f : 0.0f;
    }

    /* Scalars 12-16: pill/base ownership fractions */
    {
        int self_pills = 0, enemy_pills = 0, ally_pills = 0, total_pills = 0;
        int self_bases = 0, ally_bases = 0, total_bases = 0;

        for (int i = 0; i < bi->num_objects; i++) {
            ObjectInfo *o = &bi->objects[i];
            if (o->object == OBJECT_PILLBOX) {
                total_pills++;
                if (o->info & OBJECT_NEUTRAL) {
                    /* neutral */
                } else if (!(o->info & OBJECT_HOSTILE)) {
                    self_pills++;
                } else {
                    enemy_pills++;
                }
            } else if (o->object == OBJECT_REFBASE) {
                total_bases++;
                if (o->info & OBJECT_NEUTRAL) {
                    /* neutral */
                } else if (!(o->info & OBJECT_HOSTILE)) {
                    self_bases++;
                }
            }
        }

        if (bi->max_pillboxes > 0) total_pills = bi->max_pillboxes;
        if (bi->max_refbases > 0)  total_bases = bi->max_refbases;

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

    /* Scalars 19-25 */
    obs->scalar[19] = (float)bi->gunrange / 14.0f;
    obs->scalar[20] = bi->hidden ? 1.0f : 0.0f;
    obs->scalar[21] = bi->newtank ? 1.0f : 0.0f;
    obs->scalar[22] = bi->tankobstructed ? 1.0f : 0.0f;

    {
        BYTE ms = bi->man_status;
        if (ms == LGM_BRAIN_INTANK) {
            obs->scalar[23] = 0.0f;
        } else if (ms == LGM_BRAIN_DEAD) {
            obs->scalar[23] = 1.0f;
        } else {
            obs->scalar[23] = 0.33f;
        }
    }

    obs->scalar[24] = (float)bi->manobstructed / 2.0f;
    obs->scalar[25] = 0.0f; /* death_wait not available in BrainInfo */
}

/* Build scalars with ClientSim data for accurate pill/base fracs, LGM, death_wait */
static void obsBuildScalarsCS(const BrainInfo *bi, struct ClientSim *cs, WinBoloObs *obs) {
    bool dead = bi->destroyed != 0;
    int tank_tx = bi->tankx >> 8;
    int tank_ty = bi->tanky >> 8;
    GameSim *gs = clientSimGetGameSim(cs);
    BYTE selfPlayer = clientSimGetMyPlayerNum(cs);
    PlayerBitMap alliesBits = bi->allies ? *(bi->allies) : 0;

    /* Scalars 0-11: same as obsBuildScalars */
    {
        unsigned armor = dead ? 0 : (unsigned)bi->armour;
        float dir_rad = (float)bi->direction * (2.0f * 3.14159265f / 256.0f);

        obs->scalar[0]  = (float)armor / 40.0f;
        obs->scalar[1]  = (float)bi->shells / 40.0f;
        obs->scalar[2]  = (float)bi->mines / 40.0f;
        obs->scalar[3]  = (float)bi->trees / 40.0f;
        obs->scalar[4]  = (float)bi->speed / 128.0f;
        obs->scalar[5]  = sinf(dir_rad);
        obs->scalar[6]  = cosf(dir_rad);
        obs->scalar[7]  = (float)bi->reload / 15.0f;
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

/* Build LGM state, tank position, pill/base lists from BrainInfo */
static void obsBuildMeta(const BrainInfo *bi, WinBoloObs *obs) {
    float self_wx = (float)bi->tankx;
    float self_wy = (float)bi->tanky;
    bool dead = bi->destroyed != 0;

    /* LGM state */
    obs->man_rx = ((float)bi->man_x - self_wx) / 256.0f;
    obs->man_ry = ((float)bi->man_y - self_wy) / 256.0f;
    obs->man_direction = (float)bi->man_direction / 256.0f;

    /* Full-precision tank position */
    obs->tank_x = (float)bi->tankx / 256.0f;
    obs->tank_y = (float)bi->tanky / 256.0f;

    /* Pill list from objects */
    for (int i = 0; i < bi->num_objects; i++) {
        ObjectInfo *o = &bi->objects[i];
        if (o->object != OBJECT_PILLBOX) continue;
        if (obs->num_pillboxes >= WBGYM_MAX_PILLBOXES) break;
        WinBoloPillObs *po = &obs->pillboxes[obs->num_pillboxes];
        po->tx = (uint8_t)(o->x >> 8);
        po->ty = (uint8_t)(o->y >> 8);
        po->armor = o->pillbox_strength;
        po->owner = obsGetOwner(o);
        obs->num_pillboxes++;
    }

    /* Base list from objects */
    for (int i = 0; i < bi->num_objects; i++) {
        ObjectInfo *o = &bi->objects[i];
        if (o->object != OBJECT_REFBASE) continue;
        if (obs->num_bases >= WBGYM_MAX_BASES) break;
        WinBoloBaseObs *bo = &obs->bases[obs->num_bases];
        bo->tx = (uint8_t)(o->x >> 8);
        bo->ty = (uint8_t)(o->y >> 8);
        bo->owner = obsGetOwner(o);
        if (bo->owner == WBGYM_OWNER_SELF && bi->base != NULL &&
            bo->tx == (uint8_t)(bi->base->x >> 8) &&
            bo->ty == (uint8_t)(bi->base->y >> 8)) {
            bo->shells = bi->base_shells;
            bo->mines  = bi->base_mines;
            bo->armour = bi->base_armour;
        }
        obs->num_bases++;
    }

    obs->dead = dead ? 1 : 0;
    obs->tick = bi->server_tick;
}

/* ------------------------------------------------------------------ */
/* Single-view obs builder (unchanged interface)                       */
/* ------------------------------------------------------------------ */

void obsBuildFromBrainInfo(const BrainInfo *bi, WinBoloObs *obs) {
    memset(obs, 0, sizeof(*obs));

    BYTE selfPlayer = (BYTE)bi->player_number;
    float self_wx = (float)bi->tankx;
    float self_wy = (float)bi->tanky;

    obsBuildEvents(bi, obs);
    obsBuildTerrain(bi, obs);

    /* Entity list from BrainInfo objects (single view, no ClientSim) */
    uint16_t ne = 0;
    for (int i = 0; i < bi->num_objects && ne < WBGYM_MAX_ENTITIES; i++) {
        obsAddObjectAsEntity(&bi->objects[i], self_wx, self_wy, selfPlayer, NULL, obs, &ne);
    }
    obs->num_entities = ne;

    obsBuildScalars(bi, obs);
    obsBuildMeta(bi, obs);
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

void obsBuildMultiView(struct ClientSim *cs, const BrainInfo *tankBi, WinBoloObs *obs) {
    memset(obs, 0, sizeof(*obs));

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
    GameSim *gs = clientSimGetGameSim(cs);
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
}
