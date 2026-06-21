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
#include <stdbool.h>

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

/* ── Reward component indices (must match Python RewardConfig field order) ── */
#define WBGYM_NUM_REWARD_COMPONENTS 58

/* Category 1: Survival */
#define RC_DEATH                  0
#define RC_DEATH_WITH_PILLS       1
#define RC_DEATH_WITH_LGM_OUT     2
#define RC_SURVIVAL_TICK          3
/* Category 2: Combat */
#define RC_HIT_DEALT              4
#define RC_HIT_RECEIVED           5
#define RC_KILL                   6
#define RC_KILL_CARRIER           7
#define RC_SHOT_FIRED             8
#define RC_SHOT_ACCURACY          9
/* Category 3: Pillbox */
#define RC_PILL_CAPTURED         10
#define RC_PILL_LOST             11
#define RC_PILL_DESTROYED        12
#define RC_OWN_PILL_FRAC_DELTA   13
#define RC_PILL_PLACEMENT_QUAL   14
#define RC_PILL_HEATED_TACTICAL  15
/* Category 4: Base */
#define RC_BASE_CAPTURED         16
#define RC_BASE_LOST             17
#define RC_OWN_BASE_FRAC_DELTA   18
#define RC_BASE_RATIO            19
#define RC_ALL_BASES_OWNED       20
/* Category 5: Builder */
#define RC_LGM_LOST              21
#define RC_LGM_PARACHUTING_TICK  22
#define RC_ENEMY_LGM_KILLED      23
#define RC_SUCCESSFUL_BUILD      24
#define RC_FAILED_BUILD          25
#define RC_LGM_SENT_DANGEROUS    26
/* Category 6: Resources */
#define RC_RESUPPLY_EFFICIENCY   27
#define RC_RESUPPLY_CAMPING      28
#define RC_TREES_FARMED          29
#define RC_AMMO_CONSERVATION     30
#define RC_IDLE_PENALTY          31
/* Category 7: Mining */
#define RC_MINE_PLACED           32
#define RC_MINE_KILL             33
#define RC_MINE_PLACED_DEFENSIVE 34
#define RC_MINE_PLACED_ON_ROAD   35
#define RC_OWN_MINE_HIT          36
/* Category 8: Territory */
#define RC_OFFENSIVE_PRESSURE    37
#define RC_DEFENSIVE_COVERAGE    38
#define RC_ROAD_BUILT            39
#define RC_WALL_BUILT            40
#define RC_FLANK_BONUS           41
/* Category 9: Strategic */
#define RC_INITIATIVE_SCORE      42
#define RC_SPIKE_QUALITY         43
#define RC_DONT_CARRY_TOO_MANY   44
/* Category 10: Multi-agent (placeholders) */
#define RC_DECOY_ASSIST          45
#define RC_TEAM_COORDINATION     46
#define RC_MESSAGE_USEFUL        47
/* Category 11: Phase 1 shaping */
#define RC_EXPLORATION_BONUS     48
#define RC_BASE_PROXIMITY        49
#define RC_SPEED_BONUS           50
#define RC_BOAT_OVERSTAY         51
#define RC_ON_LAND_BONUS         52
#define RC_APPROACH_PILLBOX      53
#define RC_FACING_PILLBOX        54
#define RC_PILL_HIT              55
#define RC_RESUPPLY_SHELLS       56
#define RC_SHOOT_AT_PILL         57

/* Rolling window sizes */
#define WBGYM_SHOT_WINDOW         50
#define WBGYM_INITIATIVE_WINDOW  100
#define WBGYM_MINE_BUFFER         64
#define WBGYM_BOAT_GRACE_TICKS    30
#define WBGYM_EXPLORATION_FULL   500

/* LGM status thresholds (matches Python constants) */
#define WBGYM_LGM_IN_TANK_THRESH    0.1f
#define WBGYM_LGM_OUTSIDE_LO        0.2f
#define WBGYM_LGM_OUTSIDE_HI        0.5f
#define WBGYM_LGM_PARA_LO           0.5f
#define WBGYM_LGM_PARA_HI           0.8f

/* Failed build assistant messages */
#define WBGYM_FAILED_BUILD_MSG(m) \
    ((m)==2||(m)==3||(m)==4||(m)==5||(m)==7||(m)==8||(m)==9||(m)==10)

/* Scalar indices */
#define WBGYM_S_ARMOR            0
#define WBGYM_S_SHELLS           1
#define WBGYM_S_MINES            2
#define WBGYM_S_TREES            3
#define WBGYM_S_SPEED            4
#define WBGYM_S_RELOAD           7
#define WBGYM_S_IN_BOAT          8
#define WBGYM_S_PILL_COUNT      10
#define WBGYM_S_DEAD            11
#define WBGYM_S_OWN_PILL_FRAC   12
#define WBGYM_S_ENEMY_PILL_FRAC 13
#define WBGYM_S_OWN_BASE_FRAC   15
#define WBGYM_S_TANK_X          17
#define WBGYM_S_TANK_Y          18
#define WBGYM_S_LGM_STATUS      23

