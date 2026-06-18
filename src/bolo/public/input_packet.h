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
 *Name:          Input Packet
 *Filename:      input_packet.h
 *Author:        John Morrison
 *Purpose:
 *  Defines the InputPacket struct sent from client to
 *  server each tick, and the bitmask constants for
 *  buttons and actions.
 *********************************************************/

#ifndef INPUT_PACKET_H
#define INPUT_PACKET_H

#include <stdint.h>

/* Button bitmask bits (InputPacket.buttons) */
#define INPUT_BTN_ACCEL  0x01
#define INPUT_BTN_DECEL  0x02
#define INPUT_BTN_LEFT   0x04
#define INPUT_BTN_RIGHT  0x08

/* Action bitmask bits (InputPacket.actions) */
#define INPUT_ACTION_FIRE      0x01
#define INPUT_ACTION_LAY_MINE  0x02

/* Flags field bitmask (InputPacket.flags) */
#define INPUT_FLAG_AUTOSLOW       0x01  /* Bit 0: autoslowdown enabled */
#define INPUT_FLAG_GUNSIGHT_MASK  0x06  /* Bits 1-2: gunsight adjustment */
#define INPUT_FLAG_GUNSIGHT_SHIFT 1     /* 0=none, 1=increase, 2=decrease */

typedef struct {
    uint32_t tick;          /* Client tick number (sequence) */
    uint8_t  playerNum;     /* Which player */
    uint8_t  buttons;       /* Bitmask: accel, decel, left, right */
    uint8_t  actions;       /* Bitmask: fire, layMine */
    uint8_t  buildAction;   /* LGM build type (0 = none) */
    uint8_t  buildX;        /* LGM target X (if buildAction != 0) */
    uint8_t  buildY;        /* LGM target Y (if buildAction != 0) */
    uint8_t  flags;         /* Bit 0: autoslow, bits 2-3: gunsight adj */
    uint32_t eventAck;      /* Reliable event ACK: next expected seq (0 = none) */
    uint32_t mapEventAck;   /* Map event ACK: next expected map event seq (0 = none) */
    /* Control acks ride the CHANNEL_CONTROL channel-frame trailer, not an
     * InputPacket field. */
    uint16_t pingMs;        /* Client's self-measured RTT in ms */
    uint32_t viewTick;      /* serverTick of the snapshot being displayed when this input was sampled; 0 = unknown (server falls back to the ping estimate). */
} InputPacket;

/*********************************************************
 * Wire-format snapshot types (server → client)
 * Used by both transport_udp and screen sync.
 *********************************************************/

/* Snapshot header — precedes tank data in PACKET_STATE_SNAPSHOT */
typedef struct {
    uint32_t serverTick;
    uint32_t lastProcessedInput;  /* Last input tick server processed for THIS client */
    uint8_t  tankCount;           /* Number of TankSnapshot entries following */
    uint8_t  shellCount;          /* Number of ShellSnapshot entries following */
    uint8_t  tkExplosionCount;   /* Number of TkExplosionSnapshot entries following */
    uint8_t  baseCount;           /* Number of BaseSnapshot entries following */
    uint8_t  pillCount;           /* Number of PillSnapshot entries following */
    uint8_t  reliableEventCount;  /* Count of reliable GameEvent entries in this
                                   * snapshot: channel-merged into snapshotEvents
                                   * on UDP, per-tick on the local transport.
                                   * Not a wire field — UDP game events ride
                                   * CHANNEL_GAME. */
    /* Map events ride reliable channel 1 (CHANNEL_MAP) and control events ride
     * reliable channel 2 (CHANNEL_CONTROL); neither carries a snapshot-header
     * slot. Map events merge into the same snapshotEvents array on the client;
     * control events dispatch directly to clientSimApplyControlOrdered. */
    uint16_t mapChecksum;         /* CRC-16 of map terrain (non-zero on full sync ticks) */
    /* Forced return-to-lobby countdown. > 0 means the server is going
     * to transition to gameOver in this many serverSimTick calls.
     * 0 = no pending forced transition. Clients render their own
     * "Returning to lobby in N" off this value. */
    uint16_t returnToLobbyTicks;
} SnapshotHeader;

/* High bit of TankSnapshot.playerNum: when set, this entry is a "hidden stub"
 * — only the playerNum byte is on the wire, no tank payload follows.  The
 * server emits one entry per connected player every tick; players whose tank
 * (and LGM) are outside this client's viewport ship as stubs so the client
 * can clear stale ghost tanks instead of leaving the previous in-view position
 * lingering in the players struct.  The low 7 bits hold the actual slot index
 * (MAX_TANKS=16, so 4 bits is enough). */
#define TANK_SNAPSHOT_HIDDEN_FLAG 0x80
#define TANK_SNAPSHOT_PLAYER_MASK 0x7F

