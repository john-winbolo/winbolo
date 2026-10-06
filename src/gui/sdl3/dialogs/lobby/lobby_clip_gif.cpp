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
 * Name:          lobby_clip_gif.cpp
 * Purpose:       The recap's clip export: stepping the reel
 *                across a clip's span and reading each frame
 *                back off the render target at a fixed rate,
 *                the GIF encoder those frames are handed to,
 *                and the native save dialog that writes the
 *                finished bytes out. Also the export button
 *                a clip row and the transport place, and the
 *                progress modal the capture runs under, a
 *                batch of frames per lobby frame.
 *********************************************************/

#include <cstdio>   /* snprintf / FILENAME_MAX — the file's base name and the save paths */

#include <SDL3/SDL.h>

#include "imgui.h"
/* BOLO_MOBILE / BOLO_RECAP_CLIP_GIF; the lvEmbed* entry points the capture
 * steps and reads back through; lobbyReel for the crop frame and the last
 * blitted slice; lobbyIcons for the button's picture glyph; HighlightWindow,
 * the clip a row exports; and through imgui_dialog_utils.h imguiHelpTooltip. */
#include "lobby_internal.h"
#include "dialog_footer.h"  /* WBUI cancel styling + CancelKeyPressed — the modal's Cancel */
extern "C" {
#include "imgui_messagebox.h"    /* imguiMessageBoxEx / IMGUI_MSG_* — where the file landed, or didn't */
#include "../../sdl3draw.h"      /* sdl3DrawGetRenderer / sdl3DrawGetWindow — the readback target and the dialog's parent */
#include "../../../ui_mode.h"    /* uiShouldUseControllerMode — no usable native dialog under a controller */
#include "../../../lang.h"       /* langGetText / STR_CANCEL */
}

#if !BOLO_MOBILE && BOLO_RECAP_CLIP_GIF
/* Single-header GIF encoder behind the recap's clip export; the one
 * implementation TU is src/third_party/msf_gif/msf_gif_impl.c. Carries its own
 * extern "C" guards, so it goes outside the block above. */
#include "../../../../third_party/msf_gif/msf_gif.h"
#endif

#if !BOLO_MOBILE

#if BOLO_RECAP_CLIP_GIF
/* ── Clip GIF export ──────────────────────────────────────────────
 * A clip row can hand its moment to a GIF the player can post somewhere.
 * The round is not sitting in memory as frames — it has to be replayed to be
 * seen — so the export steps the reel five ticks at a time and reads the
 * result back off the GPU, which is a second or more of work for a long clip.
 * That runs a batch per lobby frame under a modal rather than in one loop, so
 * the lobby keeps drawing and the player can call it off.
 *
 * The caption and the modal's title are the format's name, not copy — the same
 * rule the transport's @ button follows. */
static const char *const CLIP_GIF_TITLE = "GIF";
static const char *const CLIP_GIF_POPUP = "GIF##clipgif";

/* Log ticks per captured frame. The viewer's log clock is 20 ms an entry, so
 * five of them is 100 ms — 10 fps, which is 10 centiseconds a frame. This is
 * the reel's clock only; a clip's own duration is in sim ticks and is never
 * divided by this. */
static const int CLIP_GIF_TICKS_PER_FRAME = 5;
static const int CLIP_GIF_CS_PER_FRAME    = 10;
/* Wall-clock length of one captured frame, the same 10 fps said in ms. */
static const uint32_t CLIP_GIF_FRAME_MS   = 100u;
static const int CLIP_GIF_QUALITY         = 16;  /* the encoder's own default */
/* 15 s of clip, and a floor so a clip that arrives with no duration still
 * exports something rather than an empty file. */
static const int CLIP_GIF_MAX_FRAMES      = 150;
static const int CLIP_GIF_MIN_FRAMES      = 10;
/* What the transport's own button captures, having no clip to take a length
 * from: long enough to hold a moment, short enough to still be worth posting. */
static const uint32_t CLIP_GIF_PLAYHEAD_MS = 5000u;
/* Frames per lobby frame. Four keeps the longest clip under a second of
 * wall time while leaving the readback stalls small enough to hide. */
static const int CLIP_GIF_FRAMES_PER_PASS = 4;
/* Every frame in a GIF is the same size, so the crop is fixed once at the
 * start; this caps how wide it may be, and the height follows the same ratio
 * so the clip keeps the shape the reel showed. */
static const int CLIP_GIF_MAX_WIDTH       = 480;

