/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          mapeditor_stamp.c
 * Purpose:
 *   Stamp/prefab library — file I/O, directory scanning,
 *   and thumbnail generation for reusable map snippets.
 *********************************************************/

#include "mapeditor_stamp.h"
#include "../gui/sdl3/minimap_render.h"
#include "mapeditor_imgui.h"  /* ME_TRANSPARENT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>
#include <strings.h>  /* strcasecmp */
#endif

/* -------------------------------------------------------
 * User stamps directory
 * ------------------------------------------------------- */

void stampGetUserDir(char *dest, int destLen) {
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        snprintf(dest, destLen, "%s\\WinBolo\\stamps", appdata);
    } else {
        snprintf(dest, destLen, "stamps");
    }
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(dest, destLen, "%s/Library/Application Support/WinBolo/stamps", home);
#else
    /* Linux / other POSIX */
    const char *dataHome = getenv("XDG_DATA_HOME");
    if (dataHome && dataHome[0]) {
        snprintf(dest, destLen, "%s/winbolo/stamps", dataHome);
    } else {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            if (pw) home = pw->pw_dir;
        }
        if (!home) home = ".";
        snprintf(dest, destLen, "%s/.local/share/winbolo/stamps", home);
    }
#endif
    dest[destLen - 1] = '\0';

    /* Create directory if it doesn't exist */
#ifdef _WIN32
    /* Create parent directories recursively */
    {
        char tmp[512];
        strncpy(tmp, dest, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '\\' || *p == '/') {
                *p = '\0';
                _mkdir(tmp);
                *p = '\\';
            }
        }
        _mkdir(tmp);
    }
#else
    {
        char tmp[512];
        strncpy(tmp, dest, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }
#endif
}

/* -------------------------------------------------------
 * Stamp file I/O
 * ------------------------------------------------------- */

