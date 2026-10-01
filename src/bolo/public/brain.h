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


#ifndef _BRAINS_H
#define _BRAINS_H


#pragma pack(push, 8)
#include "global.h"
#include "input_packet.h"

#define local static
#define export
#define import extern

/* jm */

typedef BYTE Boolean;
#define TRUE 1
#define FALSE 0
/* end jm */

/* The various accessible tank control functions */
enum
	{
	KEY_faster=0, KEY_slower, KEY_turnleft, KEY_turnright,
	KEY_morerange, KEY_lessrange, KEY_shoot, KEY_dropmine,
	KEY_TankView, KEY_PillView
	};
#define setkey(CONTROLVECTOR, COMMAND) (CONTROLVECTOR |= (1<<COMMAND))
#define testkey(CONTROLVECTOR, COMMAND) ((CONTROLVECTOR & (1<<COMMAND)) != 0)

typedef BYTE TERRAIN;
enum
	{
	BBUILDING=0, BRIVER, BSWAMP, BCRATER, BROAD, BFOREST, BRUBBLE, BGRASS,
	BHALFBUILDING, BBOAT, BDEEPSEA, BREFBASE_T, BPILLBOX_T,
	TERRAIN_UNKNOWN,
	NUM_TERRAINS,
	TERRAIN_MASK     = 0x0F,
	TERRAIN_TANK_VIS = 0x10,
	TERRAIN_PILL_VIS = 0x20,
	TERRAIN_UNUSED   = 0x40,
	TERRAIN_MINE     = 0x80
	};

#define is_wet(A) ((A) == RIVER || (A) == BOAT || (A) == DEEPSEA)

typedef BYTE BUILDMODE;
enum
	{
	BUILDMODE_FARM=1, BUILDMODE_ROAD,
	BUILDMODE_BUILD, BUILDMODE_PBOX, BUILDMODE_MINE
	};

typedef struct
	{
	MAP_X x;
	MAP_Y y;
	BUILDMODE action;
	} BuildInfo;

/* Farming gets you 4 tree units.
 Roads, bridges and buildings take 2 units, boats take 20
 Placing a pillbox takes 4 units, repairing takes proportionately less */

#define NEUTRAL_PLAYER 0xFF
/* player id used to identify neutral bases and pillboxes */
#define FORESTVISUAL 0x30
#define MINRANGE 2
#define MAXRANGE 14		/* In HALF map-squares */
#define MAX_PILL_ARMOUR 15
#define MAX_BASE_SHELLS 90
#define MAX_BASE_MINES  90
#define MAX_BASE_ARMOUR 90
#define ARMOUR_COST 5
#define BASE_RESIST_SHELLS (ARMOUR_COST)
#define BASE_RESIST_TANKS (ARMOUR_COST*2)
#define MIN_BASE_ARMOUR (BASE_RESIST_TANKS + ARMOUR_COST - 1)
/*
A base bust have one armour unit or more to resist a shell --
 otherwise it is 'transparent' to shells.
 It must have two armour units to or more resist a tank.
 This is so that, after shooting a base down to 'transparency',
 there is some reasonble time window in which to drive onto it.
*/

#ifndef NETWORK_H
/* Only define it if we need it */
#include "platform_net.h"
#include <stdint.h>
#ifdef _WIN32
typedef unsigned char  u_char;
typedef unsigned short u_short;
#else
/* u_char / u_short. glibc's socket headers pull this in; Emscripten's do
 * not, and its libc declares them only under _GNU_SOURCE or _BSD_SOURCE. */
#include <sys/types.h>
#endif
typedef u_char  NIBBLE; /* to be interpreted as four bits */
#ifndef _GAMEID_DEFINED
#define _GAMEID_DEFINED
#pragma pack(push, 1)
typedef struct BOLO_PACK_ATTR {
  struct in_addr serveraddress;
  unsigned short serverport;
  uint16_t _padding;        /* explicit padding - matches original 76-byte wire format */
  uint32_t start_time;  /* was u_long: 8 bytes on 64-bit POSIX — fixed to 32-bit */
} GAMEID;
#pragma pack(pop)
#endif
#endif

typedef WORD WORLD_X, WORLD_Y;

#ifndef UCHAR36_DEFINED
#define UCHAR36_DEFINED
  typedef struct { u_char c[36]; } u_char36;
#endif


/* The rules view a brain reads. One field per gameplay number that has
   moved onto the sim's rules table; the names match the names a scenario
   uses, so a rule has one spelling across C, Lua and the scenario file.
   Grows as each group of constants converts — append only. */