/* One export, from the moment it is armed to the moment its bytes are handed
 * over: the encoder itself, how far through the clip it has got, the rectangle
 * every frame is read back from, where the reel has to go afterwards and what
 * the file will be called. All of it belongs to that one export, so the reset
 * below puts the lot back. */
typedef struct ClipGifCapture {
    bool        active         = false;
    bool        failed         = false;
    MsfGifState enc            = {};
    int         frame          = 0;
    int         total          = 0;
    /* inside the viewer's render target */
    SDL_Rect    crop           = { 0, 0, 0, 0 };
    uint32_t    restoreMs      = 0;  /* where the reel was before we took it */
    bool        restorePlaying = false;
    char        name[96]       = "";  /* <map>_<mmss>, the file's base name */
} ClipGifCapture;

static ClipGifCapture s_clipGif = {};

/* <map>_<mmss>, reduced to characters every filesystem here will take — a map
 * name is free text and reaches this straight off the wire. */
static void lobbyClipGifBaseName(char *out, size_t outLen, const char *mapName,
                                 unsigned mins, unsigned secs) {
    char base[64];
    if (mapName && mapName[0]) {
        snprintf(base, sizeof(base), "%s", mapName);
        size_t blen = SDL_strlen(base);
        if (blen > 4 && SDL_strcmp(base + blen - 4, ".map") == 0) {
            base[blen - 4] = '\0';
        }
    } else {
        snprintf(base, sizeof(base), "clip");
    }
    for (char *p = base; *p != '\0'; p++) {
        char c = *p;
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '-' || c == '_';
        if (!keep) *p = '_';
    }
    snprintf(out, outLen, "%s_%02u%02u", base, mins, secs);
}

/* Put the reel back where the player had it, transport included: an export is
 * a detour, not a seek they asked for. */
static void lobbyClipGifRestoreReel(void) {
    lvEmbedSetTankLabelsInTexture(false);
    lvEmbedSeekToTime(s_clipGif.restoreMs);
    if (s_clipGif.restorePlaying) {
        lvEmbedPlay();
    }
}

/* End the encoder however the capture ended — it holds heap buffers from
 * msf_gif_begin on and only msf_gif_end releases them, so cancelling has to
 * come through here too. outResult takes the finished bytes when the caller
 * means to write them (and then owes msf_gif_free); NULL throws them away. */
static void lobbyClipGifFinish(MsfGifResult *outResult) {
    MsfGifResult res = msf_gif_end(&s_clipGif.enc);
    if (outResult != NULL) {
        *outResult = res;
    } else {
        msf_gif_free(res);
    }
    lobbyClipGifRestoreReel();
    s_clipGif.active = false;
    s_clipGif.failed = false;
    s_clipGif.frame  = 0;
    s_clipGif.total  = 0;
}

static void lobbyClipGifAbort(void) {
    if (s_clipGif.active) {
        lobbyClipGifFinish(NULL);
    }
}

/* Back to the declared values, encoder included. The abort comes first and is
 * what makes the write safe: it is the only route to msf_gif_end, and the
 * buffers a live capture holds are leaked outright if they are overwritten
 * instead of ended. A capture that was never begun has nothing to end and
 * nothing to leak. */
void lobbyClipGifReset(void) {
    lobbyClipGifAbort();
    s_clipGif = ClipGifCapture{};
}

bool lobbyClipGifActive(void) {
    return s_clipGif.active;
}

/* Park the reel on the moment and open the encoder at the size every frame of
 * this capture will be. Takes a time and a length rather than a clip: the
 * transport's button has neither a clip nor a cell, only where the playhead is.
 * centreOnCell splits the two the same way lvEmbedSeekWindowMs does — a clip
 * row names a place as well as a moment, the transport names only a moment and
 * leaves the framing the player set up alone. */
