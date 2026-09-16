/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Scenario Definitions
 *Filename:      scenario_defs.h
 *Author:        John Morrison
 *Purpose:
 *  The POD types a scenario writes the world with: the op
 *  enum, one payload struct per op, the result codes every
 *  op answers with, and the policy vtable the sim asks its
 *  decisions through.
 *
 *  Types only — no function declarations, no behaviour.
 *  server_sim_scenario.h beside it carries the calls. The
 *  directory is on the include path of scenario_host and of
 *  the privileged profiles (sim_owner, unittests, mapeditor,
 *  braintest, gym) and not of gui or runtime_only, so a
 *  frontend that includes this fails to build.
 *********************************************************/

#ifndef SCENARIO_DEFS_H
#define SCENARIO_DEFS_H

#include "global.h"         /* BYTE, PLAYER_NAME_LEN */
#include "types.h"          /* TankModifiers — the modifier op's payload */
#include "wire_limits.h"    /* PACKET_MAX_CHAT_MESSAGE — the text cap below */
#include "scenario_table.h" /* ScnTable — the init and hint payloads below */

/* Text capacity for every op that carries a line. The server-text
 * control event holds char text[PACKET_MAX_CHAT_MESSAGE + 1]
 * (control_event.h), and a scenario line is sent as one of those, so
 * the op's buffer is that buffer. Text past it is refused, never
 * truncated. */
#define SCN_TEXT_MAX (PACKET_MAX_CHAT_MESSAGE + 1)

/* Byte capacity of one panel display list. The list rides a single
 * control segment, which carries CHANNEL_CONTROL_SEG (1024) bytes
 * (channel_mux.h) and rejects a message larger than that. Of those,
 * the channel frame spends type(1) + bodyLen(2), and the panel event's
 * own header spends target(1) + panel(1) + len(2):
 *   1024 - 1 - 2 - 1 - 1 - 2 = 1017
 * server_sim_scenario.c pins this against CHANNEL_CONTROL_SEG, which
 * it can see and this header cannot. */
#define SCN_PANEL_MAX 1017

/* Buffer for a brain: the name a scenario writes — a directory under the
 * server's own brains/, such as "GoalHunter_1.7" — and the path that name is
 * resolved to before the sim is handed it. */
#define SCN_PATH_MAX 256

/* One team a scenario's lobby seats, as the sim reads it.
 *
 * bots is how many seats the engine creates when it seats the template;
 * maxBots is the most a host may leave on the team, which is the only one of
 * the two that binds again once the seats exist. fielded false asks for the
 * seats without the bots — roster entries the start sequence skips until a
 * spawn names one. brain is the path those bots run, or "" for the server's
 * own; the scenario names a brain and the host resolves that name to this
 * path, so what reaches the sim is always a file to open.
 *
 * init is the table the team's bots are built with, read once when a VM is
 * built, empty for none. It is what the countdown warms a held seat's runner
 * with, so a spawn naming that seat with the same table — or with none of its
 * own, which inherits this — is a resume rather than a build. */
typedef struct {
    uint8_t  id;                   /* team number, 1-16 */
    uint8_t  bots;
    uint8_t  maxBots;
    bool     fielded;
    char     brain[SCN_PATH_MAX];
    ScnTable init;
} ScnLobbyTeam;

/* The lobby a scenario asks for. The host reads this out of its manifest and
 * hands the sim a copy, so the engine seats and reconciles it without calling
 * back into the host — the dependency points one way and the sim needs no
 * notion of a manifest, a script or Lua.
 *
 * maxPlayers is a cap on humans only; bots seat above it. 0 leaves the
 * server's own cap alone.
 *
 * baseGameType is the game type the scenario declared, by the same words a
 * spawn op's loadout takes. It is the host's one-way hand-over of that
 * value: the sim keeps it where the spawn and start paths can read it when
 * the round is gameScripted. 0 means none declared, which plays open. */
