/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Spectator Feed
 *Filename:      lv_spec_feed.c
 *Purpose:
 *  The seed and record handling the spectator hosts share.
 *  See lv_spec_feed.h. Moved out of logviewer.c's
 *  spectatorRun unchanged, so the embedded spectator calls
 *  the same body rather than a copy of it.
 *********************************************************/

#include <string.h>

#include "backend.h"
#include "lv_players.h"   /* lv_playersSetPlayerNameQuiet */
#include "lv_global.h"
#include "logviewer.h"     /* LogViewerState: isLoaded, gamePhase */
#include "lv_host.h"       /* g_lv */
#include "draw.h"         /* lv_drawDirtyScreen */
#include "imgui/imgui_events.h"
#include "lv_spec_feed.h"

/* Apply a decoded seed/keyframe's lobby roster to the viewer's player table:
 * name every present lobby slot (silently, via the quiet setter — no newswire
 * spam) and adopt the snapshot's phase. The quiet setter touches only inUse +
 * playerName, never a tank position, so a positionless lobby-only slot stays
 * suppressed by the (0,0,0,0) guard in lv_playersMakeScreenTanks and a slot
 * holding a real forward-established tank keeps its position (only its name is
 * re-set to the same value). Safe to call on every keyframe. */
static void lvSpecApplyRoster(const SpecSeedInfo *sgi) {
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sgi->lobbyPresent[i]) {
            lv_playersSetPlayerNameQuiet((BYTE)i, sgi->lobbyName[i]);
        }
    }
    g_lv->gamePhase = sgi->specPhase;
}

/* Decode a seed or keyframe and load it, as the opening seed and as a new
 * segment's keyframe both do. The spectator never receives the lobby-settings
 * packet on the wire, but the seed's control snapshot carries that same event;
 * decode it for the synthesized header's map name / game settings. sgi outlives
 * the load call, which copies mapName into the header. Fields the event doesn't
 * carry stay zero (panel defaults). sgi is filled for the caller's roster. */
static bool lvSpecFeedLoad(const uint8_t *seed, size_t seedLen, SpecSeedInfo *sgi) {
    LvSpecSeedInfo info;
    const LvSpecSeedInfo *infoPtr = NULL;
    if (specSeedDecodeInfo(seed, seedLen, sgi) && sgi->haveInfo) {
        memset(&info, 0, sizeof(info));
        info.mapName          = sgi->mapName;
        info.gameType         = sgi->gameType;
        info.allowHiddenMines = sgi->allowHiddenMines;
        info.ai               = sgi->ai;
        infoPtr = &info;
    }
    /* Phase is populated by the decode walk regardless of haveInfo
       (specSeedDecodeInfo zeroes sgi first), so read it unconditionally. */
    g_lv->gamePhase = sgi->specPhase;
    return lv_specSeedLoad(infoPtr, seed, seedLen);
}

bool lvSpecFeedSeed(const uint8_t *seed, size_t seedLen) {
    SpecSeedInfo sgi;
    if (!lvSpecFeedLoad(seed, seedLen, &sgi)) {
        return false;
    }
    g_lv->isLoaded = TRUE;
    lv_imgui_events_clear();
    /* Resolve lobby-chat sender names for players who hold no tank (and so
       never appear in the world snapshot): inject the names the seed's
       lobby-slot roster carried into the viewer roster. sgi was zeroed by
       specSeedDecodeInfo, so absent slots are simply skipped. Silent (quiet
       setter) — no newswire spam. */
    lvSpecApplyRoster(&sgi);
    return true;
}

LvSpecFeedResult lvSpecFeedRecord(const SpecDrainRecord *rec) {
    LvSpecFeedResult result = LV_SPEC_FEED_PUMPED;

    /* A world reset (new lobby/map) regresses the game tick below the tracked
       head and arrives as a forced keyframe. Reset the DVR to the new segment —
       drop the old map's scroll-back range and restart head-time tracking —
       then re-seed from this keyframe so the decoder re-syncs on a fresh
       buffer, exactly as on the initial seed. The re-seed rebuilds the live
       buffer + decoder; lv_screenSpecResetSegment clears the seek index and DVR
       state the re-seed leaves alone. */
    if (rec->isKeyframe && lv_screenSpecIsLiveMode() &&
        lv_screenSpecHeadTick() > 0 &&
        rec->gameTick < lv_screenSpecHeadTick()) {
        SpecSeedInfo sgi;
        lv_screenSpecResetSegment();
        /* The new segment's keyframe carries its own lobby-settings control
           slice, so recover the new map name / settings from it the same way
           the initial seed does. */
        if (!lvSpecFeedLoad(rec->payload, rec->payloadLen, &sgi)) {
            /* A failed re-seed left no decoder; the host exits to a clean
               teardown rather than render against a torn-down buffer. */
            g_lv->isLoaded = FALSE;
            result = LV_SPEC_FEED_LOST;
        } else {
            /* Match the initial-seed path: drop the previous segment's cached
               overview tiles and stale event feed so a lobby map change fully
               redraws to the new map instead of lingering on the old one. */
            lv_drawDirtyScreen();
            g_lv->wantScreenUpdate = TRUE;
            lv_imgui_events_clear();
            /* Refresh lobby-chat sender names + phase from the new segment's
               roster (same silent injection as the initial seed). */
            lvSpecApplyRoster(&sgi);
            result = LV_SPEC_FEED_RESEEDED;
        }
    } else {
        /* A mid-segment keyframe still carries the current control snapshot,
           so refresh the lobby roster + phase from it: a player who joins the
           lobby while we are already spectating is named within a keyframe
           interval instead of waiting for the next segment re-seed. Silent
           (quiet setter) — no event spam. Decode fills roster/phase
           regardless of return value (specSeedDecodeInfo zeroes kf first); a
           non-keyframe forward record carries no snapshot and is pumped
           unchanged. */
        if (rec->isKeyframe) {
            SpecSeedInfo kf;
            int prevPhase = g_lv->gamePhase;
            specSeedDecodeInfo(rec->payload, rec->payloadLen, &kf);
            lvSpecApplyRoster(&kf);
            if (g_lv->gamePhase != prevPhase) {
                result = LV_SPEC_FEED_PHASE_CHANGED;
            }
        }
        lv_specRecordPump(rec->isKeyframe, rec->payload, rec->payloadLen);
    }
    lv_screenSpecNoteHeadTick(rec->gameTick);
    return result;
}