typedef struct {
	int32_t tank_reload_ticks;
	int32_t tank_full_shells;
	int32_t tank_full_mines;
	int32_t tank_full_trees;
	int32_t tank_full_armour;
	int32_t tank_death_ticks;
	int32_t tank_water_ticks;
	int32_t mine_damage;
	int32_t just_fired_ticks;
	int32_t tank_min_move;
	float   tank_accel_rate;
	float   tank_decel_rate;
	float   tank_brake_rate;
	float   tank_autoslow_rate;
	/* The cap a tank's speed clamps to on each terrain, and the bradians it
	   turns per tick there. Building, half-building and pillbox have no rule:
	   they are impassable because the collision test says so, not because
	   their speed is zero. */
	int32_t speed_road;
	int32_t speed_grass;
	int32_t speed_forest;
	int32_t speed_river;
	int32_t speed_swamp;
	int32_t speed_crater;
	int32_t speed_rubble;
	int32_t speed_boat;
	int32_t speed_deep_sea;
	int32_t speed_refuel_base;
	float   turn_road;
	float   turn_grass;
	float   turn_forest;
	float   turn_river;
	float   turn_swamp;
	float   turn_crater;
	float   turn_rubble;
	float   turn_boat;
	float   turn_deep_sea;
	float   turn_refuel_base;
	/* What a pill's armour and a base's stocks top out at, and the interval
	   a pill fires on. A brain reading a pill or base strength against a
	   literal 15 or 90 is wrong the moment a sim changes one. */
	int32_t pill_max_armour;
	int32_t pill_attack_ticks;
	int32_t pill_attack_min_ticks;
	int32_t base_full_armour;
	int32_t base_full_shells;
	int32_t base_full_mines;
	/* The damage a shell does to a tank before any modifier, and how a shell
	   flies: its life in ticks, its speed and the longest gunsight. */
	int32_t shell_damage;
	int32_t shell_life;
	int32_t shell_speed;
	int32_t gunsight_max;
	int32_t tree_hide_distance;
	/* The rest of the pill numbers: how long anger lasts, what a tree of
	   repair gives back, how far a pill fires (WORLD units), what a tank's
	   shell takes off a pill and how fast a hit angers it. */
	int32_t pill_cooldown_ticks;
	int32_t pill_repair_amount;
	int32_t pill_range;
	int32_t pill_shell_damage;
	int32_t pill_angry_divisor;
	/* The base numbers a brain prices a capture or a steal with, and how
	   often a base regenerates. */
	int32_t base_capture_armour;
	int32_t base_hit_armour;
	int32_t base_regen_ticks;
	/* The builder: how long a build takes, what each job costs in trees,
	   what a forest gives, and how fast a dead builder flies back. */
	int32_t lgm_build_ticks;
	int32_t lgm_cost_road;
	int32_t lgm_cost_building;
	int32_t lgm_cost_pill_repair;
	int32_t lgm_cost_boat;
	int32_t lgm_cost_pill_new;
	int32_t lgm_cost_mine;
	int32_t lgm_gather_trees;
	int32_t lgm_helicopter_speed;
	/* The cap the builder's walk clamps to on each terrain. */
	int32_t man_speed_road;
	int32_t man_speed_grass;
	int32_t man_speed_forest;
	int32_t man_speed_river;
	int32_t man_speed_swamp;
	int32_t man_speed_crater;
	int32_t man_speed_rubble;
	int32_t man_speed_boat;
	int32_t man_speed_deep_sea;
	int32_t man_speed_refuel_base;
} BrainRules;

/* This tank's own modifiers, each a percent of the classic figure. The
   engine stores 0 for "classic"; this view resolves that to 100 so a brain
   can multiply by it directly. */
typedef struct {
	u_short speed;
	u_short accel;
	u_short turn;
	u_short reload;
	u_short dealt;
	u_short taken;
} BrainTankMods;

enum { GameType_open=1, GameType_tournament, GameType_strict_tment };

#define GAMEINFO_HIDDENMINES 0x80
#define GAMEINFO_ALLMINES_VISIBLE 0xC0

typedef struct
	{
	u_char36 mapname;
	GAMEID gameid;
	BYTE gametype;
	BYTE hidden_mines;
	/* has the value GAMEINFO_HIDDENMINES or GAMEINFO_ALLMINES_VISIBLE */
	BYTE allow_AI;
	BYTE assist_AI;
	int32_t start_delay; /* was long — 8 bytes on 64-bit POSIX */
	int32_t time_limit;  /* was long — 8 bytes on 64-bit POSIX */
	} GAMEINFO;

/* *********************************************************************/