static void lobbyClipGifStart(uint32_t startMs, uint32_t durationMs,
                              bool centreOnCell, int mapX, int mapY,
                              const char *mapName) {
    if (s_clipGif.active || !lvEmbedIsActive()) {
        return;
    }

    lvEmbedGetProgress(&s_clipGif.restoreMs, NULL);
    s_clipGif.restorePlaying = lvEmbedIsPlaying();
    lvEmbedPause();
    /* The frames are read back off the texture, and the reel draws the tank
     * names over its image rather than into the texture, so put them in for
     * the export. lobbyClipGifRestoreReel takes them back out. */
    lvEmbedSetTankLabelsInTexture(true);
    /* Same seek the caller's own control does, so the capture opens on the
     * moment it named. */
    if (centreOnCell) {
        lvEmbedSeekToClip(startMs, mapX, mapY);
    } else {
        lvEmbedSeekToTime(startMs);
    }

    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    if (!lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) ||
        tex == NULL || srcW <= 0 || srcH <= 0) {
        lobbyClipGifRestoreReel();
        return;
    }
    /* The slice is reported against a target a tile larger than itself, but
     * clamp anyway — the crop is read back as-is for every frame after this. */
    if (srcX + srcW > texW) srcW = texW - srcX;
    if (srcY + srcH > texH) srcH = texH - srcY;
    if (srcW <= 0 || srcH <= 0) {
        lobbyClipGifRestoreReel();
        return;
    }
    int cropW, cropH;
    if (lobbyReel()->cropOn && lobbyReel()->lastSlice.w > 0 && lobbyReel()->lastSlice.h > 0) {
        /* The frame the player drew is what this takes. It is normalized
         * against the slice the reel blitted, so mapping it back is that same
         * slice's extent times the fractions — the identical arithmetic the
         * Image's uv0/uv1 used, which is what makes the GIF exactly the
         * rectangle the outline showed.
         *
         * The extent comes from the last draw (an on-screen size a seek cannot
         * change) while the origin is the fresh one read above, because the
         * seek this export just did may have moved the camera and the frame
         * names a place on the view, not on the map. */
        int visW = lobbyReel()->lastSlice.w;
        int visH = lobbyReel()->lastSlice.h;
        if (visW > srcW) visW = srcW;
        if (visH > srcH) visH = srcH;
        int fx0 = (int)(lobbyReel()->cropX0 * (float)visW + 0.5f);
        int fy0 = (int)(lobbyReel()->cropY0 * (float)visH + 0.5f);
        int fx1 = (int)(lobbyReel()->cropX1 * (float)visW + 0.5f);
        int fy1 = (int)(lobbyReel()->cropY1 * (float)visH + 0.5f);
        if (fx0 < 0) fx0 = 0;
        if (fy0 < 0) fy0 = 0;
        if (fx1 > visW) fx1 = visW;
        if (fy1 > visH) fy1 = visH;
        cropW = fx1 - fx0;
        cropH = fy1 - fy0;
        if (cropW < 1) cropW = 1;
        if (cropH < 1) cropH = 1;
        s_clipGif.crop.x = srcX + fx0;
        s_clipGif.crop.y = srcY + fy0;
        s_clipGif.crop.w = cropW;
        s_clipGif.crop.h = cropH;
        /* No CLIP_GIF_MAX_WIDTH here: that cap trims an uncropped view down to
         * something worth posting, and a frame is the player saying what to
         * take instead. */
    } else {
        cropW = srcW;
        cropH = srcH;
        if (cropW > CLIP_GIF_MAX_WIDTH) {
            cropW = CLIP_GIF_MAX_WIDTH;
            cropH = (int)((float)srcH * (float)cropW / (float)srcW);
        }
        if (cropW < 1) cropW = 1;
        if (cropH < 1) cropH = 1;
        if (cropH > srcH) cropH = srcH;
        s_clipGif.crop.x = srcX + (srcW - cropW) / 2;
        s_clipGif.crop.y = srcY + (srcH - cropH) / 2;
        s_clipGif.crop.w = cropW;
        s_clipGif.crop.h = cropH;
    }

    /* From the length in ms, not in ticks: the reel steps a log clock and a
     * clip is measured in sim ticks, and the two do not share a rate. */
    uint32_t frames = durationMs / CLIP_GIF_FRAME_MS;
    if (frames > (uint32_t)CLIP_GIF_MAX_FRAMES) frames = CLIP_GIF_MAX_FRAMES;
    if (frames < (uint32_t)CLIP_GIF_MIN_FRAMES) frames = CLIP_GIF_MIN_FRAMES;

    if (!msf_gif_begin(&s_clipGif.enc, cropW, cropH)) {
        lobbyClipGifRestoreReel();
        return;
    }

    unsigned secs = (unsigned)(startMs / 1000u);
    lobbyClipGifBaseName(s_clipGif.name, sizeof(s_clipGif.name), mapName,
                         secs / 60u, secs % 60u);
    s_clipGif.frame  = 0;
    s_clipGif.total  = (int)frames;
    s_clipGif.failed = false;
    s_clipGif.active = true;
}

/* A clip row's export: the moment and the place the row names, for as long as
 * the round's scorer decided the clip runs. */
