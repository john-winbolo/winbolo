/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_voice.h
 * Purpose:
 *  The RECORD_AUDIO permission voice chat's microphone
 *  needs, asked for through SDL the first time voice.c
 *  wants to open the recording device rather than at
 *  launch, so a player who never turns voice on is never
 *  asked. See android_voice.c.
 *********************************************************/

#ifndef ANDROID_VOICE_H
#define ANDROID_VOICE_H

#include <stdbool.h>

/* Whether the recording device may be opened now. True once the permission
 * is granted. Otherwise the system's permission dialog is put up (unless one
 * is already up, or the player refused it) and this answers false, so the
 * caller treats the microphone as absent for now; SDL's own open would spin
 * the main thread until the dialog is answered. A refusal holds, however
 * often the caller tries, until androidVoiceMicAskAgain. Called from the
 * main thread. */
bool androidVoiceMicPermitted(void);

/* Lets a refusal be asked about again. Called when the player switches voice
 * on themselves, so the dialog comes back for a deliberate second try and
 * not for the runtime's own retries (Android stops showing it after the
 * second refusal of an install). Does nothing in any other state. */
void androidVoiceMicAskAgain(void);

#endif /* ANDROID_VOICE_H */
