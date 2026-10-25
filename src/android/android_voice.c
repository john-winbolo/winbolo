/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_voice.c
 * Purpose:
 *  The microphone permission for voice chat. voice.c asks
 *  here before it opens the recording device; the answer
 *  to the system dialog arrives on another thread through
 *  SDL's callback and is kept in an atomic the opener
 *  reads on its next attempt. See android_voice.h.
 *********************************************************/

#include <SDL3/SDL.h>

#include "../common/wb_log.h"
#include "android_voice.h"

#define VOICE_MIC_PERMISSION "android.permission.RECORD_AUDIO"

/* Where the request has got to. Not asked; pending while the dialog is up;
 * granted for the rest of the process; refused, which holds until the
 * player's own enable action (androidVoiceMicAskAgain) - the voice runtime
 * retries the open on its own cadence for as long as the microphone is
 * wanted, and a refusal re-asked that way would put the dialog straight
 * back up, and Android stops showing it after the second refusal of an
 * install. The callback runs on whichever thread answers (the activity's
 * when the dialog is answered, the caller's own when the permission is
 * already held), so the state is an atomic. */
enum {
  MIC_ASK     = 0,
  MIC_PENDING = 1,
  MIC_GRANTED = 2,
  MIC_DENIED  = 3
};

static SDL_AtomicInt s_micState;

static void SDLCALL androidVoicePermissionResult(void *userdata,
                                                 const char *permission,
                                                 bool granted) {
  (void)userdata;
  (void)permission;
  SDL_SetAtomicInt(&s_micState, granted ? MIC_GRANTED : MIC_DENIED);
  if (granted) {
    WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] microphone permission granted");
  } else {
    /* The same words voice.c prints for a device that will not open, since
       that is what the refusal amounts to: voice stays off with no
       microphone. */
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM,
                "Voice error: open recording device: %s refused", VOICE_MIC_PERMISSION);
  }
}

void androidVoiceMicAskAgain(void) {
  SDL_CompareAndSwapAtomicInt(&s_micState, MIC_DENIED, MIC_ASK);
}

bool androidVoiceMicPermitted(void) {
  int state = SDL_GetAtomicInt(&s_micState);

  if (state == MIC_GRANTED) return true;
  if (state == MIC_PENDING || state == MIC_DENIED) return false;

  /* Pending before the request goes out: when the permission is already
     held, SDL answers from inside the call on this thread, and the state
     set here would otherwise overwrite that answer. */
  SDL_SetAtomicInt(&s_micState, MIC_PENDING);
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] asking for the microphone permission");
  if (!SDL_RequestAndroidPermission(VOICE_MIC_PERMISSION,
                                    androidVoicePermissionResult, NULL)) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "[Android] microphone permission request failed: %s",
                SDL_GetError());
    SDL_SetAtomicInt(&s_micState, MIC_ASK);
    return false;
  }
  return SDL_GetAtomicInt(&s_micState) == MIC_GRANTED;
}
