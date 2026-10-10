/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
#include "sim_rules_names.h" /* SIM_RULE_LIST — the rule list SCN_RULE_LIST is */
#include "scenario_panel.h" /* SCN_PANEL_MAX — the panel op's byte capacity */

/* Text capacity for every op that carries a line. The server-text
 * control event holds char text[PACKET_MAX_CHAT_MESSAGE + 1]
 * (control_event.h), and a scenario line is sent as one of those, so
 * the op's buffer is that buffer. Text past it is refused, never
 * truncated. */
#define SCN_TEXT_MAX (PACKET_MAX_CHAT_MESSAGE + 1)

/* The longest announcement that may carry a position. A positioned line
 * rides as the text, a 0x00 and the two position bytes in the announce body.
 * A client older than the position reads all of that as the text and drops a
 * body past PACKET_MAX_CHAT_MESSAGE, so a positioned text of 126 to 128 bytes
 * would show nothing there. The script layer and the arm refuse a positioned
 * line past this, as they refuse every other line past its limit, rather
 * than cutting it. A line with no position keeps the full SCN_TEXT_MAX - 1. */
#define SCN_ANNOUNCE_POSITIONED_TEXT_MAX (PACKET_MAX_CHAT_MESSAGE - 3)

/* SCN_PANEL_MAX, the byte capacity of one panel display list, comes in
 * from scenario_panel.h with the rest of the panel's public types. It
 * moved there when the control event that delivers a list needed the
 * same number: control_event.h is public and cannot read this
 * directory, and two copies of the figure would drift. */

/* Buffer for a brain: the name a scenario writes — a directory under the
 * server's own brains/, such as "GoalHunter" — and the path that name is
 * resolved to before the sim is handed it. */
#define SCN_PATH_MAX 256

/* A bot mode key or a level key, as a team template or a bot op names it,
 * matched against the keys in the brain's own modes.txt. The same number
 * BRAIN_MODE_KEY_LEN holds in public/brain_list.h and SCN_BOT_KEY_LEN holds
 * on the host's side of the wall; server_sim_lobby.c is where all three
 * meet, and it is the one file that sees every one of them. */
#define SCN_BOT_KEY_MAX 16

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
 * mode and difficulty name the brain mode these bots play in and the level
 * inside it, by the keys the brain's modes.txt lists. "" for either leaves
 * the seat's config as the lobby would have had it, which is what every
 * template before these two fields said.
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
    char     mode[SCN_BOT_KEY_MAX];        /* "" = leave the lobby's */
    char     difficulty[SCN_BOT_KEY_MAX];  /* "" = leave the lobby's */
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
 * the round is gameScripted. 0 means none declared, which plays strict
 * tournament. */
typedef struct {
    uint8_t      maxPlayers;
    uint8_t      numTeams;
    uint8_t      baseGameType;   /* a gameType value, 0 for none */
    ScnLobbyTeam teams[MAX_TANKS];
} ScnLobbyTemplate;

/* What a scenario is called and what it asks for, as the sim reads it.
 *
 * The name and description lengths are SCN_SCENARIO_NAME_LEN and
 * SCN_SCENARIO_DESC_LEN in src/scenario_io/scenario_manifest.h, stated again
 * under names of their own: that header is the one a frontend includes and
 * this one is not, so a gui or runtime_only translation unit can reach it and
 * not this. scenario_dir.c sees both and holds each pair against the other,
 * so the two cannot drift.
 *
 * The file name is a name in the directory and never a path: the directory is
 * flat, and where it is on disk is the server's own business. */
#define SCN_DIR_FILE_LEN 128
#define SCN_DIR_NAME_LEN 64
#define SCN_DIR_DESC_LEN 256