void lobbyClipGifStartClip(const HighlightWindow *h, const char *mapName) {
    lobbyClipGifStart(h->startMs, h->durationMs, true, h->mapX, h->mapY,
                      mapName);
}

/* The transport's export: a fixed length from wherever the playhead sits, with
 * the view left where the player put it. */
void lobbyClipGifStartFromPlayhead(uint32_t curMs, const char *mapName) {
    lobbyClipGifStart(curMs, CLIP_GIF_PLAYHEAD_MS, false, 0, 0, mapName);
}

/* The export control itself: the picture glyph where it loaded, the format's
 * name where it didn't. `compact` drops the frame padding's vertical half so
 * the button fits a one-text-line clip row; the transport's copy keeps it and
 * comes out the height of the buttons beside it. The tooltip names the format
 * either way — a glyph on its own does not say which one. (Not `small`: the
 * Windows RPC headers define that as a type.) */
bool lobbyClipGifButton(const char *id, bool compact) {
    const float lineH = ImGui::GetTextLineHeight();
    bool clicked;

    if (lobbyIcons()->picture) {
        if (compact) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                                ImVec2(ImGui::GetStyle().FramePadding.x, 0.0f));
        }
        clicked = ImGui::ImageButton(id, (ImTextureID)lobbyIcons()->picture,
                                     ImVec2(lineH, lineH),
                                     ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0),
                                     ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (compact) {
            ImGui::PopStyleVar();
        }
    } else {
        /* id already carries its own ## prefix, so this reads as the caption
         * with the same hidden id the icon path uses. */
        char label[48];
        snprintf(label, sizeof(label), "%s%s", CLIP_GIF_TITLE, id);
        clicked = compact ? ImGui::SmallButton(label) : ImGui::Button(label);
    }
    imguiHelpTooltip(CLIP_GIF_TITLE);
    return clicked;
}

/* Width the control above will take, for a caller placing it by hand. */
float lobbyClipGifButtonWidth(void) {
    return (lobbyIcons()->picture ? ImGui::GetTextLineHeight()
                          : ImGui::CalcTextSize(CLIP_GIF_TITLE).x)
           + ImGui::GetStyle().FramePadding.x * 2.0f;
}

/* One frame: read the fixed crop out of the viewer's render target and hand it
 * to the encoder. The lobby is mid-frame and owns the render target, so
 * whatever it was pointing at goes straight back. */
static bool lobbyClipGifCaptureFrame(void) {
    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    if (!lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) ||
        tex == NULL) {
        return false;
    }
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (renderer == NULL) {
        return false;
    }

    SDL_Texture *saved = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, (SDL_Texture *)tex)) {
        return false;
    }
    SDL_Surface *raw = SDL_RenderReadPixels(renderer, &s_clipGif.crop);
    SDL_SetRenderTarget(renderer, saved);
    if (raw == NULL) {
        return false;
    }

    /* Convert rather than assume: the readback's layout follows the render
     * target, and the encoder reads RGBA8 rows. Its own pitch goes with it —
     * a converted surface is not promised to be tightly packed. */
    SDL_Surface *rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(raw);
    if (rgba == NULL) {
        return false;
    }
    bool ok = msf_gif_frame(&s_clipGif.enc, (uint8_t *)rgba->pixels,
                            CLIP_GIF_CS_PER_FRAME, CLIP_GIF_QUALITY,
                            rgba->pitch) != 0;
    SDL_DestroySurface(rgba);
    return ok;
}

/* One request's slot for the native save dialog's answer. Heap-allocated and
 * refcounted because SDL cannot cancel a dialog that is already up, so the
 * slot has to be able to outlive the lobby that asked for it.
 *
 * Two holders: the lobby and the dialog callback. The callback drops its
 * reference last, after writing path and ok, so refs falling to 1 is what
 * publishes the answer — and once it has, the lobby is the sole owner and can
 * read and free without further synchronisation. SDL runs the callback on a
 * worker thread on Windows and on the Linux zenity backend. */
typedef struct ClipGifSaveRequest {
    SDL_AtomicInt refs;
    char          path[FILENAME_MAX];
    int           ok;
} ClipGifSaveRequest;

/* The finished clip and the picker it is waiting on. The encoder's bytes are
 * owned here from lobbyClipGifSave on, and this is the only thing that frees
 * them: every exit — written, write failed, cancelled, lobby closed — goes
 * through the poll or the abandon below, and msf_gif_free is safe on a zeroed
 * result, so "free then zero" needs no special cases. Kept apart from
 * ClipGifCapture so a reel teardown mid-session can't drop a clip the player
 * is still choosing a name for. */