/* Field-presence bitmask for a non-stub TankSnapshot entry (the byte that
 * follows playerNum).  An always-present 11-byte core — playerNum, this mask,
 * worldX, worldY, angle, speed, tankStatus — is followed by only the groups
 * whose bit is set.  A group's bit is set iff any field in it is non-zero, so
 * the encoder omits the bytes that are zero anyway (for a non-owner tank the
 * owner-only resources, reload, etc. are always zero) and the decoder restores
 * absent fields to 0 — byte-for-byte equivalent to sending them all.  The
 * packer and unpacker must reference these same symbols. */
#define TANK_PRESENT_OWNER_RES 0x01  /* armour, shells, mines, trees, gunsightLen (5B) */
#define TANK_PRESENT_RELOAD    0x02  /* reload (1B) */
#define TANK_PRESENT_DEATHWAIT 0x04  /* deathWait (1B) */
#define TANK_PRESENT_LGM       0x08  /* lgmFrame, lgmMX, lgmMY, lgmPX, lgmPY (5B) */
#define TANK_PRESENT_TURNRAMP  0x10  /* firstLeft, firstRight (2B) */
#define TANK_PRESENT_PING      0x20  /* pingMs (2B) */
#define TANK_PRESENT_FLAGS     0x40  /* clientFlags (1B) */
/* 0x80 reserved (always 0 for now). */

/* Per-tank data within a snapshot (wire format).  Variable-length: a stub
 * (playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) is 1 byte on the wire; a full entry
 * is an 11-byte core plus a presence mask selecting which field groups follow,
 * at most TANK_SNAPSHOT_WIRE_SIZE bytes. */
typedef struct {
    uint8_t  playerNum;
    uint16_t worldX;
    uint16_t worldY;
    uint16_t angle;        /* TURNTYPE scaled: actual_angle * 256 */
    uint16_t speed;        /* SPEEDTYPE scaled: actual_speed * 256 */
    uint8_t  tankStatus;   /* Low nibble: onBoat (0/1), high nibble: isDead (0/1) */
    uint8_t  lgmFrame;
    uint8_t  lgmMX;
    uint8_t  lgmMY;
    uint8_t  lgmPX;
    uint8_t  lgmPY;
    uint8_t  armour;
    uint8_t  shells;       /* Only meaningful for the owning player */
    uint8_t  mines;        /* Only meaningful for the owning player */
    uint8_t  trees;        /* Only meaningful for the owning player */
    uint8_t  firstLeft;    /* Turn ramp-up counter for left turns (0-10) */
    uint8_t  firstRight;   /* Turn ramp-up counter for right turns (0-10) */
    uint8_t  gunsightLen;  /* Gunsight range (GUNSIGHT_MIN..GUNSIGHT_MAX), owning player only */
    uint8_t  deathWait;    /* Ticks remaining until respawn (0 = alive) */
    uint8_t  reload;       /* Ticks remaining until can fire again (owning player only) */
    uint16_t pingMs;       /* This player's ping in ms */
    uint8_t  clientFlags;  /* PLAYER_FLAG_* bits — see players.h */
} TankSnapshot;

/* Per-shell data within a snapshot (wire format) */
typedef struct {
    uint16_t worldX;
    uint16_t worldY;
    uint8_t  angle;
    uint8_t  owner;
    uint8_t  length;       /* Remaining life */
} ShellSnapshot;

#define SHELL_SNAPSHOT_WIRE_SIZE 7

/* Per-tk-explosion data within a snapshot (wire format) */
typedef struct {
    uint16_t worldX;       /* World X position */
    uint16_t worldY;       /* World Y position */
    uint8_t  angle;        /* Travel angle (quantized from TURNTYPE) */
    uint8_t  length;       /* Remaining distance */
    uint8_t  explodeType;  /* TK_SMALL_EXPLOSION or TK_LARGE_EXPLOSION */
    uint8_t  creator;      /* Player number who died */
} TkExplosionSnapshot;

#define TK_EXPLOSION_SNAPSHOT_WIRE_SIZE 8

/* Maximum shells/explosions in a single snapshot */
#define MAX_SNAPSHOT_SHELLS     64
#define MAX_SNAPSHOT_TK_EXPLOSIONS 16
#define MAX_SNAPSHOT_BASES      16
#define MAX_SNAPSHOT_PILLS      16

/* Per-base data within a snapshot (wire format) */
typedef struct {
    uint8_t owner;
    uint8_t armour;
    uint8_t shells;
    uint8_t mines;
} BaseSnapshot;

#define BASE_SNAPSHOT_WIRE_SIZE 4

