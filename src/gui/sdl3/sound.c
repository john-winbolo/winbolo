/*
 * $Id$
 *
 * Copyright (c) 1998-2008 John Morrison.
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
*Name:          Sound
*Filename:      sound.c
*Author:        John Morrison
*Creation Date: 26/12/98
*Last Modified: 24/02/02
*Purpose:
*  System Specific Sound Playing routines
*  (Uses SDL3)
*********************************************************/

#include "../../bolo/global.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include "../../bolo/screen.h"
#include "../sound.h"

#define NUM_SOUNDS 24
#define MAX_SOUND_SLOTS 8  /* Maximum simultaneous sounds */

/* Sound data structure for each effect - stores converted data */
typedef struct {
    Uint8 *data;    /* Converted audio data matching device format */
    Uint32 size;    /* Size of converted data in bytes */
} SoundData;

/* Active sound slot for mixing */
typedef struct {
    const Uint8 *data;  /* Pointer to sound data */
    Uint32 size;        /* Total size */
    Uint32 pos;         /* Current playback position */
    bool active;        /* Is this slot playing? */
} SoundSlot;

/* Global sound state */
static bool isPlayable = FALSE;
static SDL_AudioStream *audioStream = NULL;
static SDL_AudioSpec deviceSpec;

/* Sound effect data (pre-loaded and converted) */
static SoundData sounds[NUM_SOUNDS];

/* Active sound slots for mixing */
static SoundSlot slots[MAX_SOUND_SLOTS];
static SDL_Mutex *slotsMutex = NULL;

/* Keepalive data */
static SDL_AudioStream *keepaliveStream = NULL;
static Uint8 *keepaliveData = NULL;
static Uint32 keepaliveSize = 0;

#ifndef DIALOG_BOX_TITLE
#define DIALOG_BOX_TITLE "WinBolo"
#endif

/* Filenames for each sound effect (relative names under data/sounds/) */
static const char *soundFiles[NUM_SOUNDS] = {
    "tank_sinking_near.wav",   /*  0 */
    "tank_sinking_far.wav",    /*  1 */
    "shot_tree_near.wav",      /*  2 */
    "shot_tree_far.wav",       /*  3 */
    "shot_building_near.wav",  /*  4 */
    "shot_building_far.wav",   /*  5 */
    "shooting_self.wav",       /*  6 */
    "shooting_near.wav",       /*  7 */
    "shooting_far.wav",        /*  8 */
    "mine_explosion_near.wav", /*  9 */
    "mine_explosion_far.wav",  /* 10 */
    "big_explosion_far.wav",   /* 11 */
    "man_dying_near.wav",      /* 12 */
    "man_dying_far.wav",       /* 13 */
    "man_building_near.wav",   /* 14 */
    "man_building_far.wav",    /* 15 */
    "hit_tank_self.wav",       /* 16 */
    "hit_tank_near.wav",       /* 17 */
    "hit_tank_far.wav",        /* 18 */
    "farming_tree_near.wav",   /* 19 */
    "farming_tree_far.wav",    /* 20 */
    "bubbles.wav",             /* 21 */
    "big_explosion_near.wav",  /* 22 */
    "man_lay_mine_near.wav",   /* 23 */
};

/*********************************************************
*NAME:          convertAudioData
*AUTHOR:        John Morrison
*CREATION DATE: 2024
*LAST MODIFIED: 2024
*PURPOSE:
*  Converts audio data from source format to device format
*  using SDL3 audio stream conversion.
*
*ARGUMENTS:
*  srcData     - Source audio data
*  srcSize     - Source data size
*  srcSpec     - Source audio format
*  dstData     - Pointer to receive converted data
*  dstSize     - Pointer to receive converted size
*
*RETURNS:
*  true if successful, false otherwise
*********************************************************/
static bool convertAudioData(const Uint8 *srcData, Uint32 srcSize,
                             const SDL_AudioSpec *srcSpec,
                             Uint8 **dstData, Uint32 *dstSize) {
    int dstLen = 0;
    if (!SDL_ConvertAudioSamples(srcSpec, srcData, (int)srcSize,
                                  &deviceSpec, dstData, &dstLen)) {
        return false;
    }
    *dstSize = (Uint32)dstLen;
    return dstLen > 0;
}

