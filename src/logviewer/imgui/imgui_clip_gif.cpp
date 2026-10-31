/*
 * imgui_clip_gif.cpp - GIFs of the map and the Clips window in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * See imgui_clip_gif.h. Recording mode is the game's (lobby_reel.cpp), made
 * with the same code: its state, the length, the crop and the file name come
 * from clip_gif_core.h, its controls from clip_gif_controls.h, and Preview
 * clip, Save clip, the progress window and where the GIF goes from
 * clip_gif_capture.h. This file gives that code the viewer's log as a
 * ClipGifReplay, the part of the map that shows and the players of the log.
 */

#include "imgui_clip_gif.h"

#include "../../gui/sdl3/dialogs/clip_gif_gallery.h"  /* CLIP_GIF_GALLERY_BUILT */

#if CLIP_GIF_GALLERY_BUILT

#include "imgui.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/clip_gif_core.h"
#include "../../gui/sdl3/dialogs/clip_gif_controls.h"
#include "../../gui/sdl3/dialogs/clip_gif_capture.h"
#include "../../gui/sdl3/dialogs/imgui_dialog_utils.h"  /* imguiLoadSvgIconWhite */
#include "imgui_game_viewport.h"  /* lv_imgui_game_viewport_mouse_held */
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
#include "logviewer.h"
#include "../lv_host.h"
#include "../backend.h"
#include "../draw.h"
#include "../blocks.h"
#include "../clientmutex.h"
#include "../tiles.h"
}

/* The size the GIF button's picture is made at. */
static const int LV_GIF_PICTURE_PX = 32;

typedef struct LvGifState {
    /* Recording mode: on or off, the crop frame and the length. Back to
     * CLIP_GIF_MODE_INIT with each log that opens. */
    ClipGifMode     mode       = CLIP_GIF_MODE_INIT;
    /* Which part of the crop frame the mouse holds. */
    ClipGifCropDrag drag       = { 0, false };
    /* The GIF button's picture (data/ui/picture.svg, as the game's), and
     * the renderer it was made for. Made the first time it is drawn. */
    SDL_Renderer   *pictureFor = nullptr;
    SDL_Texture    *picture    = nullptr;

    /* The preview and the capture (clip_gif_capture.h). */
    ClipGifSession  session    = {};
    /* The size of the render target when the capture started. A zoom or a
     * window size change makes a new target, and the crop then means
     * nothing, so the capture stops. */
    int         targetW        = 0;
    int         targetH        = 0;
    /* Whether the sounds played before the capture, to put back after it. */
    bool        restoreSound   = false;

    /* The id of the open log (clipGifGameIdFromBytes), and what it was made
     * from, so a log is read for it once only. */
    char        gameId[CLIP_GIF_META_GAME_LEN] = "";
    char        gameIdPath[1024]               = "";
    const void *gameIdData                     = nullptr;
    size_t      gameIdLen                      = 0;
    bool        gameIdSet                      = false;
} LvGifState;

static LvGifState s_gif;

/* ── The open log's id ────────────────────────────────────────────── */

/* The id the gallery puts the GIFs of the open log first with. It is the
 * id of the log file's bytes, the same id the game's replay gives the same
 * file, so a GIF made in either place is listed with its game in both. */
