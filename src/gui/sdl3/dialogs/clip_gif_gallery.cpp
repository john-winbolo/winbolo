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
 * Name:          clip_gif_gallery.cpp
 * Purpose:       The GIF gallery: every clip export, from the
 *                game's replay or from the Log Viewer, is kept
 *                in a "clips" folder under the pref folder with
 *                a metadata file beside it (clip_gif_core.h),
 *                and a window lists them newest first with when
 *                each was made, the map and the players, the
 *                GIFs from the game that is open above the
 *                earlier ones. The selected GIF plays in the
 *                window, can be copied to the clipboard or
 *                deleted, and its path opens its folder.
 *                The screen that shows it gives it the
 *                renderer, the window and the open game
 *                (clip_gif_gallery.h).
 *********************************************************/

#include <cfloat>   /* FLT_MAX */
#include <climits>  /* INT_MAX */
#include <cstdio>   /* snprintf / FILENAME_MAX */

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"         /* ImTextCalcWordWrapNextLineStart — the path link's lines */
/* CLIP_GIF_GALLERY_BUILT, ClipGifGalleryHost and the calls this file
 * defines; ClipGifMeta through clip_gif_core.h. */
#include "clip_gif_gallery.h"
#include "imgui_dialog_utils.h"     /* imguiHelpTooltip / imguiHandOnHover */
#include "clip_gif_clipboard.h"     /* clipGifCopyFileToClipboard / clipGifShowFileInFolder */
#include "stb_image.h"              /* stbi_image_free */
#include "stb_image_gif_capped.h"   /* stbiLoadGifCapped — the preview's frames, with a memory limit */
#include "../../lang.h"             /* langGetText / STR_CLIPGIF_* */

#if CLIP_GIF_GALLERY_BUILT

/* The format's name, as the export's own modal and message boxes use it. */
static const char *const GALLERY_GIF_TITLE = "GIF";

/* How long a "Copied." line stays up. */
static const Uint64 GALLERY_MSG_MS = 3000;

/* How long an item must stay selected before its GIF is decoded, so moving
 * down the list with the keys or a controller does not decode every file on
 * the way. */
static const Uint64 GALLERY_DECODE_DELAY_MS = 150;

/* The players line: every name and the ", " between them. */
#define GALLERY_PLAYERS_LEN \
    (CLIP_GIF_META_MAX_PLAYERS * (CLIP_GIF_META_NAME_LEN + 2))

/* The selected GIF, decoded: every frame as RGBA in one block, the time each
 * one is shown, and one streaming texture the current frame is copied into.
 * One texture rather than one per frame, so the GPU holds a single frame
 * however long the clip is. */
typedef struct ClipGifPreview {
    char         file[CLIP_GIF_META_FILE_LEN] = "";  /* which item this is */
    bool         failed   = false;  /* the file did not decode */
    bool         tooLarge = false;  /* over CLIP_GIF_PREVIEW_MAX_BYTES, not decoded */
    stbi_uc     *pixels  = nullptr; /* frames * w * h * 4 bytes, stbi's */
    int         *delays  = nullptr; /* ms per frame, stbi's */
    int          w       = 0;
    int          h       = 0;
    int          frames  = 0;
    int          frame   = 0;
    Uint64       nextAtMs = 0;
    SDL_Texture *tex     = nullptr;
} ClipGifPreview;

/* One GIF being decoded on a worker thread, so the lobby loop (which ticks
 * the client's transport) does not stop while a long or large GIF decodes.
 * Heap-allocated and counted: two holders, the gallery and the worker. The
 * worker writes the results and then drops its count, so the count falling
 * to 1 tells the gallery the results are there.
 * When the gallery lets go first (the window closed, or the lobby shut
 * down), it sets stop, so the worker ends before the next frame, and the
 * worker is the last holder and frees the job and its frames. */
typedef struct ClipGifDecodeJob {
    SDL_AtomicInt refs;
    SDL_AtomicInt stop;                          /* 1: nobody wants the result */
    char          path[FILENAME_MAX];            /* the file the worker reads */
    char          file[CLIP_GIF_META_FILE_LEN];  /* the item it is for */
    /* Written by the worker before it drops its count. */
    stbi_uc      *pixels;
    int          *delays;
    int           w;
    int           h;
    int           frames;
    bool          tooLarge;
} ClipGifDecodeJob;