/* Per-pill data within a snapshot (wire format) */
typedef struct {
    uint8_t x, y;
    uint8_t owner;
    uint8_t armour;
    uint8_t speed;
    uint8_t inTank;
} PillSnapshot;

#define PILL_SNAPSHOT_WIRE_SIZE 6

/* Game event (map change, sound, etc.)
 * data[] is 8 bytes — enough for all current event types.
 * If a future event type needs more, increase this and
 * update gameEventDataSize() below. */
typedef struct {
    uint8_t type;
    uint8_t data[8];
} GameEvent;

#define GAME_EVENT_MAX_DATA    8
#define GAME_EVENT_MAX_WIRE_SIZE (1 + GAME_EVENT_MAX_DATA)  /* For buffer sizing */
#define MAX_SNAPSHOT_EVENTS  128

/* Event types */
#define EVENT_SHELL_FIRED   1
#define EVENT_MINE_PLACED   2
#define EVENT_EXPLOSION     3  /* data: [mx, my, px, py] */
#define EVENT_PILL_CAPTURED 4  /* data: [newOwner, prevOwner] */
#define EVENT_BASE_CAPTURED 5  /* data: [newOwner, prevOwner] */
#define EVENT_TANK_KILLED   6  /* data: [killer, killed, deathCause, carriedPills] */
#define EVENT_MAP_CHANGE    7  /* data: [mx, my, newTerrain] */
#define EVENT_SOUND         8  /* data: [soundId, mx, my, sourcePlayer] */
#define EVENT_SERVER_MSG    9  /* data: [msgId] — server status message */
#define EVENT_PILL_UPDATE  10  /* data: [pillIndex, x, y, owner, armour, speed, inTank] */
#define EVENT_BASE_UPDATE  11  /* data: [baseIndex, owner, armour, shells, mines] */
#define EVENT_PLAYER_LEAVE 12  /* data: [playerNum] */
#define EVENT_ASSISTANT_MSG 13 /* data: [targetPlayer, msgId] — player-specific assistant message */
#define EVENT_LGM_LOST     14 /* data: [victim, killer] — builder killed, broadcast newswire */
#define EVENT_SOUND_TANK_HIT 15 /* data: [soundId, mx, my, hitPlayer] */
#define EVENT_SOUND_SHOOT    16 /* data: [soundId, mx, my, firingPlayer] */
#define EVENT_MINE_VISIBLE   17 /* data: [mx, my, sourcePlayer] — bit 7 of sourcePlayer = broadcast to all */
#define EVENT_TK_EXPLOSION   18 /* data: [xHi, xLo, yHi, yLo, angle, length, explodeType, creator] — tank fireball spawn */

/* Assistant message IDs for EVENT_ASSISTANT_MSG */
#define ASSIST_MSG_MAN_DEAD          1
#define ASSIST_MSG_NO_TREE           2
#define ASSIST_MSG_NO_BUILD          3
#define ASSIST_MSG_NO_BUILD_BOAT     4
#define ASSIST_MSG_INSUFFICIENT_TREES 5
#define ASSIST_MSG_BUILDTANK         6
#define ASSIST_MSG_PILL_NO_REPAIR    7
#define ASSIST_MSG_NO_PILLS          8
#define ASSIST_MSG_INSUFFICIENT_MINES 9
#define ASSIST_MSG_PILL_ON_MINE      10
#define ASSIST_MSG_TANK_SUNK         11

/* Returns the number of data bytes for a given event type on the wire.
 * Unknown types return GAME_EVENT_MAX_DATA as a safe fallback. */
static inline int gameEventDataSize(uint8_t type) {
    switch (type) {
    case EVENT_PILL_CAPTURED:  return 2;
    case EVENT_BASE_CAPTURED:  return 2;
    case EVENT_EXPLOSION:      return 4;
    case EVENT_MAP_CHANGE:     return 3;
    case EVENT_SOUND:          return 4;
    case EVENT_SERVER_MSG:     return 1;
    case EVENT_PILL_UPDATE:    return 7;
    case EVENT_BASE_UPDATE:    return 5;
    case EVENT_PLAYER_LEAVE:   return 1;
    case EVENT_ASSISTANT_MSG:  return 2;
    case EVENT_LGM_LOST:       return 2;
    case EVENT_SOUND_TANK_HIT: return 4;
    case EVENT_SOUND_SHOOT:    return 4;
    case EVENT_TANK_KILLED:    return 4;
    case EVENT_MINE_VISIBLE:   return 3;
    case EVENT_TK_EXPLOSION:   return 8;
    default:                   return GAME_EVENT_MAX_DATA;
    }
}

/* EVENT_SERVER_MSG message IDs */
#define SERVER_MSG_GAME_LOCKED   1
#define SERVER_MSG_GAME_UNLOCKED 2

#endif /* INPUT_PACKET_H */
