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
 *Name:          Attribution Track Format
 *Filename:      attribution_track.h
 *Author:        John Morrison
 *Purpose:
 *  On-disk schema for the per-round attribution track stored
 *  as a member inside a logged .wbv archive. Carries the
 *  discrete attribution events (damage, kills, captures, LGM
 *  losses, builder/shell actions) plus per-slot identity, so
 *  a round's scoreboard and awards can be rebuilt offline with
 *  no live server. The server writes this member; the log
 *  viewer reads it.
 *
 *  Packed-binary layout: a fixed header (magic, version,
 *  per-slot identity table, record count) followed by one
 *  heterogeneous append stream of records. Stream order is
 *  authoritative — it preserves within-tick ordering, so there
 *  is no intra-tick sequence field. Each record leads with a
 *  one-byte AttrRecordType tag for reader dispatch.
 *
 *  The on-disk enums are a frozen contract. Their numeric
 *  values intentionally mirror the server's internal
 *  DMG_TARGET_* / PLAYER_ACTION_* constants but are defined
 *  standalone here; this header must not pull in sim internals.
 *  Enum values are append-only: new tags/kinds may be added at
 *  the end, existing values must never be renumbered. Bump
 *  ATTRIBUTION_TRACK_VERSION when the record layout changes.
 *
 *  Public leaf: includes only public/foundational headers
 *  (platform_types.h, wire_limits.h, global.h). No sim
 *  internals.
 *********************************************************/
#ifndef ATTRIBUTION_TRACK_H
#define ATTRIBUTION_TRACK_H

#include <stdint.h>

#include "platform_types.h"  /* BOLO_PACK_ATTR, BOLO_STATIC_ASSERT, fixed-width ints */
#include "wire_limits.h"     /* PACKET_MAX_PLAYER_NAME */
#include "global.h"          /* MAX_TANKS, NEUTRAL */

/* Zip member name for the attribution track inside the .wbv archive. */
#define ATTRIBUTION_TRACK_MEMBER   "attribution.trk"
/* File magic at the front of the member — 4 bytes, no trailing NUL stored. */
#define ATTRIBUTION_TRACK_MAGIC    "WBAT"
/* On-disk format version. Bump on any record-layout change.
 * v2: every event record carries map-cell mapX/mapY, and ATTR_REC_PICKUP
 *     records dead-pillbox scoops. */
#define ATTRIBUTION_TRACK_VERSION  2
/* Per-round size cap the writer enforces; a round that would exceed this is
 * truncated and flagged in the header. */
#define ATTRIBUTION_TRACK_CAP_BYTES (64u * 1024u * 1024u)

/* Record tag byte. Starts at 1 so a zeroed byte is never a valid record.
 * Append-only. */
typedef enum {
    ATTR_REC_DAMAGE  = 1,
    ATTR_REC_KILL    = 2,
    ATTR_REC_CAPTURE = 3,
    ATTR_REC_LGM     = 4,
    ATTR_REC_ACTION  = 5,
    ATTR_REC_PICKUP  = 6
} AttrRecordType;

/* What inflicted a damage hit. The writer supplies this at the call site;
 * a hit whose origin is unknown stays ATTR_SRC_UNKNOWN. Append-only. */
typedef enum {
    ATTR_SRC_UNKNOWN = 0,
    ATTR_SRC_SHELL   = 1,
    ATTR_SRC_MINE    = 2
} AttrDamageSource;

/* What a damage hit landed on. Mirrors internal DMG_TARGET_*. Append-only. */
typedef enum {
    ATTR_TGT_TANK = 0,
    ATTR_TGT_PILL = 1,
    ATTR_TGT_BASE = 2
} AttrDamageTarget;

/* Builder / shells-fired action kind. Mirrors internal PLAYER_ACTION_*.
 * Append-only. */
typedef enum {
    ATTR_ACT_FARM  = 0,
    ATTR_ACT_BUILD = 1,
    ATTR_ACT_MINE  = 2,
    ATTR_ACT_SHELL = 3
} AttrActionKind;

/* What kind of object a capture took ownership of. Append-only. */
typedef enum {
    ATTR_CAP_TGT_PILL = 0,
    ATTR_CAP_TGT_BASE = 1
} AttrCaptureTarget;

/* Prior-ownership classification of a capture. Append-only. */
typedef enum {
    ATTR_CAP_NEUTRAL = 0,
    ATTR_CAP_ENEMY   = 1,
    ATTR_CAP_ALLY    = 2
} AttrCaptureClass;

#pragma pack(push, 1)

/* Per-hit damage. `source` is populated by the writer at the call site;
 * recordDamage itself does not carry it, so it may be ATTR_SRC_UNKNOWN. Splash/
 * chain damage has no owner (attacker == NEUTRAL). `destroyed` = the hit dropped
 * a pill to 0 armour (credits pillKills in the live handler). */