typedef struct ClipGifGalleryState {
    bool         open      = false;
    bool         focus     = false;  /* bring the window to the front once */
    bool         needScan  = true;
    /* Set when the open window is drawn, or opened, and cleared by the
     * poll each lobby frame. Still clear at the poll means the last frame
     * did not draw the window (another lobby view was showing). */
    bool         renderedThisFrame = false;
    ClipGifMeta *items     = nullptr;  /* SDL_malloc'd by the scan */
    int          count     = 0;
    int          selected  = -1;
    /* The id of the game whose replay the reel had open at the last scan
     * (empty for none), and how many items at the front of the list are
     * from it. A new id reads the list again. */
    char         game[CLIP_GIF_META_GAME_LEN] = "";
    int          currentCount = 0;
    /* The file to select after the next scan: the GIF just added. */
    char         selectFile[CLIP_GIF_META_FILE_LEN] = "";
    ClipGifPreview preview;
    /* The decode in progress, or null. At most one runs at a time: a new
     * selection waits for it to finish and then starts its own, so the
     * memory held is never more than one decode and one preview. A decode
     * the gallery let go of still counts (see s_galDecodesRunning). */
    ClipGifDecodeJob *job = nullptr;
    /* The item the selection last moved to, and when, for
     * GALLERY_DECODE_DELAY_MS. */
    char         waitFile[CLIP_GIF_META_FILE_LEN] = "";
    Uint64       waitSinceMs = 0;
    /* <pref folder>/clips, found when the window opens or a GIF is added,
     * rather than asking SDL for the pref folder every frame. Empty while it
     * is not known. */
    char         dir[FILENAME_MAX] = "";
    /* The last copy, delete, folder or add result, as a lang id (0 for
     * none), and until when it shows. msgFile is the {string1} of that
     * line: the file an add could not write. */
    langid       msg       = 0;
    bool         msgError  = false;
    Uint64       msgUntilMs = 0;
    char         msgFile[CLIP_GIF_META_FILE_LEN] = "";
    /* The GIF the delete question is about, set when the question opens, so
     * a selection that moves under it does not change which file goes. */
    char         deleteFile[CLIP_GIF_META_FILE_LEN] = "";
    /* The picture size of sizeFile before it is decoded, from its metadata
     * or its GIF header (0 when neither gave it), so the room the picture
     * takes is kept while it loads. Found once for each selection. */
    char         sizeFile[CLIP_GIF_META_FILE_LEN] = "";
    int          sizeW     = 0;
    int          sizeH     = 0;
    /* The screen that draws the window this frame (clipGifGalleryRender):
     * the preview's texture is made on its renderer, and Copy gives its
     * window the clipboard. */
    SDL_Renderer *renderer = nullptr;
    SDL_Window   *window   = nullptr;
} ClipGifGalleryState;

/* The list's width, in unscaled pixels, as the splitter beside it last left
 * it. Outside s_gal so the shutdown's reset keeps it for the session. */
static float s_galListW = 260.0f;

static ClipGifGalleryState s_gal;

/* Finds <pref folder>/clips, the same folder the export wrote to under a
 * controller before the gallery, and keeps it in s_gal.dir. SDL is asked only
 * while the folder is not known: SDL_GetPrefPath makes the folder each time
 * it is called. False when SDL has no pref folder. */
static bool galleryFindDir(void) {
    if (s_gal.dir[0] != '\0') {
        return true;
    }
    char *prefDir = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (prefDir == NULL) {
        return false;
    }
    int n = snprintf(s_gal.dir, sizeof(s_gal.dir), "%sclips", prefDir);
    SDL_free(prefDir);
    if (n <= 0 || (size_t)n >= sizeof(s_gal.dir)) {
        s_gal.dir[0] = '\0';
        return false;
    }
    return true;
}

/* The folder and the file, with the platform's own separator, as the path
 * is also shown to the player. */
static bool galleryPath(char *out, size_t outLen, const char *file) {
    if (s_gal.dir[0] == '\0') {
        return false;
    }
#ifdef _WIN32
    int n = snprintf(out, outLen, "%s\\%s", s_gal.dir, file);
#else
    int n = snprintf(out, outLen, "%s/%s", s_gal.dir, file);
#endif
    return n > 0 && (size_t)n < outLen;
}

