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
 *                and the GIF encoder those frames are handed
 *                to. The finished bytes go to the GIF gallery
 *                (lobby_clip_gif_gallery.cpp), which keeps
 *                them and offers copy and save; on iOS they
 *                are written into Documents. Also the export
 *                button a clip row and the transport place,
 *                and the progress modal the capture runs
 *                under, a batch of frames per lobby frame.
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
#include "imgui_messagebox.h"    /* imguiMessageBoxEx / IMGUI_MSG_* — where the file landed on iOS, or didn't */
#include "../../sdl3draw.h"      /* sdl3DrawGetRenderer — the readback target */
#include "../../../lang.h"       /* langGetText / STR_CANCEL */
}

#if BOLO_RECAP_CLIP_GIF
/* Single-header GIF encoder behind the recap's clip export; the one
 * implementation TU is src/third_party/msf_gif/msf_gif_impl.c. Carries its own
 * extern "C" guards, so it goes outside the block above. */
#include "../../../../third_party/msf_gif/msf_gif.h"
#endif
#if defined(__ANDROID__)
#include "../../../../android/android_share.h"  /* androidShareFile — the written clip offered to another app */
#endif

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
    /* When, where and who, for the gallery's metadata file. Filled when the
     * capture starts, so the names are the ones in the lobby at that time. */
    ClipGifMeta meta           = {};
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
/* The gallery's metadata for a capture: the time now, the map's name without
 * its .map, the id of the replay the reel has open, and the names of the
 * round's players (the lobby's named slots when there is no round summary). */
static void lobbyClipGifFillMeta(ClipGifMeta *m, ClientSim *cs,
                                 const char *mapName) {
    *m = ClipGifMeta{};
    SDL_Time now = 0;
    if (SDL_GetCurrentTime(&now)) {
        m->createdUnix = (int64_t)(now / SDL_NS_PER_SECOND);
    }
    SDL_strlcpy(m->game, lobbyReel()->gameId, sizeof(m->game));
    if (mapName && mapName[0]) {
        SDL_utf8strlcpy(m->map, mapName, sizeof(m->map));
        size_t n = SDL_strlen(m->map);
        if (n > 4 && SDL_strcasecmp(m->map + n - 4, ".map") == 0) {
            m->map[n - 4] = '\0';
        }
    }
    if (cs == NULL) {
        return;
    }
    const RoundStatsSummary *st = clientSimGetLastRoundStats(cs);
    if (st != NULL && st->playerCount > 0) {
        for (int i = 0; i < st->playerCount && i < MAX_TANKS &&
                        m->playerCount < CLIP_GIF_META_MAX_PLAYERS; i++) {
            uint8_t slot = st->players[i].slot;
            const ClientLobbySlot *ls =
                (slot < MAX_TANKS) ? clientSimGetLobbySlot(cs, (BYTE)slot)
                                   : nullptr;
            if (ls && ls->playerName[0]) {
                SDL_utf8strlcpy(m->players[m->playerCount], ls->playerName,
                                sizeof(m->players[0]));
                m->playerCount++;
            }
        }
        return;
    }
    for (int slot = 0; slot < MAX_TANKS &&
                       m->playerCount < CLIP_GIF_META_MAX_PLAYERS; slot++) {
        const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)slot);
        if (ls && ls->connected && ls->playerName[0]) {
            SDL_utf8strlcpy(m->players[m->playerCount], ls->playerName,
                            sizeof(m->players[0]));
            m->playerCount++;
        }
    }
}

static void lobbyClipGifStart(uint32_t startMs, uint32_t durationMs,
                              bool centreOnCell, int mapX, int mapY,
                              ClientSim *cs) {
    if (s_clipGif.active || !lvEmbedIsActive()) {
        return;
    }
    const char *mapName = (cs != NULL) ? clientSimGetMapName(cs) : NULL;

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
    lobbyClipGifFillMeta(&s_clipGif.meta, cs, mapName);
    s_clipGif.meta.durationMs = frames * CLIP_GIF_FRAME_MS;
    s_clipGif.meta.width      = cropW;
    s_clipGif.meta.height     = cropH;
    s_clipGif.frame  = 0;
    s_clipGif.total  = (int)frames;
    s_clipGif.failed = false;
    s_clipGif.active = true;
}

/* A clip row's export: the moment and the place the row names, for as long as
 * the round's scorer decided the clip runs. */
void lobbyClipGifStartClip(const HighlightWindow *h, ClientSim *cs) {
    lobbyClipGifStart(h->startMs, h->durationMs, true, h->mapX, h->mapY, cs);
}

/* The transport's export: the length the player set beside the button (the
 * caller has already clamped it to the time left), from wherever the playhead
 * sits, with the view left where the player put it. */