typedef u_short OBJECT;
enum
	{
	OBJECT_TANK=0,
	OBJECT_SHOT,
	OBJECT_PILLBOX,
	OBJECT_REFBASE,
	OBJECT_BUILDMAN,
	OBJECT_PARACHUTE
	};

#define OBJECT_HOSTILE 1	/* Object is hostile to us */
#define OBJECT_NEUTRAL 2	/* Object is not loyal to any other player */
/* Note that being neutral means that an object has no particular loyalty
to any player -- whether it is hostile or friendly to us is an orthogonal
question. Currently, neutral refuelling bases are friendly to everyone
and neutral pillboxes are hostile to everyone. */

typedef struct
	{
	OBJECT object;
	WORLD_X x;
	WORLD_Y y;
	WORD idnum;
	BYTE direction;
	BYTE info;
	BYTE speed;       /* actual_speed * 4, 0 for non-tank objects */
	} ObjectInfo;

/* For pillboxes and refuelling bases, the 'direction' fiel
actually holds the armour strength */
#define pillbox_strength direction
#define refbase_strength direction

typedef struct
	{
	u_short sender;
	PlayerBitMap *receivers;
	u_char *message;
	} MessageInfo;

/* This header file describes Bolo BrainInfo structure version 3 */
#define CURRENT_BRAININFO_VERSION 3

enum { BRAIN_OPEN=0, BRAIN_CLOSE, BRAIN_THINK, BRAIN_MENU=200 };

