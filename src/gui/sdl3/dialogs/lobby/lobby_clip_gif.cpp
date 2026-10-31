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
 * Purpose:       The recap's clips: Preview clip and Save clip
 *                for the reel. The preview, the capture, the
 *                progress window and where the GIF goes are
 *                the shared code in clip_gif_capture.cpp, which
 *                the Log Viewer uses too. This file gives that
 *                code the reel (a ClipGifReplay over the
 *                lvEmbed* calls), the crop the reel shows and
 *                the names of the round's players.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
/* BOLO_RECAP_CLIP_GIF; the lvEmbed* entry points the capture steps and reads
 * back through; lobbyReel for the crop frame, the last blitted slice and the
 * game's id; HighlightWindow, the clip a row names. */
#include "lobby_internal.h"
extern "C" {
#include "../../sdl3draw.h"      /* sdl3DrawGetRenderer / Window */
#include "../../../gamefront.h"  /* gameFrontGifWinX / Y, gameFrontSaveWindowSettings */
}

#if BOLO_RECAP_CLIP_GIF
#include "../clip_gif_capture.h"
#if BOLO_CLIP_GIF_GALLERY
#include "../clip_gif_gallery.h"  /* clipGifGalleryOpen / Poll / Render / Shutdown */
#endif

/* ── The reel, as the shared capture sees it ─────────────────────── */

static void reelProgress(void *user, uint32_t *curMs, uint32_t *totalMs) {
    (void)user;
    lvEmbedGetProgress(curMs, totalMs);
}

static bool reelIsPlaying(void *user) {
    (void)user;
    return lvEmbedIsPlaying();
}

static void reelPlay(void *user) {
    (void)user;
    lvEmbedPlay();
}

static void reelPause(void *user) {
    (void)user;
    lvEmbedPause();
}

static void reelSeek(void *user, uint32_t ms) {
    (void)user;
    lvEmbedSeekToTime(ms);
}

/* The reel draws the tank names over its image and not into the texture,
 * and the texture is what is read back: so the names go in for a GIF. */
static void reelCaptureMode(void *user, bool on) {
    (void)user;
    lvEmbedSetTankLabelsInTexture(on);
}

/* One GIF frame of log ticks. The texture is drawn again when it is read
 * (lvEmbedFrameTexture). The reel's GIF keeps its full length at the end of
 * the round, as before: *ended stays false. */
static bool reelAdvance(void *user, bool *ended) {
    (void)user;
    *ended = false;
    lvEmbedStepTicks(CLIP_GIF_TICKS_PER_FRAME);
    return true;
}

static bool reelTarget(void *user, SDL_Renderer **renderer,
                       SDL_Texture **texture) {
    (void)user;
    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    if (!lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) ||
        tex == NULL) {
        return false;
    }
    *renderer = sdl3DrawGetRenderer();
    *texture  = (SDL_Texture *)tex;
    return true;
}

static const ClipGifReplay s_reelReplay = {
    NULL, reelProgress, reelIsPlaying, reelPlay, reelPause, reelSeek,
    reelCaptureMode, reelAdvance, reelTarget
};

static ClipGifSession s_session = {};
/* The clip row whose preview plays, or -1 for the transport's preview. */
static int s_previewRow = -1;

/* Back to the start values: a capture's bytes are dropped and the reel goes
 * back to where it was. */
void lobbyClipGifReset(void) {
    clipGifSessionReset(&s_session, &s_reelReplay, true);
    s_previewRow = -1;
}

bool lobbyClipGifActive(void) {
    return clipGifCaptureActive(&s_session);
}

int lobbyClipGifPreviewRow(void) {
    return clipGifPreviewActive(&s_session) ? s_previewRow : -2;
}

/* ── Save clip ───────────────────────────────────────────────────── */

/* The gallery's metadata for a capture: the time now, the map's name without
 * its .map, the id of the replay the reel has open, and the names of the
 * round's players (the lobby's named slots when there is no round summary). */
