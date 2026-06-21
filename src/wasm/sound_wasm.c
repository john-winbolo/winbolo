/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * sound_wasm.c — WebAudio-backed sound implementation for the WASM build.
 *
 * Replaces gui/sdl3/sound.c on the WASM target.  Instead of running an
 * SDL audio mixing callback on the (single-threaded) main thread, we let
 * the browser do all the heavy lifting on its dedicated audio thread:
 *
 *   1. soundSetup() reads each WAV from MEMFS (already preloaded via
 *      --preload-file) and hands the raw bytes to AudioContext
 *      .decodeAudioData(), which produces an AudioBuffer that lives in
 *      the browser's audio backend, NOT in our WASM heap or JS heap.
 *
 *   2. soundPlayEffect() creates a one-shot AudioBufferSourceNode and
 *      starts it.  Source nodes are cheap; the browser handles polyphony
 *      and mixing on its audio thread, completely independent of our
 *      render/sim loop.
 *
 *   3. There is no per-frame audio callback in C, no SDL_PutAudioStreamData,
 *      no malloc/free churn, no main-thread mixing work.
 *
 * This dramatically reduces the periodic stutter the SDL3 path caused on
 * Emscripten (audio callback running on the main thread + per-callback
 * SDL_calloc/SDL_malloc) and brings memory back down (decoded PCM lives
 * in the browser, not as ~80 MB of WebAudio AudioBuffer copies).
 */

#include "global.h"
#include "client_enums.h"  /* sndEffects */
#include "../gui/sound.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>

/* Order matches gui/sdl3/sound.c so existing soundPlayEffect() index
 * mappings still resolve to the right WAV file. */
#define WB_NUM_SOUNDS 24

static const char *kSoundFiles[WB_NUM_SOUNDS] = {
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

static bool s_isPlayable = FALSE;

/* Mirrors the last value handed to wb_audio_set_gain. WebAudio doesn't
 * expose a read path back to C, so we shadow it here for the
 * returning-to-lobby save/restore. */
static double s_currentGain = 0.5;

/* -------------------------------------------------------
 * JS-side helpers, defined inline via EM_JS.
 *
 * We attach state to a single Module.WB_audio object so it survives across
 * EM_JS calls without needing globals on `window`.
 * ------------------------------------------------------- */

EM_JS(void, wb_audio_init, (void), {
  if (Module.WB_audio) return;

  /* Use a webkit fallback for older Safari, but standard AudioContext on
     everything modern. */
  var Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) {
    console.warn("[WB_audio] AudioContext not supported");
    Module.WB_audio = { ctx: null, buffers: [], gainNode: null };
    return;
  }

  var ctx = new Ctx();
  var gain = ctx.createGain();
  /* Default to 50%. soundSetVolume() overrides once the saved preference
     has been read. */
  gain.gain.value = 0.5;
  gain.connect(ctx.destination);

  Module.WB_audio = {
    ctx: ctx,
    gainNode: gain,
    buffers: [],
    muted: false,
  };

  /* Browsers require AudioContext to start in response to a user gesture.
     Wire one-shot listeners that resume the context on the first click,
     keydown, or touchstart, then remove themselves. */
  var resumeOnGesture = function() {
    if (Module.WB_audio.ctx && Module.WB_audio.ctx.state === "suspended") {
      Module.WB_audio.ctx.resume();
    }
    window.removeEventListener("pointerdown", resumeOnGesture, true);
    window.removeEventListener("keydown", resumeOnGesture, true);
    window.removeEventListener("touchstart", resumeOnGesture, true);
  };
  window.addEventListener("pointerdown", resumeOnGesture, true);
  window.addEventListener("keydown", resumeOnGesture, true);
  window.addEventListener("touchstart", resumeOnGesture, true);
});

/* Returns 1 on success, 0 on failure. Decoding is async, so the slot may be
 * empty for a tick or two after this returns; soundPlayEffect handles that
 * gracefully. */
EM_JS(int, wb_audio_load, (int id, const char *path), {
  if (!Module.WB_audio || !Module.WB_audio.ctx) return 0;
  var p = UTF8ToString(path);
  try {
    var bytes = FS.readFile(p);  /* Uint8Array from MEMFS */
    /* decodeAudioData wants a standalone ArrayBuffer; slice() copies the
       region out of the larger MEMFS-backed buffer. */
    var ab = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    Module.WB_audio.ctx.decodeAudioData(
      ab,
      function(buf) { Module.WB_audio.buffers[id] = buf; },
      function(err) { console.warn("[WB_audio] decode failed for " + p + ":", err); }
    );
    return 1;
  } catch (e) {
    console.warn("[WB_audio] load failed for " + p + ":", e);
    return 0;
  }
});

