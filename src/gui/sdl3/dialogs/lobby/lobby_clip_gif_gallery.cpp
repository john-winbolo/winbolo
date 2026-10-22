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
 * Name:          lobby_clip_gif_gallery.cpp
 * Purpose:       The recap's GIF gallery: every clip export
 *                is kept in a "clips" folder under the pref
 *                folder with a metadata file beside it
 *                (clip_gif_core.h), and a window lists them
 *                newest first with when each was made, the
 *                map and the players. The selected GIF plays
 *                in the window and can be copied to the
 *                clipboard or saved to a place the player
 *                picks.
 *********************************************************/

#include <climits>  /* INT_MAX */
#include <cstdio>   /* snprintf / FILENAME_MAX */

#include <SDL3/SDL.h>

#include "imgui.h"
/* BOLO_CLIP_GIF_GALLERY; ClipGifMeta and the gallery calls this file
 * defines; lobbyIcons; and through imgui_dialog_utils.h imguiHelpTooltip. */
#include "lobby_internal.h"
#include "../clip_gif_clipboard.h"  /* clipGifCopyFileToClipboard */
#include "stb_image.h"              /* stbi_image_free */
#include "stb_image_gif_capped.h"   /* stbiLoadGifCapped — the preview's frames, with a memory limit */
extern "C" {
#include "imgui_messagebox.h"    /* imguiMessageBoxEx — a save that did not write */
#include "../../sdl3draw.h"      /* sdl3DrawGetRenderer / sdl3DrawGetWindow — the preview's texture, the dialog's parent */
#include "../../../ui_mode.h"    /* uiShouldUseControllerMode — no usable native dialog under a controller */
#include "../../../lang.h"       /* langGetText / STR_CLIPGIF_* */
}

#if !defined(__ANDROID__) && BOLO_CLIP_GIF_GALLERY

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
 * Heap-allocated and counted, as ClipGifSaveRequest is: two holders, the
 * gallery and the worker. The worker writes the results and then drops its
 * count, so the count falling to 1 tells the gallery the results are there.
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

/* One request's slot for the native save dialog's answer, as the export used
 * before the gallery. Heap-allocated and counted, because SDL cannot cancel a
 * dialog that is already up, so the slot has to be able to outlive the lobby
 * that asked for it. Two holders: the lobby and the dialog callback. The
 * callback drops its count last, after writing path and ok, so the count
 * falling to 1 is what tells the poll the answer is there. SDL runs the
 * callback on a worker thread on Windows and on the Linux zenity backend. */
typedef struct ClipGifSaveRequest {
    SDL_AtomicInt refs;
    char          path[FILENAME_MAX];
    int           ok;
} ClipGifSaveRequest;

typedef struct ClipGifGalleryState {
    bool         open      = false;
    bool         focus     = false;  /* bring the window to the front once */
    bool         needScan  = true;
    /* Set when the open window is drawn, or opened, and cleared by the save
     * poll each lobby frame. Still clear at the poll means the last frame
     * did not draw the window (another lobby view was showing). */
    bool         renderedThisFrame = false;
    ClipGifMeta *items     = nullptr;  /* SDL_malloc'd by the scan */
    int          count     = 0;
    int          selected  = -1;
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
    /* The last copy or add result, as a lang id (0 for none), and until when
     * it shows. msgFile is the {string1} of that line: the file an add could
     * not write. */
    langid       msg       = 0;
    bool         msgError  = false;
    Uint64       msgUntilMs = 0;
    char         msgFile[CLIP_GIF_META_FILE_LEN] = "";
    /* A Save to disk waiting on its picker, and the gallery file it copies. */
    ClipGifSaveRequest *saveReq = nullptr;
    char         saveSrc[FILENAME_MAX] = "";
} ClipGifGalleryState;

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