static void lobbyClipGifFillMeta(ClipGifMeta *m, ClientSim *cs,
                                 const char *mapName) {
    SDL_Time now = 0;
    int64_t created = 0;
    if (SDL_GetCurrentTime(&now)) {
        created = (int64_t)(now / SDL_NS_PER_SECOND);
    }
    clipGifMetaBegin(m, created, lobbyReel()->gameId, mapName);
    if (cs == NULL) {
        return;
    }
    const RoundStatsSummary *st = clientSimGetLastRoundStats(cs);
    if (st != NULL && st->playerCount > 0) {
        for (int i = 0; i < st->playerCount && i < MAX_TANKS; i++) {
            uint8_t slot = st->players[i].slot;
            const ClientLobbySlot *ls =
                (slot < MAX_TANKS) ? clientSimGetLobbySlot(cs, (BYTE)slot)
                                   : nullptr;
            if (ls != nullptr) {
                clipGifMetaAddPlayer(m, ls->playerName);
            }
        }
        return;
    }
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        const ClientLobbySlot *ls = clientSimGetLobbySlot(cs, (BYTE)slot);
        if (ls != nullptr && ls->connected) {
            clipGifMetaAddPlayer(m, ls->playerName);
        }
    }
}

/* Takes the reel, moves it to the moment, works out the crop and opens the
 * encoder. fromSpan is the transport's Save clip: the GIF's span on the
 * zoomed timeline (clipGifSpanClip), with the view left where the player put
 * it. Otherwise a clip row's: the row names a place as well as a moment, so
 * the view goes to it. */
static void lobbyClipGifStart(bool fromSpan, uint32_t startMs,
                              uint32_t lenMs, int mapX, int mapY,
                              ClientSim *cs) {
    if (!lvEmbedIsActive() || !clipGifCaptureArm(&s_session, &s_reelReplay)) {
        return;
    }
    if (fromSpan) {
        uint32_t curMs = 0, totalMs = 0;
        lvEmbedGetProgress(&curMs, &totalMs);
        clipGifSpanClip(&lobbyReel()->clip, curMs, totalMs, &startMs, &lenMs);
        lvEmbedSeekToTime(startMs);
    } else {
        lvEmbedSeekToClip(startMs, mapX, mapY);
    }

    void *tex = NULL;
    int texW = 0, texH = 0, srcX = 0, srcY = 0, srcW = 0, srcH = 0;
    bool haveCrop = false;
    ClipGifRect crop = { 0, 0, 0, 0 };
    if (lvEmbedFrameTexture(&tex, &texW, &texH, &srcX, &srcY, &srcW, &srcH) &&
        tex != NULL && srcW > 0 && srcH > 0) {
        /* The slice is given against a target a tile larger than itself, but
         * clamp anyway: the crop is read back as it is for every frame. */
        if (srcX + srcW > texW) srcW = texW - srcX;
        if (srcY + srcH > texH) srcH = texH - srcY;
        /* The crop frame is held as fractions of the slice the reel
         * blitted. Its size comes from the last draw (an on-screen size a
         * seek cannot change) and its origin from the fresh one above,
         * because the seek may have moved the camera and the frame names a
         * place on the view, not on the map. */
        ClipGifMode *mode = &lobbyReel()->clip;
        const SDL_Rect *last = &lobbyReel()->lastSlice;
        int viewW = srcW, viewH = srcH;
        if (last->w > 0 && last->h > 0) {
            if (last->w < viewW) viewW = last->w;
            if (last->h < viewH) viewH = last->h;
        }
        haveCrop = srcW > 0 && srcH > 0 &&
                   clipGifChooseCrop(mode, srcX, srcY, viewW, viewH, &crop);
    }

    const char *mapName = (cs != NULL) ? clientSimGetMapName(cs) : NULL;
    ClipGifMeta meta;
    lobbyClipGifFillMeta(&meta, cs, mapName);
    /* With no crop, this puts the reel back as the arm found it. */
    clipGifCaptureOpen(&s_session, &s_reelReplay, haveCrop ? &crop : NULL,
                       startMs, lenMs, mapName, &meta);
}

/* A clip row's Save clip: the moment and the place the row names, for as
 * long as the round's scorer decided the clip runs. */
void lobbyClipGifStartClip(const HighlightWindow *h, ClientSim *cs) {
    lobbyClipGifStart(false, h->startMs, h->durationMs, h->mapX, h->mapY, cs);
}

/* The transport's Save clip. */
void lobbyClipGifStartSpan(ClientSim *cs) {
    lobbyClipGifStart(true, 0u, 0u, 0, 0, cs);
}

/* ── Preview clip ────────────────────────────────────────────────── */

/* The transport's Preview clip: the GIF's span on the zoomed timeline.
 * Pressed while a preview plays, it stops it. */
void lobbyClipGifPreviewToggle(void) {
    if (clipGifPreviewActive(&s_session)) {
        clipGifPreviewStop(&s_session, &s_reelReplay);
        return;
    }
    if (!lvEmbedIsActive()) {
        return;
    }
    if (clipGifPreviewSpan(&s_session, &s_reelReplay, &lobbyReel()->clip)) {
        s_previewRow = -1;
    }
}