EM_JS(void, wb_audio_play, (int id), {
  if (!Module.WB_audio || !Module.WB_audio.ctx) return;
  var buf = Module.WB_audio.buffers[id];
  if (!buf) return;  /* Either out of range or still decoding. */
  if (Module.WB_audio.muted) return;
  /* AudioBufferSourceNode is fire-and-forget — it disconnects automatically
     when playback ends and is GC'd shortly after.  Cheap to create. */
  var src = Module.WB_audio.ctx.createBufferSource();
  src.buffer = buf;
  src.connect(Module.WB_audio.gainNode);
  src.start(0);
});

EM_JS(void, wb_audio_set_muted, (int muted), {
  if (!Module.WB_audio) return;
  Module.WB_audio.muted = !!muted;
  /* Suspending the whole context is heavier than just gating BufferSource
     creation.  Stick with the gate so resume is instant. */
});

EM_JS(void, wb_audio_set_gain, (double gain), {
  if (!Module.WB_audio || !Module.WB_audio.gainNode) return;
  Module.WB_audio.gainNode.gain.value = gain;
});

EM_JS(void, wb_audio_cleanup, (void), {
  if (!Module.WB_audio) return;
  try {
    if (Module.WB_audio.ctx) Module.WB_audio.ctx.close();
  } catch (e) { /* ignore */ }
  Module.WB_audio = null;
});

/* -------------------------------------------------------
 * Public sound API (matches gui/sound.h)
 * ------------------------------------------------------- */

bool soundSetup(void) {
  int i;
  char path[256];

  wb_audio_init();
  for (i = 0; i < WB_NUM_SOUNDS; i++) {
    snprintf(path, sizeof(path), "/data/sounds/%s", kSoundFiles[i]);
    wb_audio_load(i, path);
  }
  s_isPlayable = TRUE;
  return TRUE;
}

void soundCleanup(void) {
  wb_audio_cleanup();
  s_isPlayable = FALSE;
}

void soundPlayEffect(sndEffects value) {
  int index;

  switch (value) {
  case shootSelf:         index = 6;  break;
  case shootNear:         index = 7;  break;
  case shotTreeNear:      index = 2;  break;
  case shotTreeFar:       index = 3;  break;
  case shotBuildingNear:  index = 4;  break;
  case shotBuildingFar:   index = 5;  break;
  case hitTankFar:        index = 18; break;
  case hitTankNear:       index = 17; break;
  case hitTankSelf:       index = 16; break;
  case bubbles:           index = 21; break;
  case tankSinkNear:      index = 0;  break;
  case tankSinkFar:       index = 1;  break;
  case bigExplosionNear:  index = 22; break;
  case bigExplosionFar:   index = 11; break;
  case farmingTreeNear:   index = 19; break;
  case farmingTreeFar:    index = 20; break;
  case manBuildingNear:   index = 14; break;
  case manBuildingFar:    index = 15; break;
  case manDyingNear:      index = 12; break;
  case manDyingFar:       index = 13; break;
  case manLayingMineNear: index = 23; break;
  case mineExplosionNear: index = 9;  break;
  case mineExplosionFar:  index = 10; break;
  default:                index = 8;  break;  /* shootFar */
  }

  wb_audio_play(index);
}

void soundKeepalive(bool value) {
  /* The desktop SDL path streams 100 ms of silence to keep AV receivers
   * awake when no effect is playing.  Browsers and WebAudio handle this
   * themselves — there is no equivalent stutter on a downstream HDMI
   * receiver since the browser is the audio source.  No-op. */
  (void)value;
}

bool soundIsPlayable(void) {
  return s_isPlayable;
}

void soundSetMuted(bool mute) {
  wb_audio_set_muted(mute ? 1 : 0);
}

void soundSetVolume(int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  s_currentGain = (double)pct / 100.0;
  wb_audio_set_gain(s_currentGain);
}

void soundSetReturningToLobby(bool active) {
  static double savedGain = 0.5;
  static bool muteActive = false;
  if (!s_isPlayable) return;
  if (active && !muteActive) {
    savedGain = s_currentGain;
    wb_audio_set_gain(0.0);
    muteActive = true;
  } else if (!active && muteActive) {
    wb_audio_set_gain(savedGain);
    s_currentGain = savedGain;
    muteActive = false;
  }
}
