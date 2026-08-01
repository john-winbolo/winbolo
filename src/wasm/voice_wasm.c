/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * voice_wasm.c — web implementation of voice_backend.h for the WASM build.
 *
 * Replaces gui/sdl3/voice.c on the WASM target.  The client voice runtime
 * itself is shared (client_frontend/voice_client.c) and reaches the audio
 * device only through the twelve functions below, so this file is the whole
 * of what the web build has to supply.
 *
 * Capture and the local loopback output are live: a microphone acquired
 * through getUserMedia feeds an AudioWorklet that hands 20 ms mono chunks to
 * the main thread, and encoded audio played back locally goes out through a
 * second worklet on the same context.  Per-talker playback still declines -
 * nothing plays remote talkers until voice is carried over the wire.
 *
 * Voice runs its own AudioContext, pinned to 48 kHz, rather than sharing
 * sound_wasm.c's: that one is constructed with no sampleRate option and so
 * runs at whatever the output device wants, commonly 44.1 kHz, which is not
 * a rate the codec can work at.  A browser that will not give us 48 kHz
 * leaves voice without a context rather than producing pitch-shifted audio.
 *
 * Nothing here uses SharedArrayBuffer: this build has neither -pthread nor
 * the COOP/COEP headers it needs.  Frames cross between the audio thread and
 * the main thread as transferable ArrayBuffers, and cross between JS and the
 * wasm heap by being copied sample by sample.  A typed-array view of the
 * heap is never retained across a call - -sALLOW_MEMORY_GROWTH=1 detaches
 * the old buffer whenever the heap grows, which would silently invalidate
 * any view JS was still holding.
 */

#include "voice_backend.h"
#include "voice_core.h"  /* VOICE_FRAME_SAMPLES */

#include <emscripten.h>

/* Captured frames waiting for voiceBackendCaptureRead.  The audio thread
 * produces one every 20 ms and the main loop drains several per tick, so
 * this only fills when the main loop stalls - a couple of dropped animation
 * frames, a GC pause.  Past that, dropping the oldest keeps the delay the
 * player hears bounded; letting the queue grow would trade a stall for
 * permanent latency. */
#define WB_VOICE_CAPTURE_QUEUE_MAX 8

/* Frames the playback worklet will hold before it drops its oldest.  The
 * device drains this at real time, so it is a jitter cushion, not a
 * backlog. */
#define WB_VOICE_PLAYBACK_QUEUE_MAX 8

/* -------------------------------------------------------
 * JS-side helpers, defined inline via EM_JS.
 *
 * State hangs off a single Module.WB_voice object, the way sound_wasm.c
 * uses Module.WB_audio.  Neither reads or writes the other's: they are
 * separate contexts running at different rates.
 * ------------------------------------------------------- */

/* Builds both AudioWorklet processors from an inline source string, so the
 * build serves no extra .js asset.  The capture processor accumulates the
 * browser's 128-sample render quanta into whole frames and posts each one
 * out; the playback processor drains a queue of frames one quantum at a
 * time and outputs silence when it runs dry. */
