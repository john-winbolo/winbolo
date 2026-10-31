/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 * Name:          clip_gif_capture.cpp
 * Purpose:       The Preview clip playback and the Save clip
 *                capture of the game's replay and the Log
 *                Viewer. See clip_gif_capture.h.
 *********************************************************/

#include <cmath>    /* fabsf / floorf */
#include <cstdio>   /* snprintf / FILENAME_MAX */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"  /* GImGui->MovingWindow: a drag of the GIF window */
#include "clip_gif_capture.h"
#include "clip_gif_controls.h"  /* CLIP_GIF_BUTTON_TITLE */
#include "clip_gif_gallery.h"   /* CLIP_GIF_GALLERY_BUILT, clipGifGalleryAdd */
#include "dialog_footer.h"      /* WBUI cancel styling + CancelKeyPressed */
extern "C" {
#include "../../lang.h"         /* langGetText / STR_CANCEL / STR_CLIPGIF_* */
}
#if defined(__IPHONEOS__) || defined(__ANDROID__)
extern "C" {
#include "imgui_messagebox.h"   /* imguiMessageBoxEx: where the file went */
}
#endif
#if defined(__ANDROID__)
#include "../../../android/android_share.h"  /* androidShareFile */
#endif

/* The progress window. Its title is the format's name, not a word to
 * translate. */
static const char *const CLIP_GIF_PROGRESS_POPUP = "GIF##clipgifcapture";

/* ── The session ──────────────────────────────────────────────────── */

/* Puts back what the capture changed: captureMode off, and, with
 * restoreTime, the time it was at, playing again if it was playing. */
static void clipGifRestore(ClipGifSession *s, const ClipGifReplay *r,
                           bool restoreTime) {
    if (r == NULL) {
        return;
    }
    if (r->captureMode != NULL) {
        r->captureMode(r->user, false);
    }
    if (restoreTime) {
        if (r->seek != NULL) {
            r->seek(r->user, s->restoreMs);
        }
        if (s->restorePlaying && r->play != NULL) {
            r->play(r->user);
        }
    }
}

/* Ends the encoder however the capture ended. It holds memory from
 * msf_gif_begin on and only msf_gif_end gives it back, so every way out of
 * a capture comes here. outResult takes the bytes when the caller saves
 * them (and then frees them with msf_gif_free); NULL drops them. */
static void clipGifFinish(ClipGifSession *s, const ClipGifReplay *r,
                          MsfGifResult *outResult, bool restoreTime) {
    MsfGifResult res = msf_gif_end(&s->enc);
    if (outResult != NULL) {
        *outResult = res;
    } else {
        msf_gif_free(res);
    }
    clipGifRestore(s, r, restoreTime);
    s->active = false;
    s->failed = false;
    s->done   = false;
    s->frame  = 0;
    s->total  = 0;
}

void clipGifSessionReset(ClipGifSession *s, const ClipGifReplay *r,
                         bool restoreTime) {
    if (s == NULL) {
        return;
    }
    if (s->active) {
        clipGifFinish(s, r, NULL, restoreTime);
    }
    /* Back to the start values, encoder included. The finish above is what
     * makes this safe: a live encoder's buffers would be lost if they were
     * written over instead of ended. */
    *s = ClipGifSession{};
}

bool clipGifCaptureActive(const ClipGifSession *s) {
    return s != NULL && s->active;
}

/* ── Preview clip ────────────────────────────────────────────────── */

bool clipGifPreviewActive(const ClipGifSession *s) {
    return s != NULL && s->play.active;
}

bool clipGifPreviewStart(ClipGifSession *s, const ClipGifReplay *r,
                         uint32_t lenMs) {
    if (s == NULL || r == NULL || s->active || r->progress == NULL) {
        return false;
    }
    uint32_t curMs = 0, totalMs = 0;
    r->progress(r->user, &curMs, &totalMs);
    if (!clipGifPlayStart(&s->play, curMs, lenMs, totalMs)) {
        return false;
    }
    if (r->play != NULL) {
        r->play(r->user);
    }
    return true;
}

