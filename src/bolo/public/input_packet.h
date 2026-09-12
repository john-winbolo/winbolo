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

#include <stdbool.h>
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

/* Field-presence bitmask for a non-stub TankSnapshot entry (the two bytes that
 * follow playerNum, big-endian).  An always-present 12-byte core — playerNum,
 * this mask, worldX, worldY, angle, speed, tankStatus — is followed by only the
 * groups whose bit is set.  A group's bit is set iff any field in it is
 * non-zero, so the encoder omits the bytes that are zero anyway (for a
 * non-owner tank the owner-only resources, reload, etc. are always zero) and
 * the decoder restores absent fields to 0 — byte-for-byte equivalent to sending
 * them all.  The packer and unpacker must reference these same symbols. */
#define TANK_PRESENT_OWNER_RES 0x01  /* armour, shells, mines, trees, gunsightLen (5B) */
#define TANK_PRESENT_RELOAD    0x02  /* reload (1B) */
#define TANK_PRESENT_DEATHWAIT 0x04  /* deathWait (1B) */
#define TANK_PRESENT_LGM       0x08  /* lgmFrame, lgmMX, lgmMY, lgmPX, lgmPY (5B) */
#define TANK_PRESENT_TURNRAMP  0x10  /* firstLeft, firstRight (2B) */
#define TANK_PRESENT_PING      0x20  /* pingMs (2B) */
#define TANK_PRESENT_FLAGS     0x40  /* clientFlags (1B) */
#define TANK_PRESENT_HIDDEN    0x80  /* hiddenFlags (1B) */
#define TANK_PRESENT_MODS    0x0100  /* modSpeed, modAccel, modTurn, modReload, modDealt, modTaken (6B) */

/* TankSnapshot.tankStatus bits. The low nibble carries the boat, the high
 * nibble the two death signals, which are not the same question:
 *   TANK_STATUS_DEAD       deathWait > 0 — the tank is in its respawn wait.
 *   TANK_STATUS_DESTROYED  the tank has been destroyed and not yet respawned.
 * The wait ends on its own; the respawn does not happen until a start position
 * is found, so a tank can be DESTROYED with DEAD clear. DESTROYED is what
 * tankIsDestroyed() answers on the other end; DEAD is what the interpolation
 * and the brain's "dead" read. Both bits go to every recipient, unlike armour,
 * which only the owning player sees — so this byte, not armour, is how a
 * client learns that any tank is destroyed. armour itself is a plain
 * 0..TANK_FULL_ARMOUR value and never carries a death sentinel. */
#define TANK_STATUS_ON_BOAT    0x01
#define TANK_STATUS_DEAD       0x10
#define TANK_STATUS_DESTROYED  0x20

/* TankSnapshot.hiddenFlags bits.  An entry whose tank is withheld but whose LGM
 * must still be drawn is not a stub — it is a full entry with the tank's own
 * fields zeroed and this flag set, so the LGM group can ride along.  The tank's
 * real position is never on the wire in that case: a client that ignores the
 * flag reads (0,0), not the hiding place. */
#define TANK_HIDDEN_POSITION   0x01  /* worldX/worldY/angle/speed are withheld */

/* Per-tank data within a snapshot (wire format).  Variable-length: a stub
 * (playerNum & TANK_SNAPSHOT_HIDDEN_FLAG) is 1 byte on the wire; a full entry
 * is a 12-byte core including the presence mask that selects which field
 * groups follow, at most TANK_SNAPSHOT_WIRE_SIZE bytes. */
typedef struct {
    uint8_t  playerNum;
    uint16_t worldX;
    uint16_t worldY;
    uint16_t angle;        /* TURNTYPE scaled: actual_angle * 256 */
    uint16_t speed;        /* SPEEDTYPE scaled: actual_speed * 256 */
    uint8_t  tankStatus;   /* TANK_STATUS_* bits: onBoat, dead (in respawn wait), destroyed */
    uint8_t  lgmFrame;
    uint8_t  lgmMX;
    uint8_t  lgmMY;
    uint8_t  lgmPX;
    uint8_t  lgmPY;
    uint8_t  armour;       /* 0..TANK_FULL_ARMOUR, owning player only; death is in tankStatus */
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
    uint8_t  hiddenFlags;  /* TANK_HIDDEN_* bits — which fields are withheld */
    uint8_t  modSpeed;     /* Per-tank percentages, 0 = classic; owning player only. */
    uint8_t  modAccel;
    uint8_t  modTurn;
    uint8_t  modReload;
    uint8_t  modDealt;
    uint8_t  modTaken;
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
    uint8_t armourInTank;
} PillSnapshot;