EM_JS(void, wb_voice_init, (int frameSamples, int captureQueueMax,
                            int playbackQueueMax), {
  if (Module.WB_voice) return;

  var v = {
    ctx: null,
    modulePromise: null,
    micStream: null,
    srcNode: null,
    capNode: null,
    sinkNode: null,
    playNode: null,
    capQueue: [],
    capMax: captureQueueMax,
    moduleOk: false,
    live: false,
    capturing: false,
    pending: false
  };
  Module.WB_voice = v;

  var Ctx = window.AudioContext || window.webkitAudioContext;
  if (!Ctx) {
    console.warn("[WB_voice] AudioContext not supported");
    return;
  }

  var ctx;
  try {
    ctx = new Ctx({ sampleRate: 48000 });
  } catch (e) {
    console.warn("[WB_voice] could not create a 48 kHz AudioContext:", e);
    return;
  }

  /* The sampleRate option is a request, not a guarantee.  A context at any
     other rate would play the codec's output at the wrong speed, so drop it
     and leave voice without a device. */
  if (ctx.sampleRate !== 48000) {
    console.warn("[WB_voice] browser gave " + ctx.sampleRate +
                 " Hz, not 48000 - voice capture unavailable");
    try { ctx.close(); } catch (e2) { }
    return;
  }
  v.ctx = ctx;

  /* Browsers require an AudioContext to start in response to a user
     gesture.  sound_wasm.c's listeners remove themselves after the first
     gesture, so they cannot be shared - wire our own against this
     context. */
  var resumeOnGesture = function() {
    var w = Module.WB_voice;
    if (w && w.ctx && w.ctx.state === "suspended") {
      w.ctx.resume();
    }
    window.removeEventListener("pointerdown", resumeOnGesture, true);
    window.removeEventListener("keydown", resumeOnGesture, true);
    window.removeEventListener("touchstart", resumeOnGesture, true);
  };
  window.addEventListener("pointerdown", resumeOnGesture, true);
  window.addEventListener("keydown", resumeOnGesture, true);
  window.addEventListener("touchstart", resumeOnGesture, true);

  if (!ctx.audioWorklet) {
    console.warn("[WB_voice] AudioWorklet not available");
    return;
  }

  var src =
    "class WBVoiceCapture extends AudioWorkletProcessor {" +
    "  constructor() {" +
    "    super();" +
    "    this.buf = new Float32Array(" + frameSamples + ");" +
    "    this.n = 0;" +
    "    this.port.onmessage = () => { this.n = 0; };" +
    "  }" +
    "  process(inputs) {" +
    "    const ch = inputs[0] && inputs[0][0];" +
    "    if (!ch) return true;" +
    "    for (let i = 0; i < ch.length; i++) {" +
    "      this.buf[this.n++] = ch[i];" +
    "      if (this.n === " + frameSamples + ") {" +
    "        const out = this.buf.slice(0);" +
    "        this.n = 0;" +
    "        this.port.postMessage(out.buffer, [out.buffer]);" +
    "      }" +
    "    }" +
    "    return true;" +
    "  }" +
    "}" +
    "registerProcessor('wb-voice-capture', WBVoiceCapture);" +
    "class WBVoicePlayback extends AudioWorkletProcessor {" +
    "  constructor() {" +
    "    super();" +
    "    this.q = [];" +
    "    this.pos = 0;" +
    "    this.max = " + playbackQueueMax + ";" +
    "    this.port.onmessage = (e) => {" +
    "      if (typeof e.data === 'string') { this.q.length = 0; this.pos = 0; return; }" +
    "      if (this.q.length >= this.max) { this.q.shift(); this.pos = 0; }" +
    "      this.q.push(new Float32Array(e.data));" +
    "    };" +
    "  }" +
    "  process(inputs, outputs) {" +
    "    const out = outputs[0][0];" +
    "    for (let i = 0; i < out.length; i++) {" +
    "      if (this.q.length === 0) { out[i] = 0; continue; }" +
    "      const f = this.q[0];" +
    "      out[i] = f[this.pos++];" +
    "      if (this.pos >= f.length) { this.q.shift(); this.pos = 0; }" +
    "    }" +
    "    return true;" +
    "  }" +
    "}" +
    "registerProcessor('wb-voice-playback', WBVoicePlayback);";

  var url = URL.createObjectURL(new Blob([src], { type: "application/javascript" }));
  v.modulePromise = ctx.audioWorklet.addModule(url).then(function() {
    URL.revokeObjectURL(url);
    var w = Module.WB_voice;
    if (!w || !w.ctx) return;
    w.moduleOk = true;
    /* The loopback bus: one mono output straight to the speakers. */
    w.playNode = new AudioWorkletNode(w.ctx, "wb-voice-playback",
                                      { numberOfInputs: 0, numberOfOutputs: 1,
                                        outputChannelCount: [1] });
    w.playNode.connect(w.ctx.destination);
    /* A silent sink for the capture node to feed.  A worklet with nothing
       downstream is not reliably pulled by every browser, so the capture
       node gets an output that goes to a muted gain rather than nowhere. */
    w.sinkNode = w.ctx.createGain();
    w.sinkNode.gain.value = 0;
    w.sinkNode.connect(w.ctx.destination);
  }).catch(function(e) {
    URL.revokeObjectURL(url);
    console.warn("[WB_voice] worklet module failed to load:", e);
  });
});

/* Returns 1 when capture is running or a request is in flight, 0 when it
 * cannot be attempted at all. */