static const char *lvGifGameId(void) {
    if (g_lv == NULL || !g_lv->isLoaded) {
        return "";
    }
    const char    *path = NULL;
    const uint8_t *data = NULL;
    size_t         len  = 0;
    lv_clientMutexWaitFor();
    int src = lv_blocksGetSource(&path, &data, &len);
    lv_clientMutexRelease();

    if (src == LV_LOG_SOURCE_FILE && path != NULL) {
        if (s_gif.gameIdSet && s_gif.gameIdData == NULL &&
            SDL_strcmp(s_gif.gameIdPath, path) == 0) {
            return s_gif.gameId;
        }
        s_gif.gameId[0] = '\0';
        size_t fileLen = 0;
        void *bytes = SDL_LoadFile(path, &fileLen);
        if (bytes != NULL) {
            clipGifGameIdFromBytes(bytes, fileLen, s_gif.gameId,
                                   sizeof(s_gif.gameId));
            SDL_free(bytes);
        }
        SDL_strlcpy(s_gif.gameIdPath, path, sizeof(s_gif.gameIdPath));
        s_gif.gameIdData = NULL;
        s_gif.gameIdLen  = 0;
        s_gif.gameIdSet  = true;
        return s_gif.gameId;
    }
    if (src == LV_LOG_SOURCE_MEMORY && data != NULL) {
        if (s_gif.gameIdSet && s_gif.gameIdData == data &&
            s_gif.gameIdLen == len) {
            return s_gif.gameId;
        }
        clipGifGameIdFromBytes(data, len, s_gif.gameId, sizeof(s_gif.gameId));
        s_gif.gameIdPath[0] = '\0';
        s_gif.gameIdData = data;
        s_gif.gameIdLen  = len;
        s_gif.gameIdSet  = true;
        return s_gif.gameId;
    }
    return "";
}

static void lvGifForgetGameId(void) {
    s_gif.gameId[0]     = '\0';
    s_gif.gameIdPath[0] = '\0';
    s_gif.gameIdData    = nullptr;
    s_gif.gameIdLen     = 0;
    s_gif.gameIdSet     = false;
}

/* ── The log, as the shared capture sees it ───────────────────────── */

/* The log time now and the length of the log, both from the start of the
 * part of the log the viewer shows (as the Controls panel shows them). */
static void lvGifProgress(void *user, uint32_t *curMs, uint32_t *totalMs) {
    (void)user;
    size_t pos = 0, size = 0;
    uint32_t cur = 0, total = 0;
    lv_clientMutexWaitFor();
    lv_screenGetLogProgress(&pos, &size, &cur, &total);
    lv_clientMutexRelease();
    *curMs   = cur;
    *totalMs = total;
}

static bool lvGifIsPlaying(void *user) {
    (void)user;
    return g_lv != NULL && g_lv->playIsPlaying;
}

static void lvGifPlay(void *user) {
    (void)user;
    lv_windowPlay();
}

static void lvGifPause(void *user) {
    (void)user;
    lv_windowPause();
}

/* ms is from the start of the part of the log the viewer shows; the seek
 * takes an absolute log time. */
static void lvGifSeek(void *user, uint32_t ms) {
    (void)user;
    lv_clientMutexWaitFor();
    lv_drawDirtyScreen();
    lv_screenSeekToTimeMs(lv_screenWindowStartMs() + ms);
    lv_drawDirtyScreen();
    lv_clientMutexRelease();
    if (g_lv != NULL) {
        g_lv->wantScreenUpdate = TRUE;
    }
    lv_windowNeedRedraw();
}

/* On: the names over the tanks go into the render target, which is what is
 * read back, and the sounds stop (the ticks of a capture go by many times
 * faster than real time, and their sounds would all play at once). Off: both
 * go back. */
static void lvGifCaptureMode(void *user, bool on) {
    (void)user;
    if (on) {
        s_gif.restoreSound = (g_lv != NULL && g_lv->isSoundsPlaying);
        if (g_lv != NULL) {
            g_lv->isSoundsPlaying = FALSE;
        }
        lv_clientMutexWaitFor();
        lv_drawSetTankLabelsInTarget(TRUE);
        lv_clientMutexRelease();
        return;
    }
    lv_clientMutexWaitFor();
    lv_drawSetTankLabelsInTarget(FALSE);
    lv_clientMutexRelease();
    if (g_lv != NULL) {
        g_lv->isSoundsPlaying = s_gif.restoreSound ? TRUE : FALSE;
        g_lv->wantScreenUpdate = TRUE;
    }
    lv_windowNeedRedraw();
}

/* One frame: the log moves on one frame's ticks and the map is drawn into
 * the render target. False when the frame cannot be made: no map shows, or
 * the render target is not the one the crop was made for. *ended is set
 * when the log has no more to play. */