typedef struct {
    uint8_t      maxPlayers;
    uint8_t      numTeams;
    uint8_t      baseGameType;   /* a gameType value, 0 for none */
    ScnLobbyTeam teams[MAX_TANKS];
} ScnLobbyTemplate;

/* How many roster changes may be outstanding at once. Spawns and
 * removals share one first-in first-out queue and the sim drains one of
 * them a tick, so a script that asks for ten bots gets them over ten
 * ticks without knowing the rate. The one past the last is refused
 * SCN_OP_FULL rather than displacing anything already accepted. */
#define SCN_ROSTER_QUEUE_MAX 32

/* How many tiles a fill may change in one tick. A rectangle that
 * changes more than this applies what the budget allows, keeps the
 * remainder and answers SCN_OP_QUEUED; the sim carries the rest on
 * later ticks, one budget each. A fill with no budget left in the
 * tick it arrives in is refused rather than queued, so nothing that
 * cannot move is left on the sim. A whole-map fill takes 256 ticks.
 *
 * The number is also the depth of the server's per-frame map event
 * buffer, which is a collision rather than a design: a fill spending
 * the whole budget fills that buffer on its own and the frame's other
 * terrain changes are dropped where they are recorded. Lowering this
 * is what would leave them room. */
#define SCN_TILES_PER_TICK 256

/* ScnKV, ScnTable and the SCN_TABLE_* caps are in
 * public/scenario_table.h: the init table is also a parameter of
 * server_sim.h's bot constructor, which a frontend calls, so the type
 * cannot live behind the scenario surface. */

/* What a spawning tank is handed. Either a game type names the loadout
 * or the four amounts do; useGameType picks between them. */
typedef struct {
    uint8_t useGameType;   /* non-zero: gameType names the loadout */
    uint8_t gameType;      /* a gameType value */
    uint8_t shells, mines, armour, trees;   /* used when useGameType is 0 */
} ScnLoadout;

/* Every write a scenario can make. The enum is complete; an op whose
 * arm has not been written is refused with SCN_OP_UNSUPPORTED rather
 * than silently doing nothing. */
typedef enum {
    SCN_OP_NONE = 0,

    /* Tank */
    SCN_OP_TANK_SET_STOCKS,
    SCN_OP_TANK_KILL,
    SCN_OP_TANK_TELEPORT,
    SCN_OP_TANK_SET_BOAT,
    SCN_OP_TANK_GIVE_PILL,
    SCN_OP_TANK_DROP_PILL,
    SCN_OP_TANK_SET_MODIFIERS,

    /* Builder */
    SCN_OP_LGM_DISPATCH,
    SCN_OP_LGM_RECALL,
    SCN_OP_LGM_KILL,
    SCN_OP_LGM_PARACHUTE,
    SCN_OP_LGM_SET_CARRIED,

    /* Pill */
    SCN_OP_PILL_SET_OWNER,
    SCN_OP_PILL_SET_ARMOUR,
    SCN_OP_PILL_SET_SPEED,
    SCN_OP_PILL_MOVE,

    /* Base */
    SCN_OP_BASE_SET_OWNER,
    SCN_OP_BASE_SET_STOCK,

    /* Entities */
    SCN_OP_ENTITY_ADD_PILL,
    SCN_OP_ENTITY_REMOVE_PILL,
    SCN_OP_ENTITY_ADD_BASE,
    SCN_OP_ENTITY_REMOVE_BASE,
    SCN_OP_ENTITY_ADD_START,
    SCN_OP_ENTITY_REMOVE_START,

    /* Map */
    SCN_OP_MAP_SET_TILE,
    SCN_OP_MAP_FILL_RECT,
    SCN_OP_MAP_PLACE_MINE,
    SCN_OP_MAP_REMOVE_MINE,

    /* Roster, in-round */
    SCN_OP_ROSTER_SPAWN_BOT,
    SCN_OP_ROSTER_REMOVE_BOT,
    SCN_OP_ROSTER_SET_TEAM,

    /* Roster, lobby */
    SCN_OP_LOBBY_ADD_BOT,
    SCN_OP_LOBBY_REMOVE_BOT,
    SCN_OP_LOBBY_SET_TEAM,

    /* Bots */
    SCN_OP_BOT_HINT,

    /* Comms */
    SCN_OP_MSG_ALL,
    SCN_OP_MSG_TEAM,
    SCN_OP_MSG_PLAYER,
    SCN_OP_SOUND,
    SCN_OP_LOG,

    /* Presentation */
    SCN_OP_PANEL,
    SCN_OP_SCORE,
    SCN_OP_ANNOUNCE,
    SCN_OP_MARKER,

    /* Flow */
    SCN_OP_END_ROUND,
    SCN_OP_SET_GAME_TIME,

    /* Rules */
    SCN_OP_SET_RULE
} ScenarioOpType;