bool clipGifPreviewSpan(ClipGifSession *s, const ClipGifReplay *r,
                        const ClipGifMode *mode) {
    if (s == NULL || r == NULL || mode == NULL || s->active ||
        r->progress == NULL) {
        return false;
    }
    uint32_t curMs = 0, totalMs = 0;
    r->progress(r->user, &curMs, &totalMs);
    uint32_t startMs = 0, lenMs = 0;
    clipGifSpanClip(mode, curMs, totalMs, &startMs, &lenMs);
    if (startMs != curMs && r->seek != NULL) {
        r->seek(r->user, startMs);
    }
    return clipGifPreviewStart(s, r, lenMs);
}

/* The replay stops and goes back to the start of the clip. */
static void clipGifPreviewBack(ClipGifSession *s, const ClipGifReplay *r) {
    if (r->pause != NULL) {
        r->pause(r->user);
    }
    if (r->seek != NULL) {
        r->seek(r->user, s->play.startMs);
    }
}

bool clipGifPreviewToggle(ClipGifSession *s, const ClipGifReplay *r,
                          const ClipGifMode *mode) {
    if (clipGifPreviewActive(s)) {
        clipGifPreviewStop(s, r);
        return false;
    }
    return clipGifPreviewSpan(s, r, mode);
}

void clipGifPreviewStop(ClipGifSession *s, const ClipGifReplay *r) {
    if (s == NULL || !s->play.active) {
        return;
    }
    clipGifPlayStop(&s->play);
    if (r != NULL) {
        clipGifPreviewBack(s, r);
    }
}

void clipGifPreviewPoll(ClipGifSession *s, const ClipGifReplay *r, bool keep) {
    if (s == NULL || r == NULL || !s->play.active) {
        return;
    }
    if (!keep || r->progress == NULL || r->isPlaying == NULL) {
        clipGifPlayStop(&s->play);
        return;
    }
    uint32_t curMs = 0, totalMs = 0;
    r->progress(r->user, &curMs, &totalMs);
    if (clipGifPlayUpdate(&s->play, curMs, r->isPlaying(r->user)) ==
        CLIP_GIF_PLAY_END) {
        clipGifPreviewBack(s, r);
    }
}

uint32_t clipGifPreviewBandStart(const ClipGifSession *s, uint32_t curMs) {
    return clipGifPlayBandStart(s != NULL ? &s->play : NULL, curMs);
}

/* ── Save clip ───────────────────────────────────────────────────── */

bool clipGifCaptureArm(ClipGifSession *s, const ClipGifReplay *r) {
    if (s == NULL || r == NULL || s->active) {
        return false;
    }
    /* The GIF of the clip the preview plays: from its start. */
    clipGifPreviewStop(s, r);
    uint32_t curMs = 0, totalMs = 0;
    if (r->progress != NULL) {
        r->progress(r->user, &curMs, &totalMs);
    }
    s->restoreMs      = curMs;
    s->restorePlaying = (r->isPlaying != NULL) && r->isPlaying(r->user);
    if (r->pause != NULL) {
        r->pause(r->user);
    }
    if (r->captureMode != NULL) {
        r->captureMode(r->user, true);
    }
    return true;
}

bool clipGifCaptureOpen(ClipGifSession *s, const ClipGifReplay *r,
                        const ClipGifRect *crop, uint32_t startMs,
                        uint32_t lenMs, const char *mapName,
                        const ClipGifMeta *meta) {
    if (s == NULL || s->active) {
        return false;
    }
    if (crop == NULL || crop->w <= 0 || crop->h <= 0 ||
        !msf_gif_begin(&s->enc, crop->w, crop->h)) {
        clipGifRestore(s, r, true);
        return false;
    }
    s->crop.x = crop->x;
    s->crop.y = crop->y;
    s->crop.w = crop->w;
    s->crop.h = crop->h;

    /* From the length in ms, not in ticks: the replay steps a log clock and
     * a highlight is measured in sim ticks, and the two do not share a
     * rate. */
    const uint32_t frames = clipGifFrameCount(lenMs);
    const unsigned secs = (unsigned)(startMs / 1000u);
    clipGifBaseName(s->name, sizeof(s->name), mapName, secs / 60u, secs % 60u);
    if (meta != NULL) {
        s->meta = *meta;
    } else {
        s->meta = ClipGifMeta{};
    }
    s->meta.durationMs = frames * CLIP_GIF_FRAME_MS;
    s->meta.width      = crop->w;
    s->meta.height     = crop->h;
    s->frame  = 0;
    s->total  = (int)frames;
    s->failed = false;
    s->done   = false;
    s->active = true;
    return true;
}