typedef struct BOLO_PACK_ATTR {
    uint8_t  type;          /* ATTR_REC_DAMAGE */
    uint32_t tick;
    uint8_t  source;        /* AttrDamageSource */
    uint8_t  target;        /* AttrDamageTarget */
    uint8_t  targetIndex;   /* tank slot, or pill/base index */
    uint8_t  attacker;      /* owner slot; NEUTRAL for splash/owner-less */
    uint16_t amount;        /* effective armour removed this hit */
    uint8_t  destroyed;     /* 1 if this hit destroyed the target (pill->0) */
    uint8_t  mapX;          /* map cell of the event; 0 if unknown (v2+) */
    uint8_t  mapY;
} AttrDamageRecord;

/* Tank death, from the serverSimAddEvent EVENT_TANK_KILLED tap. deathCause,
 * carriedPills (data[3]) and treesWasted (data[4]) are the server-internal
 * GameEvent bytes that never hit the wire. */
typedef struct BOLO_PACK_ATTR {
    uint8_t  type;          /* ATTR_REC_KILL */
    uint32_t tick;
    uint8_t  killer;        /* slot, or NEUTRAL for environmental */
    uint8_t  killed;
    uint8_t  deathCause;
    uint8_t  carriedPills;
    uint8_t  treesWasted;
    uint8_t  mapX;          /* map cell of the death (v2+) */
    uint8_t  mapY;
} AttrKillRecord;

/* Pill/base ownership gain, with the neutral/enemy/ally classification byte. */
typedef struct BOLO_PACK_ATTR {
    uint8_t  type;          /* ATTR_REC_CAPTURE */
    uint32_t tick;
    uint8_t  target;        /* AttrCaptureTarget */
    uint8_t  targetIndex;
    uint8_t  newOwner;
    uint8_t  prevOwner;
    uint8_t  captureClass;  /* AttrCaptureClass */
    uint8_t  mapX;          /* map cell of the pill/base (v2+) */
    uint8_t  mapY;
} AttrCaptureRecord;

/* LGM lost, from EVENT_LGM_LOST [victim, killer]. */
typedef struct BOLO_PACK_ATTR {
    uint8_t  type;          /* ATTR_REC_LGM */
    uint32_t tick;
    uint8_t  victim;
    uint8_t  killer;
    uint8_t  mapX;          /* map cell of the LGM loss (v2+) */
    uint8_t  mapY;
} AttrLgmRecord;

/* Builder / shells-fired action, from recordPlayerAction. */
typedef struct BOLO_PACK_ATTR {
    uint8_t  type;          /* ATTR_REC_ACTION */
    uint32_t tick;
    uint8_t  player;
    uint8_t  action;        /* AttrActionKind */
    uint8_t  mapX;          /* map cell of the action (v2+) */
    uint8_t  mapY;
} AttrActionRecord;

/* Dead (0-armour) pillbox scooped into a tank's inventory. Backs the
 * dead-pill pickup-spree highlight signal. (v2+) */
typedef struct BOLO_PACK_ATTR {
    uint8_t  type;          /* ATTR_REC_PICKUP */
    uint32_t tick;
    uint8_t  picker;        /* tank slot that grabbed it */
    uint8_t  pillIndex;
    uint8_t  mapX;
    uint8_t  mapY;
} AttrPickupRecord;

/* Per-slot identity, captured once in the header so offline rebuild can name
 * and team-group each slot without a live roster. */
typedef struct BOLO_PACK_ATTR {
    uint8_t  isBot;
    uint8_t  team;
    char     name[PACKET_MAX_PLAYER_NAME];
} AttrSlotIdentity;

/* Fixed header written once at the front of the member, ahead of the record
 * stream. */
typedef struct BOLO_PACK_ATTR {
    char     magic[4];                    /* ATTRIBUTION_TRACK_MAGIC, no NUL */
    uint8_t  version;                     /* ATTRIBUTION_TRACK_VERSION */
    uint8_t  truncated;                   /* 1 if the cap was hit */
    uint16_t slotCount;                   /* identity entries (== MAX_TANKS) */
    AttrSlotIdentity slots[MAX_TANKS];    /* per-slot identity for offline rebuild */
    uint32_t recordCount;                 /* records following the header */
} AttrTrackHeader;

#pragma pack(pop)

BOLO_STATIC_ASSERT(sizeof(AttrDamageRecord)  == 14, attr_damage_size);
BOLO_STATIC_ASSERT(sizeof(AttrKillRecord)    == 12, attr_kill_size);
BOLO_STATIC_ASSERT(sizeof(AttrCaptureRecord) == 12, attr_capture_size);
BOLO_STATIC_ASSERT(sizeof(AttrLgmRecord)     ==  9, attr_lgm_size);
BOLO_STATIC_ASSERT(sizeof(AttrActionRecord)  ==  9, attr_action_size);
BOLO_STATIC_ASSERT(sizeof(AttrPickupRecord)  ==  9, attr_pickup_size);
BOLO_STATIC_ASSERT(sizeof(AttrSlotIdentity)  == 2 + PACKET_MAX_PLAYER_NAME, attr_slot_size);
BOLO_STATIC_ASSERT(sizeof(AttrTrackHeader)   == 12 + MAX_TANKS * sizeof(AttrSlotIdentity), attr_header_size);

#endif /* ATTRIBUTION_TRACK_H */