void lobbyClipGifStartFromPlayhead(uint32_t curMs, uint32_t lenMs,
                                   ClientSim *cs) {
    lobbyClipGifStart(curMs, lenMs, false, 0, 0, cs);
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

#if defined(__IPHONEOS__)
static bool lobbyClipGifWriteFile(const char *path, const MsfGifResult *res) {
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return false;
    }
    bool ok = (SDL_WriteIO(io, res->data, res->dataSize) == res->dataSize);
    SDL_CloseIO(io);
    return ok;
}

/* Where the bytes go. On iOS, as before the gallery, into Documents under
 * the clip's own name. Everywhere else into the gallery, which keeps the GIF
 * with its metadata and opens on it, so the player can look at it before
 * copying it or saving it somewhere of their own. Takes the encoder's result
 * by value and frees it. */
static void lobbyClipGifSave(MsfGifResult res) {
    /* No save dialog on iOS: the clip goes into Documents under its own name,
       where the Files app shows it, and the box names the file and says where
       to find it; the sandbox path would mean nothing to the player. */
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    char fullPath[FILENAME_MAX];
    bool ok = false;
    if (docs != NULL && docs[0] != '\0') {
        size_t n = SDL_strlen(docs);
        snprintf(fullPath, sizeof(fullPath), "%s%s%s.gif", docs,
                 (n > 0 && docs[n - 1] == '/') ? "" : "/", s_clipGif.name);
        ok = lobbyClipGifWriteFile(fullPath, &res);
    }
    msf_gif_free(res);
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s.gif", s_clipGif.name);
    imguiMessageBoxEx(CLIP_GIF_TITLE,
                      langGetTextFmt(ok ? STR_CLIPGIF_SAVED_IOS
                                        : STR_CLIPGIF_SAVE_FAILED_IOS, &args),
                      ok ? IMGUI_MSG_INFO : IMGUI_MSG_ERROR,
                      IMGUI_MSG_OK);
}
#elif defined(__ANDROID__)
static bool lobbyClipGifWriteFile(const char *path, const MsfGifResult *res) {
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return false;
    }
    bool ok = (SDL_WriteIO(io, res->data, res->dataSize) == res->dataSize);
    SDL_CloseIO(io);
    return ok;
}

/* No save dialog or gallery on Android: the clip goes into the app's files
   directory (the pref path) under its own name, the box names the file and
   says the share sheet comes next, and the share sheet then offers it to
   another app, which is the only way out of the app's private storage. */
static void lobbyClipGifSave(MsfGifResult res) {
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    char fullPath[FILENAME_MAX];
    bool ok = false;
    if (prefDir != NULL) {
        snprintf(fullPath, sizeof(fullPath), "%s%s.gif", prefDir, s_clipGif.name);
        SDL_free(prefDir);
        ok = lobbyClipGifWriteFile(fullPath, &res);
    }
    msf_gif_free(res);
    MessageArgs args = {};
    snprintf(args.string1, sizeof(args.string1), "%s.gif", s_clipGif.name);
    if (ok) {
        imguiMessageBoxEx(CLIP_GIF_TITLE,
                          langGetTextFmt(STR_CLIPGIF_SAVED_ANDROID, &args),
                          IMGUI_MSG_INFO, IMGUI_MSG_OK);
        androidShareFile(fullPath, "image/gif");
    } else {
        imguiMessageBoxEx(CLIP_GIF_TITLE,
                          langGetTextFmt(STR_CLIPGIF_SAVE_FAILED_IOS, &args),
                          IMGUI_MSG_ERROR, IMGUI_MSG_OK);
    }
}
#else
static void lobbyClipGifSave(MsfGifResult res) {
    lobbyClipGifGalleryAdd(res.data, res.dataSize, &s_clipGif.meta,
                           s_clipGif.name);
    msf_gif_free(res);
}
#endif

/* Called once per frame from the lobby's event loop, above whichever view is
 * on screen, so the gallery lets go of its preview when the player has
 * navigated away from the recap. */
void lobbyClipGifPoll(void) {
#if BOLO_CLIP_GIF_GALLERY
    lobbyClipGifGalleryPoll();
#endif
}

/* Leaving the lobby lets go of the gallery's preview and of a decode still
 * running. */
void lobbyClipGifShutdown(void) {
#if BOLO_CLIP_GIF_GALLERY
    lobbyClipGifGalleryShutdown();
#endif
}

/* Drives a capture from the recap's own frames and draws the modal over it.
 * Called once per body render, after the clip rows that arm it. */
void lobbyClipGifRender(float s) {
#if BOLO_CLIP_GIF_GALLERY
    /* The gallery window, when it is open. Here because this is drawn once per
     * recap body render, the same as the capture's modal below. */
    lobbyClipGifGalleryRender(s);
#endif
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
