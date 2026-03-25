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
 *Name:          WinBolo Gym
 *Filename:      winbolo_gym.h
 *Purpose:
 *  Shared library C API for ML training environments.
 *  Each WinBoloGym instance is a self-contained fast-mode
 *  game that can be stepped synchronously.
 *********************************************************/

#ifndef WINBOLO_GYM_H
#define WINBOLO_GYM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
  #ifdef WINBOLO_GYM_EXPORTS
    #define WBGYM_API __declspec(dllexport)
  #else
    #define WBGYM_API __declspec(dllimport)
  #endif
#else
  #define WBGYM_API __attribute__((visibility("default")))
#endif

/* Observation dimensions */
#define WBGYM_SPATIAL_SIZE    29
#define WBGYM_NUM_SCALARS     26
#define WBGYM_MAX_EVENTS      16
#define WBGYM_MAX_PILLBOXES   16
#define WBGYM_MAX_BASES       16
#define WBGYM_MAX_ENTITIES   256
#define WBGYM_MAX_SOUNDS      32

/* Kept for ml_brain.c compat until Phase 4 model update */
#define WBGYM_NUM_CHANNELS    10

/* Game type constants (matches engine gameType enum) */
#define WBGYM_GAME_OPEN              1
#define WBGYM_GAME_TOURNAMENT        2
#define WBGYM_GAME_STRICT_TOURNAMENT 3

/* Build action constants (matches engine buildAction values) */
#define WBGYM_BUILD_NONE  0
#define WBGYM_BUILD_TREE  1
#define WBGYM_BUILD_ROAD  2
#define WBGYM_BUILD_WALL  3
#define WBGYM_BUILD_PILL  4
#define WBGYM_BUILD_MINE  5

/* Event types in the events array */
#define WBGYM_EVENT_HIT_DEALT          0
#define WBGYM_EVENT_KILL               1
#define WBGYM_EVENT_DEATH              2
#define WBGYM_EVENT_HIT_RECEIVED       3
#define WBGYM_EVENT_PILL_CAPTURED      4
#define WBGYM_EVENT_PILL_LOST          5
#define WBGYM_EVENT_BASE_CAPTURED      6
#define WBGYM_EVENT_BASE_LOST          7
#define WBGYM_EVENT_LGM_LOST           8
#define WBGYM_EVENT_ASSIST_MAN_DEAD    9
#define WBGYM_EVENT_ASSIST_NO_TREE    10
#define WBGYM_EVENT_ASSIST_BUILDTANK  11

/* Entity type constants */
#define WBGYM_ENT_TANK       0
#define WBGYM_ENT_SHELL      1
#define WBGYM_ENT_PILLBOX    2
#define WBGYM_ENT_BASE       3
#define WBGYM_ENT_LGM        4
#define WBGYM_ENT_PARACHUTE  5
#define WBGYM_ENT_EXPLOSION  6

/* Entity allegiance constants */
#define WBGYM_ALLEG_ENEMY   -1
#define WBGYM_ALLEG_NEUTRAL  0
#define WBGYM_ALLEG_SELF     1
#define WBGYM_ALLEG_ALLY     2

/* Entity flags bitfield */
#define WBGYM_FLAG_IN_BOAT    0x01
#define WBGYM_FLAG_IN_TANK    0x02
#define WBGYM_FLAG_IS_SELF    0x04
#define WBGYM_FLAG_DEAD       0x08
#define WBGYM_FLAG_HIDDEN     0x10

/* Sound event type constants */
#define WBGYM_SND_SHOOT       0
#define WBGYM_SND_EXPLOSION   1
#define WBGYM_SND_HIT_TANK    2
#define WBGYM_SND_MINE_PLACE  3
#define WBGYM_SND_GENERIC     4
#define WBGYM_SND_LGM_LOST    5
#define WBGYM_SND_SHELL_FIRED 6

/* Pillbox owner constants */
#define WBGYM_OWNER_NEUTRAL  0
#define WBGYM_OWNER_SELF     1
#define WBGYM_OWNER_ENEMY    2
#define WBGYM_OWNER_ALLY     3

/* Entity in the observation entity list */
typedef struct {
    float rx;            /* tile-unit X relative to self tank (signed) */
    float ry;            /* tile-unit Y relative to self tank (signed) */
    uint8_t type;        /* WBGYM_ENT_* */
    int8_t  allegiance;  /* WBGYM_ALLEG_* */
    float direction;     /* 0..1 normalized angle (for tanks, shells, lgm) */
    float speed;         /* actual_speed / 128.0 (tanks only, 0 otherwise) */
    float strength;      /* pill: armour/15, shell: life/8, base: armour/90 */
    uint8_t flags;       /* WBGYM_FLAG_* bitfield */
    uint8_t id;          /* playerNum for tanks/lgm, pill/base index, 0xFF=n/a */
} WinBoloEntity;

/* Positional sound event */
typedef struct {
    float rx;          /* tile-unit X relative to self tank */
    float ry;          /* tile-unit Y relative to self tank */
    uint8_t type;      /* WBGYM_SND_* */
    int8_t  allegiance; /* who caused it: self/ally/enemy/neutral */
} WinBoloSoundEvent;

