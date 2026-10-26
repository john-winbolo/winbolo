/*
 * imgui_clip_gif.cpp - GIFs of the map and the Clips window in the Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * See imgui_clip_gif.h. The GIF is made with the code the game's replay uses:
 * the frame count, the crop and the file name come from clip_gif_core.h, each
 * frame is read back and encoded by clip_gif_frame.h, and the finished bytes
 * go to the gallery (clip_gif_gallery.h), which writes them into the clips
 * folder with their metadata file.
 */

#include "imgui_clip_gif.h"

#include "../../gui/sdl3/dialogs/clip_gif_gallery.h"  /* CLIP_GIF_GALLERY_BUILT */

#if CLIP_GIF_GALLERY_BUILT

#include "imgui.h"
#include "../../gui/lang.h"
#include "../../gui/sdl3/dialogs/clip_gif_core.h"
#include "../../gui/sdl3/dialogs/clip_gif_frame.h"
#include "../../gui/sdl3/dialogs/dialog_footer.h"
#include <SDL3/SDL.h>

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

/* Log ticks for each frame. The log clock is 20 ms a tick, so five ticks are
 * CLIP_GIF_FRAME_MS (100 ms, 10 frames a second), as in the game's replay. */
static const int LV_GIF_TICKS_PER_FRAME = 5;
/* Frames made in each frame of the viewer. As in the game's replay: the
 * longest GIF takes under a second or two, and the window still updates. */
static const int LV_GIF_FRAMES_PER_PASS = 4;

static const char *const LV_GIF_MAKE_POPUP     = "###lvgifmake";
static const char *const LV_GIF_PROGRESS_POPUP = "GIF##lvgifprogress";