#define PILL_SNAPSHOT_WIRE_SIZE 4

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
#define EVENT_MINE_PLACED   2  /* data: [player, mx, my] — local-only, see gameEventIsLocal */
#define EVENT_EXPLOSION     3  /* data: [mx, my, px, py] */
#define EVENT_PILL_CAPTURED 4  /* data: [newOwner, prevOwner, index, quiet] */
#define EVENT_BASE_CAPTURED 5  /* data: [newOwner, prevOwner, index, quiet] */
#define EVENT_TANK_KILLED   6  /* data: [killer, killed, deathCause, carriedPills] */

/* The `quiet` byte, on the events whose payload names one (both captures and
 * EVENT_LGM_LOST), is the announce policy's answer, stamped by the server
 * where it built the fact: 0 to announce, 1 to hold the line back. The
 * client's line-emitting site reads it before messageAdd and writes nothing
 * for a 1; every other effect of the event — the scoreboard, the Steam stat,
 * the map state — runs either way. With no scenario policy registered it is
 * always 0, which is the classic newswire. */

/* Both capture events carry four bytes on the wire and three more the server
 * keeps to itself:
 *
 *   [0] newOwner   the slot that now holds it, or NEUTRAL for a neutralisation
 *   [1] prevOwner  the slot that held it, or NEUTRAL
 *   [2] index      the 0-based pill/base item[] slot, as every op uses
 *   [3] quiet      the announce policy's answer
 *   [4] captureClass    CAPTURE_CLASS_*, server-internal
 *   [5] mapX            the objective's square, server-internal
 *   [6] mapY
 *
 * data[4] and beyond are past gameEventDataSize() for these events, so they
 * are never serialized — the server stats funnel is what reads them. */
#define CAPTURE_CLASS_NEUTRAL 0  /* from neutral — a capture */
#define CAPTURE_CLASS_ENEMY   1  /* from an enemy — a steal (also a capture) */
#define CAPTURE_CLASS_ALLY    2  /* from an ally — tracked for nobody */

#define EVENT_MAP_CHANGE    7  /* data: [mx, my, newTerrain] */
#define EVENT_SOUND         8  /* data: [soundId, tier or mx, direction or my, sourcePlayer] — see Sound event payloads below */
#define EVENT_SERVER_MSG    9  /* data: [msgId] — server status message */
#define EVENT_PILL_UPDATE  10  /* data: [pillIndex, x, y, owner, armourInTank] */
#define EVENT_BASE_UPDATE  11  /* data: [baseIndex, owner] — owner change (reliable) */
#define EVENT_PLAYER_LEAVE 12  /* data: [playerNum] */
#define EVENT_ASSISTANT_MSG 13 /* data: [targetPlayer, msgId] — player-specific assistant message */
#define EVENT_LGM_LOST     14 /* data: [victim, killer, quiet] — builder killed, broadcast newswire.
                               * The man's map cell follows at data[3]/data[4], past the wire size
                               * and read by the server's stats funnel alone. */
#define EVENT_SOUND_TANK_HIT 15 /* data: [soundId, tier or mx, direction or my, hitPlayer] — see Sound event payloads below */
#define EVENT_SOUND_SHOOT    16 /* data: [soundId, tier or mx, direction or my, firingPlayer] — see Sound event payloads below */
#define EVENT_MINE_VISIBLE   17 /* data: [mx, my, sourcePlayer] — bit 7 of sourcePlayer = broadcast to all */
#define EVENT_TK_EXPLOSION   18 /* data: [xHi, xLo, yHi, yLo, angle, length, explodeType, creator] — tank fireball spawn */
#define EVENT_BASE_STOCK   19  /* data: [baseIndex, armour, shells, mines] — best-effort, culled to recipient's closest base */
#define EVENT_PING         20  /* data: [senderPlayer, kind, xHi, xLo, yHi, yLo] — map ping, world coords, team-only */
#define EVENT_TANK_SPAWNED   21 /* data: [player, mx, my, respawn] — respawn 0 for a first spawn */
#define EVENT_LGM_LANDED     22 /* data: [player, mx, my] — builder finished his flight back */
#define EVENT_PILL_PLACED    23 /* data: [player, index, mx, my] — a carried pill put down */
#define EVENT_PILL_PICKED_UP 24 /* data: [player, index] — a dead pill scooped into a tank */
#define EVENT_PILL_KILLED    25 /* data: [index, attacker] — attacker NEUTRAL when nobody is named */
#define EVENT_BUILT          26 /* data: [player, action, mx, my] — see BUILT action below */
#define EVENT_MINE_EXPLODED  27 /* data: [mx, my, layer] — layer NEUTRAL when the mine had no owner */