/* A payload byte holding 0xFF means there is nothing there: no slot, no
 * item, or leave the field as it is. Which of the three a field means is
 * on the field. */
#define SCN_NONE 0xFF

/* ScnOpTankSetStocks.mode. Absolute writes the value; delta adds it. */
#define SCN_STOCK_ABSOLUTE 0
#define SCN_STOCK_DELTA    1

/* ScnOpTankTeleport.mode. Square takes x and y; start takes a start index. */
#define SCN_TELEPORT_SQUARE 0
#define SCN_TELEPORT_START  1

/* ── Tank ──────────────────────────────────────────────────────── */

typedef struct {
    BYTE    slot;
    BYTE    mode;                            /* absolute or delta */
    int16_t shells, mines, armour, trees;    /* -1 absolute / 0 delta = leave alone */
} ScnOpTankSetStocks;

typedef struct {
    BYTE slot;
    BYTE killer;    /* 0xFF = environmental */
    BYTE cause;     /* a LAST_DEATH_BY_* value */
} ScnOpTankKill;

typedef struct {
    BYTE slot;
    BYTE mode;      /* square or start */
    BYTE x, y;      /* square mode */
    BYTE start;     /* start mode; 0xFF = let the engine choose */
    BYTE dir;       /* 0xFF = keep */
} ScnOpTankTeleport;

typedef struct {
    BYTE slot;
    bool onBoat;
} ScnOpTankSetBoat;

typedef struct {
    BYTE slot;
    BYTE pill;
} ScnOpTankGivePill;

typedef struct {
    BYTE slot;
    BYTE pill;
    BYTE x, y;      /* 0xFF, 0xFF = under the tank */
} ScnOpTankDropPill;

typedef struct {
    BYTE          slot;
    TankModifiers mods;
} ScnOpTankSetModifiers;

/* ── Builder ───────────────────────────────────────────────────── */

typedef struct {
    BYTE slot;
    BYTE action;    /* an LGM_*_REQUEST value */
    BYTE x, y;
} ScnOpLgmDispatch;

typedef struct {
    BYTE slot;
} ScnOpLgmRecall;

typedef struct {
    BYTE slot;
    BYTE killer;    /* 0xFF = environmental */
} ScnOpLgmKill;

typedef struct {
    BYTE slot;
    BYTE x, y;      /* 0xFF, 0xFF = to the tank */
} ScnOpLgmParachute;

typedef struct {
    BYTE slot;
    BYTE trees, mines;   /* 0xFF = keep */
} ScnOpLgmSetCarried;

/* ── Pill ──────────────────────────────────────────────────────── */

typedef struct {
    BYTE pill;
    BYTE owner;     /* a slot or NEUTRAL */
} ScnOpPillSetOwner;

typedef struct {
    BYTE pill;
    BYTE armour;
} ScnOpPillSetArmour;

typedef struct {
    BYTE pill;
    BYTE speed;
} ScnOpPillSetSpeed;

typedef struct {
    BYTE pill;
    BYTE x, y;
} ScnOpPillMove;

/* ── Base ──────────────────────────────────────────────────────── */

typedef struct {
    BYTE base;
    BYTE owner;
    bool keepStock;
} ScnOpBaseSetOwner;