/* Incoming message observation (placeholder for Phase 6) */
typedef struct {
    uint8_t from;          /* sender player number */
    uint8_t type;          /* message type */
    float   data[4];       /* type-dependent payload */
} WinBoloMsgObs;

/* Alliance state observation (placeholder for Phase 6) */
typedef struct {
    uint8_t alliance_id;         /* which alliance (0 = none) */
    uint8_t alliance_members[16]; /* player IDs in same alliance */
    uint8_t alliance_count;       /* number of players in alliance */
} WinBoloAllianceObs;

/* Pillbox observation */
typedef struct {
    uint8_t tx;
    uint8_t ty;
    uint8_t owner;    /* WBGYM_OWNER_* */
    uint8_t armor;    /* 0-15 */
} WinBoloPillObs;

/* Base observation */
typedef struct {
    uint8_t tx;
    uint8_t ty;
    uint8_t owner;    /* WBGYM_OWNER_* */
    uint8_t shells;
    uint8_t mines;
    uint8_t armour;
} WinBoloBaseObs;

/* Action input for winbolo_step */
typedef struct {
    int accel;              /* -1 = decel, 0 = coast, 1 = accel */
    int turn;               /* -1 = left, 0 = straight, 1 = right */
    int shoot;              /* 0 = no, 1 = fire */
    int lay_mine;           /* 0 = no, 1 = lay */
    int gun_range_adjust;   /* -1 = shorten, 0 = no change, 1 = extend */
    int build_action;       /* WBGYM_BUILD_* constant */
    int build_rx;           /* -14 to +14, relative to tank tile */
    int build_ry;           /* -14 to +14, relative to tank tile */
} WinBoloAction;

/* Observation returned by step/reset */
typedef struct {
    /* Terrain (static, tile-level) — 2 layers */
    float terrain[WBGYM_SPATIAL_SIZE][WBGYM_SPATIAL_SIZE];
    float mines_map[WBGYM_SPATIAL_SIZE][WBGYM_SPATIAL_SIZE];

    /* Entity list (world-unit precision, all view rects) */
    WinBoloEntity entities[WBGYM_MAX_ENTITIES];
    uint16_t num_entities;

    /* Self tank scalars */
    float scalar[WBGYM_NUM_SCALARS];

    /* Own LGM state */
    float man_rx, man_ry, man_direction;

    /* Positional sound events */
    WinBoloSoundEvent sounds[WBGYM_MAX_SOUNDS];
    uint8_t num_sounds;

    /* Discrete game events */
    uint8_t events[WBGYM_MAX_EVENTS];
    uint8_t num_events;

    /* Assistant message */
    uint8_t assistant_msg;

    /* Alliance state (reserved — zeroed until Phase 6) */
    WinBoloAllianceObs alliance;

    /* Incoming messages (reserved — zeroed until Phase 6) */
    WinBoloMsgObs messages[4];
    uint8_t num_messages;

    /* Metadata */
    uint8_t dead;
    uint8_t game_over;
    uint8_t game_won;
    uint32_t tick;
    float tank_x, tank_y;   /* absolute tile coords for reward calc */

    /* Pill/base structured lists (for reward shaping, global knowledge) */
    uint8_t num_pillboxes;
    WinBoloPillObs pillboxes[WBGYM_MAX_PILLBOXES];
    uint8_t num_bases;
    WinBoloBaseObs bases[WBGYM_MAX_BASES];
} WinBoloObs;

/* Opaque handle */
typedef struct WinBoloGym WinBoloGym;

/* Create a new game instance from a map file.
 * game_type: WBGYM_GAME_OPEN, WBGYM_GAME_TOURNAMENT, or WBGYM_GAME_STRICT_TOURNAMENT.
 * Returns NULL on failure. */
WBGYM_API WinBoloGym *winbolo_create(const char *map_path, int game_type);

/* Destroy a game instance and free all resources. */
WBGYM_API void winbolo_destroy(WinBoloGym *game);

/* Step the game by one game tick (2 engine ticks: game + keys).
 * Fills obs_out with the resulting observation. */
WBGYM_API void winbolo_step(WinBoloGym *game, const WinBoloAction *action, WinBoloObs *obs_out);

/* Reset the game to initial state.
 * Fills obs_out with the tick-0 observation. */
WBGYM_API void winbolo_reset(WinBoloGym *game, WinBoloObs *obs_out);

/* Returns sizeof(WinBoloObs) for Python ctypes verification. */
WBGYM_API int winbolo_obs_size(void);

/* Returns sizeof(WinBoloAction) for Python ctypes verification. */
WBGYM_API int winbolo_action_size(void);

/* Step multiple game instances in one call.
 * Each game[i] is stepped with actions[i], result written to obs_out[i]. */
WBGYM_API void winbolo_step_batch(
    WinBoloGym **games,
    const WinBoloAction *actions,
    WinBoloObs *obs_out,
    int count
);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_GYM_H */