typedef struct
	{
	u_short BoloVersion;	// two hex bytes, eg. 0x0098 means version 0.98
	u_short InfoVersion;	// current version of the BrainInfo structure is 1
	void *userdata;		// Initially points to address of your CODE resource
	u_short PrefsVRefNum;
	u_char *PrefsFileName;
	u_short operation;		// 0=OPEN, 1=CLOSE, 2=THINK, 200= menu
	u_short menu_item;

	// Interface providing information about the Bolo world
	
	u_short max_players;	// Players are numbered from 0 to max_players-1
	u_short max_pillboxes;	// Pillboxes are numbered from 0 to max_pillboxes-1
	u_short max_refbases;	// Bases are numbered from 0 to max_refbases-1
	u_short player_number;	// Who Am I?
	u_short num_players;	// How many players currently active in this game?
	u_char36 **playernames;	// Array of pointers to pascal strings
	PlayerBitMap *allies;	// Who you are currently allied to
	/* Per-slot PLAYER_FLAG_BOT bitmap: bit N set iff slot N is a
	 * brain-driven player. Brains use (allies & ~player_bots) to
	 * pick out allied humans only — e.g. for chat that should reach
	 * teammates but not other bots' inboxes. */
	PlayerBitMap *player_bots;

	WORLD_X tankx;
	WORLD_Y tanky;
	
	BYTE direction;
	/* Float (un-quantized) tank angle, 0..256. The engine stores
	 * tank->angle as a float and fires shells at that exact value;
	 * `direction` above is just (BYTE)angle (a floor). Brains that
	 * need pixel-precise aiming — crosshair display, fire-when-
	 * lined-up checks, simulate_shot prediction — must use this
	 * instead of `direction` or the prediction won't match the
	 * engine's actual flight. */
	float tank_angle;
	BYTE speed;				// 64 is top speed on road, 48 on grass,
      							// 24 in forest, 12 on rubble, crater, water etc.
	BYTE inboat;			// non-zero means currently on boat
	BYTE hidden;			// non-zero means hidden inside forest
	BYTE shells;			// Range 0-40
	BYTE mines;				// Range 0-40
	BYTE armour;			// Range 0-8
	BYTE trees;				// Range 0-40

	BYTE carriedpills;		// Number of pillboxes the tank is carrying
	BYTE carriedbases;		// Number of refuelling bases (zero in current versions)
	WORD padding2;

	BYTE gunrange;			// in units of half map squares
	BYTE reload;			// non-zero means cannot fire shell immediately
	BYTE newtank;			// Set initially, and each time tank is killed
	BYTE tankobstructed;	// Set if tank has hit obstacle
	
	ObjectInfo *base;		// will be NULL if no friendly base nearby
	BYTE base_shells;
	BYTE base_mines;
	BYTE base_armour;
	BYTE padding3;			// unused at present

	BYTE man_status;		// 0 = in tank, 1 = dead, other = outside
	BYTE man_direction;
	WORLD_X man_x;
	WORLD_Y man_y;
	BYTE manobstructed;		// 0 = free, 1 = touching wall, 2 = completely stuck
	BYTE padding4;			// unused at present

	WORD *pillview;			// top bit set (0x8000) means view is from tank
	MAP_Y view_top;			// coordinate of topmost square of current view
	MAP_X view_left;		// coordinate of leftmost square of current view
	BYTE  view_height;		// height of the view, in map squares
	BYTE  view_width;		// width of the view, in map squares
	TERRAIN *viewdata;		// view_width*view_height bytes of terrain data

	WORD padding5;			// unused at present
	u_short num_objects;	// number of moving objects visible
	ObjectInfo *objects;	// array information about those objects

	/* Legacy single-message field. Set to &messages[0] when num_messages > 0
	 * (NULL otherwise). New brain code should iterate the messages array
	 * below to avoid losing extra arrivals in the same tick — historically
	 * this was a single-slot buffer that overwrote on every arrival. */
	MessageInfo *message;
	/* Full per-tick inbox: every chat message addressed to this brain
	 * during the current tick, in FIFO order. Owned by the BrainInfo
	 * (freed in brainDataExtractInfo); pointers inside each MessageInfo
	 * (receivers, message) are also owned and freed there. */
	MessageInfo *messages;
	u_short num_messages;

	// Interface to control the tank
	uint32_t *holdkeys; /* was u_long: key bitmask, must be 32-bit */
	uint32_t *tapkeys;  /* was u_long: key bitmask, must be 32-bit */
	BuildInfo *build;
	PlayerBitMap *wantallies;// Who you want to be allied to
	PlayerBitMap *messagedest;
	u_char *sendmessage;
	
	// New additions for version 3
	const TERRAIN *theWorld;// Pointer to 256x256 map squares (arranged in rows)
	GAMEINFO gameinfo;		// The game options

	// New additions for brain event interface
	uint32_t server_tick;       // Server tick counter
	BYTE assistant_msg;         // ASSIST_MSG_* or 0
	u_short num_events;         // Count of game events this tick
	GameEvent *events;          // Pointer to event array

	// Dead-tick hook: TRUE when the brain is invoked for a tank that is dead
	// (waiting to respawn). The think still runs so the brain can reset its
	// own state for a clean respawn, but its outputs are ignored (a dead tank
	// can't act) and it should early-return without acting on world data.
	BYTE dead;

	// TRUE once the tank has been destroyed, which is not the same question as
	// "dead" above: "dead" is set for the whole respawn wait, while "destroyed"
	// tracks the tank's own state and clears when it respawns. Ask this rather
	// than comparing "armour" against tank_full_armour — a live tank can sit at
	// zero armour, so the armour value alone cannot tell the two apart.
	BYTE destroyed;

	// The gameplay numbers this sim runs on, as of this tick. Read these
	// rather than assuming the classic values: a scenario can change any of
	// them, and a brain that divides by 40 or waits 13 ticks is wrong the
	// moment one does. brain_data.c fills it from the sim once per tick.
	// Appended, as every addition to this struct is — brains index the
	// fields in front of it by offset, so nothing above may move.
	BrainRules rules;

	// A smart ping the brain asks the engine to put on the map this think.
	// ping_pending is 0 on every think the engine builds, so a brain that
	// says nothing places nothing; set it to non-zero and the other three
	// fields are read. The engine turns the request into a CMD_PING from
	// the brain's OWN player slot, so the marker is team-filtered, drawn
	// and recorded exactly like a marker a person placed. ping_kind is a
	// PING_KIND_* (input_packet.h); ping_x / ping_y are WORLD units, 256
	// to a map square. The engine rate-limits bot pings, so a request can
	// be dropped and the brain is not told.
	BYTE    ping_pending;
	BYTE    ping_kind;
	WORLD_X ping_x;
	WORLD_Y ping_y;

	// Which pill and base numbers are on the map this tick: bit n set means
	// pill (or base) n, 0 based, is on it. A scenario can take a pill or a
	// base off the map for good. The slot stays, so the numbers above it do
	// not move, but the item is not shown in objects again. A brain that
	// remembers items it saw earlier needs these to know one has gone,
	// because an item that is merely out of sight looks the same in objects.
	// A carried pill is on the map for this purpose; only a removal clears
	// the bit. Bits at or past max_pillboxes / max_refbases are always 0.
	uint32_t pills_on_map;
	uint32_t bases_on_map;

	// This tank's own modifiers (a scenario sets them per tank, at any tick)
	// and the reload interval they give, in ticks. brain_data.c fills both
	// once per tick. Appended, like everything else here.
	BrainTankMods mods;
	BYTE    reload_ticks;

	} BrainInfo;

#pragma pack(pop)
#endif /* _BRAINS_H */
