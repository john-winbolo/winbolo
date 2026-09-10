/*
 * $Id$
 *
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
*Name:          Sound
*Filename:      sound.c
*Author:        John Morrison
*Creation Date: 26/12/98
*Last Modified: 24/02/02
*Purpose:
*  System Specific Sound Playing routines
*  (Uses SDL3)
*********************************************************/

#include "global.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include "client_enums.h"  /* sndEffects */
#include "../sound.h"
#include "../lang.h"
#include "skin_source.h"
#include "sound_variants.h"
#include "../ping_sounds.h"
#include "../../common/wb_log.h"

#define NUM_SOUNDS 38
#define MAX_SOUND_SLOTS 16  /* Maximum simultaneous sounds */
#define RESERVED_SHOOT_SELF_SLOT 0  /* Slot 0 reserved for player shooting */
#define SHOOT_SELF_INDEX 6  /* Index of shooting_self.wav in sounds[] */

/* The ping sounds sit at the end of soundFiles[] in one run: the default
 * first, then one per PING_KIND_* in kind order, which is what lets the mask
 * pingSoundResolve reads be built with a loop rather than a table. */
#define PING_DEFAULT_INDEX 31
#define PING_KIND_FIRST_INDEX 32

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
    int sound;          /* Index into sounds[] being played, -1 for none */
    bool active;        /* Is this slot playing? */
    /* How loud this one sound is against the others, 1 for as authored. Set
     * when the slot is started and read by the mixer, so it is a property of
     * the play rather than of the sample: the same converted data is shared by
     * every slot that plays it. The pings are the only effect that uses it
     * (PING_SOUND_GAIN); everything else runs at 1 and is mixed untouched. */
    float gain;
} SoundSlot;

/* Global sound state */
static bool isPlayable = FALSE;
static bool s_muted = FALSE;   /* logical mute state, tracked across (un)playable */
static SDL_AudioStream *audioStream = NULL;
static SDL_AudioSpec deviceSpec;

/* The two settings the stream gain is made of, and the one place the gain is
 * worked out from them.  Opening the device and changing either setting both
 * go through applyStreamGain, so neither can be applied without the other.
 * Seeded with the same defaults the frontend globals carry, for the case where
 * the device opens before the preferences have been read. */
static int masterVolumePct = 100;
static int effectsVolumePct = 50;

static void applyStreamGain(void) {
    if (audioStream) {
        SDL_SetAudioStreamGain(
            audioStream, (float)(masterVolumePct * effectsVolumePct) / 10000.0f);
    }
}

/* Sound effect data (pre-loaded and converted).
 * Each sound's pool. Members 0 .. variantCount-1 are loaded and playable;
 * a member that would not decode never takes a position, so there are no
 * gaps to land on. lastPlayed keeps the previous pick out of the next one. */
static SoundData     sounds[NUM_SOUNDS][SOUND_VARIANT_MAX];
static unsigned char variantCount[NUM_SOUNDS];
static unsigned char lastPlayed[NUM_SOUNDS];

/* Seeded on first use. The picks are cosmetic and per client, so this is
 * deliberately not the Bolo RNG: that one is the sim's and is pinned
 * deterministic by its own test. playSound holds slotsMutex, which is what
 * makes this state safe to touch from the timer thread. */
static Uint64 variantSeed = 0;

/* Active sound slots for mixing.  The mutex is created by the first
 * soundSetup and then kept for the life of the process: soundCleanup runs
 * on the main thread for a skin reload while the game timer thread is still
 * ticking the sim, and that thread reaches playSound through
 * frontEndPlaySound.  A mutex that is destroyed and recreated across the
 * reload leaves that caller a window in which it locks a dead one.  Keeping
 * it means the reload and the tick serialise on it instead, and the small
 * price is one mutex that outlives the last soundCleanup at exit. */
static SoundSlot slots[MAX_SOUND_SLOTS];
static SDL_Mutex *slotsMutex = NULL;

