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

#include <cstdio>   /* snprintf / FILENAME_MAX */

#include <SDL3/SDL.h>

#include "imgui.h"
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
