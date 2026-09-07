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
*Name:          Voice
*Filename:      voice.c
*Author:        John Morrison
*Purpose:
*  The SDL3 backend for voice capture and playback: the
*  device half of voice_backend.h.  The voice runtime that
*  drives it is shared, in
*  src/client_frontend/voice_client.c.
*
*  Runs its own 48 kHz mono streams rather than sharing the
*  22.05 kHz effects mixer in sound.c, because 48 kHz mono
*  is the codec's native format.
*
*  Every stream is opened with a NULL callback, so SDL owns
*  the cross-thread pull and mix internally and the only two
*  calls that cross to the audio thread -
*  SDL_GetAudioStreamData and SDL_PutAudioStreamData - are
*  themselves thread safe.  Everything here therefore runs on
*  the main thread with no lock of its own.  Each remote
*  talker gets its own playback stream for the same reason:
*  SDL mixes the bound streams, so nothing here has to.
*********************************************************/

#include <SDL3/SDL.h>
#include <stdio.h>

#include "global.h"
#include "voice_core.h"
#include "voice_backend.h"

#define VOICE_FRAME_BYTES ((int)(VOICE_FRAME_SAMPLES * sizeof(int16_t)))

static SDL_AudioStream *captureStream = NULL;
static SDL_AudioStream *playbackStream = NULL;

/* One output stream per tank slot, opened the first time a frame arrives
 * from that player, so a quiet game costs nothing. */
static SDL_AudioStream *speakerStreams[MAX_TANKS];

/* The resolved device choice for each direction: a display name that was
 * present when it was set, or "" for the system default.  The name the player
 * asked for is held by voice_client.c, which keeps it whether the device is
 * there or not; only what can actually be opened gets this far. */
static char recordingDeviceName[VOICE_DEVICE_NAME_MAX];
static char playbackDeviceName[VOICE_DEVICE_NAME_MAX];

/* Devices reported to a caller in one enumeration.  Well past what a machine
 * has plugged in; anything past it is ignored rather than grown into. */
#define VOICE_DEVICE_LIST_MAX 32

/* The last enumeration of each direction, copied out of SDL because
 * SDL_GetAudioDeviceName hands back a string it owns and the name calls
 * promise a pointer that outlives the enumeration it came from.  Separate per
 * direction so counting one does not throw away the other. */
static char recordingDeviceList[VOICE_DEVICE_LIST_MAX][VOICE_DEVICE_NAME_MAX];
static int recordingDeviceListCount = 0;
static char playbackDeviceList[VOICE_DEVICE_LIST_MAX][VOICE_DEVICE_NAME_MAX];
static int playbackDeviceListCount = 0;

/* Set by the event watch below when a device appears or disappears, read and
 * cleared by voiceBackendDevicesChanged.  Atomic because the watch runs on
 * whichever thread pushed the event and the read is on the main one. */
static SDL_AtomicInt s_devicesChanged;

/*********************************************************
*NAME:          voiceOpenChatDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens one voice device with its stream role declared as
*  game chat, and hands back the stream or NULL.
*
*  The role tells the OS this is a voice call, which on the
*  backends that implement it - AAudio, WASAPI and PipeWire -
*  is what lets the platform apply its own echo
*  cancellation, gain control and noise suppression.
*  CoreAudio ignores it, so this buys macOS nothing.
*  "GameChat" rather than "Communications": SDL treats the
*  two alike except that "GameChat" does not attenuate other
*  audio streams, and a voice call must not duck the game.
*
*  The hint is global to the process and sound.c opens the
*  effects device through the same SDL, so the previous
*  value is put back before returning - including back to
*  unset - and the open is wrapped rather than the hint left
*  standing.
*
*ARGUMENTS:
*  device - the device to open
*  spec   - the format to open it in
*********************************************************/
static SDL_AudioStream *voiceOpenChatDevice(SDL_AudioDeviceID device,
                                            const SDL_AudioSpec *spec) {
    SDL_AudioStream *stream;
    const char *previous = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE);
    /* Copied because SDL_GetHint hands back a string it owns, and only its
     * internals say how long that lives. */
    char *saved = previous ? SDL_strdup(previous) : NULL;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "GameChat");
    stream = SDL_OpenAudioDeviceStream(device, spec, NULL, NULL);

    if (saved) {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, saved);
        SDL_free(saved);
    } else {
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE);
    }

    return stream;
}