/* One scenario a server offers on its own, independently of any map: a
 * .scenario package or a loose .lua in the scenarios directory, read into the
 * few fields a chooser needs to show it.
 *
 * The sim holds no notion of what is in either file. It is handed a filled
 * array by the lister registered on it (serverSimSetScenarioLister), which is
 * the scenario library's to implement, and it passes the entries to the wire
 * layer — the dependency points one way, as it does for the lobby template.
 *
 * maxPlayers is the cap the scenario asks for, 0 leaving the server's own.
 * bots is the seats its lobby template asks for, summed over its teams and
 * held at 255 because it travels in one byte. bound true says the scenario is
 * tied to the map it was written against, which is what makes it no use as a
 * mod. keepsWinCondition true says the file declares itself a mod: it changes
 * how the game plays and leaves the win condition alone, so several of them
 * can run at once behind one scenario. The two are separate questions and a
 * file may answer either way to both.
 *
 * source says which of the server's directories the row was read from, one
 * of the SCN_DIR_SOURCE_* values below. A directory read leaves it
 * SCN_DIR_SOURCE_SERVER; the merged lister sets it for the rows it takes
 * from the uploads directory. workshopId is the Steam Workshop item the file
 * came from, 0 for none, and workshopAuthor the SteamID64 of the account that
 * published it, 0 for none. The author is for this computer's own listings
 * (the Settings dialog's Workshop section asks it to offer Update rather than
 * Publish) and is never sent: the scenario-list packet and the script-list
 * event carry the id and not the author. */
typedef struct {
    char     file[SCN_DIR_FILE_LEN];  /* the name in the directory */
    char     name[SCN_DIR_NAME_LEN];  /* the manifest's */
    char     description[SCN_DIR_DESC_LEN];
    uint8_t  maxPlayers;
    uint8_t  bots;
    bool     bound;
    bool     keepsWinCondition;
    uint8_t  source;
    uint64_t workshopId;
    uint64_t workshopAuthor;
} ScnDirEntry;

/* ScnDirEntry.source. One byte on the wire, in the scenario-list packet and
 * the script-list event. */
#define SCN_DIR_SOURCE_SERVER   0 /* one of the server's own directories */
#define SCN_DIR_SOURCE_UPLOAD   1 /* the directory players upload scripts to */
#define SCN_DIR_SOURCE_WORKSHOP 2 /* the directory Workshop items sync into */

/* How many roster changes may be outstanding at once. Spawns and
 * removals share one first-in first-out queue and the sim drains one of
 * them a tick, so a script that asks for ten bots gets them over ten
 * ticks without knowing the rate. The one past the last is refused
 * SCN_OP_FULL rather than displacing anything already accepted. */
#define SCN_ROSTER_QUEUE_MAX 32

/* How many tiles a fill and set_tile may change in one tick between
 * them. A rectangle that changes more than this applies what the
 * budget allows, keeps the remainder and answers SCN_OP_QUEUED; the
 * sim carries the rest on later ticks, one budget each. A fill with no
 * budget left in the tick it arrives in is refused rather than queued,
 * so nothing that cannot move is left on the sim. A whole-map fill
 * takes 256 ticks. Every set_tile that applies spends one square of
 * the same budget, and one that arrives with none left is refused
 * SCN_OP_RATE.
 *
 * The number is also the depth of the server's per-frame map event
 * buffer, which is a collision rather than a design: a fill spending
 * the whole budget fills that buffer on its own and the frame's other
 * terrain changes are dropped where they are recorded. Lowering this
 * is what would leave them room. */
#define SCN_TILES_PER_TICK 256

/* How many ops a script may send in one tick, whatever they are. The
 * one past the last is refused SCN_OP_RATE without being looked at,
 * and the allowance comes back with the next tick. The host's own ops
 * — the scenario file's rules at the round start, and the line saying
 * a script was switched off — are not counted.
 *
 * The same as SCN_TILES_PER_TICK and the frame's map event buffer, so
 * a script redrawing a coast one square at a time is held by the tile
 * budget rather than by this. A starting value; a measurement may want
 * it somewhere else. */
#define SCN_OPS_PER_TICK 256

/* And how many of those may reach the players: SCN_OP_MSG_ALL,
 * SCN_OP_MSG_TEAM, SCN_OP_MSG_PLAYER, SCN_OP_MSG_SAY and SCN_OP_SOUND,
 * and no others. The one past the last is refused SCN_OP_RATE, as the
 * op count's is. SCN_OP_LOG is not among them: it writes the round log
 * and sends nothing to anybody, so SCN_OPS_PER_TICK is the only thing
 * that bounds it. A starting value, as the op count is. */