typedef struct {
    BYTE    base;
    int16_t armour, shells, mines;   /* -1 = keep */
} ScnOpBaseSetStock;

/* ── Entities ──────────────────────────────────────────────────── */

typedef struct {
    BYTE x, y;
    BYTE owner;
    BYTE armour;
    BYTE speed;
} ScnOpEntityAddPill;

typedef struct {
    BYTE pill;
} ScnOpEntityRemovePill;

typedef struct {
    BYTE x, y;
    BYTE owner;
    BYTE armour, shells, mines;
} ScnOpEntityAddBase;

typedef struct {
    BYTE base;
} ScnOpEntityRemoveBase;

typedef struct {
    BYTE x, y;
    BYTE dir;
} ScnOpEntityAddStart;

typedef struct {
    BYTE start;
} ScnOpEntityRemoveStart;

/* ── Map ───────────────────────────────────────────────────────── */

typedef struct {
    BYTE x, y;
    BYTE terrain;   /* 0..15 or DEEP_SEA */
} ScnOpMapSetTile;

typedef struct {
    BYTE x0, y0, x1, y1;
    BYTE terrain;
} ScnOpMapFillRect;

typedef struct {
    BYTE x, y;
    BYTE owner;     /* a slot or NEUTRAL */
    bool visible;
} ScnOpMapPlaceMine;

typedef struct {
    BYTE x, y;
} ScnOpMapRemoveMine;

/* ── Roster, in-round ──────────────────────────────────────────── */

typedef struct {
    BYTE     slot;                   /* 0xFF = first free seat above the cap */
    char     name[PLAYER_NAME_LEN];
    char     brain[SCN_PATH_MAX];    /* the brain the script named, resolved
                                      * to a path before the op is submitted */
    BYTE     team;
    BYTE     start;                  /* 0xFF = let the engine choose */
    BYTE     loadout;                /* 0 = ask the policy */
    ScnTable init;
} ScnOpRosterSpawnBot;

typedef struct {
    BYTE slot;
} ScnOpRosterRemoveBot;

typedef struct {
    BYTE slot;
    BYTE team;
} ScnOpRosterSetTeam;

/* ── Roster, lobby ─────────────────────────────────────────────── */

typedef struct {
    BYTE slot;
    char name[PLAYER_NAME_LEN];
    char brain[SCN_PATH_MAX];   /* resolved the same way a spawn's is */
    BYTE team;
    bool fielded;
} ScnOpLobbyAddBot;

typedef struct {
    BYTE slot;
} ScnOpLobbyRemoveBot;

typedef struct {
    BYTE slot;
    BYTE team;
} ScnOpLobbySetTeam;

/* ── Bots ──────────────────────────────────────────────────────── */

typedef struct {
    BYTE     slot;
    ScnTable hint;   /* "verb" is one of the keys */
} ScnOpBotHint;

/* ── Comms ─────────────────────────────────────────────────────── */

typedef struct {
    char text[SCN_TEXT_MAX];
} ScnOpMsgAll;

typedef struct {
    BYTE team;
    char text[SCN_TEXT_MAX];
} ScnOpMsgTeam;

typedef struct {
    BYTE slot;
    char text[SCN_TEXT_MAX];
} ScnOpMsgPlayer;

typedef struct {
    BYTE sound;     /* an sndEffects value */
    BYTE x, y;      /* 0xFF, 0xFF = everywhere */
} ScnOpSound;

typedef struct {
    char text[SCN_TEXT_MAX];
} ScnOpLog;

/* ── Presentation ──────────────────────────────────────────────── */
/* target: 0 = all, 1..15 = team, 0x80 | slot = one player. */

typedef struct {
    BYTE     target;
    BYTE     panel;                 /* 0..3 */
    uint16_t len;
    uint8_t  bytes[SCN_PANEL_MAX];
} ScnOpPanel;

typedef struct {
    BYTE    target;                 /* a slot or a team, as kind says */
    BYTE    kind;
    int32_t score;
    char    label[16];
} ScnOpScore;

