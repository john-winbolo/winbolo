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
 * Name:          lobby_internal.h
 * Purpose:       Feature macros for the parts of the recap
 *                a given build leaves out, and the
 *                hand-written declarations for the embedded
 *                log-viewer reel and the wasm round-log
 *                fetch. Shared by the lobby dialog sources.
 *********************************************************/

#ifndef LOBBY_INTERNAL_H
#define LOBBY_INTERNAL_H

/* Include this at file scope, never from inside an extern "C" block: the
 * header below pulls in imgui.h, whose templates do not compile with C
 * linkage. The declarations further down carry their own extern "C". */
#include "../imgui_dialog_utils.h"

/* Parts of the recap that need something a given build does not have, and the
 * one that has a transport per platform. Named for what they need rather than
 * for the platform, so the reason each one is out reads at the site as well as
 * here.
 *
 * BOLO_REEL_WBN_FETCH      — the reel's WinBolo.net fallback source, used when
 *   a WBN-registered host uploads its round log instead of serving it over the
 *   game socket. On for every build: the retry ladder, the attempt cap, the
 *   size ceiling and the progress reporting are one copy, and only the
 *   transport underneath forks.
 * BOLO_REEL_WBN_FETCH_CURL — which transport that is. Desktop downloads on a
 *   std::thread through http.c. The browser has neither: http.c refuses to
 *   build under Emscripten because it embeds the WinBolo.net signing key, and
 *   the wasm client is single-threaded, so it drives the page's fetch()
 *   through src/wasm/round_log_fetch_wasm.c instead.
 * BOLO_RECAP_WBN_RATING    — the round's stars and comments, on the same
 *   http.c through wbn_comments; neither that nor the star widget is in the
 *   wasm target's sources.
 * BOLO_RECAP_CLIP_GIF      — the clip export. The GIF encoder's implementation
 *   TU (third_party/msf_gif/msf_gif_impl.c) is not in the wasm target's
 *   sources, and the save path spins its own SDL event loop waiting on a
 *   native file dialog, which a browser main loop cannot do. */
#define BOLO_REEL_WBN_FETCH 1
#ifdef __EMSCRIPTEN__
#define BOLO_REEL_WBN_FETCH_CURL 0
#define BOLO_RECAP_WBN_RATING    0
#define BOLO_RECAP_CLIP_GIF      0
#else
#define BOLO_REEL_WBN_FETCH_CURL 1
#define BOLO_RECAP_WBN_RATING    1
#define BOLO_RECAP_CLIP_GIF      1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Embedded log-viewer reel (src/logviewer/lv_embed.c). Hand-declared rather
 * than included: logviewer.h pulls in backend.h / viewport_types.h, whose
 * screen / screenMines types collide with the client's — the same rule
 * gamefront.c documents. Scalars and void * only, so no viewer type crosses
 * the seam. Must stay inside this extern "C" block or the calls compile and
 * then fail to link on a mangled symbol. */
#if !BOLO_MOBILE
bool lvEmbedBegin(struct SDL_Window *window, struct SDL_Renderer *renderer,
                  uint8_t *zipData, size_t zipLen, int viewW, int viewH);
void lvEmbedEnd(void);
bool lvEmbedIsActive(void);
void lvEmbedSetViewportSize(int viewW, int viewH);
bool lvEmbedFrameTexture(void **outTexture, int *outTexW, int *outTexH,
                         int *outSrcX, int *outSrcY, int *outSrcW, int *outSrcH);
float lvEmbedGetZoomLevel(void);
void lvEmbedPlay(void);
void lvEmbedPause(void);
bool lvEmbedIsPlaying(void);
void lvEmbedWheel(int localX, int localY, float wheelY);
void lvEmbedPanBegin(void);
void lvEmbedPanDelta(float dxScreenPx, float dyScreenPx);
void lvEmbedGetProgress(uint32_t *outCurMs, uint32_t *outTotalMs);
void lvEmbedSeekRatio(float ratio);
void lvEmbedSeekToClip(uint32_t roundRelMs, int mapX, int mapY);
void lvEmbedSeekToTime(uint32_t roundRelMs);
bool lvEmbedFocusPlayerByName(const char *name);
void lvEmbedStepTicks(int ticks);
void lvEmbedSetSelfName(const char *name);
#endif
#if !BOLO_REEL_WBN_FETCH_CURL
/* Round-log download for the reel's WinBolo.net source
 * (src/wasm/round_log_fetch_wasm.c). Start hands the request to the page and
 * returns; the poll is read once a frame, so nothing here suspends the C stack
 * inside an ImGui frame. Poll gives 0 while the request is in flight, otherwise
 * the HTTP status, or -1 when it never completed; on a 200 whose body fits the
 * ceiling it hands over a malloc'd buffer the caller owns. Hand-declared on the
 * same terms as the reel above. */
void wbRoundLogFetchStart(const char *key);
int  wbRoundLogFetchPoll(uint8_t **outBuf, int *outLen);
void wbRoundLogFetchCancel(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LOBBY_INTERNAL_H */