static bool lvGifAdvance(void *user, bool *ended) {
    (void)user;
    *ended = false;
    if (g_lv == NULL || !g_lv->isLoaded || g_lv->gameView) {
        return false;
    }
    int texW = 0, texH = 0;
    lv_clientMutexWaitFor();
    for (int i = 0; i < CLIP_GIF_TICKS_PER_FRAME; i++) {
        lv_screenLogTick();
    }
    lv_screenUpdate(redraw);
    lv_drawGetGameTargetSize(&texW, &texH);
    bool playing = g_lv->isPlaying ? true : false;
    lv_clientMutexRelease();

    if (texW != s_gif.targetW || texH != s_gif.targetH) {
        return false;
    }
    *ended = !playing;
    return true;
}

static bool lvGifTarget(void *user, SDL_Renderer **renderer,
                        SDL_Texture **texture) {
    (void)user;
    if (g_lv == NULL) {
        return false;
    }
    *renderer = g_lv->renderer;
    *texture  = lv_drawGetGameTexture();
    return *texture != NULL;
}

static const ClipGifReplay s_lvReplay = {
    NULL, lvGifProgress, lvGifIsPlaying, lvGifPlay, lvGifPause, lvGifSeek,
    lvGifCaptureMode, lvGifAdvance, lvGifTarget
};

/* ── Save clip ────────────────────────────────────────────────────── */

/* The map as the last blit drew it into the window: the render-target pixels
 * that show in the map area (*slice, see clipGifBlitView), and the box in
 * window points they are drawn in (*view), which the crop frame is drawn on
 * and is a fraction of. *blitX, *blitY are where the blit's own slice of the
 * render target started. False in the game view, and when no map shows. */
static bool lvGifMapView(ClipGifRect *slice, ClipGifBox *view, int *blitX,
                         int *blitY) {
    float src[4], dst[4], pxScale = 1.0f;
    if (g_lv == NULL || !g_lv->isLoaded || g_lv->gameView ||
        !lv_drawGetMapBlit(src, dst, &pxScale)) {
        return false;
    }
    float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
    lvHostGetMapArea(&ax, &ay, &aw, &ah);
    const ClipGifBox s = { src[0], src[1], src[0] + src[2], src[1] + src[3] };
    const ClipGifBox d = { dst[0], dst[1], dst[0] + dst[2], dst[1] + dst[3] };
    const ClipGifBox a = { ax, ay, ax + aw, ay + ah };
    if (!clipGifBlitView(&s, &d, &a, pxScale, slice, view)) {
        return false;
    }
    if (blitX != NULL) *blitX = (int)floorf(src[0] + 0.5f);
    if (blitY != NULL) *blitY = (int)floorf(src[1] + 0.5f);
    return true;
}

/* The gallery's metadata: the time now, the map, the players of the log and
 * the id of the log. */
static void lvGifFillMeta(ClipGifMeta *m, const char *mapName) {
    SDL_Time now = 0;
    int64_t created = 0;
    if (SDL_GetCurrentTime(&now)) {
        created = (int64_t)(now / SDL_NS_PER_SECOND);
    }
    clipGifMetaBegin(m, created, lvGifGameId(), mapName);

    static LvRosterRow rows[LV_ROSTER_MAX];
    lv_clientMutexWaitFor();
    int n = lv_screenGetRoster(rows, LV_ROSTER_MAX);
    lv_clientMutexRelease();
    for (int i = 0; i < n; i++) {
        clipGifMetaAddPlayer(m, rows[i].name);
    }
}

/* Starts a capture from the current time (or from the start of the preview
 * that plays) for the length the player set. The frames are the map as it is
 * shown now: the same zoom, the same place, the same camera, the same
 * colours, and they are the part of the map in the crop frame
 * (clipGifChooseCrop, as the game's replay has it). */
