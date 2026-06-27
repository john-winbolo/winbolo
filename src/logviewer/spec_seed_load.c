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

/*
 * Turns a raw spectator-ring keyframe seed blob into the v2 header + opening
 * snapshot the log-viewer decoder consumes, then loads it through
 * lv_screenLoadFromStream so the decoder reconstructs the world a spectator
 * joins mid-game.
 *
 * This translation unit is logviewer-world. It bridges the dep-free spectator
 * replay translator (spectator_replay.h, which pulls only platform_types.h —
 * the same header lv_global.h already includes) to the decoder (backend.h). It
 * deliberately does NOT include clientSim.h or the bolo sim headers: those pull
 * global.h, whose world types collide with the decoder's. The captured seed
 * therefore arrives here as raw bytes; the ClientSim capture-to-load wiring
 * crosses that boundary elsewhere.
 *
 * The keyframe's control-snapshot slice is not part of the LOG stream the
 * decoder reads. It is copied into a module-owned buffer here so a later HUD
 * consumer can read it (lv_specSeedControl); it is not discarded.
 *
 * Once seeded, each subsequent forward record (lv_specRecordPump) is translated
 * the same way — keyframe vs event-tick — and fed to lv_screenStreamPump so the
 * decoder's delayed view advances. A mid-stream keyframe re-syncs the decoder
 * (a fresh LOG_SNAPSHOT) and refreshes the stashed control slice.
 */

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lv_global.h"
#include "backend.h"

#include "spectator_replay.h"

/* Upper bound on the synthesized header: 8 id + 1 version + 1 map-length byte +
 * up to 255 map-name bytes + 8 game-info + 4 addr + 2 port + 4 time + 32 key. */
#define LV_SPEC_HEADER_MAX (8 + 1 + 1 + 255 + 8 + 4 + 2 + 4 + 32)

/* Module-owned copy of the most recent keyframe's control-snapshot slice. */
static uint8_t *gSpecControl = NULL;
static size_t gSpecControlLen = 0;

static void specSeedStashControl(const BYTE *ctrl, int ctrlLen) {
  free(gSpecControl);
  gSpecControl = NULL;
  gSpecControlLen = 0;
  if (ctrl == NULL || ctrlLen <= 0) {
    return;
  }
  gSpecControl = (uint8_t *) malloc((size_t) ctrlLen);
  if (gSpecControl == NULL) {
    return; /* the stash is best-effort; a failed copy must not fail the load */
  }
  memcpy(gSpecControl, ctrl, (size_t) ctrlLen);
  gSpecControlLen = (size_t) ctrlLen;
}

const uint8_t *lv_specSeedControl(size_t *outLen) {
  if (outLen != NULL) {
    *outLen = gSpecControlLen;
  }
  return gSpecControl;
}

void lv_specSeedControlClear(void) {
  free(gSpecControl);
  gSpecControl = NULL;
  gSpecControlLen = 0;
}