typedef struct {
    BYTE     target;
    uint16_t ticks;
    char     text[SCN_TEXT_MAX];
} ScnOpAnnounce;

typedef struct {
    BYTE target;
    BYTE id;
    BYTE kind;      /* square, follow player, or clear */
    BYTE x, y;      /* square kind */
    BYTE slot;      /* follow-player kind */
    BYTE colour;
} ScnOpMarker;

/* ── Flow ──────────────────────────────────────────────────────── */

typedef struct {
    char text[SCN_TEXT_MAX];   /* the lobby's win line */
    BYTE winnerTeam;           /* 0 = none */
} ScnOpEndRound;

typedef struct {
    int32_t ticks;
    bool    relative;
} ScnOpSetGameTime;

/* ── Rules ─────────────────────────────────────────────────────── */

/* Every SimRules field, in the order the struct declares them. The index
 * enum below is generated from this one list, and so is the arm's write
 * table in server_sim_scenario.c, so an index and a field cannot drift
 * apart by hand: there is one list and two readings of it.
 *
 * What holds the list against the struct is in server_sim_scenario.c,
 * which can see both: a static assertion that the table is exactly
 * SCN_RULE_COUNT fields wide, so a field added to SimRules without a line
 * here does not compile, and the offsets case in
 * tests/unit/test_scenario_rule_arms.c, which walks the list against the
 * struct and fails on a line out of order or a field named twice.
 *
 * A field's name is the name a scenario uses for it (sim_rules.h), so the
 * enumerator carries that name verbatim rather than an upper-case
 * respelling of it: one spelling, and no second column to get wrong. */
#define SCN_RULE_LIST(X)                                                     \
    /* Tank */                                                               \
    X(tank_reload_ticks)                                                     \
    X(tank_full_shells)                                                      \
    X(tank_full_mines)                                                       \
    X(tank_full_trees)                                                       \
    X(tank_full_armour)                                                      \
    X(tank_death_ticks)                                                      \
    X(tank_water_ticks)                                                      \
    X(shell_damage)                                                          \
    X(mine_damage)                                                           \
    X(just_fired_ticks)                                                      \
    X(gunsight_min)                                                          \
    X(gunsight_max)                                                          \
    X(tank_accel_rate)                                                       \
    X(tank_decel_rate)                                                       \
    X(tank_brake_rate)                                                       \
    X(tank_autoslow_rate)                                                    \
    X(tank_min_move)                                                         \
    /* Terrain: the cap a tank's speed clamps to */                          \
    X(speed_road)                                                            \
    X(speed_grass)                                                           \
    X(speed_forest)                                                          \
    X(speed_river)                                                           \
    X(speed_swamp)                                                           \
    X(speed_crater)                                                          \
    X(speed_rubble)                                                          \
    X(speed_boat)                                                            \
    X(speed_deep_sea)                                                        \
    X(speed_refuel_base)                                                     \
    /* Terrain: bradians turned per tick */                                  \
    X(turn_road)                                                             \
    X(turn_grass)                                                            \
    X(turn_forest)                                                           \
    X(turn_river)                                                            \
    X(turn_swamp)                                                            \
    X(turn_crater)                                                           \
    X(turn_rubble)                                                           \
    X(turn_boat)                                                             \
    X(turn_deep_sea)                                                         \
    X(turn_refuel_base)                                                      \
    /* Shells */                                                             \
    X(shell_life)                                                            \
    X(shell_speed)                                                           \
    X(shell_start_add)                                                       \
    /* Builder */                                                            \
    X(lgm_build_ticks)                                                       \
    X(lgm_cost_road)                                                         \
    X(lgm_cost_building)                                                     \
    X(lgm_cost_repair_building)                                              \
    X(lgm_cost_pill_repair)                                                  \
    X(lgm_cost_boat)                                                         \
    X(lgm_cost_pill_new)                                                     \
    X(lgm_cost_mine)                                                         \
    X(lgm_pill_repair_load)                                                  \
    X(lgm_gather_trees)                                                      \
    X(lgm_helicopter_speed)                                                  \
    /* Pillbox */                                                            \
    X(pill_max_armour)                                                       \
    X(pill_attack_ticks)                                                     \
    X(pill_attack_min_ticks)                                                 \
    X(pill_cooldown_ticks)                                                   \
    X(pill_repair_amount)                                                    \
    X(pill_range)                                                            \
    /* Base */                                                               \
    X(base_full_armour)                                                      \
    X(base_full_shells)                                                      \
    X(base_full_mines)                                                       \
    X(base_capture_armour)                                                   \
    X(base_hit_armour)                                                       \
    X(base_min_armour)                                                       \
    X(base_min_shells)                                                       \
    X(base_min_mines)                                                        \
    X(base_armour_give)                                                      \
    X(base_shells_give)                                                      \
    X(base_mines_give)                                                       \
    X(base_refuel_armour_ticks)                                              \
    X(base_refuel_shells_ticks)                                              \
    X(base_refuel_mines_ticks)                                               \
    X(base_regen_ticks)                                                      \
    /* Terrain destruction and explosions */                                 \
    X(building_life)                                                         \
    X(rubble_life)                                                           \
    X(grass_life)                                                            \
    X(swamp_life)                                                            \
    X(mine_fuse_ticks)                                                       \
    X(big_explosion_threshold)                                               \
    /* Tree growth */                                                        \
    X(tree_grow_ticks)                                                       \
    X(tree_grow_initial_ticks)                                               \
    X(tree_weight_forest)                                                    \
    X(tree_weight_grass)                                                     \
    X(tree_weight_river)                                                     \
    X(tree_weight_boat)                                                      \
    X(tree_weight_deep_sea)                                                  \
    X(tree_weight_swamp)                                                     \
    X(tree_weight_rubble)                                                    \
    X(tree_weight_building)                                                  \
    X(tree_weight_half_building)                                             \
    X(tree_weight_crater)                                                    \
    X(tree_weight_road)                                                      \
    X(tree_weight_mine)