#define SCN_MSGS_PER_TICK 8

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
    SCN_OP_ROSTER_BOT_INIT,

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
    SCN_OP_MSG_SAY,
    SCN_OP_SOUND,
    SCN_OP_LOG,
    SCN_OP_SET_VOICE_EVERYONE,

    /* Presentation */
    SCN_OP_PANEL,
    SCN_OP_SCORE,
    SCN_OP_ANNOUNCE,
    SCN_OP_MARKER,
    SCN_OP_STATUS,
    SCN_OP_POPUP,

    /* Flow */
    SCN_OP_END_ROUND,
    SCN_OP_SET_GAME_TIME,

    /* Rules */
    SCN_OP_SET_RULE,

    /* Test hooks. Not part of the round a player plays: each one drives a
     * server path a script has no other way to reach. */
    SCN_OP_SHELL_EXPIRED
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
    /* The brain mode and the level inside it, by the keys the brain's
     * modes.txt lists; "" leaves the seat's config alone. */
    char     mode[SCN_BOT_KEY_MAX];
    char     difficulty[SCN_BOT_KEY_MAX];
    ScnTable init;
} ScnOpRosterSpawnBot;

typedef struct {
    BYTE slot;
} ScnOpRosterRemoveBot;

typedef struct {
    BYTE slot;
    BYTE team;
} ScnOpRosterSetTeam;

/* New data for a bot that is already playing. The same flat table a spawn
 * hands a bot at its first breath, handed to one in the middle of a round:
 * the bot's BRAIN_INIT is rebuilt from it and the brain is told, so a script
 * can change a bot's orders rather than only choose them once. */
typedef struct {
    BYTE     slot;
    ScnTable init;
} ScnOpRosterBotInit;

/* ── Roster, lobby ─────────────────────────────────────────────── */