/*********************************************************
*NAME:          loadSoundFromFile
*AUTHOR:        John Morrison
*CREATION DATE: 2024
*LAST MODIFIED: 2024
*PURPOSE:
*  Loads a WAV file from disk and converts it to the
*  device audio format.
*
*ARGUMENTS:
*  basePath  - Base path to look for sounds
*  filename  - WAV filename (relative to data/sounds/)
*  sound     - Pointer to SoundData to fill
*
*RETURNS:
*  true if successful, false otherwise
*********************************************************/
static bool loadSoundFromFile(const char *basePath, const char *filename, SoundData *sound) {
    SDL_AudioSpec wavSpec;
    Uint8 *wavData;
    Uint32 wavLength;
    char fullPath[4096];

    sound->data = NULL;
    sound->size = 0;

    SDL_snprintf(fullPath, sizeof(fullPath), "%sdata/sounds/%s", basePath, filename);

    if (!SDL_LoadWAV(fullPath, &wavSpec, &wavData, &wavLength)) {
        return false;
    }

    /* Convert audio data to device format */
    if (!convertAudioData(wavData, wavLength, &wavSpec, &sound->data, &sound->size)) {
        /* If conversion fails, use data directly */
        sound->data = (Uint8 *)SDL_malloc(wavLength);
        if (!sound->data) {
            SDL_free(wavData);
            return false;
        }
        SDL_memcpy(sound->data, wavData, wavLength);
        sound->size = wavLength;
    }

    SDL_free(wavData);
    return true;
}

/*********************************************************
*NAME:          mixAudioCallback
*AUTHOR:        John Morrison
*CREATION DATE: 2024
*LAST MODIFIED: 2024
*PURPOSE:
*  Audio callback that mixes all active sound slots.
*
*ARGUMENTS:
*  userdata         - Unused
*  stream           - Audio output stream
*  additional_amount - Bytes requested
*  total_amount     - Total bytes in stream
*********************************************************/
static void SDLCALL mixAudioCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    int i;
    int bytes_to_mix;
    int sample_size;
    int samples_to_mix;
    Sint32 *mix_buffer;
    int j;
    Sint16 *src_sample;
    Sint16 *dst_sample;
    (void)userdata;
    (void)total_amount;

    if (additional_amount <= 0)
        return;

    /* Allocate a mixing buffer */
    mix_buffer = (Sint32 *)SDL_calloc(1, additional_amount * sizeof(Sint32));
    if (!mix_buffer)
        return;

    /* Lock the slots mutex */
    if (slotsMutex) {
        SDL_LockMutex(slotsMutex);
    }

    sample_size = 2 * deviceSpec.channels;  /* 2 bytes per sample * channels */
    samples_to_mix = additional_amount / sample_size;
    (void)samples_to_mix;

    /* Mix all active slots */
    for (i = 0; i < MAX_SOUND_SLOTS; i++) {
        if (!slots[i].active || !slots[i].data)
            continue;

        /* Calculate how many bytes we can mix from this slot */
        bytes_to_mix = slots[i].size - slots[i].pos;
        if (bytes_to_mix > additional_amount) {
            bytes_to_mix = additional_amount;
        }

        if (bytes_to_mix <= 0) {
            slots[i].active = false;
            continue;
        }

        /* Mix 16-bit samples into 32-bit buffer to avoid clipping */
        src_sample = (Sint16 *)(slots[i].data + slots[i].pos);
        for (j = 0; j < bytes_to_mix / 2; j++) {
            mix_buffer[j] += src_sample[j];
        }

        slots[i].pos += bytes_to_mix;

        /* Check if sound finished */
        if (slots[i].pos >= slots[i].size) {
            slots[i].active = false;
        }
    }

    if (slotsMutex) {
        SDL_UnlockMutex(slotsMutex);
    }

    /* Convert mix buffer back to 16-bit with clipping */
    Uint8 *output = (Uint8 *)SDL_malloc(additional_amount);
    if (output) {
        dst_sample = (Sint16 *)output;
        for (j = 0; j < additional_amount / 2; j++) {
            Sint32 sample = mix_buffer[j];
            /* Clip to 16-bit range */
            if (sample > 32767) sample = 32767;
            else if (sample < -32768) sample = -32768;
            dst_sample[j] = (Sint16)sample;
        }
        SDL_PutAudioStreamData(stream, output, additional_amount);
        SDL_free(output);
    }

    SDL_free(mix_buffer);
}