static void lvGifStart(void) {
    if (!lv_clip_gif_can_make()) {
        return;
    }
    /* The part of the map that shows, from the last blit, before anything
     * here draws the map again. */
    ClipGifRect shown = { 0, 0, 0, 0 };
    ClipGifBox  view  = { 0.0f, 0.0f, 0.0f, 0.0f };
    int blitX = 0, blitY = 0;
    const bool haveView = lvGifMapView(&shown, &view, &blitX, &blitY);

    if (!clipGifCaptureArm(&s_gif.session, &s_lvReplay)) {
        return;
    }
    uint32_t curMs = 0, totalMs = 0;
    lvGifProgress(NULL, &curMs, &totalMs);
    const uint32_t lenMs = clipGifLenClampMs(s_gif.mode.lenMs, curMs, totalMs);

    int srcX = 0, srcY = 0, texW = 0, texH = 0;
    lv_clientMutexWaitFor();
    lv_screenUpdate(redraw);
    lv_screenGetSubOffset(&srcX, &srcY);
    int srcW = (int)lv_screenGetSizeX() * TILE_SIZE_X;
    int srcH = (int)lv_screenGetSizeY() * TILE_SIZE_Y;
    lv_drawGetGameTargetSize(&texW, &texH);
    lv_clientMutexRelease();

    if (haveView) {
        /* Only the part that shows, from where the camera is now: the
         * drawing above can have moved it, and the crop frame names a place
         * on the view, not on the map. */
        srcX += shown.x - blitX;
        srcY += shown.y - blitY;
        srcW = shown.w;
        srcH = shown.h;
    }
    if (srcX < 0) srcX = 0;
    if (srcY < 0) srcY = 0;
    if (srcX + srcW > texW) srcW = texW - srcX;
    if (srcY + srcH > texH) srcH = texH - srcY;
    ClipGifRect c;
    const bool haveCrop =
        lv_drawGetGameTexture() != NULL &&
        clipGifChooseCrop(&s_gif.mode, srcX, srcY, srcW, srcH, &c);
    s_gif.targetW = texW;
    s_gif.targetH = texH;

    char mapName[64];
    mapName[0] = '\0';
    lv_screenGetMapName(mapName);
    mapName[sizeof(mapName) - 1] = '\0';
    ClipGifMeta meta;
    lvGifFillMeta(&meta, mapName);
    /* With no crop, this puts the log back as the arm found it. */
    clipGifCaptureOpen(&s_gif.session, &s_lvReplay, haveCrop ? &c : NULL,
                       curMs, lenMs, mapName, &meta);
}

/* ── The calls the host makes ─────────────────────────────────────── */

bool lv_clip_gif_available(void) {
    return true;
}

bool lv_clip_gif_can_toggle_mode(void) {
    return g_lv != NULL && g_lv->isLoaded && !lv_screenSpecIsLiveMode() &&
           !clipGifCaptureActive(&s_gif.session);
}

bool lv_clip_gif_mode_on(void) {
    return s_gif.mode.on;
}

void lv_clip_gif_toggle_mode(void) {
    if (lv_clip_gif_can_toggle_mode()) {
        clipGifModeToggle(&s_gif.mode, clipGifCaptureActive(&s_gif.session));
    }
}

bool lv_clip_gif_tools_shown(void) {
    return g_lv != NULL && g_lv->isLoaded && !lv_screenSpecIsLiveMode() &&
           clipGifToolsShown(s_gif.mode.on,
                             clipGifCaptureActive(&s_gif.session));
}

bool lv_clip_gif_can_make(void) {
    return lv_clip_gif_can_toggle_mode() && !g_lv->gameView;
}

bool lv_clip_gif_active(void) {
    return clipGifCaptureActive(&s_gif.session);
}

void lv_clip_gif_capture_step(void) {
    clipGifCaptureStep(&s_gif.session, &s_lvReplay);
}

/* ── The crop frame on the map ────────────────────────────────────── */

/* True when the crop frame shows on the map now (whenever the GIF controls
 * show), with *view the box of the map it is drawn on and *slice the
 * render-target pixels that show in it. */
static bool lvGifFrameView(ClipGifBox *view, ClipGifRect *slice) {
    if (!lv_clip_gif_tools_shown()) {
        return false;
    }
    return lvGifMapView(slice, view, NULL, NULL);
}