static bool galleryWriteBytes(const char *path, const void *data, size_t len) {
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

static void galleryPreviewFree(void) {
    if (s_gal.preview.tex != nullptr) {
        SDL_DestroyTexture(s_gal.preview.tex);
    }
    if (s_gal.preview.pixels != nullptr) {
        stbi_image_free(s_gal.preview.pixels);
    }
    if (s_gal.preview.delays != nullptr) {
        stbi_image_free(s_gal.preview.delays);
    }
    s_gal.preview = ClipGifPreview{};
}

static void galleryFreeItems(void) {
    SDL_free(s_gal.items);
    s_gal.items    = nullptr;
    s_gal.count    = 0;
    s_gal.selected = -1;
}

/* ── Decoding the preview ───────────────────────────────────────── */

/* The workers that have not yet ended, including one whose job the gallery
 * let go of when the window closed or the lobby shut down. Outside s_gal so
 * the shutdown's reset does not clear it. A new decode starts only at 0, so
 * a window opened again waits for the old worker (told to stop, so it ends
 * within one frame) and two decodes never run at the same time. */
static SDL_AtomicInt s_galDecodesRunning;

static void galleryJobFree(ClipGifDecodeJob *job) {
    if (job->pixels != nullptr) {
        stbi_image_free(job->pixels);
    }
    if (job->delays != nullptr) {
        stbi_image_free(job->delays);
    }
    SDL_free(job);
}

/* Drops one holder's count, and frees the job when it was the last. */
static void galleryJobRelease(ClipGifDecodeJob *job) {
    if (job != nullptr && SDL_AtomicDecRef(&job->refs)) {
        galleryJobFree(job);
    }
}

/* The decode's stop function: true once the gallery has let go of the job. */
static int galleryJobStopped(void *user) {
    return SDL_GetAtomicInt(&((ClipGifDecodeJob *)user)->stop) != 0;
}

/* The worker: reads the file and decodes it within the preview's memory
 * limit. Touches only the job (and s_galDecodesRunning). */
static int SDLCALL galleryDecodeThread(void *userdata) {
    ClipGifDecodeJob *job = (ClipGifDecodeJob *)userdata;
    size_t len = 0;
    void *data = NULL;
    if (!galleryJobStopped(job)) {
        /* A GIF whose compressed bytes are over the limit cannot decode
         * within it, so the file is not read. If the size cannot be found,
         * the load below runs as before. */
        SDL_PathInfo info;
        if (SDL_GetPathInfo(job->path, &info) &&
            info.size > (Uint64)CLIP_GIF_PREVIEW_MAX_BYTES) {
            job->tooLarge = true;
        } else {
            data = SDL_LoadFile(job->path, &len);
        }
    }
    if (data != NULL && len > (size_t)INT_MAX) {
        job->tooLarge = true;  /* stb takes an int length */
    } else if (data != NULL && len > 0) {
        int tooLarge = 0;
        job->pixels = stbiLoadGifCapped((const stbi_uc *)data, (int)len,
                                        CLIP_GIF_PREVIEW_MAX_BYTES,
                                        &job->delays, &job->w, &job->h,
                                        &job->frames, &tooLarge,
                                        galleryJobStopped, job);
        job->tooLarge = (tooLarge != 0);
    }
    SDL_free(data);
    /* After this the job may already be freed; nothing of it is touched. */
    galleryJobRelease(job);
    /* Lowered last, so a new decode does not start while this job's frames
     * are still held. */
    SDL_AtomicDecRef(&s_galDecodesRunning);
    return 0;
}

/* Starts decoding item's GIF. The old preview is let go first, so its frames
 * and the new ones are not held at the same time. */
static void galleryStartDecode(const ClipGifMeta *item) {
    galleryPreviewFree();
    ClipGifPreview &p = s_gal.preview;
    ClipGifDecodeJob *job =
        (ClipGifDecodeJob *)SDL_calloc(1, sizeof(ClipGifDecodeJob));
    if (job == NULL ||
        !galleryPath(job->path, sizeof(job->path), item->gifFile)) {
        SDL_free(job);
        SDL_strlcpy(p.file, item->gifFile, sizeof(p.file));
        p.failed = true;
        return;
    }
    SDL_strlcpy(job->file, item->gifFile, sizeof(job->file));
    SDL_SetAtomicInt(&job->refs, 2);  /* the gallery's and the worker's */
    s_gal.job = job;
    SDL_AtomicIncRef(&s_galDecodesRunning);
    SDL_Thread *thread =
        SDL_CreateThread(galleryDecodeThread, "clipgifdecode", job);
    if (thread != NULL) {
        SDL_DetachThread(thread);
    } else {
        /* No thread: decode here, as the gallery did before it had one. The
         * result is picked up the same way, on the next frame. */
        galleryDecodeThread(job);
    }
}

/* Copy frame i of the decoded GIF into the texture. */
static void galleryPreviewShowFrame(int i) {
    ClipGifPreview &p = s_gal.preview;
    if (p.tex == nullptr || p.pixels == nullptr || i < 0 || i >= p.frames) {
        return;
    }
    const size_t frameBytes = (size_t)p.w * (size_t)p.h * 4u;
    SDL_UpdateTexture(p.tex, NULL, p.pixels + frameBytes * (size_t)i,
                      p.w * 4);
    p.frame = i;
    int delay = (p.delays != nullptr) ? p.delays[i] : 100;
    /* A delay of 0 or 1 cs is read as the browsers' 100 ms. */
    if (delay < 20) {
        delay = 100;
    }
    p.nextAtMs = SDL_GetTicks() + (Uint64)delay;
}

/* Makes a finished job the preview. The frames move to the preview; what the
 * preview does not take is freed with the job. A file that did not decode
 * leaves the preview marked failed or too large, so the window says so
 * instead of trying again every frame. */
static void galleryPreviewTake(ClipGifDecodeJob *job) {
    galleryPreviewFree();
    ClipGifPreview &p = s_gal.preview;
    SDL_strlcpy(p.file, job->file, sizeof(p.file));
    if (job->tooLarge) {
        p.tooLarge = true;
        return;
    }
    if (job->pixels == nullptr || job->w <= 0 || job->h <= 0 ||
        job->frames <= 0) {
        p.failed = true;
        return;
    }
    SDL_Renderer *renderer = s_gal.renderer;
    SDL_Texture *tex =
        (renderer != NULL)
            ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING, job->w, job->h)
            : NULL;
    if (tex == NULL) {
        p.failed = true;
        return;
    }
    p.pixels = job->pixels;
    p.delays = job->delays;
    job->pixels = nullptr;
    job->delays = nullptr;
    p.w      = job->w;
    p.h      = job->h;
    p.frames = job->frames;
    p.tex    = tex;
    galleryPreviewShowFrame(0);
}

/* Each frame, for the selected item: takes a decode that has finished, and
 * starts the item's own once no decode is running (not even one the gallery
 * let go of) and the item has stayed selected for GALLERY_DECODE_DELAY_MS. */
static void galleryPreviewUpdate(const ClipGifMeta *item) {
    ClipGifDecodeJob *job = s_gal.job;
    if (job != nullptr && SDL_GetAtomicInt(&job->refs) == 1) {
        /* Sole owner now: the worker has written the results and let go. */
        s_gal.job = nullptr;
        /* A stopped job holds no frames, so it is not taken; the item's own
         * decode starts after the wait instead. */
        if (SDL_GetAtomicInt(&job->stop) == 0 &&
            SDL_strcmp(job->file, item->gifFile) == 0) {
            galleryPreviewTake(job);
        }
        galleryJobFree(job);
    }
    if (SDL_strcmp(s_gal.waitFile, item->gifFile) != 0) {
        /* A new selection: the old item's frames are not shown under it. */
        SDL_strlcpy(s_gal.waitFile, item->gifFile, sizeof(s_gal.waitFile));
        s_gal.waitSinceMs = SDL_GetTicks();
        /* A decode for another file is not needed now: tell its worker to
         * stop, so the new item does not wait for the whole decode. */
        if (s_gal.job != nullptr &&
            SDL_strcmp(s_gal.job->file, item->gifFile) != 0) {
            SDL_SetAtomicInt(&s_gal.job->stop, 1);
        }
        if (SDL_strcmp(s_gal.preview.file, item->gifFile) != 0) {
            galleryPreviewFree();
        }
        return;
    }
    if (s_gal.job != nullptr ||
        SDL_GetAtomicInt(&s_galDecodesRunning) != 0 ||
        SDL_strcmp(s_gal.preview.file, item->gifFile) == 0) {
        return;
    }
    if (SDL_GetTicks() - s_gal.waitSinceMs >= GALLERY_DECODE_DELAY_MS) {
        galleryStartDecode(item);
    }
}