/*********************************************************
*NAME:          soundSetup
*AUTHOR:        John Morrison
*CREATION DATE: 26/10/98
*LAST MODIFIED: 2024
*PURPOSE:
*  Sets up sound systems, SDL3 audio structures etc.
*  Returns whether the operation was successful or not
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool soundSetup(void) {
    bool returnValue = TRUE;
    int i;

    isPlayable = FALSE;

    /* Initialize SDL audio subsystem */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, DIALOG_BOX_TITLE,
                                 "Error initializing SDL audio", NULL);
        return FALSE;
    }

    /* Open audio device with a reasonable default format */
    SDL_zero(deviceSpec);
    deviceSpec.format = SDL_AUDIO_S16;
    deviceSpec.channels = 2;
    deviceSpec.freq = 22050;

    /* Create mutex for sound slots */
    slotsMutex = SDL_CreateMutex();
    if (!slotsMutex) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, DIALOG_BOX_TITLE,
                                 "Error creating mutex", NULL);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return FALSE;
    }

    /* Initialize sound slots */
    for (i = 0; i < MAX_SOUND_SLOTS; i++) {
        slots[i].active = false;
        slots[i].data = NULL;
        slots[i].size = 0;
        slots[i].pos = 0;
    }

    /* Open audio stream with callback for mixing */
    audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec, mixAudioCallback, NULL);
    if (!audioStream) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, DIALOG_BOX_TITLE,
                                 "Error opening audio device", NULL);
        SDL_DestroyMutex(slotsMutex);
        slotsMutex = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return FALSE;
    }

    SDL_SetAudioStreamGain(audioStream, 0.25f);

    /* Load all sound effects from data/sounds/ relative to the executable */
    {
        const char *basePath = SDL_GetBasePath();
        if (!basePath) basePath = "./";

        SDL_zero(sounds);

        for (i = 0; i < NUM_SOUNDS; i++) {
            if (!loadSoundFromFile(basePath, soundFiles[i], &sounds[i])) {
                fprintf(stderr, "Sound error: %sdata/sounds/%s: %s\n",
                    basePath, soundFiles[i], SDL_GetError());
                fflush(stderr);
                returnValue = FALSE;
                break;
            }
        }
    }

    /* Create keepalive (silent audio for AV receivers) */
    if (returnValue) {
        /* Create 100ms of silence at the device sample rate */
        keepaliveSize = (deviceSpec.freq * deviceSpec.channels * 2) / 10; /* 2 bytes per sample */
        keepaliveData = (Uint8 *)SDL_calloc(1, keepaliveSize);
        if (keepaliveData) {
            keepaliveStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec, NULL, NULL);
            if (!keepaliveStream) {
                SDL_free(keepaliveData);
                keepaliveData = NULL;
            }
        }
    }

    if (returnValue) {
        isPlayable = TRUE;
        /* Resume the audio stream (SDL3 requires explicit resume) */
        SDL_ResumeAudioStreamDevice(audioStream);
    } else {
        /* Cleanup on failure */
        for (i = 0; i < NUM_SOUNDS; i++) {
            if (sounds[i].data) {
                SDL_free(sounds[i].data);
                sounds[i].data = NULL;
            }
        }
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = NULL;
        }
        if (slotsMutex) {
            SDL_DestroyMutex(slotsMutex);
            slotsMutex = NULL;
        }
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    return returnValue;
}

/*********************************************************
*NAME:          soundCleanup
*AUTHOR:        John Morrison
*CREATION DATE: 26/12/98
*LAST MODIFIED: 28/12/98
*PURPOSE:
*  Destroys and cleans up sound systems, SDL3 audio
*  structures etc.
*
*ARGUMENTS:
*
*********************************************************/
void soundCleanup(void) {
    int i;

    /* Free all sound data */
    for (i = 0; i < NUM_SOUNDS; i++) {
        if (sounds[i].data) {
            SDL_free(sounds[i].data);
            sounds[i].data = NULL;
            sounds[i].size = 0;
        }
    }

    /* Destroy audio stream first - this also closes the associated device */
    if (audioStream) {
        SDL_DestroyAudioStream(audioStream);
        audioStream = NULL;
    }

    /* Cleanup keepalive */
    if (keepaliveStream) {
        SDL_DestroyAudioStream(keepaliveStream);
        keepaliveStream = NULL;
    }
    if (keepaliveData) {
        SDL_free(keepaliveData);
        keepaliveData = NULL;
    }

    /* Destroy mutex */
    if (slotsMutex) {
        SDL_DestroyMutex(slotsMutex);
        slotsMutex = NULL;
    }

    isPlayable = FALSE;
}

/*********************************************************
*NAME:          playSound
*AUTHOR:        John Morrison
*CREATION DATE: 2024
*LAST MODIFIED: 2024
*PURPOSE:
*  Plays a sound from the sounds array by index.
*  Finds an available slot and starts playback.
*
*ARGUMENTS:
*  index - Index into the sounds array
*********************************************************/
static void playSound(int index) {
    int i;
    int slot_found = -1;

    if (!isPlayable || !audioStream || index < 0 || index >= NUM_SOUNDS)
        return;

    if (!sounds[index].data || sounds[index].size == 0)
        return;

    /* Lock mutex to access slots */
    if (slotsMutex) {
        SDL_LockMutex(slotsMutex);
    }

    /* Find an available slot (prefer inactive, otherwise reuse oldest) */
    for (i = 0; i < MAX_SOUND_SLOTS; i++) {
        if (!slots[i].active) {
            slot_found = i;
            break;
        }
    }

    /* If no inactive slot, reuse slot 0 */
    if (slot_found == -1) {
        slot_found = 0;
    }

    /* Start playback in the slot */
    slots[slot_found].data = sounds[index].data;
    slots[slot_found].size = sounds[index].size;
    slots[slot_found].pos = 0;
    slots[slot_found].active = true;

    if (slotsMutex) {
        SDL_UnlockMutex(slotsMutex);
    }
}