EM_JS(int, wb_voice_capture_start, (void), {
  var v = Module.WB_voice;
  if (!v || !v.ctx || !v.modulePromise) return 0;

  /* The gesture listeners cover the usual case; this catches a context that
     was suspended again later (a backgrounded tab). */
  if (v.ctx.state === "suspended") v.ctx.resume();

  v.capturing = true;

  /* Already have the microphone: re-arm the graph the stop disconnected.
     Connecting an existing pair a second time is a no-op per spec. */
  if (v.live) {
    if (v.srcNode && v.capNode) v.srcNode.connect(v.capNode);
    return 1;
  }
  if (v.pending) return 1;

  if (!window.isSecureContext) {
    console.warn("[WB_voice] microphone needs a secure context (https)");
    v.capturing = false;
    return 0;
  }
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    console.warn("[WB_voice] getUserMedia not available");
    v.capturing = false;
    return 0;
  }

  /* Asynchronous from here, and deliberately not awaited: this is called
     from inside an ImGui frame, and unwinding the stack mid-draw is not a
     state this build should be put in.  voiceBackendCaptureIsOpen is what
     reports the outcome, once there is one. */
  v.pending = true;
  v.modulePromise.then(function() {
    var w = Module.WB_voice;
    /* Ask only once there is something to feed.  Prompting for a microphone
       we could not use anyway would spend the player's one permission
       decision on nothing. */
    if (!w || !w.moduleOk) throw new Error("voice worklet unavailable");
    return navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1 },
                                                 video: false });
  }).then(function(stream) {
    var w = Module.WB_voice;
    if (!w || !w.ctx) {
      stream.getTracks().forEach(function(t) { t.stop(); });
      return;
    }
    w.micStream = stream;
    w.srcNode = w.ctx.createMediaStreamSource(stream);
    w.capNode = new AudioWorkletNode(w.ctx, "wb-voice-capture",
                                     { numberOfInputs: 1, numberOfOutputs: 1,
                                       outputChannelCount: [1],
                                       channelCount: 1,
                                       channelCountMode: "explicit" });
    w.capNode.port.onmessage = function(e) {
      var q = Module.WB_voice;
      /* Gated on capturing, not on the stream: the node stays in the graph
         across a stop so it can be re-armed, and a browser that keeps
         pulling it must not refill the queue behind a stop. */
      if (!q || !q.capturing) return;
      /* e.data was transferred, so wrapping it moves rather than copies. */
      if (q.capQueue.length >= q.capMax) q.capQueue.shift();
      q.capQueue.push(new Float32Array(e.data));
    };
    if (w.sinkNode) w.capNode.connect(w.sinkNode);
    /* The player may have switched capture off again while the browser was
       still asking.  Leave the microphone unhooked in that case; the next
       start re-arms it without a second prompt. */
    if (w.capturing) w.srcNode.connect(w.capNode);
    /* A track the user revokes, or a device that disappears, ends here -
       the mic status the other players see follows it. */
    stream.getAudioTracks().forEach(function(t) {
      t.onended = function() {
        var z = Module.WB_voice;
        if (z) { z.live = false; z.capQueue.length = 0; }
      };
    });
    w.live = true;
    w.pending = false;
  }).catch(function(e) {
    var w = Module.WB_voice;
    if (w) {
      w.pending = false;
      w.live = false;
      /* A stream that was granted but could not be wired up is released
         rather than left holding the microphone open for nothing. */
      if (w.micStream) {
        w.micStream.getTracks().forEach(function(t) { t.stop(); });
        w.micStream = null;
      }
    }
    console.warn("[WB_voice] microphone unavailable:", e);
  });
  return 1;
});

EM_JS(void, wb_voice_capture_stop, (void), {
  var v = Module.WB_voice;
  if (!v) return;
  v.capturing = false;
  /* Unhook the microphone from the worklet rather than stopping the track:
     the stream stays live so turning capture back on does not raise the
     permission prompt a second time. */
  if (v.srcNode) {
    try { v.srcNode.disconnect(); } catch (e) { }
  }
  /* Both sides drop what they hold - the worklet's part-built frame and
     everything already queued here - so re-enabling does not open with
     audio recorded before it was switched off. */
  if (v.capNode) v.capNode.port.postMessage("reset");
  v.capQueue.length = 0;
});