void clipGifMetaBeginNow(ClipGifMeta *m, const char *gameId,
                         const char *mapName) {
    SDL_Time now = 0;
    int64_t created = 0;
    if (SDL_GetCurrentTime(&now)) {
        created = (int64_t)(now / SDL_NS_PER_SECOND);
    }
    clipGifMetaBegin(m, created, gameId, mapName);
}

bool clipGifCaptureStart(ClipGifSession *s, const ClipGifReplay *r,
                         ClipGifMode *mode, const ClipGifStartFrom *from,
                         const char *mapName, const ClipGifMeta *meta) {
    if (s == NULL || r == NULL || mode == NULL || from == NULL ||
        from->view == NULL || r->progress == NULL ||
        !clipGifCaptureArm(s, r)) {
        return false;
    }
    uint32_t startMs = from->startMs, lenMs = from->lenMs;
    if (from->seekClip == NULL) {
        uint32_t curMs = 0, totalMs = 0;
        r->progress(r->user, &curMs, &totalMs);
        clipGifSpanClip(mode, curMs, totalMs, &startMs, &lenMs);
        if (startMs != curMs && r->seek != NULL) {
            r->seek(r->user, startMs);
        }
    } else {
        from->seekClip(from->user, startMs);
    }

    ClipGifRect shown = { 0, 0, 0, 0 };
    ClipGifRect crop  = { 0, 0, 0, 0 };
    int texW = 0, texH = 0;
    bool haveCrop = false;
    if (from->view(from->user, &shown, &texW, &texH)) {
        /* Inside the render target: the crop is read back as it is for
         * every frame. */
        if (shown.x < 0) shown.x = 0;
        if (shown.y < 0) shown.y = 0;
        if (shown.x + shown.w > texW) shown.w = texW - shown.x;
        if (shown.y + shown.h > texH) shown.h = texH - shown.y;
        haveCrop = shown.w > 0 && shown.h > 0 &&
                   clipGifChooseCrop(mode, shown.x, shown.y, shown.w,
                                     shown.h, &crop);
    }
    /* With no crop, this puts the replay back as the arm found it. */
    return clipGifCaptureOpen(s, r, haveCrop ? &crop : NULL, startMs, lenMs,
                              mapName, meta);
}

void clipGifCaptureStep(ClipGifSession *s, const ClipGifReplay *r) {
    if (s == NULL || !s->active || s->done || s->failed) {
        return;
    }
    for (int i = 0; i < CLIP_GIF_FRAMES_PER_PASS && s->frame < s->total; i++) {
        bool ended = false;
        SDL_Renderer *renderer = NULL;
        SDL_Texture  *texture  = NULL;
        if (r == NULL || r->advance == NULL || r->target == NULL ||
            !r->advance(r->user, &ended) ||
            !r->target(r->user, &renderer, &texture) ||
            !clipGifEncodeFrame(renderer, texture, &s->crop, &s->enc)) {
            /* A failed readback stops the GIF here, rather than writing a
             * GIF with a part of the time not in it. */
            s->failed = true;
            return;
        }
        s->frame++;
        if (ended) {
            /* The replay ended in the GIF: keep what there is. */
            s->total = s->frame;
            s->meta.durationMs = (uint32_t)s->frame * CLIP_GIF_FRAME_MS;
            break;
        }
    }
    if (s->frame >= s->total) {
        s->done = true;
    }
}

