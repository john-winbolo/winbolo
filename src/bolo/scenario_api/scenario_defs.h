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
 *  directory is on the include path of scenario_host,
 *  sim_owner and unittests alone, so a frontend that
 *  includes this fails to build.
 *********************************************************/

#ifndef SCENARIO_DEFS_H
#define SCENARIO_DEFS_H

#include "global.h"      /* BYTE, PLAYER_NAME_LEN */
#include "types.h"       /* TankModifiers — the modifier op's payload */
#include "wire_limits.h" /* PACKET_MAX_CHAT_MESSAGE — the text cap below */

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

/* Buffer for a brain path or a "package:NAME" reference. */
#define SCN_PATH_MAX 256

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

/* The init and hint tables: how many pairs, and how long a key and a
 * value may be. */
#define SCN_TABLE_MAX         16
#define SCN_TABLE_KEY_LEN     24
#define SCN_TABLE_VALUE_LEN   64

typedef struct {
    char key[SCN_TABLE_KEY_LEN];
    char value[SCN_TABLE_VALUE_LEN];
} ScnKV;

typedef struct {
    uint8_t count;
    ScnKV   kv[SCN_TABLE_MAX];
} ScnTable;

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
    char     brain[SCN_PATH_MAX];    /* a path or "package:NAME" */
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
    char brain[SCN_PATH_MAX];
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

typedef struct {
    uint16_t rule;
    int32_t  value;
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
    SCN_OP_NOT_FOUND,       /* brain path or package name that does not resolve */
    SCN_OP_NO_STOCK         /* a builder order the tank cannot pay for */
} ScnOpResult;

/* What an add or a spawn chose. */
typedef struct {
    BYTE index;
    BYTE slot;
} ScnOpOut;

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
                        * players? kind: joined, left, base captured,
                        * pill captured, builder lost, name changed,
                        * alliance changed, vote line. NULL = always */
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
