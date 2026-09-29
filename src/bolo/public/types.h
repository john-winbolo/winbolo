/*
 * $Id$
 *
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


#ifndef _TYPES_H
#define _TYPES_H

/* Ensure this header is isolated from ambient pack state */
#pragma pack(push, 4)

#include "global.h"
#include <stddef.h>
/* Defines */
#define MAX_BASES 16


/* Typedefs */


#define SIZEOF_BASES 260 /* 196 */
typedef struct {
  BYTE x;            /* Co-ordinates on the map */
  BYTE y;
  BYTE owner;        /* should be 0xFF except in speciality maps */
  BYTE armour;       /* initial stocks of base. Maximum value 90 */
  BYTE shells;       /* initial stocks of base. Maximum value 90 */
  BYTE mines;        /* initial stocks of base. Maximum value 90 */
  BYTE refuelTime;   /* Refuelling Time */
  int32_t baseTime;  /* Time between stock updates — was int, fixed to 32-bit */
  bool justStopped;  /* Is this the first time the tank has stopped on the base */
} base;


typedef struct basesObj *bases;

/* Per-(newOwner, prevOwner) debounce slot for client-side STOLE_BASE
 * newswire messages. Suppresses oscillation spam (e.g. two bots flipping
 * a base back and forth) while preserving leading and trailing edges.
 * Lives past SIZEOF_BASES so it's never serialized. */
#define BASE_STEAL_TABLE_SIZE 16

typedef struct {
  BYTE active;
  BYTE newOwner;
  BYTE prevOwner;
  BYTE pendingActive;
  uint16_t lastEmitTicks;
  uint16_t pendingAge;
} baseStealDebounceSlot;

struct basesObj {
  base item[MAX_BASES];
  BYTE numBases;
  /* Wire format ends here at SIZEOF_BASES (260 bytes). The 3 trailing
   * pad bytes that round numBases up to 4-byte alignment are made
   * explicit so subsequent fields land safely past the wire region and
   * are not clobbered by basesSetBaseCompressData's memcpy. */
  BYTE _wirePad[3];
  baseStealDebounceSlot stealDebounce[BASE_STEAL_TABLE_SIZE];
  /* Which base numbers name a base that is on the map. 1 is a live base, 0 a
   * removed slot whose index is kept, so the numbers above a removal go on
   * meaning the same base. It sits past the wire region because the blob is
   * the map, and an item removed during a round is the sim's own state. */
  BYTE active[MAX_BASES];
};

#define MAP_ARRAY_SIZE 256 /* maps are 256x256 units square */

#ifndef _MAP_TYPEDEF
#define _MAP_TYPEDEF
typedef struct mapObj *map;
#endif

struct mapObj {
	BYTE mapItem[MAP_ARRAY_SIZE][MAP_ARRAY_SIZE]; /* The actual map */
};



/* Defines */
#define MAX_PILLS 16
#define SIZEOF_PILLS 145 /* 129 */

typedef struct {
  BYTE x;        /* Co-ordinates on the map */
  BYTE y;
  BYTE owner;    /* should be 0xFF except in speciality maps */
  BYTE armour;   /* range 0-15 (dead pillbox = 0, full strength = 15) */
  BYTE speed;    /* typically 50. Time between shots, in 20ms units */
                 /* Lower values makes the pillbox start off 'angry' */
  bool inTank;   /* Is the pillbox in the tank? */
  BYTE reload;   /* Reload timer - When shoots goes to zero then counts back to speed to fire again */
  BYTE coolDown; /* When hit it is set. When the cool down rate = 0 the speed is notched up and cool down rate is set again */
  bool justSeen; /* True if this is the first time we have seen a tank */
} pillbox;

typedef struct pillsObj *pillboxes;

struct pillsObj {
  pillbox item[MAX_PILLS];
  BYTE numPills;
  /* Wire format ends here at SIZEOF_PILLS (145 bytes). Past it: what this
   * client knows about each pill's square. One of the three PILL_SQUARE_
   * values in pillbox.h — the server has just told us the pill is on it, we
   * are only remembering it from the last time we were told, or we watched the
   * pill go into a tank and come out again and the square means nothing. Zero
   * — the value pillsCreate's memset and every map install leave — is the
   * confirmed state, so the server's own list is never affected. */
  BYTE posStale[MAX_PILLS];
  /* Which pill numbers name a pill that is on the map. 1 is a live pill, 0 a
   * removed slot whose index is kept, so the numbers above a removal go on
   * meaning the same pill. It sits past the wire region because the blob is
   * the map, and an item removed during a round is the sim's own state. */
  BYTE active[MAX_PILLS];
};

/* 25B = 25x8 = 200b needed to be allocated */
typedef struct vectorBodyObj *vectorBody;
struct vectorBodyObj {
    BYTE mass;              /* unsigned char, 1B */
	TURNTYPE  angle;        /* float, 4B */
	TURNTYPE  angle_prev;   /* float, 4B */
	SPEEDTYPE speedX;       /* float, 4B */
	SPEEDTYPE speedY;       /* float, 4B */
	SPEEDTYPE speedX_prev;  /* float, 4B */
	SPEEDTYPE speedY_prev;  /* float, 4B */
};