/* Lets go of the decode in progress, if any, and tells its worker to stop
 * before its next frame; the worker frees the job when it ends. */
static void galleryDropJob(void) {
    if (s_gal.job != nullptr) {
        SDL_SetAtomicInt(&s_gal.job->stop, 1);
    }
    galleryJobRelease(s_gal.job);
    s_gal.job = nullptr;
    s_gal.waitFile[0] = '\0';
}

/* ── The list ───────────────────────────────────────────────────── */

/* Read the folder again, put the GIFs from the open game first, and keep (or
 * move to) the selection by file name, since the scan's order is newest first
 * and a new GIF shifts every index. */
static void galleryRescan(const char *gameId) {
    char keep[CLIP_GIF_META_FILE_LEN] = "";
    if (s_gal.selectFile[0] != '\0') {
        SDL_strlcpy(keep, s_gal.selectFile, sizeof(keep));
    } else if (s_gal.selected >= 0 && s_gal.selected < s_gal.count) {
        SDL_strlcpy(keep, s_gal.items[s_gal.selected].gifFile, sizeof(keep));
    }
    s_gal.selectFile[0] = '\0';
    s_gal.sizeFile[0] = '\0';  /* found again from the new list */
    galleryFreeItems();

    if (galleryFindDir()) {
        s_gal.count = clipGifGalleryScan(s_gal.dir, &s_gal.items);
    }
    SDL_strlcpy(s_gal.game, gameId, sizeof(s_gal.game));
    s_gal.currentCount =
        clipGifMetaCurrentGameFirst(s_gal.items, s_gal.count, s_gal.game);
    s_gal.selected = (s_gal.count > 0) ? 0 : -1;
    for (int i = 0; keep[0] != '\0' && i < s_gal.count; i++) {
        if (SDL_strcmp(s_gal.items[i].gifFile, keep) == 0) {
            s_gal.selected = i;
            break;
        }
    }
    s_gal.needScan = false;
}

static void galleryShowMessage(langid id, bool error, const char *file) {
    s_gal.msg        = id;
    s_gal.msgError   = error;
    s_gal.msgUntilMs = SDL_GetTicks() + GALLERY_MSG_MS;
    SDL_utf8strlcpy(s_gal.msgFile, file ? file : "", sizeof(s_gal.msgFile));
}

void clipGifGalleryOpen(void) {
    galleryFindDir();
    s_gal.open     = true;
    s_gal.focus    = true;
    s_gal.needScan = true;
    /* The window may first be drawn after the next poll, so the poll must
     * not count this frame as one the window was missing from. */
    s_gal.renderedThisFrame = true;
}

bool clipGifGalleryIsOpen(void) {
    return s_gal.open;
}

void clipGifGalleryAdd(const void *data, size_t len, const ClipGifMeta *meta,
                       const char *baseName) {
    if (data == NULL || len == 0 || meta == NULL) {
        return;
    }
    /* The gallery opens whatever happens below, so a GIF that could not be
     * kept is never dropped without a word. */
    s_gal.open     = true;
    s_gal.focus    = true;
    s_gal.needScan = true;
    s_gal.renderedThisFrame = true;  /* as in clipGifGalleryOpen */
    if (!galleryFindDir()) {
        /* The same words iOS uses when its copy in Documents fails. */
        galleryShowMessage(STR_CLIPGIF_SAVE_FAILED_IOS, true,
                           GALLERY_GIF_TITLE);
        return;
    }
    SDL_CreateDirectory(s_gal.dir);

    /* <map>_<mmss>_<date>-<time>.gif, with a number on the end in the rare
     * case two land in the same second, so nothing already in the gallery is
     * written over. */
    char stamp[32];
    clipGifFormatFileStamp(meta->createdUnix, stamp, sizeof(stamp));
    char file[CLIP_GIF_META_FILE_LEN];
    char path[FILENAME_MAX];
    bool named = false;
    for (int n = 1; n <= CLIP_GIF_FILE_NAME_MAX_N && !named; n++) {
        if (!clipGifFileName(file, sizeof(file), baseName, stamp, n) ||
            !galleryPath(path, sizeof(path), file)) {
            break;
        }
        SDL_PathInfo info;
        named = !SDL_GetPathInfo(path, &info);
    }
    if (!named || !galleryWriteBytes(path, data, len)) {
        if (named) {
            /* No file was there before (the name loop checked), so whatever
             * is there now is this write's part file: remove it, or it would
             * be listed as a GIF that cannot be opened. */
            SDL_RemovePath(path);
        }
        galleryShowMessage(STR_CLIPGIF_SAVE_FAILED_IOS, true,
                           named ? file : GALLERY_GIF_TITLE);
        return;
    }
    /* The metadata is extra: a GIF whose metadata did not write is still
     * listed, under the file's own time. */
    char metaPath[FILENAME_MAX];
    if (clipGifMetaPathFor(path, metaPath, sizeof(metaPath))) {
        clipGifMetaWriteFile(metaPath, meta);
    }
    SDL_strlcpy(s_gal.selectFile, file, sizeof(s_gal.selectFile));
}

