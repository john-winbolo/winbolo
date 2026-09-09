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
 *Name:          Voice Talker Select
 *Filename:      voice_talker_select.c
 *Author:        John Morrison
 *Purpose:
 *  Rank candidate talkers and keep the most recently
 *  started ones. See voice_talker_select.h for the model.
 *
 *  The ranking is the whole of the decision the voice
 *  fan-out makes per recipient, held here as a pure
 *  function of its inputs so it can be tested without a
 *  server, a socket or a second client.
 *********************************************************/

#include "voice_talker_select.h"

#include <stdbool.h>

#include "global.h"

/* Every candidate is a distinct player slot, so the clamp below can never
 * discard a real talker. */
BOLO_STATIC_ASSERT(VOICE_TALKER_SELECT_MAX >= MAX_TANKS,
                   voice_talker_select_max_below_tank_slots);

/* True when a is the newer sequence number. Sequence numbers are a byte and
 * wrap at 256, so a raw > would rank 3 behind 250 across a wrap; the signed
 * difference gets it right for any pair less than 128 apart.
 *
 * This repeats voiceSeqAfter in the client's voice_core.c rather than
 * including it: that header belongs to the client runtime and links Opus,
 * which this file must stay clear of so the dedicated server can link it.
 * Same reason voice_segment.c carries no codec include. */
static bool talkerSeqAfter(uint8_t a, uint8_t b) {
    return (int8_t)(a - b) > 0;
}

/* Strict ordering: true when a outranks b. The final slot comparison means
 * no two distinct candidates ever tie, so the insertion below is
 * deterministic for equal inputs.
 *
 * The sequence tie-break only decides between talkers who began speaking on
 * the same tick, and it compares counters belonging to different senders, so
 * which of them it favours is arbitrary by nature - what matters is that it
 * is the same answer every time for the same input. The wrap-safe compare is
 * cyclic rather than transitive, so a set spread more than half the sequence
 * space apart has no single "newest"; the caller offers candidates in slot
 * order, which keeps even that case stable. */
static bool talkerRanksBefore(const VoiceTalkerCandidate *a,
                              const VoiceTalkerCandidate *b) {
    if (a->onsetTick != b->onsetTick) {
        return a->onsetTick > b->onsetTick;
    }
    if (a->newestSeq != b->newestSeq) {
        return talkerSeqAfter(a->newestSeq, b->newestSeq);
    }
    return a->slot < b->slot;
}

int voiceSelectTalkers(const VoiceTalkerCandidate *cands, int count,
                       int maxTalkers, uint8_t *outSlots) {
    /* Indices into cands, best first. Never longer than maxTalkers, so a
     * candidate that falls off the end is simply dropped. */
    int keep[VOICE_TALKER_SELECT_MAX];
    int kept = 0;
    int i, j;

    if (cands == NULL || outSlots == NULL || count <= 0 || maxTalkers <= 0) {
        return 0;
    }
    if (maxTalkers > VOICE_TALKER_SELECT_MAX) {
        maxTalkers = VOICE_TALKER_SELECT_MAX;
    }

    /* Insertion into a bounded top-N list: each candidate walks at most
     * maxTalkers kept entries, so the work is linear in count with no scratch
     * the size of the input and no copy of it. */
    for (i = 0; i < count; i++) {
        int pos = kept;
        while (pos > 0 && talkerRanksBefore(&cands[i], &cands[keep[pos - 1]])) {
            pos--;
        }
        if (pos >= maxTalkers) {
            continue;   /* outranked by every kept talker, and the list is full */
        }
        if (kept < maxTalkers) {
            kept++;
        }
        for (j = kept - 1; j > pos; j--) {
            keep[j] = keep[j - 1];
        }
        keep[pos] = i;
    }

    for (i = 0; i < kept; i++) {
        outSlots[i] = cands[keep[i]].slot;
    }
    return kept;
}