/*********************************************************
*NAME:          voiceEnumerateDevices
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Refills one direction's name list from the devices present
*  and returns how many were kept.
*
*  The names are copied rather than pointed at: SDL owns what
*  SDL_GetAudioDeviceName returns, and the list the ids came
*  from is freed before the caller ever reads them.
*
*ARGUMENTS:
*  recording - true for the microphones, false for playback
*  list      - the name list to refill
*********************************************************/
static int voiceEnumerateDevices(bool recording,
                                 char list[][VOICE_DEVICE_NAME_MAX]) {
    SDL_AudioDeviceID *ids;
    int count = 0;
    int kept = 0;
    int i;

    ids = recording ? SDL_GetAudioRecordingDevices(&count)
                    : SDL_GetAudioPlaybackDevices(&count);
    if (ids == NULL) {
        return 0;
    }
    if (count > VOICE_DEVICE_LIST_MAX) {
        count = VOICE_DEVICE_LIST_MAX;
    }

    for (i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (name == NULL) {
            continue;
        }
        SDL_strlcpy(list[kept], name, VOICE_DEVICE_NAME_MAX);
        kept++;
    }

    SDL_free(ids);
    return kept;
}

/*********************************************************
*NAME:          resolveDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Turns a chosen device name into the id to open it by,
*  answering with the fallback when that name is not among
*  the devices present.
*
*  Opening SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK or _RECORDING
*  leaves SDL following the system default by itself, so
*  "system default" is this fallback and is not a case
*  handled anywhere else.
*
*ARGUMENTS:
*  recording - true for a microphone, false for playback
*  chosen    - the display name, or "" for the system default
*  fallback  - the id to answer with when chosen is not there
*********************************************************/
static SDL_AudioDeviceID resolveDevice(bool recording, const char *chosen,
                                       SDL_AudioDeviceID fallback) {
    SDL_AudioDeviceID *ids;
    const char *names[VOICE_DEVICE_LIST_MAX];
    SDL_AudioDeviceID resolved = fallback;
    int count = 0;
    int match;
    int i;

    if (chosen == NULL || chosen[0] == '\0') {
        return fallback;
    }

    ids = recording ? SDL_GetAudioRecordingDevices(&count)
                    : SDL_GetAudioPlaybackDevices(&count);
    if (ids == NULL) {
        return fallback;
    }
    if (count > VOICE_DEVICE_LIST_MAX) {
        count = VOICE_DEVICE_LIST_MAX;
    }

    /* The names are only read while the id list is still held, so pointing at
     * SDL's own storage is safe here in a way it is not for the snapshot the
     * name calls hand out. */
    for (i = 0; i < count; i++) {
        names[i] = SDL_GetAudioDeviceName(ids[i]);
    }

    match = voiceDeviceResolveName(chosen, names, count);
    if (match >= 0) {
        resolved = ids[match];
    }

    SDL_free(ids);
    return resolved;
}

/*********************************************************
*NAME:          voiceDevicePresent
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether a device of that display name is plugged in now.
*
*  Asked by resolving against an id of 0, which SDL never
*  hands out for a real device, so the fallback coming back
*  is unambiguously "not there".
*
*ARGUMENTS:
*  recording - true for a microphone, false for playback
*  name      - the display name to look for
*********************************************************/
static bool voiceDevicePresent(bool recording, const char *name) {
    return resolveDevice(recording, name, 0) != 0;
}

/*********************************************************
*NAME:          voiceDeviceEventWatch
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Notes that the set of audio devices moved, for the next
*  tick to act on.
*
*  It raises a flag and does nothing else, and that is the
*  point.  A watch runs on whichever thread pushed the event,
*  which for device hotplug is SDL's own and not the main
*  thread, while every other line in this file assumes the
*  main thread and takes no lock of its own - see the file
*  header.  Closing and reopening a stream from in here would
*  be doing that work on the wrong thread.
*
*ARGUMENTS:
*  userdata - unused
*  event    - the event being pushed
*********************************************************/
static bool SDLCALL voiceDeviceEventWatch(void *userdata, SDL_Event *event) {
    (void)userdata;

    if (event->type == SDL_EVENT_AUDIO_DEVICE_ADDED ||
        event->type == SDL_EVENT_AUDIO_DEVICE_REMOVED) {
        SDL_SetAtomicInt(&s_devicesChanged, 1);
    }

    /* A watch's answer is ignored - it cannot drop the event - but the
     * signature calls for one. */
    return true;
}