typedef struct LvGifState {
    /* Make GIF... was chosen: the length window opens on the next frame. */
    bool        wantMakeWindow = false;
    /* The length the window shows, kept for the next GIF of this session. */
    uint32_t    lenMs          = CLIP_GIF_LEN_DEFAULT_MS;

    /* One capture, from the start to the bytes given to the gallery. */
    bool        active         = false;
    bool        failed         = false;
    /* True when the frames are all made: the next render ends it. */
    bool        done           = false;
    MsfGifState enc            = {};
    int         frame          = 0;
    int         total          = 0;
    SDL_Rect    crop           = { 0, 0, 0, 0 };
    /* The size of the render target when the capture started. A zoom or a
     * window size change makes a new target, and the crop then means
     * nothing, so the capture stops. */
    int         targetW        = 0;
    int         targetH        = 0;
    /* What the capture changed, to put back after it. */
    uint32_t    restoreMs      = 0;  /* the log time, as an absolute time */
    bool        restorePlaying = false;
    bool        restoreSound   = false;
    char        name[96]       = "";
    ClipGifMeta meta           = {};

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

/* ── The capture ──────────────────────────────────────────────────── */

/* The log time now and the length of the log, both from the start of the
 * part of the log the viewer shows (as the Controls panel shows them). */
static void lvGifProgress(uint32_t *curMs, uint32_t *totalMs) {
    size_t pos = 0, size = 0;
    uint32_t cur = 0, total = 0;
    lv_clientMutexWaitFor();
    lv_screenGetLogProgress(&pos, &size, &cur, &total);
    lv_clientMutexRelease();
    *curMs   = cur;
    *totalMs = total;
}

/* Puts back what the capture changed. restoreTime false leaves the log where
 * it is: another log is about to open. */
static void lvGifRestore(bool restoreTime) {
    lv_clientMutexWaitFor();
    lv_drawSetTankLabelsInTarget(FALSE);
    if (restoreTime) {
        lv_drawDirtyScreen();
        lv_screenSeekToTimeMs(s_gif.restoreMs);
        lv_drawDirtyScreen();
    }
    lv_clientMutexRelease();
    if (g_lv != NULL) {
        g_lv->isSoundsPlaying = s_gif.restoreSound ? TRUE : FALSE;
        g_lv->wantScreenUpdate = TRUE;
    }
    lv_windowNeedRedraw();
    if (restoreTime && s_gif.restorePlaying) {
        lv_windowPlay();
    }
}

/* Ends the encoder. It holds memory from msf_gif_begin on, and only
 * msf_gif_end gives it back, so every way out of a capture comes here.
 * outResult takes the bytes when the caller saves them (and then frees them
 * with msf_gif_free); NULL drops them. */
static void lvGifFinish(MsfGifResult *outResult, bool restoreTime) {
    MsfGifResult res = msf_gif_end(&s_gif.enc);
    if (outResult != NULL) {
        *outResult = res;
    } else {
        msf_gif_free(res);
    }
    lvGifRestore(restoreTime);
    s_gif.active = false;
    s_gif.failed = false;
    s_gif.done   = false;
    s_gif.frame  = 0;
    s_gif.total  = 0;
}

/* The gallery's metadata: the time now, the map, the players of the log and
 * the id of the log. */
static void lvGifFillMeta(ClipGifMeta *m, const char *mapName) {
    *m = ClipGifMeta{};
    SDL_Time now = 0;
    if (SDL_GetCurrentTime(&now)) {
        m->createdUnix = (int64_t)(now / SDL_NS_PER_SECOND);
    }
    SDL_strlcpy(m->game, lvGifGameId(), sizeof(m->game));
    clipGifMetaSetMap(m, mapName);

    static LvRosterRow rows[LV_ROSTER_MAX];
    lv_clientMutexWaitFor();
    int n = lv_screenGetRoster(rows, LV_ROSTER_MAX);
    lv_clientMutexRelease();
    for (int i = 0; i < n && m->playerCount < CLIP_GIF_META_MAX_PLAYERS; i++) {
        if (rows[i].name[0] != '\0') {
            SDL_utf8strlcpy(m->players[m->playerCount], rows[i].name,
                            sizeof(m->players[0]));
            m->playerCount++;
        }
    }
}

/* Starts a capture of lenMs from the current time. The frames are the middle
 * of the map as it is shown now: the same zoom, the same place, the same
 * camera, the same colours. */
static bool lvGifStart(uint32_t lenMs) {
    if (!lv_clip_gif_can_make()) {
        return false;
    }
    uint32_t curMs = 0, totalMs = 0;
    lvGifProgress(&curMs, &totalMs);
    lenMs = clipGifLenClampMs(lenMs, curMs, totalMs);

    s_gif.restoreMs      = lv_screenWindowStartMs() + curMs;
    s_gif.restorePlaying = g_lv->playIsPlaying ? true : false;
    s_gif.restoreSound   = g_lv->isSoundsPlaying ? true : false;
    lv_windowPause();
    /* The ticks of a capture go by many times faster than real time, and
     * their sounds would all play at once. */
    g_lv->isSoundsPlaying = FALSE;

    /* The names over the tanks go into the render target, which is what is
     * read back. lvGifRestore takes them out again. */
    int srcX = 0, srcY = 0, texW = 0, texH = 0;
    lv_clientMutexWaitFor();
    lv_drawSetTankLabelsInTarget(TRUE);
    lv_screenUpdate(redraw);
    lv_screenGetSubOffset(&srcX, &srcY);
    int srcW = (int)lv_screenGetSizeX() * TILE_SIZE_X;
    int srcH = (int)lv_screenGetSizeY() * TILE_SIZE_Y;
    lv_drawGetGameTargetSize(&texW, &texH);
    lv_clientMutexRelease();

    if (srcX + srcW > texW) srcW = texW - srcX;
    if (srcY + srcH > texH) srcH = texH - srcY;
    ClipGifRect c;
    if (lv_drawGetGameTexture() == NULL ||
        !clipGifCentreCrop(srcX, srcY, srcW, srcH, CLIP_GIF_MAX_WIDTH, &c) ||
        !msf_gif_begin(&s_gif.enc, c.w, c.h)) {
        lvGifRestore(true);
        return false;
    }
    s_gif.crop.x  = c.x;
    s_gif.crop.y  = c.y;
    s_gif.crop.w  = c.w;
    s_gif.crop.h  = c.h;
    s_gif.targetW = texW;
    s_gif.targetH = texH;

    uint32_t frames = clipGifFrameCount(lenMs);
    char mapName[64];
    mapName[0] = '\0';
    lv_screenGetMapName(mapName);
    mapName[sizeof(mapName) - 1] = '\0';
    unsigned secs = (unsigned)(curMs / 1000u);
    clipGifBaseName(s_gif.name, sizeof(s_gif.name), mapName, secs / 60u,
                    secs % 60u);
    lvGifFillMeta(&s_gif.meta, mapName);
    s_gif.meta.durationMs = frames * CLIP_GIF_FRAME_MS;
    s_gif.meta.width      = c.w;
    s_gif.meta.height     = c.h;

    s_gif.frame  = 0;
    s_gif.total  = (int)frames;
    s_gif.failed = false;
    s_gif.done   = false;
    s_gif.active = true;
    return true;
}

/* One frame: the log moves on one frame's ticks, the map is drawn into the
 * render target, and the crop is read back into the encoder. False when the
 * frame cannot be made. *ended is set when the log has no more to play. */
static bool lvGifCaptureFrame(bool *ended) {
    *ended = false;
    int texW = 0, texH = 0;
    lv_clientMutexWaitFor();
    for (int i = 0; i < LV_GIF_TICKS_PER_FRAME; i++) {
        lv_screenLogTick();
    }
    lv_screenUpdate(redraw);
    lv_drawGetGameTargetSize(&texW, &texH);
    bool playing = g_lv->isPlaying ? true : false;
    lv_clientMutexRelease();

    if (texW != s_gif.targetW || texH != s_gif.targetH) {
        return false;
    }
    if (!clipGifEncodeFrame(g_lv->renderer, lv_drawGetGameTexture(),
                            &s_gif.crop, &s_gif.enc)) {
        return false;
    }
    *ended = !playing;
    return true;
}

/* ── The calls the host makes ─────────────────────────────────────── */

bool lv_clip_gif_available(void) {
    return true;
}

bool lv_clip_gif_can_make(void) {
    return g_lv != NULL && g_lv->isLoaded && !g_lv->gameView &&
           !lv_screenSpecIsLiveMode() && !s_gif.active;
}

void lv_clip_gif_request(void) {
    if (lv_clip_gif_can_make()) {
        s_gif.wantMakeWindow = true;
    }
}

bool lv_clip_gif_active(void) {
    return s_gif.active;
}

void lv_clip_gif_capture_step(void) {
    if (!s_gif.active || s_gif.done || s_gif.failed) {
        return;
    }
    if (g_lv == NULL || !g_lv->isLoaded || g_lv->gameView) {
        s_gif.failed = true;
        return;
    }
    for (int i = 0; i < LV_GIF_FRAMES_PER_PASS && s_gif.frame < s_gif.total;
         i++) {
        bool ended = false;
        if (!lvGifCaptureFrame(&ended)) {
            /* A failed readback stops the GIF here, rather than writing a
             * GIF with a part of the time not in it. */
            s_gif.failed = true;
            return;
        }
        s_gif.frame++;
        if (ended) {
            /* The log ended in the GIF: keep what there is. */
            s_gif.total = s_gif.frame;
            s_gif.meta.durationMs = (uint32_t)s_gif.frame * CLIP_GIF_FRAME_MS;
            break;
        }
    }
    if (s_gif.frame >= s_gif.total) {
        s_gif.done = true;
    }
}

/* The Make GIF window: the length, and Make GIF or Cancel. */
static void lvGifRenderMakeWindow(float s) {
    if (s_gif.wantMakeWindow) {
        s_gif.wantMakeWindow = false;
        if (lv_clip_gif_can_make()) {
            ImGui::OpenPopup(LV_GIF_MAKE_POPUP);
        }
    }
    /* The id is the part after "###", so the title can be in any language
     * and still be the popup OpenPopup opened. */
    char title[128];
    std::snprintf(title, sizeof(title), "%s%s", langGetText(STR_LV_GIF_START),
                  LV_GIF_MAKE_POPUP);
    if (!ImGui::BeginPopupModal(title, NULL,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoCollapse |
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    uint32_t curMs = 0, totalMs = 0;
    lvGifProgress(&curMs, &totalMs);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280.0f * s);
    ImGui::TextUnformatted(langGetText(STR_LV_MAKE_GIF_TIP));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    ImGui::TextUnformatted(langGetText(STR_LV_GIF_LENGTH));
    float secs = clipGifLenToSeconds(
        clipGifLenClampMs(s_gif.lenMs, curMs, totalMs));
    ImGui::SetNextItemWidth(280.0f * s);
    if (ImGui::SliderFloat("##lvgiflen", &secs,
                           (float)CLIP_GIF_LEN_MIN_MS / 1000.0f,
                           (float)CLIP_GIF_LEN_MAX_MS / 1000.0f, "%.1f")) {
        s_gif.lenMs = clipGifLenFromSeconds(secs, 0, 0);
    }
    ImGui::Spacing();

    int r = WBUI::DialogFooter(langGetText(STR_CANCEL),
                               langGetText(STR_LV_GIF_START), false, true,
                               !lv_clip_gif_can_make());
    if (r == WBUI::FOOTER_CONFIRM) {
        ImGui::CloseCurrentPopup();
        lvGifStart(s_gif.lenMs);
    } else if (r == WBUI::FOOTER_CANCEL) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* The window shown while a GIF is made: how far it has got, and Cancel. */
static void lvGifRenderProgress(float s) {
    if (!s_gif.active) {
        return;
    }
    if (!ImGui::IsPopupOpen(LV_GIF_PROGRESS_POPUP)) {
        ImGui::OpenPopup(LV_GIF_PROGRESS_POPUP);
    }
    MsfGifResult finished = {};
    bool haveFinished = false;
    if (ImGui::BeginPopupModal(LV_GIF_PROGRESS_POPUP, NULL,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoSavedSettings)) {
        /* The same width for both: in a window that sizes itself, a width
         * of -1 follows the window down to the width of the button. */
        const float rowW = 280.0f * s;
        float part = (s_gif.total > 0)
                         ? (float)s_gif.frame / (float)s_gif.total
                         : 0.0f;
        ImGui::ProgressBar(part, ImVec2(rowW, 0.0f));
        WBUI::PushCancelStyle();
        bool cancel = ImGui::Button(langGetText(STR_CANCEL),
                                    ImVec2(rowW, 0.0f));
        WBUI::PopCancelStyle();
        if (WBUI::CancelKeyPressed()) {
            cancel = true;
        }
        if (cancel || s_gif.failed) {
            lvGifFinish(NULL, true);
            ImGui::CloseCurrentPopup();
        } else if (s_gif.done) {
            lvGifFinish(&finished, true);
            haveFinished = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (haveFinished) {
        if (finished.data != NULL) {
            /* Writes the GIF and its metadata into the clips folder, and
             * opens the Clips window on it. */
            clipGifGalleryAdd(finished.data, finished.dataSize, &s_gif.meta,
                              s_gif.name);
        }
        msf_gif_free(finished);
    }
}

void lv_clip_gif_render(void) {
    if (g_lv == NULL) {
        return;
    }
    const float s = ImGui::GetFontSize() / 18.0f;
    clipGifGalleryPoll();
    lvGifRenderMakeWindow(s);
    lvGifRenderProgress(s);

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
    s_gif.wantMakeWindow = false;
    if (s_gif.active) {
        lvGifFinish(NULL, false);
    }
    lvGifForgetGameId();
}

void lv_clip_gif_session_end(void) {
    lv_clip_gif_cancel();
    clipGifGalleryShutdown();
}

#else /* !CLIP_GIF_GALLERY_BUILT */

bool lv_clip_gif_available(void) { return false; }
bool lv_clip_gif_can_make(void) { return false; }
void lv_clip_gif_request(void) {}
bool lv_clip_gif_active(void) { return false; }
void lv_clip_gif_capture_step(void) {}
void lv_clip_gif_render(void) {}
void lv_clip_gif_toggle_gallery(void) {}
bool lv_clip_gif_gallery_is_open(void) { return false; }
void lv_clip_gif_cancel(void) {}
void lv_clip_gif_session_end(void) {}

#endif /* CLIP_GIF_GALLERY_BUILT */