typedef struct ClipGifPendingSave {
    ClipGifSaveRequest *req   = nullptr;
    MsfGifResult        bytes = {};
} ClipGifPendingSave;

static ClipGifPendingSave s_clipGifSave = {};

static void SDLCALL lobbyClipGifSaveCallback(void *userdata,
                                             const char *const *filelist,
                                             int filter) {
    ClipGifSaveRequest *st = (ClipGifSaveRequest *)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(st->path, filelist[0], sizeof(st->path));
        /* Not every platform's dialog applies the filter's extension to a name
         * typed without one, so a name that arrives bare gets it here — the
         * file has to open as a GIF wherever the player shares it. Matched
         * case-insensitively so a name already ending .GIF keeps the one it
         * has. If there is no room for the suffix the path stands as typed;
         * SDL_strlcat leaves it terminated either way. */
        size_t len = SDL_strlen(st->path);
        if (len > 0 &&
            (len < 4 || SDL_strcasecmp(st->path + len - 4, ".gif") != 0)) {
            SDL_strlcat(st->path, ".gif", sizeof(st->path));
        }
        st->ok = 1;
    }
    /* Dropped last: this is what publishes path and ok to the poll, and the
     * slot must not be touched again after it. */
    if (SDL_AtomicDecRef(&st->refs)) {
        SDL_free(st);
    }
}

static bool lobbyClipGifWriteFile(const char *path, const MsfGifResult *res) {
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return false;
    }
    bool ok = (SDL_WriteIO(io, res->data, res->dataSize) == res->dataSize);
    SDL_CloseIO(io);
    return ok;
}

/* Where the bytes go, on the split windowSaveMap uses: no usable native dialog
 * under a controller, so that path names the file itself under the pref dir and
 * reports where it went; the desktop path asks. The report is the path — the
 * box's title is the format name and its icon carries the rest, so neither
 * outcome needs a sentence.
 *
 * Takes the encoder's result by value and owns it from here: the desktop path
 * outlives this call, so the caller cannot free the bytes behind it. */
static void lobbyClipGifSave(MsfGifResult res) {
    if (uiShouldUseControllerMode()) {
        char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
        if (prefDir == NULL) {
            msf_gif_free(res);
            return;
        }
        char clipsDir[FILENAME_MAX];
        snprintf(clipsDir, sizeof(clipsDir), "%sclips", prefDir);
        SDL_CreateDirectory(clipsDir);

        char fullPath[FILENAME_MAX];
        snprintf(fullPath, sizeof(fullPath), "%s/%s.gif", clipsDir,
                 s_clipGif.name);
        SDL_free(prefDir);

        bool ok = lobbyClipGifWriteFile(fullPath, &res);
        msf_gif_free(res);
        imguiMessageBoxEx(CLIP_GIF_TITLE, fullPath,
                          ok ? IMGUI_MSG_INFO : IMGUI_MSG_ERROR,
                          IMGUI_MSG_OK);
        return;
    }

    /* The picker is shown and returned from immediately. Waiting here would
     * stop the lobby's event loop, and that loop is what ticks the client
     * transport (imguiLobbyShow), so a player choosing a folder was dropped by
     * the server. lobbyClipGifSavePoll picks the answer up on a later frame.
     *
     * A second export while one is outstanding is not reachable through the UI
     * — the picker owns the window — but if it ever were, the newer clip is
     * released rather than overwriting the record that owns the older one. */
    if (s_clipGifSave.req != NULL) {
        msf_gif_free(res);
        return;
    }

    ClipGifSaveRequest *req =
        (ClipGifSaveRequest *)SDL_calloc(1, sizeof(*req));
    if (req == NULL) {
        msf_gif_free(res);
        return;
    }
    SDL_SetAtomicInt(&req->refs, 2);  /* the lobby's and the callback's */

    s_clipGifSave.req   = req;
    s_clipGifSave.bytes = res;

    SDL_DialogFileFilter filters[] = {
        { "GIF Images", "gif" },
    };
    SDL_ShowSaveFileDialog(lobbyClipGifSaveCallback, req,
                           sdl3DrawGetWindow(), filters, 1, NULL);
}

/* Write out a clip whose picker has been answered. Called once per frame from
 * the lobby's event loop, above whichever view is on screen, so navigating
 * away from the recap with the picker open still lands the file. */