/*********************************************************
*NAME:          voiceBackendInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the playback stream.  No recording device is opened
*  here: voiceBackendCaptureStart does that on first use, so
*  launching the game never prompts for the microphone.
*  Returns whether voice is usable.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendInit(void) {
    SDL_AudioSpec spec;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "Voice error: init audio: %s\n", SDL_GetError());
        fflush(stderr);
        return false;
    }

    /* Follow devices coming and going.  Not worth failing init over: without
     * the watch voice still runs, it just stays on whatever it opened with
     * until the game is restarted.
     *
     * SDL announces every device it already knows about as it starts up, so
     * the flag can be set before the first tick ever runs.  That costs
     * nothing - re-applying a name that is already the choice does no work -
     * and needs no suppressing. */
    if (!SDL_AddEventWatch(voiceDeviceEventWatch, NULL)) {
        fprintf(stderr, "Voice error: watch audio devices: %s\n",
                SDL_GetError());
        fflush(stderr);
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = VOICE_SAMPLE_RATE;

    playbackStream = voiceOpenChatDevice(
        resolveDevice(false, playbackDeviceName,
                      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK),
        &spec);
    if (!playbackStream) {
        fprintf(stderr, "Voice error: open playback device: %s\n",
                SDL_GetError());
        fflush(stderr);
        voiceBackendShutdown();
        return false;
    }

    /* The playback device runs from here on - it only ever sees data we
     * push. */
    SDL_ResumeAudioStreamDevice(playbackStream);

    return true;
}

/*********************************************************
*NAME:          voiceBackendShutdown
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Destroys the recording and playback streams.  Safe to
*  call when init failed or never ran.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendShutdown(void) {
    /* Removing a watch that was never added does nothing, so this needs no
     * guard for the init that failed or never ran. */
    SDL_RemoveEventWatch(voiceDeviceEventWatch, NULL);

    if (captureStream) {
        SDL_DestroyAudioStream(captureStream);
        captureStream = NULL;
    }
    if (playbackStream) {
        SDL_DestroyAudioStream(playbackStream);
        playbackStream = NULL;
    }
}

/*********************************************************
*NAME:          voiceBackendCaptureStart
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens the recording stream the first time the microphone
*  is asked for, starts it, and reports whether one is
*  available.
*
*  Opening a recording device is what makes the OS ask the
*  user for microphone permission, so it is deferred until
*  something actually wants to capture - a player who never
*  turns voice on is never prompted.  The stream is kept for
*  the rest of the process once open, so turning the
*  microphone off and on again does not re-prompt.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureStart(void) {
    SDL_AudioSpec spec;

    if (!captureStream) {
        SDL_zero(spec);
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = VOICE_SAMPLE_RATE;

        captureStream = voiceOpenChatDevice(
            resolveDevice(true, recordingDeviceName,
                          SDL_AUDIO_DEVICE_DEFAULT_RECORDING),
            &spec);
        if (!captureStream) {
            fprintf(stderr, "Voice error: open recording device: %s\n",
                    SDL_GetError());
            fflush(stderr);
            return false;
        }
    }

    SDL_ResumeAudioStreamDevice(captureStream);
    return true;
}

/*********************************************************
*NAME:          voiceBackendCaptureStop
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Pauses the microphone and drops what both sides still
*  hold, or re-enabling would open with a burst of audio
*  recorded before it was switched off.  The stream itself
*  is kept, because re-opening it would re-prompt.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendCaptureStop(void) {
    if (captureStream) {
        SDL_PauseAudioStreamDevice(captureStream);
        SDL_ClearAudioStream(captureStream);
    }
}

/*********************************************************
*NAME:          voiceBackendCaptureIsOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether a recording device has been opened.  It
*  stays open across a stop and start, so this is whether a
*  microphone was ever had, not whether one is capturing
*  now.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureIsOpen(void) {
    return captureStream != NULL;
}

/*********************************************************
*NAME:          voiceBackendOsCancelsEcho
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether the platform cancels echo on the capture device
*  itself, which is true of exactly the drivers that act on
*  the game chat role voiceOpenChatDevice declares.  Three of
*  SDL's audio drivers do; every other one, CoreAudio among
*  them, ignores the role and leaves the work to Speex.
*
*  The live driver is asked for by name rather than inferred
*  from the platform, because more than one driver is built
*  for most platforms and which of them comes up is decided
*  at runtime - a Linux build carries PipeWire, PulseAudio
*  and ALSA, and only the first of those cancels.
*
*  SDL hands back NULL when the audio subsystem is not up.
*  That is not a driver that cancels, so it answers false.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendOsCancelsEcho(void) {
    const char *driver = SDL_GetCurrentAudioDriver();

    if (driver == NULL) {
        return false;
    }

    /* Matched case sensitively against the names the drivers register
     * themselves under.  "AAudio" really is the odd one out: that is the
     * capitalisation SDL gives Android's driver, not a slip to tidy up, and
     * lower-casing it would quietly leave Android running both cancellers. */
    return SDL_strcmp(driver, "wasapi") == 0 ||
           SDL_strcmp(driver, "pipewire") == 0 ||
           SDL_strcmp(driver, "AAudio") == 0;
}