bool stampSave(const char *filePath,
               int clipW, int clipH,
               const BYTE clipTerrain[256][256],
               int numPills, const pillbox *pills,
               int numBases, const base *bases,
               int numStarts, const start *starts) {
    if (clipW < 1 || clipW > 256 || clipH < 1 || clipH > 256) return false;
    if (numPills < 0 || numPills > MAX_PILLS) return false;
    if (numBases < 0 || numBases > MAX_BASES) return false;
    if (numStarts < 0 || numStarts > MAX_STARTS) return false;

    FILE *f = fopen(filePath, "wb");
    if (!f) return false;

    StampHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, STAMP_MAGIC, STAMP_MAGIC_LEN);
    hdr.width = (uint16_t)clipW;
    hdr.height = (uint16_t)clipH;
    hdr.numPills = (uint16_t)numPills;
    hdr.numBases = (uint16_t)numBases;
    hdr.numStarts = (uint16_t)numStarts;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }

    /* Terrain data: column-major (for x { for y { ... } }) */
    for (int x = 0; x < clipW; x++) {
        if (fwrite(&clipTerrain[x][0], 1, (size_t)clipH, f) != (size_t)clipH) {
            fclose(f);
            return false;
        }
    }

    /* Objects */
    if (numPills > 0) {
        if (fwrite(pills, sizeof(pillbox), (size_t)numPills, f) != (size_t)numPills) {
            fclose(f);
            return false;
        }
    }
    if (numBases > 0) {
        if (fwrite(bases, sizeof(base), (size_t)numBases, f) != (size_t)numBases) {
            fclose(f);
            return false;
        }
    }
    if (numStarts > 0) {
        if (fwrite(starts, sizeof(start), (size_t)numStarts, f) != (size_t)numStarts) {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

bool stampLoad(const char *filePath,
               BYTE clipTerrain[256][256], int *clipW, int *clipH,
               pillbox *pills, int *numPills,
               base *bases, int *numBases,
               start *starts, int *numStarts) {
    FILE *f = fopen(filePath, "rb");
    if (!f) return false;

    StampHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) { fclose(f); return false; }

    /* Validate magic */
    if (memcmp(hdr.magic, STAMP_MAGIC, STAMP_MAGIC_LEN) != 0) {
        fclose(f);
        return false;
    }

    /* Validate dimensions */
    if (hdr.width < 1 || hdr.width > 256 || hdr.height < 1 || hdr.height > 256) {
        fclose(f);
        return false;
    }
    if (hdr.numPills > MAX_PILLS || hdr.numBases > MAX_BASES || hdr.numStarts > MAX_STARTS) {
        fclose(f);
        return false;
    }

    *clipW = (int)hdr.width;
    *clipH = (int)hdr.height;

    /* Clear terrain buffer */
    memset(clipTerrain, 0, 256 * 256);

    /* Read terrain: column-major */
    for (int x = 0; x < (int)hdr.width; x++) {
        if (fread(&clipTerrain[x][0], 1, (size_t)hdr.height, f) != (size_t)hdr.height) {
            fclose(f);
            return false;
        }
    }

    /* Read objects */
    *numPills = (int)hdr.numPills;
    if (hdr.numPills > 0) {
        if (fread(pills, sizeof(pillbox), (size_t)hdr.numPills, f) != (size_t)hdr.numPills) {
            fclose(f);
            return false;
        }
    }

    *numBases = (int)hdr.numBases;
    if (hdr.numBases > 0) {
        if (fread(bases, sizeof(base), (size_t)hdr.numBases, f) != (size_t)hdr.numBases) {
            fclose(f);
            return false;
        }
    }

    *numStarts = (int)hdr.numStarts;
    if (hdr.numStarts > 0) {
        if (fread(starts, sizeof(start), (size_t)hdr.numStarts, f) != (size_t)hdr.numStarts) {
            fclose(f);
            return false;
        }
    }

    fclose(f);
    return true;
}

/* -------------------------------------------------------
 * Thumbnail generation
 * ------------------------------------------------------- */

static void stampGeneratePreview(StampEntry *entry, const BYTE *terrainData, int w, int h) {
    /* Fill with dark background */
    for (int py = 0; py < STAMP_PREVIEW_SIZE; py++) {
        for (int px = 0; px < STAMP_PREVIEW_SIZE; px++) {
            entry->previewPixels[py][px] = 0xFF1A1A1A; /* ABGR: dark grey */
        }
    }

    if (w <= 0 || h <= 0) {
        entry->previewReady = true;
        return;
    }

    /* Compute scale to fit within 64x64, preserving aspect ratio */
    float scaleX = (float)STAMP_PREVIEW_SIZE / (float)w;
    float scaleY = (float)STAMP_PREVIEW_SIZE / (float)h;
    float scale = scaleX < scaleY ? scaleX : scaleY;

    int dstW = (int)(w * scale);
    int dstH = (int)(h * scale);
    if (dstW > STAMP_PREVIEW_SIZE) dstW = STAMP_PREVIEW_SIZE;
    if (dstH > STAMP_PREVIEW_SIZE) dstH = STAMP_PREVIEW_SIZE;

    /* Center offset */
    int offX = (STAMP_PREVIEW_SIZE - dstW) / 2;
    int offY = (STAMP_PREVIEW_SIZE - dstH) / 2;

    for (int py = 0; py < dstH; py++) {
        for (int px = 0; px < dstW; px++) {
            /* Nearest-neighbor sampling */
            int srcX = (int)((float)px / scale);
            int srcY = (int)((float)py / scale);
            if (srcX >= w) srcX = w - 1;
            if (srcY >= h) srcY = h - 1;

            /* terrainData is column-major: terrainData[x * h + y] */
            BYTE terrain = terrainData[srcX * h + srcY];

            uint32_t pixel;
            if (terrain == ME_TRANSPARENT) {
                /* Checkerboard for transparency */
                int cx = (offX + px) / 4;
                int cy = (offY + py) / 4;
                if ((cx + cy) & 1) {
                    pixel = 0xFF606060; /* ABGR */
                } else {
                    pixel = 0xFF404040; /* ABGR */
                }
            } else {
                uint8_t r, g, b;
                minimapTerrainColor(terrain, &r, &g, &b);
                pixel = 0xFF000000 | ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
            }

            entry->previewPixels[offY + py][offX + px] = pixel;
        }
    }

    entry->previewReady = true;
}

/* -------------------------------------------------------
 * Library scanning
 * ------------------------------------------------------- */

static void stampLibraryGrow(StampLibrary *lib) {
    if (lib->count >= lib->capacity) {
        int newCap = lib->capacity == 0 ? 16 : lib->capacity * 2;
        StampEntry *newEntries = (StampEntry *)realloc(lib->entries, (size_t)newCap * sizeof(StampEntry));
        if (!newEntries) return;
        lib->entries = newEntries;
        lib->capacity = newCap;
    }
}

static void stampNameFromFilename(const char *filename, char *name, int nameLen) {
    /* Copy filename without extension */
    strncpy(name, filename, (size_t)(nameLen - 1));
    name[nameLen - 1] = '\0';

    /* Strip .bstamp extension */
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';

    /* Replace underscores with spaces */
    for (char *p = name; *p; p++) {
        if (*p == '_') *p = ' ';
    }
}

static bool stampHasBstampExt(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
#ifdef _WIN32
    return _stricmp(dot, ".bstamp") == 0;
#else
    return strcasecmp(dot, ".bstamp") == 0;
#endif
}

static void stampScanDirectory(StampLibrary *lib, const char *dirPath, bool isBundled) {
#ifdef _WIN32
    char searchPath[512];
    snprintf(searchPath, sizeof(searchPath), "%s\\*.bstamp", dirPath);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        stampLibraryGrow(lib);
        if (lib->count >= lib->capacity) break;

        StampEntry *entry = &lib->entries[lib->count];
        memset(entry, 0, sizeof(*entry));

        stampNameFromFilename(fd.cFileName, entry->name, sizeof(entry->name));
        snprintf(entry->filePath, sizeof(entry->filePath), "%s\\%s", dirPath, fd.cFileName);
        entry->isBundled = isBundled;

        /* Read header for metadata + terrain for preview */
        FILE *f = fopen(entry->filePath, "rb");
        if (f) {
            StampHeader hdr;
            if (fread(&hdr, sizeof(hdr), 1, f) == 1 &&
                memcmp(hdr.magic, STAMP_MAGIC, STAMP_MAGIC_LEN) == 0 &&
                hdr.width >= 1 && hdr.width <= 256 &&
                hdr.height >= 1 && hdr.height <= 256) {

                entry->width = (int)hdr.width;
                entry->height = (int)hdr.height;
                entry->numPills = (int)hdr.numPills;
                entry->numBases = (int)hdr.numBases;
                entry->numStarts = (int)hdr.numStarts;

                /* Read terrain for thumbnail */
                size_t terrainSize = (size_t)hdr.width * (size_t)hdr.height;
                BYTE *terrainBuf = (BYTE *)malloc(terrainSize);
                bool stampOk = false;
                if (terrainBuf) {
                    size_t bytesRead = 0;
                    for (int x = 0; x < (int)hdr.width; x++) {
                        bytesRead += fread(terrainBuf + x * (int)hdr.height,
                                           1, (size_t)hdr.height, f);
                    }
                    if (bytesRead == terrainSize) {
                        stampGeneratePreview(entry, terrainBuf, (int)hdr.width, (int)hdr.height);
                        stampOk = true;
                    }
                    free(terrainBuf);
                }

                if (stampOk) lib->count++;
            }
            fclose(f);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

#else
    DIR *dir = opendir(dirPath);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!stampHasBstampExt(ent->d_name)) continue;

        stampLibraryGrow(lib);
        if (lib->count >= lib->capacity) break;

        StampEntry *entry = &lib->entries[lib->count];
        memset(entry, 0, sizeof(*entry));

        stampNameFromFilename(ent->d_name, entry->name, sizeof(entry->name));
        snprintf(entry->filePath, sizeof(entry->filePath), "%s/%s", dirPath, ent->d_name);
        entry->isBundled = isBundled;

        /* Read header for metadata + terrain for preview */
        FILE *f = fopen(entry->filePath, "rb");
        if (f) {
            StampHeader hdr;
            if (fread(&hdr, sizeof(hdr), 1, f) == 1 &&
                memcmp(hdr.magic, STAMP_MAGIC, STAMP_MAGIC_LEN) == 0 &&
                hdr.width >= 1 && hdr.width <= 256 &&
                hdr.height >= 1 && hdr.height <= 256) {

                entry->width = (int)hdr.width;
                entry->height = (int)hdr.height;
                entry->numPills = (int)hdr.numPills;
                entry->numBases = (int)hdr.numBases;
                entry->numStarts = (int)hdr.numStarts;

                /* Read terrain for thumbnail */
                size_t terrainSize = (size_t)hdr.width * (size_t)hdr.height;
                BYTE *terrainBuf = (BYTE *)malloc(terrainSize);
                bool stampOk = false;
                if (terrainBuf) {
                    size_t bytesRead = 0;
                    for (int x = 0; x < (int)hdr.width; x++) {
                        bytesRead += fread(terrainBuf + x * (int)hdr.height,
                                           1, (size_t)hdr.height, f);
                    }
                    if (bytesRead == terrainSize) {
                        stampGeneratePreview(entry, terrainBuf, (int)hdr.width, (int)hdr.height);
                        stampOk = true;
                    }
                    free(terrainBuf);
                }

                if (stampOk) lib->count++;
            }
            fclose(f);
        }
    }

    closedir(dir);
#endif
}

/* -------------------------------------------------------
 * Public API
 * ------------------------------------------------------- */

void stampLibraryInit(StampLibrary *lib, const char *dataDir) {
    memset(lib, 0, sizeof(*lib));

    /* Scan bundled stamps directory */
    if (dataDir && dataDir[0]) {
        char bundledDir[512];
        snprintf(bundledDir, sizeof(bundledDir), "%s/stamps", dataDir);
        stampScanDirectory(lib, bundledDir, true);
    }

    /* Scan user stamps directory */
    char userDir[512];
    stampGetUserDir(userDir, sizeof(userDir));
    stampScanDirectory(lib, userDir, false);
}

void stampLibraryFree(StampLibrary *lib) {
    if (lib->entries) {
        for (int i = 0; i < lib->count; i++) {
            if (lib->entries[i].tex) {
                SDL_DestroyTexture(lib->entries[i].tex);
                lib->entries[i].tex = NULL;
            }
        }
        free(lib->entries);
        lib->entries = NULL;
    }
    lib->count = 0;
    lib->capacity = 0;
}

void stampLibraryRefresh(StampLibrary *lib, const char *dataDir) {
    stampLibraryFree(lib);
    stampLibraryInit(lib, dataDir);
}