typedef struct tankCarrypbObj *tankCarryPb;
struct tankCarrypbObj {
  tankCarryPb next; /* Next item */
  BYTE pillNum;     /* The pill box number that is being carried */
};



/*
typedef struct {
    BYTE mass;
	TURNTYPE  angle;
	TURNTYPE  angle_prev;
	SPEEDTYPE speedX;
	SPEEDTYPE speedY;
	SPEEDTYPE speedX_prev;
	SPEEDTYPE speedY_prev;
	//TODO: add a factor to mean how 'bouncy' the object is
} vectorBodyObj;
*/

/* Per-tank movement and combat percentages. 0 means classic, so a
 * zeroed struct is the unmodified tank; every other value is a percent
 * of the classic figure. */
typedef struct {
    uint8_t speed, accel, turn, reload, dealt, taken; /* percent; 0 = classic */
} TankModifiers;

typedef struct tankObj *tank;


#pragma pack(push, 1)

/* Boat state tracking */
typedef enum {
  BoatState_NotOnBoat = 0,
  BoatState_InBoat = 1,
  BoatState_LeavingBoat = 2
} BoatState;

struct tankObj {
  WORLD x;            /* World Co-ordinates */
  WORLD y;
  BYTE armour;        /* Amount of armour in tank, 0..tank_full_armour. Never wraps. */
  bool destroyed;     /* TRUE once the tank has been destroyed. Set where damage
                       * exceeds the armour remaining, cleared on respawn. Ask
                       * tankIsDestroyed() rather than comparing armour: a live
                       * tank can legitimately sit at zero armour. */
  BYTE shells;        /* Amount of shells in tank. Maximum value 45 */
  BYTE mines;         /* Amount of mines in tank. Maximum value 45 */
  SPEEDTYPE speed;    /* Tank speed */
  BYTE trees;         /* Amount of trees in tank */
  TURNTYPE angle;     /* The angle the tank is pointing on 0-256 */
  BYTE reload;        /* Reload tick 0 is OK to shoot else counts back */
  bool onBoat;        /* Is the tank on a boat? */
  BoatState boatState;      /* Granular boat state tracking */
  BYTE lastBoatRiverX;     /* Last river X map position while on boat */
  BYTE lastBoatRiverY;     /* Last river Y map position while on boat */
  bool showSight;     /* Is the gunsight on or not */
  BYTE sightLen;      /* Length of the gunsight measured in map units */
  int32_t numKills;   /* Number of kills the tank has had — was int, fixed to 32-bit */
  int32_t numDeaths;  /* Number of deaths the tank has had — was int, fixed to 32-bit */
  uint16_t deathWait; /* How long it is going to be on the screen till it refreshes */
  BYTE waterCount;    /* Count for bubbles */
  bool obstructed;    /* Used by brains. Did the tank hit anything */
  bool newTank;       /* Is this a new tank or not (ie just died */
  bool autoSlowdown;  /* Do we use autoslowdown or not */
  bool autoHideGunsight;  /* Auto show/hide of gunsight enabled/disabled */
  BYTE justFired;         /* Tick countdown — set to just_fired_ticks on shell fire, decremented each tankUpdate. Non-zero means "recently fired" and bypasses tree-hide for pillbox targeting. */
  BYTE tankHitCount;  /* Number of times a tank has been hit to determine if they are cheating */
  WORLD x_prev;       /* World Coordinates at last tick */
  WORLD y_prev;
  WORLD x_prev_prev;
  WORLD y_prev_prev;
  tankCarryPb carryPills; /* The Pillboxes being carried */
  float tankSlideVx;       /* Knockback slide X velocity (WORLD units/tick) */
  float tankSlideVy;       /* Knockback slide Y velocity (WORLD units/tick) */
  BYTE firstLeft;          /* Turn ramp-up counter for left turns (0-10) */
  BYTE firstRight;         /* Turn ramp-up counter for right turns (0-10) */
  BYTE lastTankDeath;      /* How did the most recent death to the tank occur? */
  /* DEATH_CAUSE_* for the death currently being served out (armour has
   * overflowed, deathWait is counting down). Stamped by whichever damage site
   * pushed armour past TANK_FULL_ARMOUR, read and cleared by tankDeath on the
   * server when the death is finally counted. Observation only. */
  BYTE pendingDeathCause;
  /* Countdown of game ticks since a shell last came within
   * TANK_SHELL_NEAR_WU of this tank, or since a shell knocked it off its
   * boat.  Set to TANK_SHELL_NEAR_MEMORY_FRAMES by those events and
   * decremented once per shellsUpdate (server only); non-zero therefore
   * means "a shell was near me within the last second".  Read only by the
   * drowning site, to split DEATH_CAUSE_DROWNED from
   * DEATH_CAUSE_DROWNED_UNFORCED.  Observation only — no movement, damage
   * or network path reads it. */
  BYTE shellNearFrames;
  vectorBody vectorBodyTank; /* Holds tank's actual moving direction and component vectors (x and y axis speed) */
  vectorBody vectorBodyCollide; /* Holds physics stuff for what hit the tank */
  int32_t bumpX;            /* X bump effect from shells (>>9 applied per tick); 32 bits so tank_slide_step * 512 fits */
  int32_t bumpY;            /* Y bump effect from shells (>>9 applied per tick) */
  /* The Mac Bolo shell push (tank_slide_mac): displacement still to come in
   * world units, with fractions, and the per-tick multiplier taken at the hit. */
  double slideX;
  double slideY;
  double slideRetention;
  BYTE residualSpeed;       /* Accumulated sub-tick movement */
  BYTE leavingBoatTimer;    /* Ticks remaining in LeavingBoat before returning to InBoat */
  BYTE leavingBoatAxis;     /* Bank-crossing axis bitmask (1=X, 2=Y); only checked for pastGrace */
  TankModifiers mods;       /* Per-tank percentages; zeroed at create and kept across a respawn */
};