/* EVENT_BUILT's action byte is the builder's own request code, which is the
 * same number BuilderJob uses in server_sim.h — the two are already pinned
 * together by a static assert in server_sim_accessors.c, so this event needs
 * no third spelling of the list. Placing a pill is EVENT_PILL_PLACED rather
 * than a build, so action 3 here always means a repair. Laying a mine is
 * EVENT_MINE_PLACED, so action 4 never appears on this event. */

/* Pill and base indices on every event above are 0-based item[] slots, the
 * base every scenario op and policy uses. pillsGetPillNum and basesGetBaseNum
 * return a number counted from one; those are not what goes on an event. */

/* PING_KIND_* — which smart ping was sent. On the wire (EVENT_PING data[1],
 * CmdPing.kind and the replay's log_Ping), so the values are fixed. The
 * standard ping is the pie menu's centre; the other five are its sectors,
 * clockwise from the top. Colours and icons for each live in one table in
 * src/gui/ping_kinds.h — add a kind here and there together. */
#define PING_KIND_STANDARD    0
#define PING_KIND_CAUTION     1
#define PING_KIND_ASSIST      2
#define PING_KIND_ATTACK      3
#define PING_KIND_ON_MY_WAY   4
#define PING_KIND_BOT_COMMAND 5
#define PING_KIND_COUNT       6

/* How long a received ping is shown, and how much of the tail of that is
 * spent fading out. Here rather than with the drawing code because the
 * client's ping ring expires entries on the same clock the drawers fade
 * them on, and the replay viewer has to match both. */
#define PING_DISPLAY_MS  5000
#define PING_FADE_MS     1000

/* Smart-ping anti-spam caps, per sender, shared by the two ends that enforce
 * them so they cannot drift apart: the server relay limiter (the CMD_PING arm
 * in server_command_dispatch.c, measured in sim ticks) and the client render
 * backstop (the EVENT_PING arm in client_snapshot.c, measured in wall-clock
 * ms). At most PING_SPAM_MAX_5S pings in any PING_SPAM_WINDOW_5S_SECONDS, AND
 * at most PING_SPAM_MAX_30S in any PING_SPAM_WINDOW_30S_SECONDS; over EITHER
 * window the ping is dropped (server: not relayed; client: not drawn/sounded).
 * The two windows are nested, so the ring that tracks them is sized to the
 * larger cap. */
#define PING_SPAM_MAX_5S              3
#define PING_SPAM_WINDOW_5S_SECONDS   5
#define PING_SPAM_MAX_30S            10
#define PING_SPAM_WINDOW_30S_SECONDS 30

/* The client render backstop measures its windows in wall-clock ms while the
 * server measures the authoritative limit in sim ticks. Network and frame
 * jitter mean a ping the server accepted right at a window edge can arrive a
 * little early on the client, so if the client used the exact same window it
 * could drop a server-APPROVED ping (no marker, sound or line — invisible to
 * the sender). The backstop only exists to catch a peer or server flooding,
 * so it must never be the stricter end: the client shortens each counting
 * window by this slack, which lets an edge ping through and keeps the real
 * flood cap intact. */
#define PING_SPAM_CLIENT_SLACK_MS  1000

/* Sound event payloads. EVENT_SOUND, EVENT_SOUND_TANK_HIT and
 * EVENT_SOUND_SHOOT each carry four bytes, and the middle two carry one of two
 * shapes:
 *
 *   to a human: [soundId, tier, direction, sourcePlayer]
 *   to a bot:   [soundId, mx, my, sourcePlayer]
 *
 * Nothing on the wire says which. A recipient tells them apart by knowing its
 * own client type: the server works the tier and direction out against the
 * human recipient's own tank and sends those instead of the square, while a bot
 * keeps the square its observation builder reads. "Bot" here means a
 * bot-manager bot or a local slot the host marked with
 * serverSimSetSoundSquares (the gym agent, the headless brain harness). A
 * wire client is always sent the tier and direction.
 *
 * The direction is map-absolute — north is decreasing map Y — and is not
 * relative to where the listener is facing. The main view is north-up, so the
 * map axis and the screen axis are the same one, and a stereo panner can read
 * the direction as its pan axis. */
#define SOUND_TIER_NEAR 0
#define SOUND_TIER_FAR  1

#define SOUND_DIR_CENTRE 0
#define SOUND_DIR_N      1
#define SOUND_DIR_NE     2
#define SOUND_DIR_E      3
#define SOUND_DIR_SE     4
#define SOUND_DIR_S      5
#define SOUND_DIR_SW     6
#define SOUND_DIR_W      7
#define SOUND_DIR_NW     8

/* True if this game event must arrive (rides the reliable game channel);
 * false if it is ephemeral and rides the best-effort channel. Single source
 * of truth shared by the server send path and the client ingest path. */