/* How a rule is named on the op: one member per SimRules field, in the
 * struct's own field order. SCN_RULE_COUNT is one past the last, and an
 * index at or above it names no rule. */
typedef enum {
#define SCN_RULE_ENUM_MEMBER(name) SCN_RULE_##name,
    SCN_RULE_LIST(SCN_RULE_ENUM_MEMBER)
#undef SCN_RULE_ENUM_MEMBER
    SCN_RULE_COUNT
} ScnRuleIndex;

/* Sixteen of the fields are float — the four tank rates, the ten terrain
 * turn rates and the two half-tick refuel intervals — so the value is a
 * double: an int32_t could not carry the 0.25 four of them hold today.
 * A double is exact for every integer rule in the table and for every
 * value a float rule can hold, which keeps the wire's fixed-point
 * encoding a wire concern rather than something the op has to know. The
 * arm converts to each field's own type as it writes. */
typedef struct {
    uint16_t rule;    /* a ScnRuleIndex */
    double   value;
} ScnOpSetRule;

/* One op, tagged by type. */
typedef struct {
    ScenarioOpType type;
    union {
        ScnOpTankSetStocks     tankSetStocks;
        ScnOpTankKill          tankKill;
        ScnOpTankTeleport      tankTeleport;
        ScnOpTankSetBoat       tankSetBoat;
        ScnOpTankGivePill      tankGivePill;
        ScnOpTankDropPill      tankDropPill;
        ScnOpTankSetModifiers  tankSetModifiers;
        ScnOpLgmDispatch       lgmDispatch;
        ScnOpLgmRecall         lgmRecall;
        ScnOpLgmKill           lgmKill;
        ScnOpLgmParachute      lgmParachute;
        ScnOpLgmSetCarried     lgmSetCarried;
        ScnOpPillSetOwner      pillSetOwner;
        ScnOpPillSetArmour     pillSetArmour;
        ScnOpPillSetSpeed      pillSetSpeed;
        ScnOpPillMove          pillMove;
        ScnOpBaseSetOwner      baseSetOwner;
        ScnOpBaseSetStock      baseSetStock;
        ScnOpEntityAddPill     entityAddPill;
        ScnOpEntityRemovePill  entityRemovePill;
        ScnOpEntityAddBase     entityAddBase;
        ScnOpEntityRemoveBase  entityRemoveBase;
        ScnOpEntityAddStart    entityAddStart;
        ScnOpEntityRemoveStart entityRemoveStart;
        ScnOpMapSetTile        mapSetTile;
        ScnOpMapFillRect       mapFillRect;
        ScnOpMapPlaceMine      mapPlaceMine;
        ScnOpMapRemoveMine     mapRemoveMine;
        ScnOpRosterSpawnBot    rosterSpawnBot;
        ScnOpRosterRemoveBot   rosterRemoveBot;
        ScnOpRosterSetTeam     rosterSetTeam;
        ScnOpLobbyAddBot       lobbyAddBot;
        ScnOpLobbyRemoveBot    lobbyRemoveBot;
        ScnOpLobbySetTeam      lobbySetTeam;
        ScnOpBotHint           botHint;
        ScnOpMsgAll            msgAll;
        ScnOpMsgTeam           msgTeam;
        ScnOpMsgPlayer         msgPlayer;
        ScnOpSound             sound;
        ScnOpLog               log;
        ScnOpPanel             panel;
        ScnOpScore             score;
        ScnOpAnnounce          announce;
        ScnOpMarker            marker;
        ScnOpEndRound          endRound;
        ScnOpSetGameTime       setGameTime;
        ScnOpSetRule           setRule;
    } u;
} ScenarioOp;