/*********************************************************
*NAME:          voiceBackendCaptureRead
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the next whole captured frame, or reports that one
*  is not ready.  Returns VOICE_FRAME_SAMPLES on success and
*  0 otherwise; a partial frame is never handed back.
*
*ARGUMENTS:
*  pcm - filled with VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
int voiceBackendCaptureRead(int16_t *pcm) {
    if (!captureStream) {
        return 0;
    }
    if (SDL_GetAudioStreamAvailable(captureStream) < VOICE_FRAME_BYTES) {
        return 0;
    }
    if (SDL_GetAudioStreamData(captureStream, pcm, VOICE_FRAME_BYTES) !=
        VOICE_FRAME_BYTES) {
        return 0;
    }
    return VOICE_FRAME_SAMPLES;
}

/*********************************************************
*NAME:          voiceBackendSpeakerOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Opens one remote player's output stream on the first
*  frame heard from them, and reports whether that player
*  can be played.
*
*ARGUMENTS:
*  player - the player number the frame came from
*********************************************************/
bool voiceBackendSpeakerOpen(int player) {
    SDL_AudioSpec spec;

    /* player is a wire byte, unpacked by voiceSegmentUnpackDown, and the
     * only check that it fits speakerStreams lives in voice_client.c.  The
     * four speaker entry points hold the bound here as well, so the array
     * does not depend on a caller two layers up remembering to. */
    if (player < 0 || player >= MAX_TANKS) {
        return false;
    }
    if (speakerStreams[player] != NULL) {
        return true;
    }

    SDL_zero(spec);
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = VOICE_SAMPLE_RATE;

    speakerStreams[player] = voiceOpenChatDevice(
        resolveDevice(false, playbackDeviceName,
                      SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK),
        &spec);
    if (speakerStreams[player] == NULL) {
        fprintf(stderr, "Voice error: open playback device: %s\n",
                SDL_GetError());
        fflush(stderr);
        return false;
    }
    /* Bound to the device with no callback of our own, so SDL pulls and
     * mixes this talker against the others on its own thread. */
    SDL_ResumeAudioStreamDevice(speakerStreams[player]);

    return true;
}

/*********************************************************
*NAME:          voiceBackendSpeakerClose
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Destroys one remote player's output stream.  Anything
*  they had queued goes with it.
*
*ARGUMENTS:
*  player - the player number to release
*********************************************************/
void voiceBackendSpeakerClose(int player) {
    if (player < 0 || player >= MAX_TANKS) {
        return;
    }
    if (speakerStreams[player]) {
        SDL_DestroyAudioStream(speakerStreams[player]);
        speakerStreams[player] = NULL;
    }
}

