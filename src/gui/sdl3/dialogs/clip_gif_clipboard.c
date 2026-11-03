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

/*
 * A GIF file on the system clipboard, and a GIF shown in its folder. See
 * clip_gif_clipboard.h.
 */

#include "clip_gif_clipboard.h"

#include <string.h>

#include <SDL3/SDL.h>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>   /* DROPFILES */
#include <shellapi.h> /* ShellExecuteW */

/* Another program can hold the clipboard open for a moment, so the open is
 * tried a few times before giving up. */
static bool clipOpen(HWND hwnd) {
    for (int i = 0; i < 5; i++) {
        if (OpenClipboard(hwnd)) {
            return true;
        }
        SDL_Delay(10);
    }
    return false;
}

/* gifPath as a full wide path with backslashes, which is what Explorer
 * wants. SDL_malloc'd; NULL on failure. */
static WCHAR *clipFullWidePath(const char *gifPath) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, gifPath, -1, NULL, 0);
    if (wlen <= 1) {
        return NULL;
    }
    WCHAR *wpath = (WCHAR *)SDL_malloc((size_t)wlen * sizeof(WCHAR));
    if (wpath == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, gifPath, -1, wpath, wlen);
    for (int i = 0; i < wlen; i++) {
        if (wpath[i] == L'/') {
            wpath[i] = L'\\';
        }
    }
    DWORD fullLen = GetFullPathNameW(wpath, 0, NULL, NULL);
    WCHAR *full = NULL;
    if (fullLen > 0) {
        full = (WCHAR *)SDL_malloc((size_t)fullLen * sizeof(WCHAR));
        if (full != NULL &&
            GetFullPathNameW(wpath, fullLen, full, NULL) == 0) {
            SDL_free(full);
            full = NULL;
        }
    }
    if (full == NULL) {
        return wpath;
    }
    SDL_free(wpath);
    return full;
}

/* The file as a CF_HDROP block: a DROPFILES header and then the one wide
 * path, ended by two NULs. NULL on failure. */
static HGLOBAL clipMakeDrop(const char *gifPath) {
    WCHAR *use = clipFullWidePath(gifPath);
    if (use == NULL) {
        return NULL;
    }
    size_t useLen = wcslen(use);

    SIZE_T bytes = sizeof(DROPFILES) + (useLen + 2) * sizeof(WCHAR);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (h != NULL) {
        DROPFILES *df = (DROPFILES *)GlobalLock(h);
        if (df != NULL) {
            df->pFiles = sizeof(DROPFILES);
            df->fWide  = TRUE;
            memcpy((BYTE *)df + sizeof(DROPFILES), use,
                   useLen * sizeof(WCHAR));
            /* The two closing NULs are already there from GMEM_ZEROINIT. */
            GlobalUnlock(h);
        } else {
            GlobalFree(h);
            h = NULL;
        }
    }
    SDL_free(use);
    return h;
}

static HGLOBAL clipMakeBytes(const void *data, size_t len) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
    if (h == NULL) {
        return NULL;
    }
    void *p = GlobalLock(h);
    if (p == NULL) {
        GlobalFree(h);
        return NULL;
    }
    memcpy(p, data, len);
    GlobalUnlock(h);
    return h;
}