typedef enum {
    SCN_OP_OK = 0,
    SCN_OP_QUEUED,          /* accepted; applies on a later tick (spawn, remove, fill remainder) */
    SCN_OP_UNSUPPORTED,     /* the arm has not landed yet */
    SCN_OP_IN_POLICY,       /* issued from inside a policy callback */
    SCN_OP_WRONG_STATE,     /* lobby op while running, in-round op while in lobby, op during start */
    SCN_OP_NO_SUCH_PLAYER,  /* slot empty, or not connected when the op needs a tank */
    SCN_OP_TANK_DEAD,       /* op needs a live tank */
    SCN_OP_IS_HUMAN,        /* remove or lobby-remove aimed at a human */
    SCN_OP_NO_SUCH_ITEM,    /* pill, base, start or region index out of range or inactive */
    SCN_OP_BAD_SQUARE,      /* x or y off the map */
    SCN_OP_BAD_TERRAIN,     /* the square cannot hold this item (base on deep sea, mine on building) */
    SCN_OP_RANGE,           /* a numeric field outside the catalogue's range */
    SCN_OP_PAIR,            /* a rule value that breaks its paired invariant; the log names the pair */
    SCN_OP_CARRIED,         /* pill is in a tank; drop it first */
    SCN_OP_FULL,            /* 16 live items, 16 seats, or the carry list is full */
    SCN_OP_ALREADY,         /* add of an active item, give of a carried pill, spawn of a fielded seat */
    SCN_OP_TOO_BIG,         /* a list or text exceeds its buffer */
    SCN_OP_RATE,            /* a second panel update in one tick, or a budget */
    SCN_OP_NOT_FOUND,       /* a brain that does not resolve: a name this server does not have, a path where a name belongs, or a "package:" the funnel refuses */
    SCN_OP_NO_STOCK,        /* a builder order the tank cannot pay for */
    SCN_OP_BAD_CALL         /* no sim or no op: the call itself is malformed */
} ScnOpResult;

/* What an add or a spawn chose. */
typedef struct {
    BYTE index;
    BYTE slot;
} ScnOpOut;

/* canDie kind — what the blow would destroy. index is the tank slot for a
 * tank and for the builder riding in it, and the pill index for a pill. */
#define DIE_KIND_TANK    0
#define DIE_KIND_BUILDER 1
#define DIE_KIND_PILL    2