bool lv_clip_gif_frame_has_mouse(void) {
    if (s_gif.drag.dragging) {
        return true;
    }
    ClipGifBox  view;
    ClipGifRect slice;
    if (clipGifCaptureActive(&s_gif.session) ||
        !lvGifFrameView(&view, &slice)) {
        return false;
    }
    clipGifModeFitFrame(&s_gif.mode, slice.w, slice.h);
    const ImGuiIO &io = ImGui::GetIO();
    if (io.WantCaptureMouse || !ImGui::IsMousePosValid()) {
        return false;
    }
    const ClipGifBox box = clipGifFrameToBox(&s_gif.mode.frame, &view);
    return clipGifFrameHit(&box, io.MousePos.x, io.MousePos.y,
                           CLIP_GIF_CROP_GRAB_PX) != 0;
}

/* The crop frame over the map. The map is not an ImGui item (draw.c blits
 * it straight into the window), so the frame goes on the background draw
 * list, under every panel, and takes a press only where no panel is. */
static void lvGifRenderFrame(void) {
    ClipGifBox  view;
    ClipGifRect slice;
    if (!lvGifFrameView(&view, &slice)) {
        s_gif.drag.grab = 0;
        s_gif.drag.dragging = false;
        return;
    }
    clipGifCropFrame("##lvgifcrop", ImGui::GetBackgroundDrawList(),
                     ImVec2(view.x0, view.y0),
                     ImVec2(view.x1 - view.x0, view.y1 - view.y0),
                     &s_gif.mode, slice.w, slice.h, &s_gif.drag,
                     clipGifCaptureActive(&s_gif.session),
                     lv_imgui_game_viewport_mouse_held(), false);
}

/* ── The GIF controls in the Controls panel ───────────────────────── */

/* The GIF button's picture, made the first time it is drawn on a renderer.
 * 0 when the file is not there: the button then shows "GIF". */
static ImTextureID lvGifPicture(void) {
    if (g_lv == NULL || g_lv->renderer == NULL) {
        return (ImTextureID)0;
    }
    if (s_gif.pictureFor != g_lv->renderer) {
        /* The picture of another renderer went with that renderer. */
        s_gif.pictureFor = g_lv->renderer;
        s_gif.picture = imguiLoadSvgIconWhite(g_lv->renderer,
                                              "data/ui/picture.svg",
                                              LV_GIF_PICTURE_PX);
        if (s_gif.picture == NULL) {
            /* An installed build finds data/ from where the program is, not
             * from the working directory. */
            const char *base = SDL_GetBasePath();
            if (base != NULL) {
                char path[1024];
                SDL_snprintf(path, sizeof(path), "%sdata/ui/picture.svg", base);
                s_gif.picture = imguiLoadSvgIconWhite(g_lv->renderer, path,
                                                      LV_GIF_PICTURE_PX);
            }
        }
        if (s_gif.picture != NULL) {
            SDL_SetTextureScaleMode(s_gif.picture, SDL_SCALEMODE_LINEAR);
        }
    }
    return (ImTextureID)(intptr_t)s_gif.picture;
}

void lv_clip_gif_tools_row(void) {
    if (!lv_clip_gif_tools_shown()) {
        return;
    }
    uint32_t curMs = 0, totalMs = 0;
    lvGifProgress(NULL, &curMs, &totalMs);
    const ImTextureID picture = lvGifPicture();

    /* The row in the middle of the panel, as the panel's other rows are. */
    const float rowW = clipGifToolsRowWidth(picture != (ImTextureID)0);
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > rowW) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + floorf((avail - rowW) * 0.5f));
    }

    /* Preview clip and Save clip from the current time, of the map as it is
     * shown; the length, the Clips window and the crop frame. The same row
     * the game's replay shows. */
    const ClipGifToolsPressed tools = clipGifToolsRow(
        "##lvgif", &s_gif.mode, &s_gif.session.play,
        clipGifCaptureActive(&s_gif.session), lv_clip_gif_can_make(), picture,
        curMs, totalMs);
    if (tools.preview) {
        if (clipGifPreviewActive(&s_gif.session)) {
            clipGifPreviewStop(&s_gif.session, &s_lvReplay);
        } else {
            clipGifPreviewStart(&s_gif.session, &s_lvReplay, s_gif.mode.lenMs);
        }
    }
    if (tools.save) {
        lvGifStart();
    }
    if (tools.gallery) {
        clipGifGalleryOpen();
    }
}