static inline bool gameEventIsReliable(uint8_t type) {
    switch (type) {
    case EVENT_TANK_KILLED:   /* event-driven score; not in any snapshot */
    case EVENT_MAP_CHANGE:    /* rides CHANNEL_MAP */
    case EVENT_SERVER_MSG:    /* user-visible status notice */
    case EVENT_ASSISTANT_MSG: /* one-shot per-player text */
    case EVENT_LGM_LOST:      /* one-shot newswire */
    case EVENT_MINE_VISIBLE:  /* gameplay-critical reveal */
    case EVENT_BASE_UPDATE:   /* base owner (colour) change must arrive */
    case EVENT_PING:          /* one-shot player signal; a dropped ping is gone */
    case EVENT_TANK_SPAWNED:  /* one-shot, one per spawn; who is back matters */
    case EVENT_LGM_LANDED:    /* one-shot, one per builder flight */
    case EVENT_PILL_PLACED:   /* one-shot: a new objective on the map */
    case EVENT_PILL_PICKED_UP:/* one-shot: a pill left the map */
    case EVENT_PILL_KILLED:   /* one-shot: a pill stopped firing */
        return true;
    /* EVENT_BUILT is best-effort on purpose. A builder laying road fires one
     * per square for as long as it works, which is the firehose the split
     * above exists to keep off the reliable window, and the square itself
     * already arrives on the reliable EVENT_MAP_CHANGE — a dropped one costs
     * the attribution, not the map. EVENT_MINE_EXPLODED is best-effort for the
     * same reason: a mine field goes up in one burst, and the crater rides
     * EVENT_MAP_CHANGE. EVENT_MINE_PLACED never reaches a client at all
     * (gameEventIsLocal), so its answer here is never asked on the wire. */
    /* EVENT_PILL_UPDATE and EVENT_BASE_STOCK are best-effort: they fire
     * continuously as pill armour/reload and base stock change under combat (a
     * per-shot/per-refuel firehose), so they cannot sit on the reliable window.
     * Best-effort newest-wins suits a state delta; the periodic full snapshot
     * re-sync is the backstop for a dropped update. Base owner changes
     * (EVENT_BASE_UPDATE) are split out above onto the reliable channel. */
    default:
        return false;
    }
}

/* True if this game event is for the host and the recording only and must
 * never be serialized to a client. EVENT_MINE_PLACED names the square a mine
 * went into, which is exactly what hidden mines exist to withhold: sending it
 * would hand every recipient a map of the minefield. The host hears it through
 * the in-process subscriber channel, which is not the wire, and the god-view
 * recording build keeps it; every per-client build and the UDP drain drop it.
 *
 * A local-only event still needs a gameEventDataSize row: the recording packs
 * it, and the brain event table is sized from the same function. */
static inline bool gameEventIsLocal(uint8_t type) {
    return type == EVENT_MINE_PLACED;
}

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
    case EVENT_PILL_CAPTURED:  return 4;
    case EVENT_BASE_CAPTURED:  return 4;
    case EVENT_EXPLOSION:      return 4;
    case EVENT_MAP_CHANGE:     return 3;
    case EVENT_SOUND:          return 4;
    case EVENT_SERVER_MSG:     return 1;
    case EVENT_PILL_UPDATE:    return 5;
    case EVENT_BASE_UPDATE:    return 2;
    case EVENT_BASE_STOCK:     return 4;
    case EVENT_PLAYER_LEAVE:   return 1;
    case EVENT_ASSISTANT_MSG:  return 2;
    case EVENT_LGM_LOST:       return 3;
    case EVENT_SOUND_TANK_HIT: return 4;
    case EVENT_SOUND_SHOOT:    return 4;
    case EVENT_TANK_KILLED:    return 4;
    case EVENT_MINE_VISIBLE:   return 3;
    case EVENT_TK_EXPLOSION:   return 8;
    case EVENT_PING:           return 6;
    case EVENT_MINE_PLACED:    return 3;
    case EVENT_TANK_SPAWNED:   return 4;
    case EVENT_LGM_LANDED:     return 3;
    case EVENT_PILL_PLACED:    return 4;
    case EVENT_PILL_PICKED_UP: return 2;
    case EVENT_PILL_KILLED:    return 2;
    case EVENT_BUILT:          return 4;
    case EVENT_MINE_EXPLODED:  return 2;   /* the layer stays behind the wire */
    default:                   return GAME_EVENT_MAX_DATA;
    }
}

/* EVENT_SERVER_MSG message IDs */
#define SERVER_MSG_GAME_LOCKED   1
#define SERVER_MSG_GAME_UNLOCKED 2

#endif /* INPUT_PACKET_H */
