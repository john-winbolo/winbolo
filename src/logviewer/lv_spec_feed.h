/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Spectator Feed
 *Filename:      lv_spec_feed.h
 *Purpose:
 *  What a spectator host does with the live delayed feed's
 *  seed and records once the spectator_drain.h seam has
 *  handed them over: load the seed into the decoder, and
 *  route each record - a new segment re-seeds, a keyframe
 *  refreshes the lobby roster and the phase, every record
 *  moves the live head. One body for both hosts: the modal
 *  viewer's spectatorRun (logviewer.c) and the embedded
 *  spectator on clients that only build the reel host
 *  (lv_embed.c).
 *
 *  The host still owns the loop, the transport pump, the
 *  stall watchdog, the window title and live-DVR mode.
 *********************************************************/

#ifndef LV_SPEC_FEED_H
#define LV_SPEC_FEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spectator_drain.h"   /* SpecDrainRecord */

/* What lvSpecFeedRecord did with a record, for the host to act on. */
typedef enum {
    LV_SPEC_FEED_PUMPED = 0,     /* appended to the live buffer */
    LV_SPEC_FEED_PHASE_CHANGED,  /* a keyframe moved the game phase */
    LV_SPEC_FEED_RESEEDED,       /* a new segment (lobby or map) re-seeded */
    LV_SPEC_FEED_LOST            /* the re-seed failed; isLoaded is now FALSE */
} LvSpecFeedResult;

/*********************************************************
*NAME:          lvSpecFeedSeed
*PURPOSE:
*  Loads the feed's opening seed into the decoder: the map
*  and game settings and the phase from its control slice,
*  then the world, then the lobby roster's names. Sets
*  isLoaded on success. The seed stays the caller's.
*
*ARGUMENTS:
*  seed    - the seed blob from specDrainTakeSeed
*  seedLen - its length in bytes
*********************************************************/
bool lvSpecFeedSeed(const uint8_t *seed, size_t seedLen);

/*********************************************************
*NAME:          lvSpecFeedRecord
*PURPOSE:
*  Routes one drained record into the decoder and notes its
*  tick as the live head. The payload stays the caller's.
*
*ARGUMENTS:
*  rec - the record from specDrainPopRecord
*********************************************************/
LvSpecFeedResult lvSpecFeedRecord(const SpecDrainRecord *rec);

#endif /* LV_SPEC_FEED_H */