/* canCapture kind — what is being taken, with index the pill or base. */
#define CAPTURE_KIND_PILL 0
#define CAPTURE_KIND_BASE 1

/* What inflicted a hit. Mirrors ATTR_SRC_* on-disk. The stats funnel records
 * one of these with every blow, and canDie is handed one as the cause of a
 * builder's or a pill's death — a tank's cause is a LAST_DEATH_BY_* instead.
 * Here rather than beside DMG_TARGET_* in game_sim.h because the policy
 * surface hands them out and the host cannot see internal/. */
#define DMG_SRC_UNKNOWN 0
#define DMG_SRC_SHELL   1
#define DMG_SRC_MINE    2

/* announce kind — which newswire-worthy fact is being put to the policy.
 * The values are the policy's own vocabulary and never reach the wire; what
 * travels is the one-byte answer.
 *
 * subject is what the fact is about: the player slot for a roster fact, the
 * 0-based item[] index for a capture, the team for a vote line. actor is who
 * caused it, NEUTRAL where nobody did. */
#define ANNOUNCE_KIND_JOINED        0
#define ANNOUNCE_KIND_LEFT          1
#define ANNOUNCE_KIND_BASE_CAPTURED 2
#define ANNOUNCE_KIND_PILL_CAPTURED 3
#define ANNOUNCE_KIND_BUILDER_LOST  4
#define ANNOUNCE_KIND_NAME_CHANGED  5
#define ANNOUNCE_KIND_ALLIANCE      6
#define ANNOUNCE_KIND_VOTE          7

/* The decisions the sim asks the scenario, in the shape GameSimCallbacks
 * has. The sim calls through this and never knows Lua exists. */
typedef struct ScenarioPolicy {
    /* Every pointer optional; NULL = classic rules. */
    bool (*chooseStart)(void *ctx, BYTE player, BYTE *startIdx);
    bool (*allowBaseWin)(void *ctx);
    bool (*allowExtraTeams)(void *ctx);
    int  (*maxPlayers)(void *ctx);                 /* 0 = no cap */
    bool (*spawnLoadout)(void *ctx, BYTE player, ScnLoadout *out);
                       /* false = sim rules; out is a game-type mode
                        * or four explicit amounts, so a script can
                        * answer "open" or {shells, mines, armour,
                        * trees} */
    bool (*canRespawn)(void *ctx, BYTE player);    /* tickets, elimination */
    int  (*damageScale)(void *ctx, BYTE attacker, BYTE victim, BYTE cause);
                       /* percent: boss armour, handicaps, friendly fire
                        * off, an invulnerable escort */
    bool (*canBuild)(void *ctx, BYTE player, BYTE action, BYTE x, BYTE y,
                     BYTE idx);
                       /* tutorials, protected zones; idx is the pill
                        * for the place-pill action, unused otherwise */
    bool (*canCapture)(void *ctx, BYTE kind, BYTE idx, BYTE player);
                       /* locked objectives, flag rules */
    bool (*announce)(void *ctx, BYTE kind, BYTE subject, BYTE actor);
                       /* may this newswire-worthy fact be shown to
                        * players? kind is an ANNOUNCE_KIND_* above, with
                        * subject and actor as documented there. False
                        * stamps the quiet byte the fact carries, which
                        * every client line site reads before it writes a
                        * line; a vote line, which the server writes as
                        * text, is simply not sent. NULL = always */
    bool (*canDie)(void *ctx, BYTE kind, BYTE index, BYTE killer, BYTE cause);
                       /* may this tank, builder or pill be destroyed
                        * by this blow? kind: tank, builder, pill;
                        * index: the slot or the pill index; cause: a
                        * LAST_DEATH_BY_* value for a tank, the damage
                        * source otherwise. NULL = always. false leaves
                        * a tank at zero armour and alive, a builder
                        * untouched, a pill at one armour */
    void *ctx;
} ScenarioPolicy;

#endif /* SCENARIO_DEFS_H */
