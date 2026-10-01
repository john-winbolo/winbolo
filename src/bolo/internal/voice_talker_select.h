/*
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

/*********************************************************
 *Name:          Voice Talker Select
 *Filename:      voice_talker_select.h
 *Author:        John Morrison
 *Purpose:
 *  Picks which talkers one listener hears when more people
 *  are speaking at once than anyone can follow. The choice
 *  is the most recently started utterances: a listener
 *  follows the voices that just began, not the ones that
 *  have been running.
 *
 *  Selection is by arrival bookkeeping alone - not by
 *  loudness. The server forwards encoded audio and never
 *  decodes it, so it has no per-frame energy to rank by.
 *
 *  Deliberately free of any codec, transport or sim
 *  dependency, for the same reason voice_segment.c is: the
 *  dedicated server links this without libopus, and a test
 *  can drive it with nothing else standing up.
 *
 *  T2 (sim internals): includable within src/bolo/,
 *  src/server/, and tests/unit/ only.
 *********************************************************/

#ifndef VOICE_TALKER_SELECT_H
#define VOICE_TALKER_SELECT_H

#include <stdint.h>

/* Most talkers one call can return. A candidate is a distinct player slot,
 * so no caller can offer more than the tank slots hold; maxTalkers is
 * clamped to this, which keeps the ranking allocation-free. The .c pins it
 * against MAX_TANKS. */
#define VOICE_TALKER_SELECT_MAX 16

typedef struct {
    uint8_t  slot;       /* sender's player slot                       */
    uint32_t onsetTick;  /* tick their current utterance began         */
    uint8_t  newestSeq;  /* highest seq staged from them this tick     */
} VoiceTalkerCandidate;

/* Choose at most maxTalkers of cands, writing the chosen slots into outSlots
 * (which must hold maxTalkers entries) and returning how many were written.
 *
 * Ranked by onsetTick descending - most recently started first - tie-broken
 * by newest newestSeq, then by lowest slot so the result is stable for
 * inputs that tie on both keys. count <= maxTalkers selects everything.
 * Returns 0 for a NULL argument or a non-positive count / maxTalkers. */
int voiceSelectTalkers(const VoiceTalkerCandidate *cands, int count,
                       int maxTalkers, uint8_t *outSlots);

#endif /* VOICE_TALKER_SELECT_H */