bool clipGifCopyFileToClipboard(const char *gifPath, void *window) {
    if (gifPath == NULL || gifPath[0] == '\0') {
        return false;
    }
    size_t len = 0;
    void *data = SDL_LoadFile(gifPath, &len);
    if (data == NULL) {
        return false;
    }
    HGLOBAL drop = clipMakeDrop(gifPath);
    HGLOBAL gif  = (len > 0) ? clipMakeBytes(data, len) : NULL;
    SDL_free(data);
    if (drop == NULL) {
        if (gif != NULL) GlobalFree(gif);
        return false;
    }

    HWND hwnd = NULL;
    if (window != NULL) {
        hwnd = (HWND)SDL_GetPointerProperty(
            SDL_GetWindowProperties((SDL_Window *)window),
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    }
    if (!clipOpen(hwnd)) {
        GlobalFree(drop);
        if (gif != NULL) GlobalFree(gif);
        return false;
    }
    EmptyClipboard();
    /* Once SetClipboardData takes a handle the system owns it; a handle it
     * refused is still ours to free. */
    bool ok = (SetClipboardData(CF_HDROP, drop) != NULL);
    if (!ok) {
        GlobalFree(drop);
    }
    if (gif != NULL) {
        UINT fmt = RegisterClipboardFormatW(L"GIF");
        if (fmt == 0 || SetClipboardData(fmt, gif) == NULL) {
            GlobalFree(gif);
        }
    }
    CloseClipboard();
    return ok;
}

bool clipGifShowFileInFolder(const char *gifPath) {
    if (gifPath == NULL || gifPath[0] == '\0') {
        return false;
    }
    WCHAR *full = clipFullWidePath(gifPath);
    if (full == NULL) {
        return false;
    }
    /* explorer /select,"<path>" opens the folder with the file selected. */
    size_t fullLen = wcslen(full);
    static const WCHAR prefix[] = L"/select,\"";
    size_t prefixLen = (sizeof(prefix) / sizeof(prefix[0])) - 1;
    WCHAR *args =
        (WCHAR *)SDL_malloc((prefixLen + fullLen + 2) * sizeof(WCHAR));
    bool ok = false;
    if (args != NULL) {
        SDL_memcpy(args, prefix, prefixLen * sizeof(WCHAR));
        SDL_memcpy(args + prefixLen, full, fullLen * sizeof(WCHAR));
        args[prefixLen + fullLen] = L'"';
        args[prefixLen + fullLen + 1] = L'\0';
        HINSTANCE r = ShellExecuteW(NULL, L"open", L"explorer.exe", args,
                                    NULL, SW_SHOWNORMAL);
        ok = ((INT_PTR)r > 32);
        SDL_free(args);
    }
    if (!ok) {
        /* Explorer did not start with the file selected: open the folder
         * itself, which SDL hands to the shell. */
        WCHAR *cut = wcsrchr(full, L'\\');
        if (cut != NULL) {
            *cut = L'\0';
            char *folder = SDL_iconv_wchar_utf8(full);
            if (folder != NULL) {
                ok = SDL_OpenURL(folder);
                SDL_free(folder);
            }
        }
    }
    SDL_free(full);
    return ok;
}

#else /* !_WIN32 */

/* What the clipboard callback hands out: the GIF's bytes, the file's
 * file:// address, and on Linux the same address in the form GNOME's file
 * managers paste as a file. Freed by the cleanup callback when the clipboard
 * is replaced.
 *
 * On macOS SDL turns each MIME type into a system type: "image/gif" becomes
 * the GIF type and pastes as a picture, but "text/uri-list" has no system
 * type, so Finder and chat apps do not see a file there. On macOS the copy
 * therefore pastes the GIF's bytes only. */
typedef struct {
    void  *bytes;
    size_t len;
    char  *uri;
    char  *gnome;  /* "copy\nfile://...", NULL when not made */
} ClipGifClipboardData;

static const char *const CLIP_GIF_MIME_GIF = "image/gif";
static const char *const CLIP_GIF_MIME_URI = "text/uri-list";
#if !defined(__APPLE__)
static const char *const CLIP_GIF_MIME_GNOME = "x-special/gnome-copied-files";
#endif

static const void *SDLCALL clipDataCallback(void *userdata,
                                            const char *mime_type,
                                            size_t *size) {
    ClipGifClipboardData *d = (ClipGifClipboardData *)userdata;
    if (d == NULL || mime_type == NULL) {
        *size = 0;
        return NULL;
    }
    if (SDL_strcmp(mime_type, CLIP_GIF_MIME_GIF) == 0) {
        *size = d->len;
        return d->bytes;
    }
    if (SDL_strcmp(mime_type, CLIP_GIF_MIME_URI) == 0 && d->uri != NULL) {
        *size = SDL_strlen(d->uri);
        return d->uri;
    }
#if !defined(__APPLE__)
    if (SDL_strcmp(mime_type, CLIP_GIF_MIME_GNOME) == 0 && d->gnome != NULL) {
        *size = SDL_strlen(d->gnome);
        return d->gnome;
    }
#endif
    *size = 0;
    return NULL;
}

static void SDLCALL clipCleanupCallback(void *userdata) {
    ClipGifClipboardData *d = (ClipGifClipboardData *)userdata;
    if (d != NULL) {
        SDL_free(d->bytes);
        SDL_free(d->uri);
        SDL_free(d->gnome);
        SDL_free(d);
    }
}

/* "file://" and the path, with every byte other than letters, digits, '/',
 * '-', '_', '.' and '~' written as %XX. NULL on failure. */
static char *clipFileUri(const char *path) {
    size_t n = SDL_strlen(path);
    char *out = (char *)SDL_malloc(7 + n * 3 + 3);
    if (out == NULL) {
        return NULL;
    }
    static const char hex[] = "0123456789ABCDEF";
    char *w = out;
    SDL_memcpy(w, "file://", 7);
    w += 7;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        bool keep = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || c == '/' || c == '-' ||
                    c == '_' || c == '.' || c == '~';
        if (keep) {
            *w++ = (char)c;
        } else {
            *w++ = '%';
            *w++ = hex[c >> 4];
            *w++ = hex[c & 15];
        }
    }
    *w++ = '\r';
    *w++ = '\n';
    *w = '\0';
    return out;
}