EM_JS(int, wb_voice_capture_is_open, (void), {
  var v = Module.WB_voice;
  return (v && v.live) ? 1 : 0;
});

/* Writes one whole frame into the wasm heap and returns its sample count,
 * or 0 when nothing is queued. */
EM_JS(int, wb_voice_capture_read, (int16_t *pcm, int frameSamples), {
  var v = Module.WB_voice;
  if (!v || !v.live || v.capQueue.length === 0) return 0;
  var f = v.capQueue.shift();
  if (f.length !== frameSamples) return 0;
  /* HEAP16 is re-read here on every call: -sALLOW_MEMORY_GROWTH=1 replaces
     it whenever the heap grows, and a view cached from an earlier call
     would be pointing at a detached buffer. */
  var base = pcm >> 1;
  for (var i = 0; i < frameSamples; i++) {
    var s = f[i];
    if (s > 1) s = 1; else if (s < -1) s = -1;
    HEAP16[base + i] = (s * 32767) | 0;
  }
  return frameSamples;
});

EM_JS(void, wb_voice_loopback_play, (const int16_t *pcm, int frameSamples), {
  var v = Module.WB_voice;
  if (!v || !v.playNode) return;
  /* A fresh array per call, copied out of the heap and then transferred.
     Handing the worklet a view of the heap instead would hand it something
     the next allocation can detach. */
  var f = new Float32Array(frameSamples);
  var base = pcm >> 1;
  for (var i = 0; i < frameSamples; i++) {
    f[i] = HEAP16[base + i] / 32768;
  }
  v.playNode.port.postMessage(f.buffer, [f.buffer]);
});

EM_JS(void, wb_voice_loopback_clear, (void), {
  var v = Module.WB_voice;
  if (!v || !v.playNode) return;
  v.playNode.port.postMessage("clear");
});

EM_JS(void, wb_voice_shutdown, (void), {
  var v = Module.WB_voice;
  if (!v) return;
  try {
    if (v.srcNode) v.srcNode.disconnect();
    if (v.capNode) { v.capNode.port.onmessage = null; v.capNode.disconnect(); }
    if (v.playNode) v.playNode.disconnect();
    if (v.sinkNode) v.sinkNode.disconnect();
    if (v.micStream) {
      v.micStream.getTracks().forEach(function(t) { t.stop(); });
    }
    if (v.ctx) v.ctx.close();
  } catch (e) { }
  Module.WB_voice = null;
});

/*********************************************************
*NAME:          voiceBackendInit
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Brings up the voice AudioContext and loads the worklet
*  module, and reports that voice is usable.  The microphone
*  is deliberately not touched: the permission prompt is
*  raised on the first capture, not at startup.
*
*  Always true, even when the browser gave us no usable
*  context.  The codec still comes up and the settings
*  controls still work; what a missing context costs is
*  capture, which every device-facing call below reports for
*  itself.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendInit(void) {
    wb_voice_init(VOICE_FRAME_SAMPLES, WB_VOICE_CAPTURE_QUEUE_MAX,
                  WB_VOICE_PLAYBACK_QUEUE_MAX);
    return true;
}

/*********************************************************
*NAME:          voiceBackendShutdown
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Stops the microphone, disconnects the graph and closes
*  the context.
*
*  Structural parity rather than a path that runs: the wasm
*  build's shutdown tail sits after
*  emscripten_set_main_loop(..., 1), which never returns, so
*  the browser tears the page down instead.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendShutdown(void) {
    wb_voice_shutdown();
}

/*********************************************************
*NAME:          voiceBackendCaptureStart
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Starts capturing, asking the browser for the microphone
*  the first time.  Returns true once a request is in flight
*  or the stream is already live, and false only when there
*  is no way to ask at all - no getUserMedia, an insecure
*  page, or no 48 kHz context to feed.
*
*  Acquiring the microphone is asynchronous and is not
*  awaited: this runs inside an ImGui frame, and unwinding
*  the stack mid-draw to resume it later is not a state
*  worth introducing.  So a true here is "asked", not
*  "capturing" - voiceBackendCaptureIsOpen answers that once
*  the browser has decided.  A player who refuses the prompt
*  leaves the switch on and hears nothing.
*
*  Calling it again while capture is already running is
*  harmless and does not re-prompt.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureStart(void) {
    return wb_voice_capture_start() != 0;
}

/*********************************************************
*NAME:          voiceBackendCaptureStop
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Stops delivery and drops what is buffered on both sides -
*  the worklet's part-built frame and the frames already
*  queued here - so switching capture back on does not open
*  with audio recorded while it was off.
*
*  The microphone stream itself is kept.  Releasing it would
*  mean a second permission prompt on the next start, which
*  is the whole reason the open is deferred in the first
*  place.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendCaptureStop(void) {
    wb_voice_capture_stop();
}

/*********************************************************
*NAME:          voiceBackendCaptureIsOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Returns whether the microphone stream is live.  False
*  while a request is still pending and false after a
*  refusal, so the mic status the other players see never
*  claims a microphone this client was not actually given.
*
*  It stays true across a stop and start, because stopping
*  only unhooks the stream from the graph, and goes false on
*  its own if the track ends - a revoked permission, a
*  device unplugged.
*
*ARGUMENTS:
*  (none)
*********************************************************/
bool voiceBackendCaptureIsOpen(void) {
    return wb_voice_capture_is_open() != 0;
}