/* Keepalive data */
static SDL_AudioStream *keepaliveStream = NULL;
static Uint8 *keepaliveData = NULL;
static Uint32 keepaliveSize = 0;

#include "dialogs/imgui_messagebox.h"

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
    "lobby_chat.wav",          /* 24 */
    "lobby_ready.wav",         /* 25 */
    "lobby_unready.wav",       /* 26 */
    "lobby_countdown.wav",     /* 27 */
    "lobby_game_start.wav",    /* 28 */
    "lobby_player_join.wav",   /* 29 */
    "lobby_player_leave.wav",  /* 30 */
    /* Smart pings. Named after the same keys the icons use (data/ui/ping/
     * <kind>.svg), so the sound and the picture for a kind are found by the
     * same word. Only the default and the caution ping ship with the game;
     * the other five are looked for all the same, so dropping one in — here
     * or in a skin — is all it takes to give that kind its own sound.
     * PING_DEFAULT_INDEX / PING_KIND_FIRST_INDEX name where this run starts;
     * the six after the default are in PING_KIND_* order. */
    "ping_default.wav",        /* 31 */
    "ping_standard.wav",       /* 32 */
    "ping_caution.wav",        /* 33 */
    "ping_assist.wav",         /* 34 */
    "ping_attack.wav",         /* 35 */
    "ping_onmyway.wav",        /* 36 */
    "ping_botcommand.wav",     /* 37 */
};
BOLO_STATIC_ASSERT(PING_KIND_FIRST_INDEX + PING_KIND_COUNT == NUM_SOUNDS,
                   every_ping_kind_needs_a_sound_file_name);

/* The per-kind ping sounds, and only those: a game or a skin that does not
 * hold one is the normal case, not a fault, so a file missing here is not
 * warned about and is not counted among the members that would not decode.
 * ping_default is not one of them — it is what the others fall back to, and
 * a game missing it is worth the usual warning. */
static bool soundIsOptional(int index) {
    return index >= PING_KIND_FIRST_INDEX && index < NUM_SOUNDS;
}

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
*  true if the conversion produced audio.  false leaves
*  *dstData NULL and *dstSize 0, with nothing to free.
*********************************************************/
static bool convertAudioData(const Uint8 *srcData, Uint32 srcSize,
                             const SDL_AudioSpec *srcSpec,
                             Uint8 **dstData, Uint32 *dstSize) {
    int dstLen = 0;
    if (!SDL_ConvertAudioSamples(srcSpec, srcData, (int)srcSize,
                                  &deviceSpec, dstData, &dstLen)) {
        return false;
    }
    if (dstLen <= 0) {
        /* A conversion that succeeds with nothing in it still hands back a
         * buffer to free.  The caller reads false as "no audio came out" and
         * allocates its own, so this one has to go here or it is lost. */
        SDL_free(*dstData);
        *dstData = NULL;
        *dstSize = 0;
        return false;
    }
    *dstSize = (Uint32)dstLen;
    return true;
}

/*********************************************************
*NAME:          loadWavFromSkin
*PURPOSE:
*  Reads one WAV out of a skin and decodes it. Leaves the
*  outputs untouched when the skin does not hold relName.
*
*ARGUMENTS:
*  src       - Skin to read from
*  relName   - Name of the WAV inside the skin
*  spec      - Filled with the WAV's format
*  data      - Filled with the WAV bytes (SDL_free by caller)
*  length    - Filled with the byte count
*
*RETURNS:
*  true if the skin held relName and it decoded
*********************************************************/
static bool loadWavFromSkin(SkinSource *src, const char *relName,
                            SDL_AudioSpec *spec, Uint8 **data, Uint32 *length) {
    void  *buf = NULL;
    size_t len = 0;

    if (!skinSourceRead(src, relName, &buf, &len)) {
        return false;
    }
    /* closeio closes the stream, not the bytes behind it. */
    bool ok = SDL_LoadWAV_IO(SDL_IOFromMem(buf, len), true, spec, data, length);
    SDL_free(buf);
    return ok;
}