bool lv_specSeedLoad(const LvSpecSeedInfo *info, const uint8_t *seed,
                     size_t seedLen) {
  SpecReplayHeaderInfo hdrInfo;
  BYTE header[LV_SPEC_HEADER_MAX];
  BYTE *keyframe;
  BYTE *stream;
  const BYTE *ctrl;
  int ctrlLen;
  int headerLen;
  int keyframeLen;
  bool loaded;

  /* >= INT_MAX (not >) so seedLen + 1 below stays within int for the cap. */
  if (seed == NULL || seedLen == 0 || seedLen >= (size_t) INT_MAX) {
    return FALSE;
  }

  /* Map the dep-free seam info onto the translator's header struct. The live
   * game-info arrives with the spectate-start handshake; until then a caller
   * may pass NULL and accept zeroed defaults. */
  memset(&hdrInfo, 0, sizeof(hdrInfo));
  if (info != NULL) {
    hdrInfo.mapName = info->mapName;
    hdrInfo.gameType = info->gameType;
    hdrInfo.allowHiddenMines = info->allowHiddenMines;
    hdrInfo.ai = info->ai;
    hdrInfo.usePassword = info->usePassword;
    hdrInfo.maxPlayers = info->maxPlayers;
    hdrInfo.versionMajor = info->versionMajor;
    hdrInfo.versionMinor = info->versionMinor;
    hdrInfo.versionRevision = info->versionRevision;
  }
  if (hdrInfo.mapName == NULL) {
    hdrInfo.mapName = "";
  }

  headerLen = specReplayWriteHeader(&hdrInfo, header, (int) sizeof(header));
  if (headerLen < 0) {
    return FALSE;
  }

  /* The translated keyframe is [LOG_EVENT_SNAPSHOT][world body]; the body is a
   * slice of the seed, so seedLen + 1 bounds the output. */
  keyframe = (BYTE *) malloc(seedLen + 1);
  if (keyframe == NULL) {
    return FALSE;
  }
  ctrl = NULL;
  ctrlLen = 0;
  keyframeLen = specReplayTranslateKeyframe(seed, (int) seedLen, keyframe,
                                            (int) (seedLen + 1), &ctrl, &ctrlLen);
  if (keyframeLen < 0) {
    free(keyframe);
    return FALSE;
  }

  /* Hold the control slice for the HUD before the seed bytes leave the caller. */
  specSeedStashControl(ctrl, ctrlLen);

  /* Concatenate header + opening snapshot and hand the decoder one stream. */
  stream = (BYTE *) malloc((size_t) headerLen + (size_t) keyframeLen);
  if (stream == NULL) {
    free(keyframe);
    return FALSE;
  }
  memcpy(stream, header, (size_t) headerLen);
  memcpy(stream + headerLen, keyframe, (size_t) keyframeLen);

  loaded = lv_screenLoadFromStream(stream,
                                   (size_t) headerLen + (size_t) keyframeLen);

  free(keyframe);
  free(stream);
  return loaded;
}

bool lv_specRecordPump(bool isKeyframe, const uint8_t *payload, size_t len) {
  BYTE *out;
  int outCap;
  int outLen;
  bool playing;

  /* len is cast to int for the translators below; keep it in range and leave
   * room for the marker bytes they prepend (LOG_EVENT_LONG's 3-byte marker is
   * the largest). An empty event tick is legitimate (payload NULL, len 0). */
  if (len >= (size_t) INT_MAX - 3) {
    return FALSE;
  }
  if (payload == NULL && len != 0) {
    return FALSE;
  }

  /* Output bound: a keyframe emits [LOG_EVENT_SNAPSHOT][body] with body the
   * record's world slice (<= len), an event tick emits up to a 3-byte marker
   * plus the payload verbatim. len + 3 covers both. */
  outCap = (int) len + 3;
  out = (BYTE *) malloc((size_t) outCap);
  if (out == NULL) {
    return FALSE;
  }

  if (isKeyframe) {
    const BYTE *ctrl = NULL;
    int ctrlLen = 0;
    outLen = specReplayTranslateKeyframe(payload, (int) len, out, outCap,
                                         &ctrl, &ctrlLen);
    if (outLen < 0) {
      free(out);
      return FALSE;
    }
    /* A mid-stream keyframe carries fresh control state (the decoder re-syncs on
     * the snapshot); refresh the stash so a later HUD reads current roster/score
     * /phase rather than the join-time slice. */
    specSeedStashControl(ctrl, ctrlLen);
  } else {
    outLen = specReplayTranslateEvents(payload, (int) len, out, outCap);
    if (outLen < 0) {
      free(out);
      return FALSE;
    }
  }

  /* Append the translated, record-aligned bytes; the pump advances over the
   * whole record and parks caught up. Returns the decoder's isPlaying state. */
  playing = lv_screenStreamPump(out, (size_t) outLen);
  free(out);
  return playing;
}
