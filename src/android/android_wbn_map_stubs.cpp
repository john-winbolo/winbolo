/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * android_wbn_map_stubs.cpp - Stubs for the WinBolo.net map catalogue
 * chooser source (wbn_map_source.cpp) and the stored-map preview fetch
 * (map_preview_fetch.cpp).
 *
 * Both files fetch over the WinBolo.net HTTP transport, which the Android
 * client does not build (see winbolonet_stub.c). The lobby's chooser and
 * the Last Round reel still name these calls, so they answer here with an
 * empty catalogue, no download and no preview.
 */

#include <cstring>

#include "../gui/sdl3/wbn_map_source.h"
#include "../gui/sdl3/map_preview_fetch.h"

/* The chooser zeroes state->numMaps before calling the provider, so an
   empty body leaves the WinBolo.net tab listing nothing. */
void wbnMapsListProvider(MapChooserState *state, const char *relPath, void *ctx) {
  (void)state; (void)relPath; (void)ctx;
}

void wbnMapsOnFolderJump(MapChooserState *state, const char *jumpPath, void *ctx) {
  (void)state; (void)jumpPath; (void)ctx;
}

bool wbnMapsGeneratePreview(const char *entryPath, MapPreviewPixels *outBuf, void *ctx) {
  (void)entryPath; (void)outBuf; (void)ctx;
  return false;
}

void wbnMapsTooltipPrefix(MapChooserState *state, void *ctx) {
  (void)state; (void)ctx;
}

void wbnMapSourceResetRetry(void) {}

const char *wbnMapSourceListError(void) { return nullptr; }

void wbnMapSourceSubmitDownload(uint32_t mapId) { (void)mapId; }

bool wbnMapSourcePollDownload(WbnMapDownloadResult *out) {
  (void)out;
  return false;
}

void wbnMapSourceResetDownload(void) {}

void mapPreviewFetchRequest(const char *md5Hex) { (void)md5Hex; }

MapPreviewFetchState mapPreviewFetchTryGet(const char *md5Hex,
                                           const uint8_t **bytesOut,
                                           size_t *lenOut) {
  (void)md5Hex;
  if (bytesOut) *bytesOut = nullptr;
  if (lenOut) *lenOut = 0;
  return MapPreviewFetchState::Unavailable;
}