void lobbyClipGifSavePoll(void) {
    ClipGifSaveRequest *req = s_clipGifSave.req;
    if (req == NULL || SDL_GetAtomicInt(&req->refs) != 1) {
        return;
    }

    /* Sole owner now, so the slot reads without further synchronisation. */
    bool ok = (req->ok != 0);
    char path[FILENAME_MAX];
    SDL_strlcpy(path, req->path, sizeof(path));
    MsfGifResult bytes = s_clipGifSave.bytes;

    s_clipGifSave = ClipGifPendingSave{};
    SDL_free(req);

    if (ok && !lobbyClipGifWriteFile(path, &bytes)) {
        /* The player picked the place, so silence would be the only cue that
         * nothing landed there. */
        imguiMessageBoxEx(CLIP_GIF_TITLE, path, IMGUI_MSG_ERROR,
                          IMGUI_MSG_OK);
    }
    msf_gif_free(bytes);
}

/* Leaving the lobby cancels a save the player has not answered: there is no
 * loop left to poll it, and a multi-megabyte clip cannot sit in a static
 * waiting for a picker whose window has gone. The bytes go back to the
 * allocator and the lobby drops its reference to the request slot, which the
 * dialog callback then frees if it has yet to answer. */
void lobbyClipGifSaveAbandon(void) {
    ClipGifSaveRequest *req = s_clipGifSave.req;
    msf_gif_free(s_clipGifSave.bytes);
    s_clipGifSave = ClipGifPendingSave{};
    if (req != NULL && SDL_AtomicDecRef(&req->refs)) {
        SDL_free(req);
    }
}

/* Drives a capture from the recap's own frames and draws the modal over it.
 * Called once per body render, after the clip rows that arm it. */
void lobbyClipGifRender(float s) {
    if (!s_clipGif.active) {
        return;
    }
    /* Opened from here rather than from the row that started the capture: a
     * popup's id is seeded from the window submitting it, and the recap body
     * is drawn from two different containers. Re-asserting it every frame the
     * capture is live is what keeps the modal with the capture if the lobby
     * swaps layouts underneath it. */
    if (!ImGui::IsPopupOpen(CLIP_GIF_POPUP)) {
        ImGui::OpenPopup(CLIP_GIF_POPUP);
    }

    MsfGifResult finished = {};
    bool         haveFinished = false;

    if (ImGui::BeginPopupModal(CLIP_GIF_POPUP, NULL,
                               ImGuiWindowFlags_AlwaysAutoResize
                               | ImGuiWindowFlags_NoCollapse
                               | ImGuiWindowFlags_NoSavedSettings)) {
        for (int i = 0; i < CLIP_GIF_FRAMES_PER_PASS &&
                        s_clipGif.frame < s_clipGif.total; i++) {
            lvEmbedStepTicks(CLIP_GIF_TICKS_PER_FRAME);
            if (!lobbyClipGifCaptureFrame()) {
                /* A refused readback or a spent encoder stops here rather than
                 * writing a clip that cuts off mid-moment. */
                s_clipGif.failed = true;
                break;
            }
            s_clipGif.frame++;
        }

        /* Both widgets take the same explicit width rather than -1: the window
         * auto-resizes, and a fill-the-rest width inside one chases its own
         * previous frame until the modal is as narrow as the button. */
        const float rowW = 280.0f * s;
        float done = (s_clipGif.total > 0)
                         ? (float)s_clipGif.frame / (float)s_clipGif.total
                         : 0.0f;
        ImGui::ProgressBar(done, ImVec2(rowW, 0.0f));

        WBUI::PushCancelStyle();
        bool cancel = ImGui::Button(langGetText(STR_CANCEL),
                                    ImVec2(rowW, 0.0f));
        WBUI::PopCancelStyle();
        if (WBUI::CancelKeyPressed()) {
            cancel = true;
        }

        if (cancel || s_clipGif.failed) {
            lobbyClipGifFinish(NULL);
            ImGui::CloseCurrentPopup();
        } else if (s_clipGif.frame >= s_clipGif.total) {
            lobbyClipGifFinish(&finished);
            haveFinished = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* The bytes are ours from msf_gif_end on, and saving takes them: the
     * desktop picker outlives this frame, so ownership passes rather than the
     * buffer being freed underneath it. A capture that produced nothing has
     * nothing to hand over. */
    if (haveFinished) {
        if (finished.data != NULL) {
            lobbyClipGifSave(finished);
        } else {
            msf_gif_free(finished);
        }
    }
}
#endif /* BOLO_RECAP_CLIP_GIF */
#endif /* !BOLO_MOBILE */
