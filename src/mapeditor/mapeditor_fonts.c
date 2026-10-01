/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_fonts.c
 * Purpose:
 *   Font enumeration for the map editor. Scans bundled
 *   and system font directories for TTF/OTF files.
 *********************************************************/

#include "mapeditor_fonts.h"

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__) && !defined(__IPHONEOS__)

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

/* Check if a filename ends with .ttf, .otf, or .ttc (case-insensitive) */
static bool isFontFile(const char *name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    if (SDL_strcasecmp(ext, ".ttf") == 0) return true;
    if (SDL_strcasecmp(ext, ".otf") == 0) return true;
    if (SDL_strcasecmp(ext, ".ttc") == 0) return true;
    return false;
}

/* Add a font file to the list, reading family/style via SDL_TTF */
static void fontListAdd(FontList *list, const char *filePath, bool isBundled) {
    if (list->count >= FONT_MAX_ENTRIES) return;

    TTF_Font *font = TTF_OpenFont(filePath, 16.0f);
    if (!font) return;

    const char *family = TTF_GetFontFamilyName(font);
    const char *style = TTF_GetFontStyleName(font);

    FontEntry *e = &list->entries[list->count];
    SDL_strlcpy(e->familyName, family ? family : "Unknown", FONT_NAME_MAX);
    SDL_strlcpy(e->styleName, style ? style : "Regular", FONT_STYLE_MAX);
    SDL_strlcpy(e->filePath, filePath, FONT_PATH_MAX);
    e->isBundled = isBundled;
    list->count++;

    TTF_CloseFont(font);
}

/* Recursive directory scan context */
typedef struct {
    FontList *list;
    bool isBundled;
    int depth;
} ScanContext;

/* SDL_EnumerateDirectory callback for recursive font scanning */
static SDL_EnumerationResult fontScanCallback(void *userdata, const char *dirname, const char *fname) {
    ScanContext *ctx = (ScanContext *)userdata;
    if (ctx->list->count >= FONT_MAX_ENTRIES) return SDL_ENUM_SUCCESS;

    char fullPath[FONT_PATH_MAX];
    SDL_snprintf(fullPath, sizeof(fullPath), "%s/%s", dirname, fname);

    if (isFontFile(fname)) {
        fontListAdd(ctx->list, fullPath, ctx->isBundled);
    } else if (ctx->depth < 4) {
        /* Only recurse into actual directories, skip regular non-font files */
        SDL_PathInfo info;
        if (SDL_GetPathInfo(fullPath, &info) && info.type == SDL_PATHTYPE_DIRECTORY) {
            ScanContext sub;
            sub.list = ctx->list;
            sub.isBundled = ctx->isBundled;
            sub.depth = ctx->depth + 1;
            SDL_EnumerateDirectory(fullPath, fontScanCallback, &sub);
        }
    }

    return SDL_ENUM_CONTINUE;
}

static void scanDirectory(FontList *list, const char *dirPath, bool isBundled) {
    ScanContext ctx;
    ctx.list = list;
    ctx.isBundled = isBundled;
    ctx.depth = 0;
    SDL_EnumerateDirectory(dirPath, fontScanCallback, &ctx);
}

/* qsort comparator: bundled first, then alphabetical by family name */
static int fontEntryCmp(const void *a, const void *b) {
    const FontEntry *fa = (const FontEntry *)a;
    const FontEntry *fb = (const FontEntry *)b;

    /* Bundled fonts first */
    if (fa->isBundled != fb->isBundled) {
        return fa->isBundled ? -1 : 1;
    }

    /* Alphabetical by family name */
    int cmp = SDL_strcasecmp(fa->familyName, fb->familyName);
    if (cmp != 0) return cmp;

    /* Within same family, sort by style name */
    return SDL_strcasecmp(fa->styleName, fb->styleName);
}

void fontListInit(FontList *list) {
    list->count = 0;

    /* Bundled fonts first — use absolute path via SDL_GetBasePath() */
    {
        const char *base = SDL_GetBasePath();
        char bundledPath[512];
        SDL_snprintf(bundledPath, sizeof(bundledPath), "%sdata/fonts", base ? base : "");
        scanDirectory(list, bundledPath, true);
    }

    /* System fonts — platform-specific directories */
#ifdef _WIN32
    {
        char winFonts[MAX_PATH];
        if (SHGetFolderPathA(NULL, CSIDL_FONTS, NULL, 0, winFonts) == S_OK) {
            scanDirectory(list, winFonts, false);
        }
    }
#elif defined(__APPLE__)
    scanDirectory(list, "/System/Library/Fonts", false);
    scanDirectory(list, "/Library/Fonts", false);
    {
        const char *home = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "HOME");
        if (home) {
            char userFonts[FONT_PATH_MAX];
            SDL_snprintf(userFonts, sizeof(userFonts), "%s/Library/Fonts", home);
            scanDirectory(list, userFonts, false);
        }
    }
#else
    /* Linux / BSD */
    scanDirectory(list, "/usr/share/fonts", false);
    scanDirectory(list, "/usr/local/share/fonts", false);
    {
        const char *home = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "HOME");
        if (home) {
            char buf[FONT_PATH_MAX];
            SDL_snprintf(buf, sizeof(buf), "%s/.fonts", home);
            scanDirectory(list, buf, false);
            SDL_snprintf(buf, sizeof(buf), "%s/.local/share/fonts", home);
            scanDirectory(list, buf, false);
        }
    }
#endif

    /* Sort: bundled first, then alphabetical by family name */
    if (list->count > 1) {
        qsort(list->entries, (size_t)list->count, sizeof(FontEntry), fontEntryCmp);
    }
}

int fontListFindStyles(const FontList *list, const char *familyName,
                       int *outIndices, int outMax) {
    int count = 0;
    for (int i = 0; i < list->count && count < outMax; i++) {
        if (SDL_strcasecmp(list->entries[i].familyName, familyName) == 0) {
            outIndices[count++] = i;
        }
    }
    return count;
}

int fontListGetFamilies(const FontList *list, const char **outNames, int outMax) {
    int count = 0;
    for (int i = 0; i < list->count && count < outMax; i++) {
        /* Skip duplicates (list is sorted by family name within bundled/system groups) */
        bool dup = false;
        for (int j = 0; j < count; j++) {
            if (SDL_strcasecmp(outNames[j], list->entries[i].familyName) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            outNames[count++] = list->entries[i].familyName;
        }
    }
    return count;
}

#else /* Non-desktop platforms */

void fontListInit(FontList *list) {
    list->count = 0;
}

int fontListFindStyles(const FontList *list, const char *familyName,
                       int *outIndices, int outMax) {
    (void)list; (void)familyName; (void)outIndices; (void)outMax;
    return 0;
}

int fontListGetFamilies(const FontList *list, const char **outNames, int outMax) {
    (void)list; (void)outNames; (void)outMax;
    return 0;
}

#endif /* desktop platforms */