/*********************************************************
*NAME:          voiceBackendCaptureRead
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Takes the next whole captured frame off the queue the
*  worklet fills, converting it from float to S16 with
*  clamping as it copies into the caller's buffer.  Returns
*  VOICE_FRAME_SAMPLES, or 0 when nothing is queued - never
*  a partial frame, and pcm is untouched in that case.
*
*ARGUMENTS:
*  pcm - filled with VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
int voiceBackendCaptureRead(int16_t *pcm) {
    return wb_voice_capture_read(pcm, VOICE_FRAME_SAMPLES);
}

/*********************************************************
*NAME:          voiceBackendSpeakerOpen
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Declines to play a remote talker: there is no output
*  path yet.  The shared runtime drops that player's frames
*  rather than buffering audio nothing will ever drain.
*  Giving each talker a node on the AudioContext will happen
*  here.
*
*ARGUMENTS:
*  player - the player number the frame came from
*********************************************************/
bool voiceBackendSpeakerOpen(int player) {
    (void)player;
    return false;
}

/*********************************************************
*NAME:          voiceBackendSpeakerClose
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  No talker was ever opened, so there is nothing to
*  release.
*
*ARGUMENTS:
*  player - the player number to release
*********************************************************/
void voiceBackendSpeakerClose(int player) {
    (void)player;
}

/*********************************************************
*NAME:          voiceBackendSpeakerQueuedFrames
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Reports nothing queued for that talker, which is what an
*  unopened talker holds.
*
*ARGUMENTS:
*  player - the player number to ask about
*********************************************************/
int voiceBackendSpeakerQueuedFrames(int player) {
    (void)player;
    return 0;
}

/*********************************************************
*NAME:          voiceBackendSpeakerPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Discards the frame: there is nowhere to play it.  This
*  is only reached if a talker was opened, which cannot
*  happen yet.  Queueing onto that talker's output node
*  will happen here.
*
*ARGUMENTS:
*  player - the player number the frame came from
*  pcm    - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendSpeakerPlay(int player, const int16_t *pcm) {
    (void)player;
    (void)pcm;
}

/*********************************************************
*NAME:          voiceBackendLoopbackPlay
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Queues one frame of the microphone loopback test.  The
*  samples are converted to float and copied out of the wasm
*  heap into an array of their own, which is then handed to
*  the playback worklet.  Nothing of the heap is retained
*  past the call.
*
*ARGUMENTS:
*  pcm - VOICE_FRAME_SAMPLES mono S16 samples
*********************************************************/
void voiceBackendLoopbackPlay(const int16_t *pcm) {
    wb_voice_loopback_play(pcm, VOICE_FRAME_SAMPLES);
}

/*********************************************************
*NAME:          voiceBackendLoopbackClear
*AUTHOR:        John Morrison
*CREATION DATE: 2026
*LAST MODIFIED: 2026
*PURPOSE:
*  Drops whatever the loopback test still has queued, so
*  switching it off goes quiet at once instead of playing
*  out its tail.
*
*ARGUMENTS:
*  (none)
*********************************************************/
void voiceBackendLoopbackClear(void) {
    wb_voice_loopback_clear();
}