/*********************************************************
*NAME:          storeSoundData
*PURPOSE:
*  Converts one decoded WAV into the device format and
*  keeps it as a pool member.  The WAV bytes are freed
*  either way.  A conversion that fails is not fatal: the
*  data is used as it came, which is what one file did
*  before there were pools.
*
*ARGUMENTS:
*  sound     - Pool member to fill
*  spec      - Format the WAV bytes are in
*  wavData   - The WAV bytes, freed here
*  wavLength - Byte count
*
*RETURNS:
*  true if the member holds audio.  false if the fallback
*  allocation failed, or if there is no audio to keep, and
*  then sound->data is NULL and sound->size 0
*********************************************************/
static bool storeSoundData(SoundData *sound, const SDL_AudioSpec *spec,
                           Uint8 *wavData, Uint32 wavLength) {
    sound->data = NULL;
    sound->size = 0;

    /* Convert audio data to device format */
    if (!convertAudioData(wavData, wavLength, spec, &sound->data, &sound->size)) {
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

    /* Nothing came out of the conversion and the WAV was empty too.  Kept,
     * this member would take a position in the pool and give the sound a
     * silent trigger every time the pick landed on it. */
    if (sound->size == 0) {
        SDL_free(sound->data);
        sound->data = NULL;
        return false;
    }

    return true;
}

/* What the pool callbacks need to reach one sound's members. */
typedef struct {
    SkinSource *skin;      /* the active skin, for the skin adapters */
    const char *basePath;  /* for the built-in adapters */
    const char *baseName;  /* the plain name, so the built-in adapter can
                              answer by name shape and open nothing */
    int         index;     /* which row of sounds[] is being filled */
    int         failures;  /* members that would not decode */
    bool        optional;  /* the game need not hold this one; see
                              soundIsOptional */
} SoundPoolCtx;

/*********************************************************
*NAME:          skinHasMember
*PURPOSE:
*  Whether the active skin holds one pool member, under
*  sounds/ or at its top level for the flat 1.x layout.
*  A hash lookup either way, so asking for all eleven
*  members of a pool costs no I/O.
*
*ARGUMENTS:
*  ctx     - SoundPoolCtx for the sound being filled
*  relName - Bare member name, carrying its .wav
*
*RETURNS:
*  true if the skin holds it under either name
*********************************************************/
static bool skinHasMember(void *ctx, const char *relName) {
    SoundPoolCtx *c = (SoundPoolCtx *)ctx;
    char fullPath[4096];

    SDL_snprintf(fullPath, sizeof(fullPath), "sounds/%s", relName);
    return skinSourceExists(c->skin, fullPath) ||
           skinSourceExists(c->skin, relName);
}

/*********************************************************
*NAME:          skinLoadMember
*PURPOSE:
*  Decodes one member out of the active skin into its pool
*  position, trying sounds/<name> then the top level.
*
*ARGUMENTS:
*  ctx     - SoundPoolCtx for the sound being filled
*  relName - Bare member name, carrying its .wav
*  slot    - Pool position to fill
*
*RETURNS:
*  true if the member decoded
*********************************************************/
static bool skinLoadMember(void *ctx, const char *relName, int slot) {
    SoundPoolCtx *c = (SoundPoolCtx *)ctx;
    SDL_AudioSpec wavSpec;
    Uint8 *wavData = NULL;
    Uint32 wavLength = 0;
    char fullPath[4096];
    bool loaded;

    SDL_snprintf(fullPath, sizeof(fullPath), "sounds/%s", relName);
    loaded = loadWavFromSkin(c->skin, fullPath, &wavSpec, &wavData, &wavLength);
    if (!loaded) {
        loaded = loadWavFromSkin(c->skin, relName, &wavSpec, &wavData, &wavLength);
    }
    if (loaded) {
        loaded = storeSoundData(&sounds[c->index][slot], &wavSpec, wavData,
                                wavLength);
    }
    if (!loaded) {
        c->failures++;
        WB_LOG_WARN(WB_LOG_CAT_AUDIO, "soundSetup: skin sound %s: %s",
                    relName, SDL_GetError());
        return false;
    }
    return true;
}

/*********************************************************
*NAME:          builtinHasMember
*PURPOSE:
*  Whether the built-in set holds one pool member.  The
*  built-in set is one file per sound, so only the plain
*  name is ever a member and no _N name is looked for on
*  disk.  This answers by name shape and opens nothing:
*  answering honestly would mean up to ten failed file
*  opens per sound, 310 at startup with no skin active,
*  which is cheap on a desktop and not on a phone reading
*  through an APK asset reader.
*
*ARGUMENTS:
*  ctx     - SoundPoolCtx for the sound being filled
*  relName - Bare member name, carrying its .wav
*
*RETURNS:
*  true only for the sound's plain name
*********************************************************/
static bool builtinHasMember(void *ctx, const char *relName) {
    SoundPoolCtx *c = (SoundPoolCtx *)ctx;

    return SDL_strcmp(relName, c->baseName) == 0;
}

/*********************************************************
*NAME:          builtinLoadMember
*PURPOSE:
*  Decodes one member out of data/sounds/ beside the
*  executable into its pool position.
*
*ARGUMENTS:
*  ctx     - SoundPoolCtx for the sound being filled
*  relName - Bare member name, carrying its .wav
*  slot    - Pool position to fill
*
*RETURNS:
*  true if the member decoded
*********************************************************/
static bool builtinLoadMember(void *ctx, const char *relName, int slot) {
    SoundPoolCtx *c = (SoundPoolCtx *)ctx;
    SDL_AudioSpec wavSpec;
    Uint8 *wavData = NULL;
    Uint32 wavLength = 0;
    char fullPath[4096];

    SDL_snprintf(fullPath, sizeof(fullPath), "%sdata/sounds/%s", c->basePath,
                 relName);
    if (!SDL_LoadWAV(fullPath, &wavSpec, &wavData, &wavLength) ||
        !storeSoundData(&sounds[c->index][slot], &wavSpec, wavData, wavLength)) {
        /* builtinHasMember answers by name shape, so this is also the path a
         * file the game simply does not ship takes. For an optional sound
         * that is the expected outcome: soundSetup says so once, in its own
         * line, and it is not one of the members that would not decode. */
        if (!c->optional) {
            c->failures++;
            WB_LOG_WARN(WB_LOG_CAT_AUDIO, "soundSetup: %s: %s", fullPath,
                        SDL_GetError());
        }
        return false;
    }
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

        /* Mix 16-bit samples into 32-bit buffer to avoid clipping.
         *
         * The slot's own gain rides on top of the stream gain the master and
         * effects settings make (applyStreamGain), so the two multiply: a ping
         * is PING_SOUND_GAIN of whatever the player has the effects turned up
         * to. Every other effect has a gain of 1 and takes the plain loop, so
         * nothing but the pings is touched — by the arithmetic as well as by
         * the loudness. */
        src_sample = (Sint16 *)(slots[i].data + slots[i].pos);
        if (slots[i].gain >= 1.0f) {
            for (j = 0; j < bytes_to_mix / 2; j++) {
                mix_buffer[j] += src_sample[j];
            }
        } else {
            for (j = 0; j < bytes_to_mix / 2; j++) {
                mix_buffer[j] += (Sint32)((float)src_sample[j] * slots[i].gain);
            }
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
    int v;

    isPlayable = FALSE;

    /* Initialize SDL audio subsystem */
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        imguiMessageBoxEx(DIALOG_BOX_TITLE, "Error initializing SDL audio",
                          IMGUI_MSG_WARNING, IMGUI_MSG_OK);
        return FALSE;
    }

    /* Open audio device with a reasonable default format */
    SDL_zero(deviceSpec);
    deviceSpec.format = SDL_AUDIO_S16;
    deviceSpec.channels = 2;
    deviceSpec.freq = 22050;

    /* The slots mutex is made once and kept across reloads; see its
     * declaration for why. */
    if (!slotsMutex) {
        slotsMutex = SDL_CreateMutex();
        if (!slotsMutex) {
            imguiMessageBoxEx(DIALOG_BOX_TITLE, "Error creating mutex",
                              IMGUI_MSG_WARNING, IMGUI_MSG_OK);
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return FALSE;
        }
    }

    /* Initialize sound slots */
    SDL_LockMutex(slotsMutex);
    for (i = 0; i < MAX_SOUND_SLOTS; i++) {
        slots[i].active = false;
        slots[i].data = NULL;
        slots[i].size = 0;
        slots[i].pos = 0;
        slots[i].sound = -1;
        slots[i].gain = 1.0f;
    }
    SDL_UnlockMutex(slotsMutex);

    /* Open audio stream with callback for mixing */
    audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &deviceSpec, mixAudioCallback, NULL);
    if (!audioStream) {
        imguiMessageBoxEx(DIALOG_BOX_TITLE, "Error opening audio device",
                          IMGUI_MSG_WARNING, IMGUI_MSG_OK);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return FALSE;
    }

    /* Apply the current volumes (loaded from prefs, or the defaults above if
     * the device opened first). */
    applyStreamGain();

    /* Load every sound effect's pool: the active skin's members when it holds
     * any that decode, otherwise the one file under data/sounds/ relative to
     * the executable.  A member that will not decode is dropped and the rest
     * of its pool still plays; a sound with an empty pool is simply silent —
     * playSound skips it — so one unreadable WAV costs one effect rather than
     * the whole sound system.  An empty set is different: that is
     * data/sounds/ missing or unreadable as a whole, and it takes the failure
     * path below. */
    {
        const char *basePath = SDL_GetBasePath();
        SkinSource *skin = skinGetActiveSource();
        const char *skinLabel = skinGetActive();
        int loadedCount = 0;
        int fromSkin = 0;
        int withVariants = 0;
        int maxVariants = 0;
        int membersUnreadable = 0;
        int fellBack = 0;
        int optionalMissing = 0;
        if (!basePath) basePath = "./";
        if (skinLabel == NULL || skinLabel[0] == '\0') skinLabel = "none";

        SDL_zero(sounds);
        SDL_zero(variantCount);
        SDL_zero(lastPlayed);

        for (i = 0; i < NUM_SOUNDS; i++) {
            SoundPoolCtx ctx;
            int n = 0;

            ctx.skin = skin;
            ctx.basePath = basePath;
            ctx.baseName = soundFiles[i];
            ctx.index = i;
            ctx.failures = 0;
            ctx.optional = soundIsOptional(i);

            if (skin != NULL) {
                n = soundVariantLoad(soundFiles[i], skinHasMember,
                                     skinLoadMember, &ctx);
            }
            membersUnreadable += ctx.failures;
            if (n > 0) {
                fromSkin++;
            } else {
                /* A skin that held names none of which decoded falls back to
                   the game's own sound rather than to silence, which is what
                   one file did before there were pools.  A skin that simply
                   does not replace this sound is not a fallback. */
                if (ctx.failures > 0) fellBack++;
                ctx.failures = 0;
                n = soundVariantLoad(soundFiles[i], builtinHasMember,
                                     builtinLoadMember, &ctx);
                membersUnreadable += ctx.failures;
            }
            variantCount[i] = (unsigned char)n;

            /* Said once here, at load, rather than every time that kind is
             * pinged. The count goes in the summary line below so a reader
             * knows how many of these to expect. */
            if (n == 0 && ctx.optional) {
                optionalMissing++;
                WB_LOG_INFO(WB_LOG_CAT_AUDIO,
                            "soundSetup: no %s in the game data or the skin; that ping kind plays %s",
                            soundFiles[i], soundFiles[PING_DEFAULT_INDEX]);
            }

            if (n > 0) loadedCount++;
            if (n > 1) withVariants++;
            if (n > maxVariants) maxVariants = n;
        }

        WB_LOG_INFO(WB_LOG_CAT_AUDIO,
                    "soundSetup: %d effects, %d from skin=%s, %d with variants (max %d), %d members unreadable, %d fell back to the built-in, %d ping kinds on the default sound",
                    loadedCount, fromSkin, skinLabel, withVariants, maxVariants,
                    membersUnreadable, fellBack, optionalMissing);

        if (loadedCount == 0) {
            imguiMessageBoxEx(DIALOG_BOX_TITLE, langGetText(STR_SOUND_LOAD_FAILED),
                              IMGUI_MSG_WARNING, IMGUI_MSG_OK);
            returnValue = FALSE;
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
        /* Under the mutex so a playSound on another thread sees the
         * samples above before it sees the flag. */
        SDL_LockMutex(slotsMutex);
        isPlayable = TRUE;
        SDL_UnlockMutex(slotsMutex);
        /* Resume the audio stream (SDL3 requires explicit resume) */
        SDL_ResumeAudioStreamDevice(audioStream);
    } else {
        /* Cleanup on failure.  The mutex stays: nothing here made it
         * playable, and the next soundSetup reuses it. */
        for (i = 0; i < NUM_SOUNDS; i++) {
            for (v = 0; v < SOUND_VARIANT_MAX; v++) {
                if (sounds[i][v].data) {
                    SDL_free(sounds[i][v].data);
                    sounds[i][v].data = NULL;
                }
            }
        }
        if (audioStream) {
            SDL_DestroyAudioStream(audioStream);
            audioStream = NULL;
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
    int v;

    /* Drop the flag under the mutex.  playSound on the timer thread checks
     * it under the same mutex, so any call that gets the lock from here on
     * leaves without touching a sample, and any call that got the lock
     * first has finished with the samples before this proceeds.  The mutex
     * is released again before the stream goes: destroying the stream joins
     * the mixing callback, and that callback takes this mutex, so holding
     * it here would deadlock. */
    if (slotsMutex) SDL_LockMutex(slotsMutex);
    isPlayable = FALSE;
    if (slotsMutex) SDL_UnlockMutex(slotsMutex);

    /* Destroy the audio stream first - this stops and joins the mixing
     * callback thread. It must happen before we free anything the callback
     * reads (the sound data borrowed by active slots), otherwise the still
     * running callback can dereference freed memory. */
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

    /* Now that no callback can run, free all sound data and clear the
     * slots that borrowed it, so nothing points at a freed sample if a
     * later soundSetup fails partway. */
    if (slotsMutex) SDL_LockMutex(slotsMutex);
    for (i = 0; i < NUM_SOUNDS; i++) {
        for (v = 0; v < SOUND_VARIANT_MAX; v++) {
            if (sounds[i][v].data) {
                SDL_free(sounds[i][v].data);
                sounds[i][v].data = NULL;
                sounds[i][v].size = 0;
            }
        }
    }
    SDL_zero(variantCount);
    SDL_zero(lastPlayed);
    for (i = 0; i < MAX_SOUND_SLOTS; i++) {
        slots[i].active = false;
        slots[i].data = NULL;
        slots[i].size = 0;
        slots[i].pos = 0;
        slots[i].sound = -1;
        slots[i].gain = 1.0f;
    }
    if (slotsMutex) SDL_UnlockMutex(slotsMutex);

    /* The mutex is kept; see its declaration. */
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
*  gain  - how loud this play is against the other effects,
*          1 for as authored (see SoundSlot::gain)
*********************************************************/
static void playSound(int index, float gain) {
    int i;
    int slot_found = -1;
    int search_start;
    Uint32 most_progress;
    int evict_slot;
    int pick;

    if (index < 0 || index >= NUM_SOUNDS || !slotsMutex)
        return;

    /* Lock before reading anything soundCleanup tears down.  It drops
     * isPlayable under this mutex before freeing a sample, so a call that
     * gets the lock after that sees the flag and leaves. */
    SDL_LockMutex(slotsMutex);

    if (!isPlayable || !audioStream || variantCount[index] == 0) {
        SDL_UnlockMutex(slotsMutex);
        return;
    }

    /* Deduplicate: skip if this sound was already triggered this tick
     * (pos == 0 means it was just started and hasn't been mixed yet) */
    for (i = 0; i < MAX_SOUND_SLOTS; i++) {
        if (slots[i].active && slots[i].sound == index && slots[i].pos == 0) {
            if (slotsMutex) {
                SDL_UnlockMutex(slotsMutex);
            }
            return;
        }
    }

    /* Shoot-self gets a reserved slot so it never gets evicted */
    if (index == SHOOT_SELF_INDEX) {
        slot_found = RESERVED_SHOOT_SELF_SLOT;
    } else {
        /* Search non-reserved slots for an inactive one */
        search_start = RESERVED_SHOOT_SELF_SLOT + 1;
        for (i = search_start; i < MAX_SOUND_SLOTS; i++) {
            if (!slots[i].active) {
                slot_found = i;
                break;
            }
        }

        /* If no inactive slot, evict the one closest to finishing */
        if (slot_found == -1) {
            most_progress = 0;
            evict_slot = search_start;
            for (i = search_start; i < MAX_SOUND_SLOTS; i++) {
                if (slots[i].pos > most_progress) {
                    most_progress = slots[i].pos;
                    evict_slot = i;
                }
            }
            slot_found = evict_slot;
        }
    }

    /* Choose which member of the pool plays.  This sits below the duplicate
     * check on purpose: a trigger that check suppresses must not advance the
     * rotation, or the rotation is driven by events nobody heard. */
    pick = 0;
    if (variantCount[index] > 1) {
        if (variantSeed == 0) {
            variantSeed = SDL_GetPerformanceCounter();
            if (variantSeed == 0) variantSeed = 1;
        }
        /* Draw from the members other than the one that played last, so a
         * pool of three cannot repeat a third of the time and a pool of two
         * alternates. */
        pick = SDL_rand_r(&variantSeed, variantCount[index] - 1);
        if (pick >= lastPlayed[index]) pick++;
        lastPlayed[index] = (unsigned char)pick;
    }

    /* Start playback in the slot */
    slots[slot_found].data = sounds[index][pick].data;
    slots[slot_found].size = sounds[index][pick].size;
    slots[slot_found].pos = 0;
    slots[slot_found].sound = index;
    slots[slot_found].gain = gain;
    slots[slot_found].active = true;

    if (slotsMutex) {
        SDL_UnlockMutex(slotsMutex);
    }
}

/*********************************************************
*NAME:          soundPingFoundMask
*PURPOSE:
*  Which ping kinds have a sound of their own to play: bit
*  PING_KIND_x set when that kind's pool holds at least one
*  member, whether it came from the game's data or from the
*  active skin.  That is the loader's own record of a file
*  that resolved — an empty or undecodable file leaves the
*  pool at zero the same way a missing one does — and it is
*  all pingSoundResolve needs to make the fallback decision.
*
*  Read under the slots mutex, like every other reader of
*  variantCount, so a skin reload cannot be seen half done.
*
*ARGUMENTS:
*  (none)
*********************************************************/
static unsigned int soundPingFoundMask(void) {
    unsigned int mask = 0;
    int k;

    if (!slotsMutex) return 0;

    SDL_LockMutex(slotsMutex);
    for (k = 0; k < PING_KIND_COUNT; k++) {
        if (variantCount[PING_KIND_FIRST_INDEX + k] > 0) {
            mask |= 1u << k;
        }
    }
    SDL_UnlockMutex(slotsMutex);

    return mask;
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
    float gain;
    unsigned char pingKind = pingSoundKindOf(value);

    /* A ping kind with no sound file of its own plays the default ping
     * instead.  The rule is pingSoundResolve's and lives nowhere else; this
     * only hands it what actually loaded. */
    if (pingKind < PING_KIND_COUNT) {
        value = pingSoundResolve(pingKind, soundPingFoundMask());
    }

    /* The pings are mixed PING_SOUND_GAIN down and nothing else is. Asked
     * after the fallback, so a kind that fell back to ping_default is quieter
     * too — pingSoundKindOf does not answer for the default, which is why it
     * is named here as well. */
    gain = (value == pingDefault || pingSoundKindOf(value) < PING_KIND_COUNT)
         ? PING_SOUND_GAIN : 1.0f;

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
    case lobbyChatReceived:
        index = 24; /* lobby_chat */
        break;
    case lobbyReady:
        index = 25; /* lobby_ready */
        break;
    case lobbyUnready:
        index = 26; /* lobby_unready */
        break;
    case lobbyCountdown:
        index = 27; /* lobby_countdown */
        break;
    case lobbyGameStart:
        index = 28; /* lobby_game_start */
        break;
    case lobbyPlayerJoin:
        index = 29; /* lobby_player_join */
        break;
    case lobbyPlayerLeave:
        index = 30; /* lobby_player_leave */
        break;
    /* The ping sounds. Everything the fallback could turn into arrives here,
     * so each kind needs its own arm as well as the default. */
    case pingDefault:
        index = PING_DEFAULT_INDEX;
        break;
    case pingStandard:
        index = PING_KIND_FIRST_INDEX + PING_KIND_STANDARD;
        break;
    case pingCaution:
        index = PING_KIND_FIRST_INDEX + PING_KIND_CAUTION;
        break;
    case pingAssist:
        index = PING_KIND_FIRST_INDEX + PING_KIND_ASSIST;
        break;
    case pingAttack:
        index = PING_KIND_FIRST_INDEX + PING_KIND_ATTACK;
        break;
    case pingOnMyWay:
        index = PING_KIND_FIRST_INDEX + PING_KIND_ON_MY_WAY;
        break;
    case pingBotCommand:
        index = PING_KIND_FIRST_INDEX + PING_KIND_BOT_COMMAND;
        break;
    default:
        /* shootFar */
        index = 8;  /* shooting_far */
        break;
    }

    playSound(index, gain);
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
    /* Track the logical mute state even when audio isn't playable, so
       soundIsMuted() reflects intent for save/restore callers. */
    s_muted = mute;
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
            slots[i].sound = -1;
        }
        if (slotsMutex) {
            SDL_UnlockMutex(slotsMutex);
        }
        SDL_ResumeAudioStreamDevice(audioStream);
    }
}

bool soundIsMuted(void) {
    return s_muted;
}

void soundSetMasterVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    masterVolumePct = pct;
    applyStreamGain();
}

void soundSetEffectsVolume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    effectsVolumePct = pct;
    applyStreamGain();
}

void soundSetReturningToLobby(bool active) {
    static float savedGain = 1.0f;
    static bool muteActive = false;
    if (!isPlayable || !audioStream) return;
    if (active && !muteActive) {
        savedGain = SDL_GetAudioStreamGain(audioStream);
        SDL_SetAudioStreamGain(audioStream, 0.0f);
        muteActive = true;
    } else if (!active && muteActive) {
        SDL_SetAudioStreamGain(audioStream, savedGain);
        muteActive = false;
    }
}