/*********************************************************
*NAME:          soundPlayEffect
*AUTHOR:        John Morrison
*CREATION DATE: 28/12/98
*LAST MODIFIED: 24/02/02
*PURPOSE:
*  Plays the correct Sound file
*
*ARGUMENTS:
*  value       - The sound file number to play
*********************************************************/
void soundPlayEffect(sndEffects value) {
    int index;

    switch (value) {
    case shootSelf:
        index = 6;  /* shooting_self */
        break;
    case shootNear:
        index = 7;  /* shooting_near */
        break;
    case shotTreeNear:
        index = 2;  /* shot_tree_near */
        break;
    case shotTreeFar:
        index = 3;  /* shot_tree_far */
        break;
    case shotBuildingNear:
        index = 4;  /* shot_building_near */
        break;
    case shotBuildingFar:
        index = 5;  /* shot_building_far */
        break;
    case hitTankFar:
        index = 18; /* hit_tank_far */
        break;
    case hitTankNear:
        index = 17; /* hit_tank_near */
        break;
    case hitTankSelf:
        index = 16; /* hit_tank_self */
        break;
    case bubbles:
        index = 21; /* bubbles */
        break;
    case tankSinkNear:
        index = 0;  /* tank_sinking_near */
        break;
    case tankSinkFar:
        index = 1;  /* tank_sinking_far */
        break;
    case bigExplosionNear:
        index = 22; /* big_explosion_near */
        break;
    case bigExplosionFar:
        index = 11; /* big_explosion_far */
        break;
    case farmingTreeNear:
        index = 19; /* farming_tree_near */
        break;
    case farmingTreeFar:
        index = 20; /* farming_tree_far */
        break;
    case manBuildingNear:
        index = 14; /* man_building_near */
        break;
    case manBuildingFar:
        index = 15; /* man_building_far */
        break;
    case manDyingNear:
        index = 12; /* man_dying_near */
        break;
    case manDyingFar:
        index = 13; /* man_dying_far */
        break;
    case manLayingMineNear:
        index = 23; /* man_lay_mine_near */
        break;
    case mineExplosionNear:
        index = 9;  /* mine_explosion_near */
        break;
    case mineExplosionFar:
        index = 10; /* mine_explosion_far */
        break;
    default:
        /* shootFar */
        index = 8;  /* shooting_far */
        break;
    }

    playSound(index);
}

/*********************************************************
*NAME:          soundKeepalive
*AUTHOR:        John Morrison
*CREATION DATE: 29/12/98
*LAST MODIFIED: 29/12/98
*PURPOSE:
*  Some AV receivers go to sleep if we don't output a
*  constant data stream, especially with Spatial Audio.
*
*ARGUMENTS:
*  value - TRUE to turn on FALSE to turn off.
*********************************************************/
void soundKeepalive(bool value) {
    if (!keepaliveStream || !keepaliveData)
        return;

    if (value == TRUE) {
        SDL_PutAudioStreamData(keepaliveStream, keepaliveData, keepaliveSize);
        SDL_ResumeAudioStreamDevice(keepaliveStream);
    } else {
        SDL_PauseAudioStreamDevice(keepaliveStream);
        SDL_ClearAudioStream(keepaliveStream);
    }
}

/*********************************************************
*NAME:          soundIsPlayable
*AUTHOR:        John Morrison
*CREATION DATE: 13/6/00
*LAST MODIFIED: 13/6/00
*PURPOSE:
*  Returns whether the sound system is enabled or not. By
*  enabled I mean an error hasn't stopped us from starting
*  it
*
*ARGUMENTS:
*
*********************************************************/
bool soundIsPlayable(void) {
    return isPlayable;
}

/*********************************************************
*NAME:          soundSetMuted
*PURPOSE:
*  Pauses or resumes the main audio stream.  Used to
*  silence sound effects when the window loses focus
*  and "Background Sound" is disabled.
*
*ARGUMENTS:
*  mute - TRUE to pause, FALSE to resume
*********************************************************/
void soundSetMuted(bool mute) {
    int i;
    if (!isPlayable || !audioStream)
        return;
    if (mute) {
        SDL_PauseAudioStreamDevice(audioStream);
    } else {
        /* Clear any stale sounds that were queued while muted */
        if (slotsMutex) {
            SDL_LockMutex(slotsMutex);
        }
        for (i = 0; i < MAX_SOUND_SLOTS; i++) {
            slots[i].active = false;
        }
        if (slotsMutex) {
            SDL_UnlockMutex(slotsMutex);
        }
        SDL_ResumeAudioStreamDevice(audioStream);
    }
}