/* Runs once per frame of the screen that shows the gallery, before that
 * frame is drawn. Lets go of the decoded frames, the texture, a decode still
 * running and the list when the open window was not drawn on the last frame,
 * because the player is on another lobby view. The window stays open and
 * reads the list again when it is next drawn. */
void clipGifGalleryPoll(void) {
    if (s_gal.open && !s_gal.renderedThisFrame) {
        /* Selects the same GIF after the next scan. */
        if (s_gal.selectFile[0] == '\0' && s_gal.selected >= 0 &&
            s_gal.selected < s_gal.count) {
            SDL_strlcpy(s_gal.selectFile, s_gal.items[s_gal.selected].gifFile,
                        sizeof(s_gal.selectFile));
        }
        galleryPreviewFree();
        galleryDropJob();
        galleryFreeItems();
        s_gal.needScan = true;
    }
    s_gal.renderedThisFrame = false;
}

void clipGifGalleryShutdown(void) {
    galleryPreviewFree();
    galleryFreeItems();
    galleryDropJob();
    s_gal = ClipGifGalleryState{};
}

void clipGifGalleryClose(void) {
    /* The same as a shutdown: the window closes, and the list is read again
     * the next time it opens. */
    clipGifGalleryShutdown();
}

/* ── The window ─────────────────────────────────────────────────── */

/* "a, b, c" from the item's players. */
static void galleryJoinPlayers(const ClipGifMeta *m, char *out, size_t outLen) {
    out[0] = '\0';
    for (int i = 0; i < m->playerCount; i++) {
        if (i > 0) SDL_strlcat(out, ", ", outLen);
        SDL_strlcat(out, m->players[i], outLen);
    }
}

/* Open-gallery control for the transport row: a drawn glyph of two stacked
 * frames, so it needs no new art and follows the text colour and UI scale
 * the way the recording mode button does. The tooltip names it. */
bool clipGifGalleryButton(const char *id) {
    const ImGuiStyle &sty = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Button(id, ImVec2(lineH + sty.FramePadding.x * 2.0f,
                                            lineH + sty.FramePadding.y * 2.0f));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
    ImVec2 a(p.x + sty.FramePadding.x + 1.0f, p.y + sty.FramePadding.y + 1.0f);
    const float side = lineH - 2.0f;
    const float off  = side * 0.28f;
    /* The back frame, up and to the right, then the front one filled in
     * part so the two read as a stack of pictures. */
    dl->AddRect(ImVec2(a.x + off, a.y), ImVec2(a.x + side, a.y + side - off),
                col, 0.0f, 0, 1.0f);
    dl->AddRectFilled(ImVec2(a.x, a.y + off),
                      ImVec2(a.x + side - off, a.y + side),
                      ImGui::GetColorU32(ImGuiCol_Button));
    dl->AddRect(ImVec2(a.x, a.y + off), ImVec2(a.x + side - off, a.y + side),
                col, 0.0f, 0, 1.0f);
    imguiHelpTooltip(langGetText(STR_CLIPGIF_GALLERY_TITLE));
    return clicked;
}

/* Removes the gallery GIF file (a name in the folder) and its metadata file.
 * A preview or a decode of that GIF is let go of first, so the frames, the
 * texture and the worker's open file do not keep it. The selection moves to
 * the row that takes its place in the list. */
static void galleryDelete(const char *file) {
    int index = -1;
    for (int i = 0; i < s_gal.count; i++) {
        if (SDL_strcmp(s_gal.items[i].gifFile, file) == 0) {
            index = i;
            break;
        }
    }
    char path[FILENAME_MAX];
    if (index < 0 || !galleryPath(path, sizeof(path), file)) {
        return;
    }
    if (SDL_strcmp(s_gal.preview.file, file) == 0) {
        galleryPreviewFree();
    }
    if (s_gal.job != nullptr && SDL_strcmp(s_gal.job->file, file) == 0) {
        galleryDropJob();
    }
    if (!clipGifDeleteFiles(path)) {
        galleryShowMessage(STR_CLIPGIF_DELETE_FAILED, true, NULL);
        return;
    }
    /* The row to select is named by its file, as the scan finds it again. Its
     * index in the new list is one less than in this list when it was below
     * the removed row. */
    int next = clipGifSelectAfterRemove(s_gal.count, index);
    if (next >= 0) {
        int old = (next < index) ? next : next + 1;
        SDL_strlcpy(s_gal.selectFile, s_gal.items[old].gifFile,
                    sizeof(s_gal.selectFile));
    } else {
        s_gal.selectFile[0] = '\0';
    }
    s_gal.selected = -1;
    s_gal.needScan = true;
}

/* The path as a link: the link colour, underlined, wrapped to the width
 * there is, with the hand cursor over it. Mouse, keys and controller can
 * all press it. Returns true when it is pressed. */