void lv_clip_gif_seek_marker(float minX, float minY, float maxX, float maxY,
                             uint32_t startMs, uint32_t totalMs) {
    if (!lv_clip_gif_tools_shown()) {
        return;
    }
    /* While a preview plays, the band stays on the clip it plays, and the
     * length does not change under it. */
    clipGifLenMarker("##lvgiflenmark", ImVec2(minX, minY), ImVec2(maxX, maxY),
                     clipGifPreviewBandStart(&s_gif.session, startMs), totalMs,
                     &s_gif.mode.lenMs,
                     clipGifCaptureActive(&s_gif.session) ||
                         clipGifPreviewActive(&s_gif.session));
}

/* ── Windows ──────────────────────────────────────────────────────── */

void lv_clip_gif_render(void) {
    if (g_lv == NULL) {
        return;
    }
    const float s = ImGui::GetFontSize() / 18.0f;
    /* The preview ends at the end of its clip, and lets go when the player
     * pauses or moves the log, or turns recording mode off. */
    clipGifPreviewPoll(&s_gif.session, &s_lvReplay,
                       s_gif.mode.on && lv_clip_gif_tools_shown());
    clipGifGalleryPoll();
    lvGifRenderFrame();
    clipGifCaptureRender(&s_gif.session, &s_lvReplay, s);

    ClipGifGalleryHost host;
    host.renderer  = g_lv->renderer;
    host.window    = g_lv->window;
    host.gameId    = clipGifGalleryIsOpen() ? lvGifGameId() : "";
    host.emptyText = STR_LV_CLIPS_EMPTY;
    clipGifGalleryRender(s, &host);
}

void lv_clip_gif_toggle_gallery(void) {
    if (clipGifGalleryIsOpen()) {
        clipGifGalleryClose();
    } else {
        clipGifGalleryOpen();
    }
}

bool lv_clip_gif_gallery_is_open(void) {
    return clipGifGalleryIsOpen();
}

void lv_clip_gif_cancel(void) {
    /* The log is not moved back: another log is about to open. */
    clipGifSessionReset(&s_gif.session, &s_lvReplay, false);
    /* A new log starts with recording mode off, as a new replay does in the
     * game. */
    clipGifModeReset(&s_gif.mode);
    s_gif.drag.grab = 0;
    s_gif.drag.dragging = false;
    lvGifForgetGameId();
}

void lv_clip_gif_session_end(void) {
    lv_clip_gif_cancel();
    /* Only on the renderer it was made on, while that renderer is still
     * there; the picture of a renderer that is gone went with it. */
    if (s_gif.picture != NULL && g_lv != NULL &&
        g_lv->renderer == s_gif.pictureFor) {
        SDL_DestroyTexture(s_gif.picture);
    }
    s_gif.picture = nullptr;
    s_gif.pictureFor = nullptr;
    clipGifGalleryShutdown();
}

#else /* !CLIP_GIF_GALLERY_BUILT */

bool lv_clip_gif_available(void) { return false; }
bool lv_clip_gif_can_toggle_mode(void) { return false; }
bool lv_clip_gif_mode_on(void) { return false; }
void lv_clip_gif_toggle_mode(void) {}
bool lv_clip_gif_tools_shown(void) { return false; }
bool lv_clip_gif_can_make(void) { return false; }
bool lv_clip_gif_active(void) { return false; }
void lv_clip_gif_capture_step(void) {}
void lv_clip_gif_render(void) {}
bool lv_clip_gif_frame_has_mouse(void) { return false; }
void lv_clip_gif_tools_row(void) {}
void lv_clip_gif_seek_marker(float minX, float minY, float maxX, float maxY,
                             uint32_t startMs, uint32_t totalMs) {
    (void)minX; (void)minY; (void)maxX; (void)maxY;
    (void)startMs; (void)totalMs;
}
void lv_clip_gif_toggle_gallery(void) {}
bool lv_clip_gif_gallery_is_open(void) { return false; }
void lv_clip_gif_cancel(void) {}
void lv_clip_gif_session_end(void) {}

#endif /* CLIP_GIF_GALLERY_BUILT */