void clipGifCaptureRender(ClipGifSession *s, const ClipGifReplay *r,
                          float uiScale) {
    if (s == NULL || !s->active) {
        return;
    }
    /* Opened again each frame the capture is live, from the screen's own
     * render: a popup's id comes from the window it is opened in, and the
     * game's replay is drawn from two different containers. */
    if (!ImGui::IsPopupOpen(CLIP_GIF_PROGRESS_POPUP)) {
        ImGui::OpenPopup(CLIP_GIF_PROGRESS_POPUP);
    }
    MsfGifResult finished = {};
    bool haveFinished = false;
    if (ImGui::BeginPopupModal(CLIP_GIF_PROGRESS_POPUP, NULL,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoSavedSettings)) {
        /* The same width for both: in a window that sizes itself, a width
         * of -1 follows the window down to the width of the button. */
        const float rowW = 280.0f * uiScale;
        const float part = (s->total > 0)
                               ? (float)s->frame / (float)s->total
                               : 0.0f;
        ImGui::ProgressBar(part, ImVec2(rowW, 0.0f));
        WBUI::PushCancelStyle();
        bool cancel = ImGui::Button(langGetText(STR_CANCEL),
                                    ImVec2(rowW, 0.0f));
        WBUI::PopCancelStyle();
        if (WBUI::CancelKeyPressed()) {
            cancel = true;
        }
        if (cancel || s->failed) {
            clipGifFinish(s, r, NULL, true);
            ImGui::CloseCurrentPopup();
        } else if (s->done) {
            clipGifFinish(s, r, &finished, true);
            haveFinished = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (haveFinished) {
        if (finished.data != NULL) {
            clipGifSaveGif(finished.data, finished.dataSize, &s->meta,
                           s->name);
        }
        msf_gif_free(finished);
    }
}

/* ── The zoomed timeline ─────────────────────────────────────────── */

/* The playhead follows a press or a drag on the zoomed timeline (see
 * ClipGifScrub). */
static void clipGifScrubUpdate(ClipGifScrub *z, const ClipGifReplay *r,
                               const ClipGifZoomPressed *p, double now) {
    if (r->seek == NULL) {
        return;
    }
    if (p->held) {
        if (!z->on) {
            /* Paused for the drag, so each seek is not fought by the
             * playback timers. */
            uint32_t curMs = 0, totalMs = 0;
            r->progress(r->user, &curMs, &totalMs);
            z->on         = true;
            z->wasPlaying = (r->isPlaying != NULL) && r->isPlaying(r->user);
            z->appliedMs  = curMs;
            z->backAt     = now - CLIP_GIF_SCRUB_BACK_SECS;
            if (z->wasPlaying && r->pause != NULL) {
                r->pause(r->user);
            }
        }
        bool apply = false;
        if (p->seekMs > z->appliedMs) {
            apply = true;
        } else if (p->seekMs < z->appliedMs &&
                   now - z->backAt >= CLIP_GIF_SCRUB_BACK_SECS) {
            apply = true;
            z->backAt = now;
        }
        if (apply) {
            r->seek(r->user, p->seekMs);
            z->appliedMs = p->seekMs;
        }
        return;
    }
    if (!z->on) {
        return;
    }
    /* Let go: on the exact time, and playing again when it played. */
    z->on = false;
    if (p->seek && p->seekMs != z->appliedMs) {
        r->seek(r->user, p->seekMs);
    }
    if (z->wasPlaying && r->play != NULL) {
        r->play(r->user);
    }
    z->wasPlaying = false;
}

void clipGifZoomRow(const char *id, ClipGifSession *s, const ClipGifReplay *r,
                    ClipGifMode *mode, float width) {
    if (s == NULL || r == NULL || mode == NULL || r->progress == NULL) {
        return;
    }
    uint32_t curMs = 0, totalMs = 0;
    r->progress(r->user, &curMs, &totalMs);
    const ClipGifZoomPressed p =
        clipGifZoomBar(id, width, mode, curMs, totalMs,
                       s->active || s->play.active, s->active);
    clipGifScrubUpdate(&s->scrub, r, &p, ImGui::GetTime());
}

/* ── The GIF window ──────────────────────────────────────────────── */

void clipGifWindowPlaceLoad(ClipGifWindowPlace *p, int x, int y) {
    if (p == NULL) {
        return;
    }
    p->saved = (x >= 0 && y >= 0);
    p->x = p->saved ? (float)x : 0.0f;
    p->y = p->saved ? (float)y : 0.0f;
}

bool clipGifWindowPlaceTake(ClipGifWindowPlace *p, int *x, int *y) {
    if (p == NULL || !p->changed) {
        return false;
    }
    p->changed = false;
    if (x != NULL) *x = (int)p->x;
    if (y != NULL) *y = (int)p->y;
    return true;
}

bool clipGifWindowPlaceForget(ClipGifWindowPlace *p) {
    if (p == NULL || !p->saved) {
        return false;
    }
    p->saved = false;
    p->changed = false;
    return true;
}

ClipGifWindowPressed clipGifToolsWindow(const char *id, ClipGifSession *s,
                                        const ClipGifReplay *r,
                                        ClipGifMode *mode, bool canMake,
                                        const void *picture, float ax0,
                                        float ay0, float ax1, float ay1,
                                        ClipGifWindowPlace *place) {
    ClipGifWindowPressed out = { false, false, false };
    if (s == NULL || r == NULL || mode == NULL || place == NULL ||
        r->progress == NULL) {
        return out;
    }
    uint32_t curMs = 0, totalMs = 0;
    r->progress(r->user, &curMs, &totalMs);
    const bool capturing = s->active;
    const ImTextureID tex = (ImTextureID)(intptr_t)picture;
    const ImGuiStyle &sty = ImGui::GetStyle();
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float vx0 = vp->Pos.x, vy0 = vp->Pos.y;
    const float vx1 = vp->Pos.x + vp->Size.x, vy1 = vp->Pos.y + vp->Size.y;

    /* As wide as the transport row, and never narrower than the buttons. */
    float w = ax1 - ax0;
    const float minW =
        clipGifToolsRowWidth(picture != NULL) + sty.WindowPadding.x * 2.0f;
    if (w < minW) w = minW;
    if (w > vx1 - vx0) w = vx1 - vx0;
    /* The height is the window's own (it fits its two lines); before the
     * first frame, a guess at it. */
    float h = place->lastH;
    if (h <= 0.0f) {
        h = ImGui::GetFrameHeight() + ImGui::GetFrameHeightWithSpacing() * 2.0f +
            sty.WindowPadding.y * 2.0f;
    }

    /* Placed every frame, so a window never dragged follows the row as the
     * screen changes size; not while a drag is on, which moves it. */
    if (!place->moving) {
        float x = vx0 + place->x, y = vy0 + place->y;
        if (!place->saved) {
            clipGifWindowDefaultPos(ax0, ay0, ax1, ay1, w, h, vx0, vy0, vx1,
                                    vy1, &x, &y);
        } else {
            clipGifWindowClamp(&x, &y, w, h, vx0, vy0, vx1, vy1);
        }
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    }
    /* 0 high: the window fits its lines. */
    ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);

    char title[128];
    snprintf(title, sizeof(title), "%s###%s",
             langGetText(STR_CLIPGIF_WINDOW_TITLE), id);
    /* Its place is kept by the screen, after a real drag only, so not in
     * ImGui's settings. Not in a Log Viewer sidebar. */
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoDocking;
    bool open = true;
    if (ImGui::Begin(title, capturing ? NULL : &open, flags)) {
        /* A drag of the window: its place is kept when the drag ends
         * somewhere new. */
        ImGuiContext &g = *GImGui;
        ImGuiWindow *self = ImGui::GetCurrentWindow();
        const bool moving = g.MovingWindow != NULL &&
                            g.MovingWindow->RootWindow == self->RootWindow;
        const ImVec2 pos = ImGui::GetWindowPos();
        if (moving && !place->moving) {
            place->fromX = pos.x;
            place->fromY = pos.y;
        } else if (!moving && place->moving &&
                   (fabsf(pos.x - place->fromX) >= 1.0f ||
                    fabsf(pos.y - place->fromY) >= 1.0f)) {
            float x = pos.x, y = pos.y;
            clipGifWindowClamp(&x, &y, w, ImGui::GetWindowHeight(), vx0, vy0,
                               vx1, vy1);
            place->saved = true;
            place->x = floorf(x - vx0);
            place->y = floorf(y - vy0);
            place->changed = true;
        }
        place->moving = moving;

        /* The buttons in the middle of the room left of Cancel, which is
         * at the right end (clipGifToolsRow); the zoomed timeline under
         * them as wide as the window. */
        const float rowW = clipGifToolsRowWidth(picture != NULL);
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > rowW) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 floorf((avail - rowW) * 0.5f));
        }
        const ClipGifToolsPressed tools = clipGifToolsRow(
            "##tools", mode, &s->play, capturing, canMake, tex, curMs,
            totalMs);
        out.preview = tools.preview;
        out.save    = tools.save;
        out.gallery = tools.gallery;
        clipGifZoomRow("##zoom", s, r, mode, 0.0f);
        place->lastH = ImGui::GetWindowHeight();
    }
    ImGui::End();
    /* The X: the same as Cancel. */
    if (!open && !capturing) {
        clipGifModeToggle(mode, capturing);
        out.preview = false;
        out.save    = false;
        out.gallery = false;
    }
    return out;
}