static bool galleryPathLink(const char *id, const char *text) {
    ImFont *font = ImGui::GetFont();
    const float size  = ImGui::GetFontSize();
    const float lineH = ImGui::GetTextLineHeight();
    float wrapW = ImGui::GetContentRegionAvail().x;
    if (wrapW < size) wrapW = size;

    /* The lines, as ImGui's own wrapped text breaks them. A path with no
     * space in it breaks inside a word when the word is wider than the room;
     * at least one character goes on each line, so the loop always ends. */
    enum { MAX_LINES = 16 };
    const char *lineStart[MAX_LINES];
    const char *lineEnd[MAX_LINES];
    int lines = 0;
    float maxW = 0.0f;
    const char *end = text + SDL_strlen(text);
    const char *t = text;
    while (t < end && lines < MAX_LINES) {
        const char *e = font->CalcWordWrapPosition(size, t, end, wrapW);
        if (e <= t) {
            e = t + 1;
            while (e < end && ((unsigned char)*e & 0xC0) == 0x80) {
                e++;
            }
        }
        lineStart[lines] = t;
        lineEnd[lines]   = e;
        float w = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t, e).x;
        if (w > maxW) maxW = w;
        lines++;
        t = ImTextCalcWordWrapNextLineStart(e, end);
    }
    if (lines == 0) {
        return false;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(
        id, ImVec2(SDL_max(maxW, 1.0f), lineH * (float)lines),
        ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered
                                                 : ImGuiCol_TextLink);
    for (int i = 0; i < lines; i++) {
        const float y = pos.y + lineH * (float)i;
        dl->AddText(font, size, ImVec2(pos.x, y), col, lineStart[i],
                    lineEnd[i]);
        const float w =
            font->CalcTextSizeA(size, FLT_MAX, 0.0f, lineStart[i], lineEnd[i])
                .x;
        const float uy = IM_TRUNC(y + size) - 0.5f;
        dl->AddLine(ImVec2(pos.x, uy), ImVec2(pos.x + w, uy), col, 1.0f);
    }
    return pressed;
}

/* The width and height in a GIF file's header: the logical screen size,
 * little-endian, in bytes 6 to 9 after "GIF87a" or "GIF89a". False when the
 * file cannot be read or is not a GIF. */
static bool galleryGifHeaderSize(const char *path, int *w, int *h) {
    SDL_IOStream *io = SDL_IOFromFile(path, "rb");
    if (io == NULL) {
        return false;
    }
    Uint8 head[10];
    const bool read = (SDL_ReadIO(io, head, sizeof(head)) == sizeof(head));
    SDL_CloseIO(io);
    if (!read || SDL_memcmp(head, "GIF", 3) != 0) {
        return false;
    }
    *w = (int)head[6] | ((int)head[7] << 8);
    *h = (int)head[8] | ((int)head[9] << 8);
    return *w > 0 && *h > 0;
}

/* The size item's picture has before it is decoded: the metadata's size,
 * else the GIF header's, found once when the selection moves to it. */
static void galleryKnownSize(const ClipGifMeta *item, const char *path,
                             bool havePath, int *w, int *h) {
    if (SDL_strcmp(s_gal.sizeFile, item->gifFile) != 0) {
        SDL_strlcpy(s_gal.sizeFile, item->gifFile, sizeof(s_gal.sizeFile));
        s_gal.sizeW = 0;
        s_gal.sizeH = 0;
        if (item->width > 0 && item->height > 0) {
            s_gal.sizeW = item->width;
            s_gal.sizeH = item->height;
        } else if (!havePath ||
                   !galleryGifHeaderSize(path, &s_gal.sizeW, &s_gal.sizeH)) {
            s_gal.sizeW = 0;
            s_gal.sizeH = 0;
        }
    }
    *w = s_gal.sizeW;
    *h = s_gal.sizeH;
}

/* A picture of w by h fitted inside maxW by maxH, keeping its shape, and
 * never drawn larger than twice its own size so a small crop stays sharp
 * enough to read. A size that is not known takes all the room. */
static ImVec2 galleryFitSize(int w, int h, float maxW, float maxH) {
    if (w <= 0 || h <= 0) {
        return ImVec2(maxW, maxH);
    }
    float scale = maxW / (float)w;
    if ((float)h * scale > maxH) scale = maxH / (float)h;
    if (scale > 2.0f) scale = 2.0f;
    return ImVec2((float)w * scale, (float)h * scale);
}

#define GALLERY_DELETE_POPUP "###clipgifdelete"

/* The question before a delete, a plain Yes / No. No has the focus, so a
 * stray press of the confirm button does not delete. */