/* ── The GIF window ──────────────────────────────────────────────── */

/* The GIF window over the reel's transport row. Its place is the player's
 * after a drag ([WINDOW] "GIF Window X" and "Y", through gamefront), read
 * once at the first draw. */
void lobbyClipGifWindow(float ax0, float ay0, float ax1, float ay1,
                        ClientSim *cs) {
    static ClipGifWindowPlace s_place = CLIP_GIF_WINDOW_PLACE_INIT;
    static bool s_placeRead = false;
    if (!s_placeRead) {
        s_placeRead = true;
        if (gameFrontGifWinX >= 0 && gameFrontGifWinY >= 0) {
            s_place.saved = true;
            s_place.x = (float)gameFrontGifWinX;
            s_place.y = (float)gameFrontGifWinY;
        }
    }
    const ClipGifWindowPressed pressed = clipGifToolsWindow(
        "LobbyGifWindow", &s_session, &s_reelReplay, &lobbyReel()->clip, true,
        (const void *)lobbyIcons()->picture, ax0, ay0, ax1, ay1, &s_place);
    if (s_place.changed) {
        s_place.changed = false;
        gameFrontGifWinX = (int)s_place.x;
        gameFrontGifWinY = (int)s_place.y;
        gameFrontSaveWindowSettings();
    }
    if (pressed.preview) {
        lobbyClipGifPreviewToggle();
    }
    if (pressed.save && cs != NULL) {
        lobbyClipGifStartSpan(cs);
    }
#if BOLO_CLIP_GIF_GALLERY
    if (pressed.gallery) {
        lobbyClipGifGalleryOpen();
    }
#endif
}

/* A clip row's Preview clip: the moment and the place the row names. Pressed
 * on the row whose preview plays, it stops it. */
void lobbyClipGifPreviewClip(int row, const HighlightWindow *h) {
    if (clipGifPreviewActive(&s_session) && s_previewRow == row) {
        clipGifPreviewStop(&s_session, &s_reelReplay);
        return;
    }
    if (!lvEmbedIsActive() || clipGifCaptureActive(&s_session)) {
        return;
    }
    lvEmbedSeekToClip(h->startMs, h->mapX, h->mapY);
    if (clipGifPreviewStart(&s_session, &s_reelReplay, h->durationMs)) {
        s_previewRow = row;
    }
}

/* ── The gallery and the frame loop ──────────────────────────────── */

/* The gallery as the game shows it (lobby_internal.h): drawn with the game's
 * renderer and window, with the GIFs from the reel's game listed first. */
#if BOLO_CLIP_GIF_GALLERY
void lobbyClipGifGalleryOpen(void) {
    clipGifGalleryOpen();
}

void lobbyClipGifGalleryRender(float s) {
    ClipGifGalleryHost host = {};
    host.renderer = sdl3DrawGetRenderer();
    host.window   = sdl3DrawGetWindow();
    host.gameId   = lobbyReel()->gameId;
    clipGifGalleryRender(s, &host);
}
#endif

/* Called once per frame from the lobby's event loop, above whichever view is
 * on screen. The preview ends at the end of its clip, and lets go when the
 * player pauses or moves the reel, or turns recording mode off. The gallery
 * lets go of its picture when the player has gone away from the recap. */
void lobbyClipGifPoll(void) {
    clipGifPreviewPoll(&s_session, &s_reelReplay,
                       lvEmbedIsActive() && lobbyReel()->clip.on);
#if BOLO_CLIP_GIF_GALLERY
    clipGifGalleryPoll();
#endif
}

/* Leaving the lobby lets go of the gallery's picture and of a decode still
 * running. */
void lobbyClipGifShutdown(void) {
#if BOLO_CLIP_GIF_GALLERY
    clipGifGalleryShutdown();
#endif
}

/* Makes this frame's share of a GIF and draws the progress window over it.
 * Called once per body render, after the clip rows that start it. */
void lobbyClipGifRender(float s) {
#if BOLO_CLIP_GIF_GALLERY
    /* The gallery window, when it is open. Here because this is drawn once per
     * recap body render, the same as the progress window below. */
    lobbyClipGifGalleryRender(s);
#endif
    clipGifCaptureStep(&s_session, &s_reelReplay);
    clipGifCaptureRender(&s_session, &s_reelReplay, s);
}
#endif /* BOLO_RECAP_CLIP_GIF */