typedef struct {
    BYTE slot;
    char name[PLAYER_NAME_LEN];
    char brain[SCN_PATH_MAX];   /* resolved the same way a spawn's is */
    BYTE team;
    bool fielded;
    /* The brain mode and the level inside it, by the keys the brain's
     * modes.txt lists; "" leaves the seat's config alone. A held seat takes
     * them too — nothing loads a brain for it yet, and the spawn that fields
     * it later reads the config off the seat. */
    char mode[SCN_BOT_KEY_MAX];
    char difficulty[SCN_BOT_KEY_MAX];
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

/* A chat line said by a seat, not by the server. slot is who said it, and
 * mode is who hears it: a player's three destinations are their own team,
 * the whole game, and one other seat. A team other than the sender's own is
 * not among them — the chat path refuses a line addressed to a team the
 * sender is not on, whoever sends it. */
#define SCN_SAY_TEAM   0    /* the sender's own team */
#define SCN_SAY_ALL    1    /* everyone */
#define SCN_SAY_PLAYER 2    /* one seat, named by target */

typedef struct {
    BYTE slot;
    BYTE mode;
    BYTE target;                /* the seat, under SCN_SAY_PLAYER */
    char text[SCN_TEXT_MAX];
} ScnOpMsgSay;

typedef struct {
    BYTE sound;     /* an sndEffects value */
    BYTE x, y;      /* 0xFF, 0xFF = everywhere */
} ScnOpSound;

typedef struct {
    char text[SCN_TEXT_MAX];
} ScnOpLog;

/* Voice to everyone. While on is set, voice in a running round goes to
 * every player rather than to the talker's allies alone. It lasts until the
 * script turns it off or the round ends. */
typedef struct {
    bool on;
} ScnOpSetVoiceEveryone;

/* ── Presentation ──────────────────────────────────────────────── */
/* target: 0 = all, 1..15 = team, 0x80 | slot = one player. */

typedef struct {
    BYTE     target;
    BYTE     panel;                 /* 0..SCN_PANEL_IDS-1 */
    /* The calling script's position on the round's list, 0 for the first
       and for a call no script made. Filled by the host, never by the
       script, and packed with the panel id by SCN_PANEL_WIRE. */
    BYTE     owner;
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
    BYTE     hasPos;                /* 0: drawn where announcements always
                                       were; else posX/posY say where */
    BYTE     posX;                  /* the line's centre, 0 to
                                       SCN_ANNOUNCE_POS_MAX across the view */
    BYTE     posY;                  /* the same, down the view */
    char     text[SCN_TEXT_MAX];
} ScnOpAnnounce;

/* The status line: a line at the very top of the game view that stays until
 * the script changes it or clears it. endsAt is a server tick, on the clock
 * game.tick() answers, that the client counts down to and shows beside the
 * text as M:SS; SCN_STATUS_NO_COUNTDOWN (scenario_panel.h) is no countdown.
 * Empty text is the clear. */
typedef struct {
    BYTE     target;
    uint32_t endsAt;
    char     text[SCN_TEXT_MAX];
} ScnOpStatus;

/* A popup: a message box the player has to close, drawn through the
 * client's tutorial overlay, which pauses a local game while it is up.
 * The text may carry the tutorial's key tokens ({ACCEL}, {QUICK_TREE} and
 * so on), which each client turns into its own keys. A popup is shown
 * once and is not replayed to a client that joins later. */
typedef struct {
    BYTE target;
    char text[SCN_POPUP_TEXT_MAX + 1];
} ScnOpPopup;

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

/* Every SimRules field, in the order the struct declares them, with the
 * type its field holds and the unit it is read in. The list itself lives in
 * public/sim_rules_names.h, where a frontend can see it: the editor's rules
 * form and the lobby's popup name a rule and describe a value, and neither
 * of them sees this directory. SCN_RULE_LIST is that list under the name
 * the scenario surface has always spelled it, so the expansion sites here
 * and in server_sim_scenario.c are unchanged apart from the two columns
 * they ignore.
 *
 * The index enum below is generated from the list, and so is the arm's
 * write table in server_sim_scenario.c, so an index and a field cannot
 * drift apart by hand: there is one list and two readings of it.
 *
 * What holds the list against the struct is the static assertion in
 * sim_rules.c, which can see both — sizeof(SimRules) against
 * SIM_RULE_COUNT times four, so a field added to SimRules without a row in
 * the list does not compile — the assertion in server_sim_scenario.c that
 * the write table is exactly SCN_RULE_COUNT fields wide, and the offsets
 * case in tests/unit/test_scenario_rule_arms.c, which walks the list
 * against the struct and fails on a row out of order or a field named
 * twice. */
#define SCN_RULE_LIST(X) SIM_RULE_LIST(X)

/* How a rule is named on the op: one member per SimRules field, in the
 * struct's own field order. SCN_RULE_COUNT is one past the last, and an
 * index at or above it names no rule. */
typedef enum {
#define SCN_RULE_ENUM_MEMBER(name, kind, unit) SCN_RULE_##name,
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

/* ── Test hooks ────────────────────────────────────────────────── */

/* One of `slot`'s shells ran its full range and died on square (x, y) with
 * nothing hit. It fires the three-shot order detector exactly as a real
 * expiring shell does, without a gun having to be aimed: a script cannot
 * make a seat shoot, and three full-range shells landing on one chosen
 * square is not something a round can be steered into. Nothing else happens
 * — no explosion, no sound, no shell is created or destroyed.
 *
 * fireTick is the SERVER tick the shell LEFT THE GUN, which is what every
 * timing rule in the detector reads — the window the three have to share,
 * and the quiet second either side of them. haveFireTick is false when the
 * script did not say, and the shell then counts as fired on the current
 * tick. (x, y) must name a square the map really holds; anything outside
 * the playable band is refused with SCN_OP_BAD_SQUARE. */
typedef struct {
    BYTE     slot;
    BYTE     x, y;
    bool     haveFireTick;
    uint32_t fireTick;
} ScnOpShellExpired;

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
        ScnOpRosterBotInit     rosterBotInit;
        ScnOpLobbyAddBot       lobbyAddBot;
        ScnOpLobbyRemoveBot    lobbyRemoveBot;
        ScnOpLobbySetTeam      lobbySetTeam;
        ScnOpBotHint           botHint;
        ScnOpMsgAll            msgAll;
        ScnOpMsgTeam           msgTeam;
        ScnOpMsgPlayer         msgPlayer;
        ScnOpMsgSay            msgSay;
        ScnOpSound             sound;
        ScnOpLog               log;
        ScnOpSetVoiceEveryone  setVoiceEveryone;
        ScnOpPanel             panel;
        ScnOpScore             score;
        ScnOpAnnounce          announce;
        ScnOpMarker            marker;
        ScnOpStatus            status;
        ScnOpPopup             popup;
        ScnOpEndRound          endRound;
        ScnOpSetGameTime       setGameTime;
        ScnOpSetRule           setRule;
        ScnOpShellExpired      shellExpired;
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
    SCN_OP_NO_RUNNER,       /* a bot seat with no brain behind it: never fielded, or its runner released */
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

/* canHit kind — what a shell has reached. index is the tank slot for a tank
 * and the pill index for a pill. */
#define HIT_KIND_TANK 0
#define HIT_KIND_PILL 1

/* What inflicted a hit. Mirrors ATTR_SRC_* on-disk. The stats funnel records
 * one of these with every blow, and canDie is handed one as the cause of a
 * builder's or a pill's death — a tank's cause is a LAST_DEATH_BY_* instead.
 * Here rather than beside DMG_TARGET_* in game_sim.h because the policy
 * surface hands them out and the host cannot see internal/.
 *
 * DMG_SRC_EXPLOSION is a dying tank's blast. It is only ever put to the
 * policy questions and is never recorded: the blast's damage to a pill has
 * never been a stats record, so there is no ATTR_SRC_* beside it. */
#define DMG_SRC_UNKNOWN   0
#define DMG_SRC_SHELL     1
#define DMG_SRC_MINE      2
#define DMG_SRC_EXPLOSION 3

/* The pill argument every combat question carries: the pill index of the
 * pillbox whose shell dealt the blow, or this for a blow no pillbox's shell
 * dealt. The attacker beside it is NEUTRAL for a pillbox's shell either way,
 * so this is what says which pillbox it was. */
#define DMG_NO_PILL 0xFF

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
    int  (*damageScale)(void *ctx, BYTE attacker, BYTE victim, BYTE cause,
                        BYTE pill);
                       /* percent: boss armour, handicaps, friendly fire
                        * off, an invulnerable escort. pill is the
                        * pillbox whose shell it was, DMG_NO_PILL
                        * otherwise */
    bool (*canHit)(void *ctx, BYTE attacker, BYTE kind, BYTE index,
                   BYTE pill);
                       /* may this shell hit the tank or pill it has
                        * reached? kind is a HIT_KIND_*. NULL = always.
                        * false lets the shell fly on as if the target
                        * were not there */
    int  (*pillDamageScale)(void *ctx, BYTE attacker, BYTE index, BYTE cause,
                            BYTE pill);
                       /* percent of the armour a blow takes off pill
                        * index; cause is DMG_SRC_SHELL or
                        * DMG_SRC_EXPLOSION. NULL = 100 */
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
    bool (*canDie)(void *ctx, BYTE kind, BYTE index, BYTE killer, BYTE cause,
                   BYTE pill);
                       /* may this tank, builder or pill be destroyed
                        * by this blow? kind: tank, builder, pill;
                        * index: the slot or the pill index; cause: a
                        * LAST_DEATH_BY_* value for a tank, the damage
                        * source otherwise; pill: the pillbox whose
                        * shell it was, DMG_NO_PILL otherwise. NULL =
                        * always. false leaves a tank at zero armour and
                        * alive, a builder untouched, a pill at one
                        * armour */
    bool (*canAlly)(void *ctx, BYTE player, BYTE other);
                       /* may player ally with other? Asked when player
                        * requests the alliance and again when other
                        * accepts it. NULL = always. false refuses the
                        * request or the accept; alliances a script makes
                        * through set_team or seating are not asked */
    void *ctx;
} ScenarioPolicy;

#endif /* SCENARIO_DEFS_H */