/*********************************************************
*NAME:          voiceBackendSpeakerQueuedFrames
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the whole frames one remote player still has
*  waiting to be played.
*
*  SDL_GetAudioStreamQueued counts the bytes as they were
*  put in, and nothing here ever changes a stream's input
*  format, so those are 20 ms frames of S16 mono at the
*  codec's rate.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
int voiceBackendSpeakerQueuedFrames(int player) {
    if (player < 0 || player >= MAX_TANKS) {
        return 0;
    }
    if (speakerStreams[player] == NULL) {
        return 0;
    }
    return (int)(SDL_GetAudioStreamQueued(speakerStreams[player]) /
                 VOICE_FRAME_BYTES);
}

/*********************************************************
*NAME:          voiceBackendSpeakerPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Queues one frame for a remote player.
*
*ARGUMENTS:
*  player - the player number the frame came from
*  pcm    - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendSpeakerPlay(int player, const int16_t *pcm) {
    if (player < 0 || player >= MAX_TANKS) {
        return;
    }
    if (speakerStreams[player]) {
        SDL_PutAudioStreamData(speakerStreams[player], pcm, VOICE_FRAME_BYTES);
    }
}

/*********************************************************
*NAME:          voiceBackendLoopbackQueuedFrames
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns the whole frames the microphone loopback test
*  still has waiting to be played.
*
*  Counted the same way as a remote talker's: the bytes SDL
*  still holds, as they were put in, over the size of a
*  frame.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendLoopbackQueuedFrames(void) {
    if (playbackStream == NULL) {
        return 0;
    }
    return (int)(SDL_GetAudioStreamQueued(playbackStream) / VOICE_FRAME_BYTES);
}

/*********************************************************
*NAME:          voiceBackendLoopbackPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Queues one frame of the microphone loopback test.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendLoopbackPlay(const int16_t *pcm) {
    if (playbackStream) {
        SDL_PutAudioStreamData(playbackStream, pcm, VOICE_FRAME_BYTES);
    }
}

/*********************************************************
*NAME:          voiceBackendLoopbackClear
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drops whatever the loopback test still has queued.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendLoopbackClear(void) {
    if (playbackStream) {
        SDL_ClearAudioStream(playbackStream);
    }
}

/*********************************************************
*NAME:          voiceBackendRecordingDeviceCount
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes a fresh list of the microphones present and returns
*  how many are in it.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendRecordingDeviceCount(void) {
    recordingDeviceListCount =
        voiceEnumerateDevices(true, recordingDeviceList);
    return recordingDeviceListCount;
}

/*********************************************************
*NAME:          voiceBackendPlaybackDeviceCount
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes a fresh list of the playback devices present and
*  returns how many are in it.
*
*ARGUMENTS:
*  (none)
*********************************************************/
int voiceBackendPlaybackDeviceCount(void) {
    playbackDeviceListCount = voiceEnumerateDevices(false, playbackDeviceList);
    return playbackDeviceListCount;
}

/*********************************************************
*NAME:          voiceBackendRecordingDeviceName
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  One microphone's display name out of the last list taken,
*  or NULL when index is outside it.
*
*ARGUMENTS:
*  index - 0..count-1 from the recording device count
*********************************************************/
const char *voiceBackendRecordingDeviceName(int index) {
    if (index < 0 || index >= recordingDeviceListCount) {
        return NULL;
    }
    return recordingDeviceList[index];
}

/*********************************************************
*NAME:          voiceBackendPlaybackDeviceName
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  One playback device's display name out of the last list
*  taken, or NULL when index is outside it.
*
*ARGUMENTS:
*  index - 0..count-1 from the playback device count
*********************************************************/
const char *voiceBackendPlaybackDeviceName(int index) {
    if (index < 0 || index >= playbackDeviceListCount) {
        return NULL;
    }
    return playbackDeviceList[index];
}

