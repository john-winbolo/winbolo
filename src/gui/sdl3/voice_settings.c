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
*Name:          Voice Settings
*Filename:      voice_settings.c
*Author:        John Morrison
*Purpose:
*  The windowSetVoice* / windowGetVoice* functions the
*  settings dialog, the lobby and the prefs code in
*  gamefront.c call to change and read the voice settings.
*  Shared by the desktop client (winbolo.c), the iOS
*  client (winbolo_ios.m) and the web client
*  (main_wasm.c), so the clamps are written once.
*
*  The callers declare these themselves rather than through
*  winbolo.h, so the voice build option does not reach the
*  shared frontend header.
*********************************************************/

#include <stdbool.h>

#include "../voice.h"
#if defined(WINBOLO_VOICE_AEC)
#include "voice_aec.h"
#endif
#include "sdl3draw_status.h"
#if defined(__ANDROID__)
#include "../../android/android_voice.h"  /* androidVoiceMicAskAgain */
#endif

/* -------------------------------------------------------
 * Voice settings — apply a value to the running voice module
 * and clamp it to the range the UI offers, in one place, so
 * the settings dialog and the prefs loader agree.  The voice
 * module itself holds the value; gameFrontPutPrefs reads it
 * back out through the getters below at save time.
 * ------------------------------------------------------- */
void windowSetVoiceEnabled(bool on) {
#if defined(__ANDROID__)
  /* Switching voice on is the player's own ask for the microphone: a
     permission they refused earlier may be asked about again. */
  if (on) androidVoiceMicAskAgain();
#endif
  voiceSetEnabled(on);
}

bool windowGetVoiceEnabled(void) {
  return voiceIsEnabled();
}

void windowSetVoiceMode(int mode) {
  if (mode < VOICE_MODE_OFF || mode > VOICE_MODE_OPEN) {
    mode = VOICE_MODE_PTT;
  }
  voiceSetMode((VoiceMode)mode);
}

int windowGetVoiceMode(void) {
  return (int)voiceGetMode();
}

void windowSetVoiceMicGain(float gain) {
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 4.0f) gain = 4.0f;
  voiceSetMicGain(gain);
}

float windowGetVoiceMicGain(void) {
  return voiceGetMicGain();
}

void windowSetVoiceVolume(float gain) {
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 2.0f) gain = 2.0f;
  voiceSetOutputVolume(gain);
}

float windowGetVoiceVolume(void) {
  return voiceGetOutputVolume();
}

/* The chosen audio devices, by display name, "" for the system default.
 * Nothing to clamp — the voice module holds the name and truncates it — but
 * they sit with the rest of the façade so gameFrontPutPrefs reads them back
 * the same way as everything else here. */
void windowSetVoiceRecordingDevice(const char *name) {
  voiceSetRecordingDevice(name);
}

const char *windowGetVoiceRecordingDevice(void) {
  return voiceGetRecordingDevice();
}

void windowSetVoicePlaybackDevice(const char *name) {
  voiceSetPlaybackDevice(name);
}

const char *windowGetVoicePlaybackDevice(void) {
  return voiceGetPlaybackDevice();
}

#if defined(WINBOLO_VOICE_AEC)
/* Echo cancellation of the other players' voices out of this microphone.
 * Same shape as the voice settings above: the canceller module holds the
 * switch and gameFrontPutPrefs reads it back through the getter. */
void windowSetVoiceEchoCancel(bool on) {
  voiceAecSetEnabled(on);
}

bool windowGetVoiceEchoCancel(void) {
  return voiceAecIsEnabled();
}

/* Whether a canceller came up at all, which the getter above cannot say -
 * it reports the switch, and the switch is the player's either way. */
bool windowGetVoiceEchoCancelAvailable(void) {
  return voiceAecIsAvailable();
}

/* Whether that canceller is the operating system's rather than ours, which
 * is a different row in Settings: switched off and greyed, because there is
 * nothing here for the player to switch. */
bool windowGetVoiceEchoCancelPlatform(void) {
  return voiceAecIsPlatform();
}
#endif

/* Tank-label microphone icons are held by the status renderer that
 * draws them; same façade shape as the voice settings above. */
void windowSetShowTankMicIcons(bool on) {
  sdl3DrawStatusSetShowMicIcons(on);
}

bool windowGetShowTankMicIcons(void) {
  return sdl3DrawStatusGetShowMicIcons();
}