static bool galleryPath(char *out, size_t outLen, const char *file) {
    if (s_gal.dir[0] == '\0') {
        return false;
    }
    int n = snprintf(out, outLen, "%s/%s", s_gal.dir, file);
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
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
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

/* Read the folder again, and keep (or move to) the selection by file name,
 * since the scan's order is newest first and a new GIF shifts every index. */
static void galleryRescan(void) {
    char keep[CLIP_GIF_META_FILE_LEN] = "";
    if (s_gal.selectFile[0] != '\0') {
        SDL_strlcpy(keep, s_gal.selectFile, sizeof(keep));
    } else if (s_gal.selected >= 0 && s_gal.selected < s_gal.count) {
        SDL_strlcpy(keep, s_gal.items[s_gal.selected].gifFile, sizeof(keep));
    }
    s_gal.selectFile[0] = '\0';
    galleryFreeItems();

    if (galleryFindDir()) {
        s_gal.count = clipGifGalleryScan(s_gal.dir, &s_gal.items);
    }
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

void lobbyClipGifGalleryOpen(void) {
    galleryFindDir();
    s_gal.open     = true;
    s_gal.focus    = true;
    s_gal.needScan = true;
    /* The window may first be drawn after the next poll, so the poll must
     * not count this frame as one the window was missing from. */
    s_gal.renderedThisFrame = true;
}

void lobbyClipGifGalleryAdd(const void *data, size_t len,
                            const ClipGifMeta *meta, const char *baseName) {
    if (data == NULL || len == 0 || meta == NULL) {
        return;
    }
    /* The gallery opens whatever happens below, so a GIF that could not be
     * kept is never dropped without a word. */
    s_gal.open     = true;
    s_gal.focus    = true;
    s_gal.needScan = true;
    s_gal.renderedThisFrame = true;  /* as in lobbyClipGifGalleryOpen */
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
    for (int n = 1; n < 100 && !named; n++) {
        if (n == 1) {
            snprintf(file, sizeof(file), "%s_%s.gif",
                     (baseName && baseName[0]) ? baseName : "clip", stamp);
        } else {
            snprintf(file, sizeof(file), "%s_%s_%d.gif",
                     (baseName && baseName[0]) ? baseName : "clip", stamp, n);
        }
        if (!galleryPath(path, sizeof(path), file)) {
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

/* ── Save to disk ───────────────────────────────────────────────── */

static void SDLCALL gallerySaveCallback(void *userdata,
                                        const char *const *filelist,
                                        int filter) {
    ClipGifSaveRequest *st = (ClipGifSaveRequest *)userdata;
    (void)filter;
    if (filelist && filelist[0]) {
        SDL_strlcpy(st->path, filelist[0], sizeof(st->path));
        /* Not every platform's dialog adds the filter's extension to a name
         * typed without one, so a bare name gets it here: the file has to
         * open as a GIF wherever the player shares it. */
        size_t len = SDL_strlen(st->path);
        if (len > 0 &&
            (len < 4 || SDL_strcasecmp(st->path + len - 4, ".gif") != 0)) {
            SDL_strlcat(st->path, ".gif", sizeof(st->path));
        }
        st->ok = 1;
    }
    /* Dropped last: this is what tells the poll path and ok are written, and
     * the slot must not be touched again after it. */
    if (SDL_AtomicDecRef(&st->refs)) {
        SDL_free(st);
    }
}

/* Ask where to put the selected GIF. The picker returns at once; the answer
 * is picked up by lobbyClipGifGallerySavePoll on a later frame, so the lobby
 * keeps ticking the transport while the player chooses. */
static void galleryStartSave(const char *srcPath) {
    if (s_gal.saveReq != nullptr) {
        return;
    }
    ClipGifSaveRequest *req =
        (ClipGifSaveRequest *)SDL_calloc(1, sizeof(*req));
    if (req == NULL) {
        return;
    }
    SDL_SetAtomicInt(&req->refs, 2);  /* the lobby's and the callback's */
    s_gal.saveReq = req;
    SDL_strlcpy(s_gal.saveSrc, srcPath, sizeof(s_gal.saveSrc));

    /* The name is what the operating system's dialog shows as the file type.
     * SDL needs the filter array to stay valid until the callback runs, so it
     * is static, and the name is filled from the lang table on each call. */
    static SDL_DialogFileFilter filters[1];
    filters[0].name    = langGetText(STR_CLIPGIF_FILTER_NAME);
    filters[0].pattern = "gif";
    SDL_ShowSaveFileDialog(gallerySaveCallback, req, sdl3DrawGetWindow(),
                           filters, 1, NULL);
}

/* Runs once per lobby frame on every view, before that frame is drawn. Lets
 * go of the decoded frames, the texture, a decode still running and the
 * list when the open window was not drawn on the last frame, because the
 * player is on another lobby view. The window stays open and reads the list
 * again when it is next drawn. Then writes out a save from the gallery
 * whose picker has been answered. */
void lobbyClipGifGallerySavePoll(void) {
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

    ClipGifSaveRequest *req = s_gal.saveReq;
    if (req == NULL || SDL_GetAtomicInt(&req->refs) != 1) {
        return;
    }
    /* Sole owner now, so the slot reads without further synchronisation. */
    bool ok = (req->ok != 0);
    char dest[FILENAME_MAX];
    SDL_strlcpy(dest, req->path, sizeof(dest));
    char src[FILENAME_MAX];
    SDL_strlcpy(src, s_gal.saveSrc, sizeof(src));
    s_gal.saveReq    = nullptr;
    s_gal.saveSrc[0] = '\0';
    SDL_free(req);
    if (!ok) {
        return;  /* the player cancelled */
    }

    bool wrote = false;
    size_t len = 0;
    void *data = SDL_LoadFile(src, &len);
    if (data != NULL) {
        wrote = galleryWriteBytes(dest, data, len);
        SDL_free(data);
    }
    if (!wrote) {
        /* The player picked the place, so silence would be the only cue that
         * nothing landed there. Outside the ImGui frame, as the box runs a
         * loop of its own. */
        imguiMessageBoxEx(GALLERY_GIF_TITLE, dest, IMGUI_MSG_ERROR,
                          IMGUI_MSG_OK);
    }
}

void lobbyClipGifGalleryShutdown(void) {
    galleryPreviewFree();
    galleryFreeItems();
    galleryDropJob();
    ClipGifSaveRequest *req = s_gal.saveReq;
    s_gal = ClipGifGalleryState{};
    if (req != NULL && SDL_AtomicDecRef(&req->refs)) {
        SDL_free(req);
    }
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
 * the way the crop button beside it does. The tooltip names it. */
bool lobbyClipGifGalleryButton(const char *id) {
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

static void galleryRenderDetail(float s) {
    if (s_gal.selected < 0 || s_gal.selected >= s_gal.count) {
        return;
    }
    const ClipGifMeta *item = &s_gal.items[s_gal.selected];
    galleryPreviewUpdate(item);

    /* Room under the picture for the details and the buttons. */
    const float lineH = ImGui::GetFrameHeightWithSpacing();
    const float detailH = lineH * 5.0f;
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
        /* Fitted inside the room left, keeping its shape, and never drawn
         * larger than twice its own size so a small crop stays sharp
         * enough to read. */
        float scale = maxW / (float)p.w;
        if ((float)p.h * scale > maxH) scale = maxH / (float)p.h;
        if (scale > 2.0f) scale = 2.0f;
        ImVec2 size((float)p.w * scale, (float)p.h * scale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (maxW - size.x) * 0.5f);
        ImGui::Image((ImTextureID)p.tex, size);
    } else if (mine && p.tooLarge) {
        ImGui::TextWrapped("%s", langGetText(STR_CLIPGIF_TOO_LARGE));
    } else if (mine && p.failed) {
        ImGui::TextUnformatted(langGetText(STR_CLIPGIF_OPEN_FAILED));
    } else {
        ImGui::TextDisabled("%s", langGetText(STR_CLIPGIF_LOADING));
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

    char path[FILENAME_MAX];
    const bool havePath = galleryPath(path, sizeof(path), item->gifFile);
    ImGui::BeginDisabled(!havePath);
    if (ImGui::Button(langGetText(STR_CLIPGIF_COPY))) {
        bool ok = clipGifCopyFileToClipboard(path, sdl3DrawGetWindow());
        galleryShowMessage(ok ? STR_CLIPGIF_COPIED : STR_CLIPGIF_COPY_FAILED,
                           !ok, NULL);
    }
    /* No usable native dialog under a controller, so there the file stays
     * where the gallery keeps it, and its path is shown instead. */
    if (!uiShouldUseControllerMode()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(s_gal.saveReq != nullptr);
        if (ImGui::Button(langGetText(STR_CLIPGIF_SAVE))) {
            galleryStartSave(path);
        }
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    if (uiShouldUseControllerMode() && havePath) {
        ImGui::TextDisabled("%s", path);
    }
}

void lobbyClipGifGalleryRender(float s) {
    if (!s_gal.open) {
        return;
    }
    s_gal.renderedThisFrame = true;
    if (s_gal.needScan) {
        galleryRescan();
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
        /* The last copy or add result, for a few seconds. Filled here rather
         * than through langGetTextFmt, so a long file name is not cut. */
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
            ImGui::TextWrapped("%s", langGetText(STR_CLIPGIF_GALLERY_EMPTY));
        } else {
            ImGui::BeginChild("##clipgiflist", ImVec2(260.0f * s, 0.0f),
                              ImGuiChildFlags_Borders);
            for (int i = 0; i < s_gal.count; i++) {
                const ClipGifMeta *m = &s_gal.items[i];
                char when[32];
                clipGifFormatTime(m->createdUnix, when, sizeof(when));
                char label[CLIP_GIF_META_MAP_LEN + 48];
                snprintf(label, sizeof(label), "%s  %s##gif%d", when,
                         m->map, i);
                ImGui::PushID(i);
                if (ImGui::Selectable(label, i == s_gal.selected)) {
                    s_gal.selected = i;
                }
                if (m->playerCount > 0) {
                    char players[GALLERY_PLAYERS_LEN];
                    galleryJoinPlayers(m, players, sizeof(players));
                    ImGui::Indent();
                    ImGui::TextDisabled("%s", players);
                    ImGui::Unindent();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##clipgifdetail", ImVec2(0.0f, 0.0f));
            galleryRenderDetail(s);
            ImGui::EndChild();
        }
    }
    ImGui::End();

    if (!open) {
        /* Closing lets go of the decoded frames, the texture and a decode
         * still running (its worker stops before its next frame and frees
         * the job); the list is read
         * again the next time the window opens. */
        s_gal.open = false;
        galleryPreviewFree();
        galleryDropJob();
        galleryFreeItems();
    }
}

#endif /* !__ANDROID__ && BOLO_CLIP_GIF_GALLERY */