/*********************************************************
*NAME:          voiceBackendSetRecordingDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the microphone to capture from, by display name, and
*  moves an open capture stream onto it.  "" and NULL are the
*  system default; a name no device present answers to is
*  refused with nothing changed.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
bool voiceBackendSetRecordingDevice(const char *name) {
    bool wasCapturing;

    if (name == NULL) {
        name = "";
    }
    if (name[0] != '\0' && !voiceDevicePresent(true, name)) {
        return false;
    }
    if (SDL_strcmp(name, recordingDeviceName) == 0) {
        return true;
    }
    SDL_strlcpy(recordingDeviceName, name, sizeof(recordingDeviceName));

    if (captureStream) {
        /* A stream belongs to the device it was opened on, so moving means
         * making a new one.  Whether it was actually capturing is carried
         * across: the runtime pauses the microphone rather than closing it,
         * and a device change must not turn it live under a player who had
         * it off. */
        wasCapturing = !SDL_AudioStreamDevicePaused(captureStream);
        SDL_DestroyAudioStream(captureStream);
        captureStream = NULL;

        /* Opening a microphone again can put the operating system's
         * permission prompt back up.  That is acceptable here - the player
         * just asked for this device.  An open that fails leaves no capture
         * stream, which the runtime retries on its own; the choice was still
         * taken, so this still reports success. */
        if (voiceBackendCaptureStart() && !wasCapturing) {
            voiceBackendCaptureStop();
        }
    }

    return true;
}

/*********************************************************
*NAME:          voiceBackendSetPlaybackDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the device to play through, by display name, and
*  moves every open playback stream onto it - the microphone
*  test's and one per remote talker.  "" and NULL are the
*  system default; a name no device present answers to is
*  refused with nothing changed.
*
*ARGUMENTS:
*  name - the display name, or NULL/"" for the system default
*********************************************************/
bool voiceBackendSetPlaybackDevice(const char *name) {
    SDL_AudioSpec spec;
    int i;

    if (name == NULL) {
        name = "";
    }
    if (name[0] != '\0' && !voiceDevicePresent(false, name)) {
        return false;
    }
    if (SDL_strcmp(name, playbackDeviceName) == 0) {
        return true;
    }
    SDL_strlcpy(playbackDeviceName, name, sizeof(playbackDeviceName));

    /* Whatever these streams still held goes with them.  At 20 ms a frame
     * that is a fraction of a second of somebody mid-word, against a device
     * change the player asked for. */
    if (playbackStream) {
        SDL_zero(spec);
        spec.format = SDL_AUDIO_S16;
        spec.channels = 1;
        spec.freq = VOICE_SAMPLE_RATE;

        SDL_DestroyAudioStream(playbackStream);
        playbackStream = voiceOpenChatDevice(
            resolveDevice(false, playbackDeviceName,
                          SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK),
            &spec);
        if (playbackStream) {
            SDL_ResumeAudioStreamDevice(playbackStream);
        } else {
            fprintf(stderr, "Voice error: open playback device: %s\n",
                    SDL_GetError());
            fflush(stderr);
        }
    }

    /* Every talker as well, or the microphone test comes out of the chosen
     * device and the other players stay on the old one. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (speakerStreams[i] == NULL) {
            continue;
        }
        SDL_DestroyAudioStream(speakerStreams[i]);
        speakerStreams[i] = NULL;
        voiceBackendSpeakerOpen(i);
    }

    return true;
}

/*********************************************************
*NAME:          voiceBackendGetRecordingDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  The microphone being opened on, or "" for the system
*  default.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceBackendGetRecordingDevice(void) {
    return recordingDeviceName;
}

/*********************************************************
*NAME:          voiceBackendGetPlaybackDevice
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  The device being played through, or "" for the system
*  default.
*
*ARGUMENTS:
*  (none)
*********************************************************/
const char *voiceBackendGetPlaybackDevice(void) {
    return playbackDeviceName;
}

/*********************************************************
*NAME:          voiceBackendDevicesChanged
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Whether a device appeared or disappeared since the last
*  call, clearing the flag as it answers.
*
*  Read and cleared in the one operation, because the watch
*  that raises it runs on another thread: a separate read and
*  clear would drop a device that moved between the two.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendDevicesChanged(void) {
    return SDL_SetAtomicInt(&s_devicesChanged, 0) != 0;
}

/*********************************************************
*NAME:          voiceBackendNowMs
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Monotonic milliseconds, from SDL's tick source - the
*  milliseconds since SDL was brought up, which only ever
*  moves forwards.  Truncated to 32 bits, so callers compare
*  with a signed difference.
*
*ARGUMENTS:
*  (none)
*********************************************************/
uint32_t voiceBackendNowMs(void) {
    return (uint32_t)SDL_GetTicks();
}