/* ── Where the GIF goes ──────────────────────────────────────────── */

#if defined(__IPHONEOS__) || defined(__ANDROID__)
static bool clipGifWriteFile(const char *path, const void *data, size_t len) {
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return false;
    }
    bool ok = (SDL_WriteIO(io, data, len) == len);
    if (!SDL_CloseIO(io)) {
        ok = false;
    }
    return ok;
}
#endif

void clipGifSaveGif(const void *data, size_t len, const ClipGifMeta *meta,
                    const char *baseName) {
    if (data == NULL || len == 0) {
        return;
    }
    const char *name = (baseName != NULL && baseName[0] != '\0') ? baseName
                                                                 : "clip";
#if defined(__IPHONEOS__)
    /* No save dialog on iOS: the clip goes into Documents under its own
     * name, where the Files app shows it, and the box names the file and
     * says where to find it; the sandbox path would mean nothing to the
     * player. */
    (void)meta;
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    char fullPath[FILENAME_MAX];
    bool ok = false;
    if (docs != NULL && docs[0] != '\0') {
        size_t n = SDL_strlen(docs);
        snprintf(fullPath, sizeof(fullPath), "%s%s%s.gif", docs,
                 (n > 0 && docs[n - 1] == '/') ? "" : "/", name);
        ok = clipGifWriteFile(fullPath, data, len);
    }
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s.gif", name);
    imguiMessageBoxEx(CLIP_GIF_BUTTON_TITLE,
                      langGetTextFmt(ok ? STR_CLIPGIF_SAVED_IOS
                                        : STR_CLIPGIF_SAVE_FAILED_IOS, &args),
                      ok ? IMGUI_MSG_INFO : IMGUI_MSG_ERROR, IMGUI_MSG_OK);
#elif defined(__ANDROID__)
    /* No save dialog or gallery on Android: the clip goes into the app's
     * files folder (the pref path) under its own name, the box names the
     * file and says the share sheet comes next, and the share sheet then
     * offers it to another app, which is the only way out of the app's
     * private storage. */
    (void)meta;
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    char fullPath[FILENAME_MAX];
    bool ok = false;
    if (prefDir != NULL) {
        snprintf(fullPath, sizeof(fullPath), "%s%s.gif", prefDir, name);
        SDL_free(prefDir);
        ok = clipGifWriteFile(fullPath, data, len);
    }
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s.gif", name);
    if (ok) {
        imguiMessageBoxEx(CLIP_GIF_BUTTON_TITLE,
                          langGetTextFmt(STR_CLIPGIF_SAVED_ANDROID, &args),
                          IMGUI_MSG_INFO, IMGUI_MSG_OK);
        androidShareFile(fullPath, "image/gif");
    } else {
        imguiMessageBoxEx(CLIP_GIF_BUTTON_TITLE,
                          langGetTextFmt(STR_CLIPGIF_SAVE_FAILED_IOS, &args),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    }
#elif CLIP_GIF_GALLERY_BUILT
    /* Into the gallery folder with its metadata; the gallery opens on it,
     * so the player can look at it before copying it somewhere. */
    if (meta != NULL) {
        clipGifGalleryAdd(data, len, meta, name);
    }
#else
    (void)meta;
    (void)name;
#endif
}