/* Terrain constants */
#define WBGYM_TERRAIN_ROAD       2
#define WBGYM_TERRAIN_NORM      15.0f

/* Per-instance reward state */
typedef struct {
    float weights[WBGYM_NUM_REWARD_COMPONENTS];

    /* Previous tick state */
    float prev_scalars[26];
    float prev_pills[16][4];     /* [tx, ty, owner, armor] */
    int prev_pill_count;
    float prev_bases[16][6];     /* [tx, ty, owner, shells, mines, armour] */
    int prev_base_count;
    float prev_lgm_status;

    /* Rolling combat accuracy (50-tick ring buffer) */
    float shot_window[WBGYM_SHOT_WINDOW];
    float hit_window[WBGYM_SHOT_WINDOW];
    int window_idx;

    /* Rolling initiative (100-tick ring buffer) */
    float init_captures[WBGYM_INITIATIVE_WINDOW];
    float init_losses[WBGYM_INITIATIVE_WINDOW];
    int init_idx;

    /* Mine position tracking (64-slot ring buffer) */
    float mine_positions[WBGYM_MINE_BUFFER][2];
    int mine_idx;
    bool mine_valid[WBGYM_MINE_BUFFER];

    /* Builder tracking */
    int active_build_action;

    /* Spike/carry tracking */
    int last_spike_tick;

    /* Tick counter & first-tick guard */
    int tick;
    bool has_prev;

    /* Phase 1 shaping: exploration tracking (256x256 bitfield = 8KB) */
    uint8_t visited_tiles[256][256 / 8];
    int visited_count;

    /* Boat overstay */
    int boat_ticks;

    /* Approach pillbox shaping */
    float prev_nearest_pill_dist;
} RewardState;

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

    /* Reward output (computed in C when weights are set) */
    float reward;
    float reward_components[WBGYM_NUM_REWARD_COMPONENTS];
} WinBoloObs;

/* Output pointers for winbolo_obs_to_reward_batch */
typedef struct {
    float *scalars;         /* [N, 26] */
    uint8_t *events;        /* [N, 16] */
    int32_t *event_count;   /* [N] */
    float *pills;           /* [N, 16, 4] */
    int32_t *pill_count;    /* [N] */
    float *bases;           /* [N, 16, 6] */
    int32_t *base_count;    /* [N] */
    float *entities;        /* [N, 256, 9] */
    int32_t *entity_count;  /* [N] */
    float *sounds;          /* [N, 32, 4] */
    int32_t *sound_count;   /* [N] */
    float *terrain;         /* [N, 29, 29] */
    uint8_t *dead;          /* [N] */
    uint8_t *game_over;     /* [N] */
    uint8_t *game_won;      /* [N] */
    int32_t *tick;          /* [N] */
    float *tank_x;          /* [N] */
    float *tank_y;          /* [N] */
    uint8_t *assistant_msg; /* [N] */
} WinBoloRewardBatchOut;

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

/* Fill N WinBoloAction structs from a flat int32 array of shape [N, 8].
 * Columns: [accel, turn, shoot, lay_mine, gun_range_adjust, build_action, build_rx, build_ry]
 * where discrete indices are mapped to signed values matching fill_action_struct(). */
WBGYM_API void winbolo_fill_actions_batch(
    const int32_t *actions_flat,
    WinBoloAction *actions_out,
    int count
);

/* Extract model observation tensors from N WinBoloObs structs into pre-allocated numpy buffers.
 * Produces identical output to obs_to_tensors() called N times then np.stack'd. */
WBGYM_API void winbolo_obs_to_numpy_batch(
    const WinBoloObs *obs_array,
    float *terrain_out,      /* [N, 29, 29, 2] */
    float *scalar_out,       /* [N, 26] */
    float *entities_out,     /* [N, 256, 7] */
    float *entity_mask_out,  /* [N, 256] */
    float *sounds_out,       /* [N, 32, 4] */
    float *sound_mask_out,   /* [N, 32] */
    int count
);

/* Extract reward-computation inputs from N WinBoloObs structs into pre-allocated buffers.
 * Produces identical output to obs_to_reward_inputs() called on the same obs array. */
WBGYM_API void winbolo_obs_to_reward_batch(
    const WinBoloObs *obs_array,
    WinBoloRewardBatchOut *out,
    int count
);

/* Set reward weights for a game instance. count must equal WBGYM_NUM_REWARD_COMPONENTS.
 * Must be called before stepping if you want C-side reward computation. */
WBGYM_API void winbolo_set_reward_weights(WinBoloGym *game, const float *weights, int count);

/* Returns the number of reward components (WBGYM_NUM_REWARD_COMPONENTS). */
WBGYM_API int winbolo_reward_component_count(void);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_GYM_H */