static void galleryRenderDeleteConfirm(float s) {
    char title[128];
    snprintf(title, sizeof(title), "%s" GALLERY_DELETE_POPUP,
             langGetText(STR_CLIPGIF_DELETE));
    if (!ImGui::BeginPopupModal(title, NULL,
                                ImGuiWindowFlags_AlwaysAutoResize |
                                    ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    ImGui::TextUnformatted(langGetText(STR_CLIPGIF_DELETE_CONFIRM));
    ImGui::TextDisabled("%s", s_gal.deleteFile);
    ImGui::Spacing();
    const float btnW = 90.0f * s;
    if (ImGui::Button(langGetText(STR_YES), ImVec2(btnW, 0.0f))) {
        char file[CLIP_GIF_META_FILE_LEN];
        SDL_strlcpy(file, s_gal.deleteFile, sizeof(file));
        s_gal.deleteFile[0] = '\0';
        ImGui::CloseCurrentPopup();
        galleryDelete(file);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_NO), ImVec2(btnW, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s_gal.deleteFile[0] = '\0';
        ImGui::CloseCurrentPopup();
    }
    imguiHandOnHover();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::EndPopup();
}

static void galleryRenderDetail(float s) {
    if (s_gal.selected < 0 || s_gal.selected >= s_gal.count) {
        return;
    }
    const ClipGifMeta *item = &s_gal.items[s_gal.selected];
    galleryPreviewUpdate(item);

    char path[FILENAME_MAX];
    const bool havePath = galleryPath(path, sizeof(path), item->gifFile);

    /* Room under the picture for the details, the path and the buttons. */
    const float lineH = ImGui::GetFrameHeightWithSpacing();
    const float detailH = lineH * 7.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float maxW = avail.x;
    float maxH = avail.y - detailH;
    if (maxH < 60.0f * s) maxH = 60.0f * s;

    ClipGifPreview &p = s_gal.preview;
    const bool mine = (SDL_strcmp(p.file, item->gifFile) == 0);
    if (mine && p.tex != nullptr) {
        if (p.frames > 1 && SDL_GetTicks() >= p.nextAtMs) {
            galleryPreviewShowFrame((p.frame + 1) % p.frames);
        }
        ImVec2 size = galleryFitSize(p.w, p.h, maxW, maxH);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (maxW - size.x) * 0.5f);
        ImGui::Image((ImTextureID)p.tex, size);
    } else {
        /* The room the picture will take, kept while it loads (or when it
         * cannot be shown), so the details and the buttons under it do not
         * move down when it appears. The words go at the top of it. */
        int knownW = 0, knownH = 0;
        galleryKnownSize(item, path, havePath, &knownW, &knownH);
        const ImVec2 size = galleryFitSize(knownW, knownH, maxW, maxH);
        const ImVec2 start = ImGui::GetCursorPos();
        ImGui::Dummy(ImVec2(maxW, size.y));
        const ImVec2 after = ImGui::GetCursorPos();
        ImGui::SetCursorPos(start);
        if (mine && p.tooLarge) {
            ImGui::TextWrapped("%s", langGetText(STR_CLIPGIF_TOO_LARGE));
        } else if (mine && p.failed) {
            ImGui::TextUnformatted(langGetText(STR_CLIPGIF_OPEN_FAILED));
        } else {
            ImGui::TextDisabled("%s", langGetText(STR_CLIPGIF_LOADING));
        }
        /* Words taller than the room push the details down, as before. */
        if (ImGui::GetCursorPosY() < after.y) {
            ImGui::SetCursorPos(after);
        }
    }

    char when[32];
    clipGifFormatTime(item->createdUnix, when, sizeof(when));
    ImGui::TextUnformatted(when);
    if (item->hasMeta) {
        /* Filled here rather than through langGetTextFmt, whose {string1}
         * holds 63 bytes: the players line can be 16 names long. */
        char line[GALLERY_PLAYERS_LEN + 128];
        clipGifFillString1(langGetText(STR_CLIPGIF_MAP_FMT),
                           item->map[0] ? item->map : "-", line, sizeof(line));
        ImGui::TextWrapped("%s", line);
        char players[GALLERY_PLAYERS_LEN];
        galleryJoinPlayers(item, players, sizeof(players));
        clipGifFillString1(langGetText(STR_CLIPGIF_PLAYERS_FMT),
                           players[0] ? players : "-", line, sizeof(line));
        ImGui::TextWrapped("%s", line);
    } else {
        ImGui::TextDisabled("%s", langGetText(STR_CLIPGIF_NO_DETAILS));
    }

    /* The full path, which opens the file's folder. */
    if (havePath) {
        if (galleryPathLink("##clipgifpath", path) &&
            !clipGifShowFileInFolder(path)) {
            galleryShowMessage(STR_CLIPGIF_SHOW_FAILED, true, NULL);
        }
        imguiHelpTooltip(langGetText(STR_CLIPGIF_SHOW_IN_FOLDER_TIP));
    }

    ImGui::BeginDisabled(!havePath);
    if (ImGui::Button(langGetText(STR_CLIPGIF_COPY))) {
        bool ok = clipGifCopyFileToClipboard(path, s_gal.window);
        galleryShowMessage(ok ? STR_CLIPGIF_COPIED : STR_CLIPGIF_COPY_FAILED,
                           !ok, NULL);
    }
    imguiHandOnHover();
    ImGui::SameLine();
    if (ImGui::Button(langGetText(STR_CLIPGIF_DELETE))) {
        SDL_strlcpy(s_gal.deleteFile, item->gifFile, sizeof(s_gal.deleteFile));
        ImGui::OpenPopup(GALLERY_DELETE_POPUP);
    }
    imguiHandOnHover();
    ImGui::EndDisabled();

    /* After everything that reads item: a delete marks the list to be read
     * again, and item is not used past this point. */
    galleryRenderDeleteConfirm(s);
}

/* One row of the list: when it was made and the map, and the players under
 * it. The row is the list's width, so a long one is cut at the edge instead
 * of widening the list, and its tooltip then gives the whole of it. */
static void galleryRenderRow(int i) {
    const ClipGifMeta *m = &s_gal.items[i];
    char when[32];
    clipGifFormatTime(m->createdUnix, when, sizeof(when));
    char text[CLIP_GIF_META_MAP_LEN + 48];
    snprintf(text, sizeof(text), "%s  %s", when, m->map);
    char label[sizeof(text) + 16];
    snprintf(label, sizeof(label), "%s##gif%d", text, i);
    char players[GALLERY_PLAYERS_LEN];
    galleryJoinPlayers(m, players, sizeof(players));

    const float rowW = ImGui::GetContentRegionAvail().x;
    if (ImGui::Selectable(label, i == s_gal.selected, 0, ImVec2(rowW, 0.0f))) {
        s_gal.selected = i;
    }
    const float indent = ImGui::GetStyle().IndentSpacing;
    const bool cut =
        ImGui::CalcTextSize(text).x > rowW ||
        (players[0] != '\0' &&
         ImGui::CalcTextSize(players).x > rowW - indent);
    if (cut && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        if (players[0] != '\0') {
            ImGui::SetTooltip("%s\n%s", text, players);
        } else {
            ImGui::SetTooltip("%s", text);
        }
    }
    if (players[0] != '\0') {
        ImGui::Indent();
        ImGui::TextDisabled("%s", players);
        ImGui::Unindent();
    }
}

/* A light label over a group of rows. */
static void galleryRenderGroupLabel(langid id) {
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::SeparatorText(langGetText(id));
    ImGui::PopStyleColor();
}

/* The list, with the GIFs from the open game under one label and the rest
 * under another. Without a GIF from the open game there is one group, and
 * no label. */
static void galleryRenderList(void) {
    const bool grouped = (s_gal.currentCount > 0);
    for (int i = 0; i < s_gal.count; i++) {
        if (grouped && i == 0) {
            galleryRenderGroupLabel(STR_CLIPGIF_THIS_GAME);
        } else if (grouped && i == s_gal.currentCount) {
            ImGui::Spacing();
            galleryRenderGroupLabel(STR_CLIPGIF_EARLIER_GAMES);
        }
        ImGui::PushID(i);
        galleryRenderRow(i);
        ImGui::PopID();
    }
}

/* The bar between the list and the detail, which drags the list's width.
 * Mouse only: it is not a stop for the keys or a controller. */
static void galleryRenderSplitter(float s, float height, float minW,
                                  float maxW, float listW) {
    const float barW = 8.0f * s;
    ImGui::InvisibleButton("##clipgifsplit", ImVec2(barW, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();
    if (hovered || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (active) {
        float w = listW + ImGui::GetIO().MouseDelta.x;
        if (w < minW) w = minW;
        if (w > maxW) w = maxW;
        s_galListW = w / s;
    }
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const float x = IM_TRUNC((a.x + b.x) * 0.5f);
    const ImGuiCol col = active    ? ImGuiCol_SeparatorActive
                         : hovered ? ImGuiCol_SeparatorHovered
                                   : ImGuiCol_Separator;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y), ImVec2(x, b.y),
                                        ImGui::GetColorU32(col),
                                        (hovered || active) ? 2.0f : 1.0f);
}

void clipGifGalleryRender(float s, const ClipGifGalleryHost *host) {
    if (!s_gal.open) {
        return;
    }
    s_gal.renderedThisFrame = true;
    s_gal.renderer = (host != NULL) ? host->renderer : NULL;
    s_gal.window   = (host != NULL) ? host->window : NULL;
    const char *gameId =
        (host != NULL && host->gameId != NULL) ? host->gameId : "";
    /* A new replay moves its GIFs into the first group. */
    if (SDL_strcmp(s_gal.game, gameId) != 0) {
        s_gal.needScan = true;
    }
    if (s_gal.needScan) {
        galleryRescan(gameId);
    }

    char title[128];
    snprintf(title, sizeof(title), "%s###clipgifgallery",
             langGetText(STR_CLIPGIF_GALLERY_TITLE));
    ImGui::SetNextWindowSize(ImVec2(720.0f * s, 460.0f * s),
                             ImGuiCond_FirstUseEver);
    if (s_gal.focus) {
        ImGui::SetNextWindowFocus();
        s_gal.focus = false;
    }
    bool open = true;
    if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse)) {
        /* The last result, for a few seconds. Filled here rather than
         * through langGetTextFmt, so a long file name is not cut. */
        if (s_gal.msg != 0 && SDL_GetTicks() < s_gal.msgUntilMs) {
            char text[CLIP_GIF_META_FILE_LEN + 256];
            clipGifFillString1(langGetText(s_gal.msg), s_gal.msgFile, text,
                               sizeof(text));
            if (s_gal.msgError) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", text);
                ImGui::PopStyleColor();
            } else {
                ImGui::TextWrapped("%s", text);
            }
        }

        if (s_gal.count == 0) {
            const langid empty = (host != NULL && host->emptyText != 0)
                                     ? (langid)host->emptyText
                                     : (langid)STR_CLIPGIF_GALLERY_EMPTY;
            ImGui::TextWrapped("%s", langGetText(empty));
        } else {
            /* The list's width as last dragged, kept between a floor and the
             * room that leaves the detail a usable width. */
            const float availW = ImGui::GetContentRegionAvail().x;
            const float minW = 140.0f * s;
            float maxW = SDL_min(600.0f * s, availW - 200.0f * s);
            if (maxW < minW) maxW = minW;
            float listW = s_galListW * s;
            if (listW < minW) listW = minW;
            if (listW > maxW) listW = maxW;

            ImGui::BeginChild("##clipgiflist", ImVec2(listW, 0.0f),
                              ImGuiChildFlags_Borders);
            galleryRenderList();
            ImGui::EndChild();
            const float listH = ImGui::GetItemRectSize().y;
            ImGui::SameLine(0.0f, 0.0f);
            galleryRenderSplitter(s, listH, minW, maxW, listW);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::BeginChild("##clipgifdetail", ImVec2(0.0f, 0.0f));
            galleryRenderDetail(s);
            ImGui::EndChild();
        }
    }
    ImGui::End();

    if (!open) {
        /* Closing lets go of the decoded frames, the texture and a decode
         * still running (its worker stops before its next frame and frees
         * the job); the list is read again the next time the window opens. */
        s_gal.open = false;
        galleryPreviewFree();
        galleryDropJob();
        galleryFreeItems();
    }
}

#endif /* CLIP_GIF_GALLERY_BUILT */