bool clipGifCopyFileToClipboard(const char *gifPath, void *window) {
    (void)window;
    if (gifPath == NULL || gifPath[0] == '\0') {
        return false;
    }
    /* SDL_SetClipboardData returns before it takes the data when there is no
     * video, and then never calls the cleanup, so that case stops here. */
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        return false;
    }
    ClipGifClipboardData *d =
        (ClipGifClipboardData *)SDL_calloc(1, sizeof(*d));
    if (d == NULL) {
        return false;
    }
    d->bytes = SDL_LoadFile(gifPath, &d->len);
    if (d->bytes == NULL || d->len == 0) {
        clipCleanupCallback(d);
        return false;
    }
    d->uri = clipFileUri(gifPath);
#if !defined(__APPLE__)
    if (d->uri != NULL) {
        /* "copy", a newline, and the address without the uri-list's
         * CRLF. */
        size_t uriLen = SDL_strlen(d->uri);
        if (uriLen >= 2) {
            uriLen -= 2;
        }
        d->gnome = (char *)SDL_malloc(5 + uriLen + 1);
        if (d->gnome != NULL) {
            SDL_memcpy(d->gnome, "copy\n", 5);
            SDL_memcpy(d->gnome + 5, d->uri, uriLen);
            d->gnome[5 + uriLen] = '\0';
        }
    }
#endif

    const char *mimes[3];
    size_t mimeCount = 0;
    mimes[mimeCount++] = CLIP_GIF_MIME_GIF;
    if (d->uri != NULL) {
        mimes[mimeCount++] = CLIP_GIF_MIME_URI;
    }
#if !defined(__APPLE__)
    if (d->gnome != NULL) {
        mimes[mimeCount++] = CLIP_GIF_MIME_GNOME;
    }
#endif
    /* From here SDL owns d: when the platform refuses the data, SDL still
     * calls the cleanup the next time the clipboard is set or cleared, so d
     * is not freed here on failure. */
    return SDL_SetClipboardData(clipDataCallback, clipCleanupCallback, d,
                                mimes, mimeCount);
}

bool clipGifShowFileInFolder(const char *gifPath) {
    if (gifPath == NULL || gifPath[0] == '\0') {
        return false;
    }
    /* The folder is the path up to its last '/'. */
    const char *cut = SDL_strrchr(gifPath, '/');
    if (cut == NULL || cut == gifPath) {
        return false;
    }
    size_t folderLen = (size_t)(cut - gifPath);
    char *folder = (char *)SDL_malloc(folderLen + 1);
    if (folder == NULL) {
        return false;
    }
    SDL_memcpy(folder, gifPath, folderLen);
    folder[folderLen] = '\0';
    char *uri = clipFileUri(folder);
    SDL_free(folder);
    if (uri == NULL) {
        return false;
    }
    /* clipFileUri ends the address with the uri-list's CRLF; a URL has none. */
    size_t uriLen = SDL_strlen(uri);
    if (uriLen >= 2) {
        uri[uriLen - 2] = '\0';
    }
    bool ok = SDL_OpenURL(uri);
    SDL_free(uri);
    return ok;
}

#endif /* _WIN32 */