#pragma pack(pop)

/* Defines */
#define MAX_STARTS 16
#define SIZEOF_STARTS 49

/* Uncompressed size of a map serialised by mapSaveCompressedMap: the
 * bases/pills/starts structs followed by the terrain array. */
#define MAP_UNCOMPRESSED_SIZE \
    (SIZEOF_BASES + SIZEOF_PILLS + SIZEOF_STARTS + (MAP_ARRAY_SIZE * MAP_ARRAY_SIZE))

/* Worst-case size of a map serialised by mapSaveCompressedMap, and therefore
 * the size any buffer handed to it should be. This is zlib's compressBound()
 * for MAP_UNCOMPRESSED_SIZE, written out so it can size a static array:
 * deflate stores an input that does not compress in raw blocks, which cost a
 * few bytes of framing on top of the input and never more than this. */
#define MAP_COMPRESSED_MAX_SIZE                                       \
    (MAP_UNCOMPRESSED_SIZE + (MAP_UNCOMPRESSED_SIZE >> 12) +          \
     (MAP_UNCOMPRESSED_SIZE >> 14) + (MAP_UNCOMPRESSED_SIZE >> 25) + 13)

/* Typedefs */

typedef struct {
  BYTE x;   /* Co-ordinates on the map */
  BYTE y;
  BYTE dir;  /* Direction towards land from this start. Range 0-15 */
} start;


typedef struct startsObj *starts;

struct startsObj {
  start item[MAX_STARTS];
  BYTE numStarts;
  /* Wire format ends here at SIZEOF_STARTS (49 bytes). Past it: which start
   * numbers name a start that is on the map. 1 is a live start, 0 a removed
   * slot whose index is kept, so the numbers above a removal go on meaning
   * the same start. It sits past the wire region because the blob is the
   * map, and an item removed during a round is the sim's own state. */
  BYTE active[MAX_STARTS];
};

#pragma pack(pop)

/*
 * Sizes derived from the header constants:
 *   SIZEOF_BASES 260  = 16 * sizeof(base) + 4  → sizeof(base) == 16
 *   SIZEOF_PILLS 145  = 16 * sizeof(pillbox) + 1 → sizeof(pillbox) == 9
 *   SIZEOF_STARTS 49  = 16 * sizeof(start) + 1   → sizeof(start) == 3
 */
BOLO_STATIC_ASSERT(sizeof(base) == 16,   base_must_be_16_bytes);
BOLO_STATIC_ASSERT(sizeof(pillbox) == 9, pillbox_must_be_9_bytes);
/* basesObj's first SIZEOF_BASES bytes are the wire format; debounce
 * fields must sit strictly past that boundary. */
BOLO_STATIC_ASSERT(offsetof(struct basesObj, stealDebounce) == SIZEOF_BASES,
                   bases_steal_debounce_after_wire_format);
/* Same rule for pillsObj: the position-current flags sit strictly past the
 * SIZEOF_PILLS bytes pillsSetPillCompressData copies in. pillbox holds only
 * BYTE and bool, so item[] and numPills pack to exactly 145 with no pad member
 * of the kind basesObj needs. */
BOLO_STATIC_ASSERT(offsetof(struct pillsObj, posStale) == SIZEOF_PILLS,
                   pills_pos_stale_after_wire_format);
BOLO_STATIC_ASSERT(sizeof(start) == 3,   start_must_be_3_bytes);
/* Same rule for startsObj: the live flags sit strictly past the SIZEOF_STARTS
 * bytes startsSetStartCompressData copies in. start holds only BYTE, so
 * item[] and numStarts pack to exactly 49. */
BOLO_STATIC_ASSERT(offsetof(struct startsObj, active) == SIZEOF_STARTS,
                   starts_active_after_wire_format);

#endif
