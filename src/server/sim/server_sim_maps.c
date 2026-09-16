/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Server Simulation Map Loading
 *Filename:      server_sim_maps.c
 *Author:        John Morrison
 *Purpose:
 *  The map directory and the map-loading paths — scanning
 *  and random pick, reload and preview, path resolution,
 *  enumeration and search.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "server_sim_shared.h"      /* serverSimCacheMapMd5FromFile — hashes a loaded .map for WinBolo.net */
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"   /* lobbyAutoUnreadyOnChange */
#include "server_sim_join.h"        /* serverSimAssignLobbyStartOnJoin — start reconcile after a map change */
#include "server_sim_scenario.h"    /* serverSimScenarioListDir — what the public scenario enumeration copies from */
#include "bolo_rand.h"              /* bolo_rand_below — the rotation's random pick */
#include "bolo_map_validate.h"      /* boloMapBodyLength — where the preview's read stops */
#include "client_sim.h"             /* clientSimGetGameSim — the in-process client map reload */
#include "../../common/md5.h"       /* the compressed-map hash the preview paths compare */
#include "../../common/mp_diag_log.h"
#include "../../common/wb_log.h"

/* Every server-side map mutator calls this at the end of its success
 * path; defined below, after the reload and preview paths that call it. */
static void serverSimApplyMapChange(ServerSim *sim);

bool serverSimRandomMapRegenerate(ServerSim *sim) {
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
    int len;
    char seedStr[64];
    char msg[128];
    int x, y;
    MapGenConfig cfg;

    if (!sim->randomMapEnabled) return FALSE;
    if (sim->state != serverStateLobby) return FALSE;

    cfg = sim->randomMapConfig;

    if (!sim->randomMapFixedSeed) {
        /* Generate a new random seed */
        cfg.seed = (uint32_t)time(NULL) ^ ((uint32_t)clock() << 16);
        if (cfg.seed == 0) cfg.seed = 1;
    }
    /* else: keep the same seed for reproducible maps */

    /* Clear map */
    memset((*sim->sim.mp).mapItem, DEEP_SEA, sizeof((*sim->sim.mp).mapItem));

    /* Fill mine border */
    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
            }
        }
    }

    /* Clear and regenerate objects */
    sim->sim.pb->numPills = 0;
    sim->sim.bs->numBases = 0;
    sim->sim.ss->numStarts = 0;
    mapGenRun(sim->sim.mp, sim->sim.bs, sim->sim.pb, sim->sim.ss, &cfg);

    /* Run generated objects through the same init path as file-loaded maps */
    {
        BYTE i;
        for (i = 0; i < sim->sim.pb->numPills; i++) {
            pillbox tmp = sim->sim.pb->item[i];
            pillsSetPill(&sim->sim, &sim->sim.pb, &tmp, (BYTE)(i + 1));
        }
        for (i = 0; i < sim->sim.bs->numBases; i++) {
            base tmp = sim->sim.bs->item[i];
            basesSetBase(&sim->sim.bs, &tmp, (BYTE)(i + 1));
        }
        for (i = 0; i < sim->sim.ss->numStarts; i++) {
            start tmp = sim->sim.ss->item[i];
            startsSetStart(&sim->sim.ss, &tmp, (BYTE)(i + 1));
        }
    }
    /* The generated map is this sim's too: cap what it put in the lists
       against the rules, as a loaded one is capped. */
    mapClampToRules(&sim->sim);

    basesClearMines(&sim->sim);

    /* Update cached map */
    len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData == NULL) {
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    /* Update map name */
    mapGenConfigToSeed(&cfg, seedStr, sizeof(seedStr));
    snprintf(sim->mapName, MAP_STR_SIZE, "rand_%.30s", seedStr);

    /* Store updated config */
    sim->randomMapConfig = cfg;

    snprintf(msg, sizeof(msg), "Random map regenerated, seed: %s", seedStr);
    serverSimConsoleMessage(msg);

    /* Generated, not read — nothing to look beside. */
    sim->mapFilePath[0] = '\0';

    serverSimApplyMapChange(sim);
    return TRUE;
}

void serverSimEnableRandomMap(ServerSim *sim,
                              const MapGenConfig *cfg,
                              bool fixedSeed) {
    sim->randomMapEnabled = true;
    if (cfg != NULL) sim->randomMapConfig = *cfg;
    sim->randomMapFixedSeed = fixedSeed;
}

void serverSimInstallMapDirList(ServerSim *sim,
                                char **files, int count,
                                const char *dirPath) {
    sim->mapDirFiles = files;
    sim->mapDirCount = count;

    /* Capture the dirPath as the canonical server-side map root, the
     * same way serverSimMapDirBuild does, so serverSimEnumerateMapDir /
     * serverSimSearchMapDir / SET_MAP path resolution all read from the
     * directory the rotation list was built from rather than falling
     * back to the default "data/maps". Strip a trailing slash to keep
     * "<root>/<rel>" concatenations clean. */
    if (dirPath != NULL) {
        if (sim->mapDirPath != NULL) {
            free(sim->mapDirPath);
            sim->mapDirPath = NULL;
        }
        sim->mapDirPath = SDL_strdup(dirPath);
        if (sim->mapDirPath != NULL) {
            size_t plen = SDL_strlen(sim->mapDirPath);
            while (plen > 1 && (sim->mapDirPath[plen - 1] == '/' ||
                                sim->mapDirPath[plen - 1] == '\\')) {
                sim->mapDirPath[--plen] = '\0';
            }
        }
    }
}

void serverSimSetMapName(ServerSim *sim, const char *name) {
    /* Name only — the map md5 tracks content, not the display name, so
     * it is set/cleared by the content-load paths (serverSimReloadMap,
     * serverSimChangeMap, serverSimReloadCompressedInMemory) and the
     * revert path. Touching it here would wipe a freshly-computed hash
     * when callers fix up the display name after a content load. */
    if (name == NULL || name[0] == '\0') {
        sim->mapName[0] = '\0';
        return;
    }
    strncpy(sim->mapName, name, MAP_STR_SIZE - 1);
    sim->mapName[MAP_STR_SIZE - 1] = '\0';
}

bool serverSimScanMapDir(const char *dirPath,
                         char ***outFiles, int *outCount) {
    char fullPath[2048];
    char **tempList = NULL;
    int tempCount = 0;
    int tempCapacity = 0;
    int globCount = 0;
    int i;
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;

    /* Use SDL3's cross-platform directory globbing */
    char **files = SDL_GlobDirectory(dirPath, "*.map", 0, &globCount);
    if (files == NULL || globCount == 0) {
        fprintf(stderr, "Error: no .map files found in '%s'\n", dirPath);
        if (files) SDL_free(files);
        return FALSE;
    }

    for (i = 0; i < globCount; i++) {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, files[i]);

        /* Validate map by attempting to load it */
        mapCreate(&mp);
        pillsCreate(&pb);
        basesCreate(&bs);
        startsCreate(&ss);
        if (mapRead(fullPath, &mp, &pb, &bs, &ss) == FALSE) {
            mapDestroy(&mp);
            pillsDestroy(&pb);
            basesDestroy(&bs);
            startsDestroy(&ss);
            fprintf(stderr, "Warning: skipping invalid map '%s'\n", fullPath);
            continue;
        }
        mapDestroy(&mp);
        pillsDestroy(&pb);
        basesDestroy(&bs);
        startsDestroy(&ss);

        /* Grow array if needed */
        if (tempCount >= tempCapacity) {
            int newCap = tempCapacity == 0 ? 16 : tempCapacity * 2;
            char **newList = realloc(tempList, newCap * sizeof(char *));
            if (newList == NULL) {
                fprintf(stderr, "Error: out of memory building map list\n");
                break;
            }
            tempList = newList;
            tempCapacity = newCap;
        }

        tempList[tempCount] = SDL_strdup(fullPath);
        if (tempList[tempCount] == NULL) {
            fprintf(stderr, "Error: out of memory duplicating path\n");
            break;
        }
        tempCount++;
        fprintf(stderr, "Map directory: validated '%s'\n", files[i]);
    }

    SDL_free(files);

    if (tempCount == 0) {
        fprintf(stderr, "Error: no valid .map files found in '%s'\n", dirPath);
        free(tempList);
        return FALSE;
    }

    *outFiles = tempList;
    *outCount = tempCount;
    return TRUE;
}

bool serverSimMapDirBuild(ServerSim *sim, const char *dirPath) {
    char **files = NULL;
    int count = 0;
    if (!serverSimScanMapDir(dirPath, &files, &count)) {
        return FALSE;
    }
    sim->mapDirFiles = files;
    sim->mapDirCount = count;

    /* Capture the dirPath as the canonical server-side map root so
     * serverSimEnumerateMapDir / serverSimSearchMapDir / SET_MAP path
     * resolution all read from the same place the rotation list was
     * built from. Strip a trailing slash to keep concatenations
     * ("<root>/<rel>") clean. */
    if (sim->mapDirPath) {
        free(sim->mapDirPath);
        sim->mapDirPath = NULL;
    }
    sim->mapDirPath = SDL_strdup(dirPath);
    if (sim->mapDirPath != NULL) {
        size_t plen = SDL_strlen(sim->mapDirPath);
        while (plen > 1 && (sim->mapDirPath[plen - 1] == '/' ||
                            sim->mapDirPath[plen - 1] == '\\')) {
            sim->mapDirPath[--plen] = '\0';
        }
    }

    fprintf(stderr, "Map directory: %d valid map(s) loaded from '%s'\n",
            count, dirPath);
    return TRUE;
}

const char *serverSimGetMapDirRoot(const ServerSim *sim) {
    if (sim != NULL && sim->mapDirPath != NULL && sim->mapDirPath[0] != '\0') {
        return sim->mapDirPath;
    }
    return "data/maps";
}

void serverSimSetScenarioDir(ServerSim *sim, const char *dir) {
    if (sim == NULL) return;
    if (dir != NULL) {
        SDL_strlcpy(sim->scenarioDirPath, dir, sizeof(sim->scenarioDirPath));
    } else {
        sim->scenarioDirPath[0] = '\0';
    }
}

const char *serverSimGetScenarioDir(const ServerSim *sim) {
    if (sim != NULL && sim->scenarioDirPath[0] != '\0') {
        return sim->scenarioDirPath;
    }
    return "data/scenarios";
}

void serverSimSetSelectedScenario(ServerSim *sim, const char *file) {
    if (sim == NULL) return;
    if (file != NULL) {
        SDL_strlcpy(sim->scenarioSelectedFile, file,
                    sizeof(sim->scenarioSelectedFile));
    } else {
        sim->scenarioSelectedFile[0] = '\0';
    }
}

const char *serverSimGetSelectedScenario(const ServerSim *sim) {
    if (sim == NULL) return "";
    return sim->scenarioSelectedFile;
}

void serverSimSetUploadPersistDir(ServerSim *sim, const char *dir) {
    if (sim == NULL) return;
    if (dir != NULL) {
        SDL_strlcpy(sim->uploadPersistDir, dir, sizeof(sim->uploadPersistDir));
    } else {
        sim->uploadPersistDir[0] = '\0';
    }
}

bool serverSimMapDirPickRandom(ServerSim *sim) {
    int idx;
    char msg[512];

    if (sim->mapDirFiles == NULL || sim->mapDirCount <= 0) {
        return FALSE;
    }

    idx = (int)bolo_rand_below((uint32_t)sim->mapDirCount);

    /* Try to avoid picking the same map we're already on */
    if (sim->mapDirCount > 1) {
        int attempts;
        for (attempts = 0; attempts < 10; attempts++) {
            const char *base = sim->mapDirFiles[idx];
            const char *p;
            for (p = sim->mapDirFiles[idx]; *p; p++) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            if (strcmp(base, sim->mapName) != 0) break;
            idx = (int)bolo_rand_below((uint32_t)sim->mapDirCount);
        }
    }

    if (serverSimChangeMap(sim, sim->mapDirFiles[idx]) == FALSE) {
        /* Map may have been deleted or corrupted since startup — try others */
        int tries;
        for (tries = 0; tries < sim->mapDirCount; tries++) {
            idx = (idx + 1) % sim->mapDirCount;
            if (serverSimChangeMap(sim, sim->mapDirFiles[idx]) == TRUE) {
                break;
            }
        }
        if (tries >= sim->mapDirCount) {
            fprintf(stderr, "Error: all maps in directory failed to load\n");
            return FALSE;
        }
    }

    snprintf(msg, sizeof(msg), "Map rotation: loaded '%s'", sim->mapName);
    serverSimConsoleMessage(msg);
    serverSimApplyMapChange(sim);
    return TRUE;
}

void serverSimMapDirDestroy(ServerSim *sim) {
    if (sim->mapDirFiles != NULL) {
        int i;
        for (i = 0; i < sim->mapDirCount; i++) {
            free(sim->mapDirFiles[i]);
        }
        free(sim->mapDirFiles);
        sim->mapDirFiles = NULL;
        sim->mapDirCount = 0;
    }
    if (sim->mapDirPath != NULL) {
        SDL_free(sim->mapDirPath);
        sim->mapDirPath = NULL;
    }
}

/* ────────────────────────────────────────────────────────────────
 * Map preview / map upload — Lobby Layout A
 * ──────────────────────────────────────────────────────────────── */

static bool serverSimApplyRandomMapConfig(ServerSim *sim,
                                          const MapGenConfig *cfg) {
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
    int len;
    int x, y;

    memset((*sim->sim.mp).mapItem, DEEP_SEA, sizeof((*sim->sim.mp).mapItem));
    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
            }
        }
    }

    sim->sim.pb->numPills  = 0;
    sim->sim.bs->numBases  = 0;
    sim->sim.ss->numStarts = 0;
    mapGenRun(sim->sim.mp, sim->sim.bs, sim->sim.pb, sim->sim.ss, cfg);

    {
        BYTE i;
        for (i = 0; i < sim->sim.pb->numPills; i++) {
            pillbox tmp = sim->sim.pb->item[i];
            pillsSetPill(&sim->sim, &sim->sim.pb, &tmp, (BYTE)(i + 1));
        }
        for (i = 0; i < sim->sim.bs->numBases; i++) {
            base tmp = sim->sim.bs->item[i];
            basesSetBase(&sim->sim.bs, &tmp, (BYTE)(i + 1));
        }
        for (i = 0; i < sim->sim.ss->numStarts; i++) {
            start tmp = sim->sim.ss->item[i];
            startsSetStart(&sim->sim.ss, &tmp, (BYTE)(i + 1));
        }
    }
    /* The generated map is this sim's too: cap what it put in the lists
       against the rules, as a loaded one is capped. */
    mapClampToRules(&sim->sim);
    basesClearMines(&sim->sim);

    len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = (BYTE *)malloc(len);
    if (sim->cachedMapData == NULL) {
        sim->cachedMapDataLen = 0;
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    mapGenBuildDisplayName(cfg, sim->mapName, MAP_STR_SIZE);

    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (!sim->lobbyPlayers[i].isBot) {
                sim->lobbyPlayers[i].ready = FALSE;
            }
        }
    }
    return TRUE;
}

/* Stash the currently-committed map as the "previous" snapshot before a
 * preview touches the sim. Keep the ORIGINAL committed map across a chain of
 * previews so one Cancel rolls all the way back to where the user started —
 * that is what the outer test is for, and a second preview must not displace
 * the first one's snapshot.
 *
 * Everything a Cancel needs goes in together: the bytes, the display name,
 * the file the map was read from so a script can be found beside it again,
 * and the template seats each team holds, because the cancel re-seats the
 * template from scratch and the host's trim would otherwise go with it.
 * previousSeatsValid comes from serverSimScenarioSeatCounts, which answers
 * false when no template is attached — that is what keeps "no template" apart
 * from a team the host emptied on purpose.
 *
 * Every field is written only once the bytes are held, so a failed malloc
 * leaves no preview to cancel rather than half a snapshot. Every preview
 * entry point calls this and none keeps a copy of it: the path was missing
 * from two of the copies for as long as there were three. */
static void stashCommittedMap(ServerSim *sim) {
    if (sim->previousMapData != NULL || sim->cachedMapData == NULL) return;
    sim->previousMapData = (BYTE *)malloc(sim->cachedMapDataLen);
    if (sim->previousMapData == NULL) return;
    memcpy(sim->previousMapData, sim->cachedMapData, sim->cachedMapDataLen);
    sim->previousMapDataLen = sim->cachedMapDataLen;
    memcpy(sim->previousMapName, sim->mapName, sizeof(sim->previousMapName));
    memcpy(sim->previousMapPath, sim->mapFilePath,
           sizeof(sim->previousMapPath));
    sim->previousSeatsValid =
        serverSimScenarioSeatCounts(sim, sim->previousSeats);
}

bool serverSimReloadMap(ServerSim *sim, const char *mapFileName) {
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
    int len;
    char msg[256];

    if (sim == NULL || mapFileName == NULL || mapFileName[0] == '\0') {
        mpDiagLog("[srv] reloadMap REJECTED (null arg)");
        return FALSE;
    }
    mpDiagLog("[srv] reloadMap ENTER file='%.64s'", mapFileName);
    if (sim->state != serverStateLobby) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadMap rejected: state=%d (not lobby)",
            (int)sim->state);
        mpDiagLog("[srv] reloadMap REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }

    stashCommittedMap(sim);

    /* Wipe the existing map/pill/base/start contents before mapRead
     * touches them. mapRead's RLE-decoder only writes cells encoded
     * in the new file — any tile NOT included in the new map's runs
     * would otherwise keep the previous map's value. */
    {
        int x, y;
        memset((*sim->sim.mp).mapItem, DEEP_SEA,
               sizeof((*sim->sim.mp).mapItem));
        for (x = 0; x < 256; x++) {
            for (y = 0; y < 256; y++) {
                if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                    y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                    (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
                }
            }
        }
        sim->sim.pb->numPills = 0;
        sim->sim.bs->numBases = 0;
        sim->sim.ss->numStarts = 0;
    }

    if (mapRead((char *)mapFileName,
                &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss) == FALSE) {
        /* Roll the wipe back. cachedMapData still holds the previous
         * map's compressed bytes (the refresh below only runs on the
         * success path), so restoring from it returns the live
         * structures to whatever the lobby was showing before this
         * call — leaving the caller with an unchanged sim is much
         * less surprising than a half-wiped one, especially for the
         * upload path where a malformed map shouldn't blank the
         * host's lobby. */
        if (sim->cachedMapData != NULL && sim->cachedMapDataLen > 0) {
            (void)mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb,
                                        &sim->sim.bs, &sim->sim.ss,
                                        sim->cachedMapData,
                                        sim->cachedMapDataLen);
            /* The rolled-back map is this sim's too: cap it as the
               successful load below is capped. */
            mapClampToRules(&sim->sim);
        }
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSimReloadMap: mapRead failed for '%s'", mapFileName);
        return FALSE;
    }

    /* The map is this sim's now: cap what it brought against the rules. */
    mapClampToRules(&sim->sim);

    /* Hash the canonical BMAPBOLO file so WBN can match it. This is the
     * path the lobby map chooser uses (CMD_LOBBY_SET_MAP). */
    serverSimCacheMapMd5FromFile(sim, mapFileName);

    basesClearMines(&sim->sim);

    /* Update map name from basename, strip .map suffix. */
    {
        const char *base = mapFileName;
        const char *p;
        for (p = mapFileName; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        strncpy(sim->mapName, base, MAP_STR_SIZE - 1);
        sim->mapName[MAP_STR_SIZE - 1] = '\0';
        {
            size_t nameLen = strlen(sim->mapName);
            if (nameLen >= 4 &&
                strcmp(sim->mapName + nameLen - 4, ".map") == 0) {
                sim->mapName[nameLen - 4] = '\0';
            }
        }
    }

    /* Refresh cached compressed map. */
    len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData == NULL) {
        sim->cachedMapDataLen = 0;
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    /* Random-map provenance no longer applies. */
    sim->randomMapEnabled = false;

    snprintf(msg, sizeof(msg), "Map changed to %s", sim->mapName);
    serverSimConsoleMessage(msg);

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimReloadMap: now '%s' (%d compressed bytes)",
        sim->mapName, sim->cachedMapDataLen);

    /* Kept because a scenario is discovered beside its .map and the display
       name cannot find the file again. */
    SDL_strlcpy(sim->mapFilePath, mapFileName, sizeof(sim->mapFilePath));

    serverSimApplyMapChange(sim);
    return TRUE;
}

bool serverSimReloadCompressedInMemory(ServerSim *sim,
                                       const uint8_t *bytes, int len,
                                       const char *mapName) {
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
    int compressedLen;
    char msg[256];

    if (sim == NULL || bytes == NULL || len <= 0 ||
        mapName == NULL || mapName[0] == '\0') {
        mpDiagLog("[srv] reloadCompressedInMemory REJECTED (null/empty arg)");
        return FALSE;
    }
    mpDiagLog("[srv] reloadCompressedInMemory ENTER name='%.32s' len=%d",
              mapName, len);
    if (sim->state != serverStateLobby) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadCompressedInMemory rejected: state=%d (not lobby)",
            (int)sim->state);
        mpDiagLog("[srv] reloadCompressedInMemory REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }

    stashCommittedMap(sim);

    /* Wipe the existing map/pill/base/start contents before the
     * decoder touches them. mapLoadCompressedMap's RLE-decoder only
     * writes cells encoded in the new blob — any tile NOT included
     * in the new map's runs would otherwise keep the previous map's
     * value. */
    {
        int x, y;
        memset((*sim->sim.mp).mapItem, DEEP_SEA,
               sizeof((*sim->sim.mp).mapItem));
        for (x = 0; x < 256; x++) {
            for (y = 0; y < 256; y++) {
                if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                    y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                    (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
                }
            }
        }
        sim->sim.pb->numPills = 0;
        sim->sim.bs->numBases = 0;
        sim->sim.ss->numStarts = 0;
    }

    /* The wire / upload / WBN paths all hand us a full .map file
     * (starting with the BMAPBOLO magic + version + counts header).
     * mapLoadCompressedMap expects a different on-the-wire layout
     * (raw bases/pills/starts struct dump + LZW map), so feeding it
     * the .map file bytes misaligns every field. Detect the magic
     * and route through mapRead via a temp file when it matches.
     * Fall back to the legacy mapLoadCompressedMap path for any
     * future caller passing compressed-map-format bytes directly. */
    bool loadedOk = FALSE;
    if (len >= (int)(sizeof(MAP_HEADER) - 1) &&
        memcmp(bytes, MAP_HEADER, sizeof(MAP_HEADER) - 1) == 0) {
        char tmpPath[FILENAME_MAX];
        SDL_snprintf(tmpPath, sizeof(tmpPath),
                     "%s/.tmp_inmem_reload.map",
                     serverSimGetMapDirRoot(sim));
        FILE *tf = fopen(tmpPath, "wb");
        if (tf == NULL) {
            WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                "serverSimReloadCompressedInMemory: temp open failed '%s'",
                tmpPath);
            return FALSE;
        }
        size_t wrote = fwrite(bytes, 1, (size_t)len, tf);
        fclose(tf);
        if (wrote != (size_t)len) {
            remove(tmpPath);
            WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                "serverSimReloadCompressedInMemory: temp write short (%zu/%d)",
                wrote, len);
            return FALSE;
        }
        loadedOk = (mapRead(tmpPath, &sim->sim.mp, &sim->sim.pb,
                            &sim->sim.bs, &sim->sim.ss) == TRUE);
        remove(tmpPath);
        /* These bytes are the canonical BMAPBOLO .map file — hash them
         * so WBN can match the map against its library. */
        if (loadedOk) {
            md5Compute(bytes, (size_t)len, sim->mapMd5);
            sim->mapMd5Valid = TRUE;
            md5ToHex(sim->mapMd5, sim->mapMd5Hex);
        }
    } else {
        loadedOk = (mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb,
                                         &sim->sim.bs, &sim->sim.ss,
                                         (BYTE *)bytes, len) == TRUE);
        sim->mapMd5Valid = FALSE;
        sim->mapMd5Hex[0] = '\0';
    }
    if (!loadedOk) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSimReloadCompressedInMemory: load failed (%d bytes)",
            len);
        return FALSE;
    }
    /* The map is this sim's now: cap what it brought against the rules.
       Both arms above load into the same structures, so one call covers
       the .map file route and the compressed one. */
    mapClampToRules(&sim->sim);

    basesClearMines(&sim->sim);

    /* Use caller-supplied display name verbatim — no path or suffix
     * to strip in the in-memory case. */
    strncpy(sim->mapName, mapName, MAP_STR_SIZE - 1);
    sim->mapName[MAP_STR_SIZE - 1] = '\0';

    /* Refresh cached compressed map. */
    compressedLen = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = malloc(compressedLen);
    if (sim->cachedMapData == NULL) {
        sim->cachedMapDataLen = 0;
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, compressedLen);
    sim->cachedMapDataLen = compressedLen;

    /* Random-map provenance no longer applies. */
    sim->randomMapEnabled = false;

    snprintf(msg, sizeof(msg), "Map changed to %s", sim->mapName);
    serverSimConsoleMessage(msg);

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimReloadCompressedInMemory: now '%s' (%d compressed bytes)",
        sim->mapName, sim->cachedMapDataLen);

    /* Bytes, not a file — nothing to look beside. */
    sim->mapFilePath[0] = '\0';

    serverSimApplyMapChange(sim);
    return TRUE;
}

bool serverSimReloadClientMap(ServerSim *sim, ClientSim *cs) {
    BYTE *buf;
    int len;
    bool ok;
    if (sim == NULL || cs == NULL) return FALSE;
    buf = (BYTE *)malloc(MAP_COMPRESSED_MAX_SIZE);
    if (buf == NULL) return FALSE;
    len = serverSimGetCompressedMap(sim, buf, MAP_COMPRESSED_MAX_SIZE);
    if (len <= 0) {
        free(buf);
        return FALSE;
    }
    {
        GameSim *gs = clientSimGetGameSim(cs);
        ok = mapLoadCompressedMap(&gs->mp, &gs->pb, &gs->bs, &gs->ss, buf, len);
        /* The map is that sim's now: cap what it brought against its rules. */
        if (ok) {
            mapClampToRules(gs);
        }
    }
    free(buf);
    return ok;
}

bool serverSimReloadRandomMap(ServerSim *sim, const MapGenConfig *cfg) {
    if (!sim || !cfg) {
        mpDiagLog("[srv] reloadRandomMap REJECTED (null arg)");
        return FALSE;
    }
    mpDiagLog("[srv] reloadRandomMap ENTER");
    if (sim->state != serverStateLobby) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadRandomMap rejected: state=%d",
            (int)sim->state);
        mpDiagLog("[srv] reloadRandomMap REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }

    stashCommittedMap(sim);

    if (!serverSimApplyRandomMapConfig(sim, cfg)) return FALSE;

    {
        char seedStr[64];
        char msg[128];
        mapGenConfigToSeed(cfg, seedStr, sizeof(seedStr));
        snprintf(msg, sizeof(msg),
                 "Random preview generated, seed: %s", seedStr);
        serverSimConsoleMessage(msg);
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadRandomMap: now '%s' (%d compressed bytes)",
            sim->mapName, sim->cachedMapDataLen);
    }
    serverSimApplyMapChange(sim);
    return TRUE;
}

bool serverSimHasPreviewMap(const ServerSim *sim) {
    return sim != NULL && sim->previousMapData != NULL;
}

const char *serverSimGetPreviousMapName(const ServerSim *sim) {
    if (!sim || !sim->previousMapData) return "";
    return sim->previousMapName;
}

bool serverSimRevertPreview(ServerSim *sim) {
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
    int len;
    if (!sim || !sim->previousMapData) {
        mpDiagLog("[srv] revertPreview REJECTED (no preview to revert)");
        return FALSE;
    }
    if (sim->state != serverStateLobby) {
        mpDiagLog("[srv] revertPreview REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }
    mpDiagLog("[srv] revertPreview ENTER prevMap='%.32s'", sim->previousMapName);

    if (mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb,
                              &sim->sim.bs, &sim->sim.ss,
                              sim->previousMapData,
                              sim->previousMapDataLen) == FALSE) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSimRevertPreview: mapLoadCompressedMap failed");
        return FALSE;
    }
    /* The map is this sim's now: cap what it brought against the rules. */
    mapClampToRules(&sim->sim);
    basesClearMines(&sim->sim);

    /* Reverted to the previous map from its compressed bytes — we no
     * longer have its .map file to hash, so clear the cached md5. */
    sim->mapMd5Valid = FALSE;
    sim->mapMd5Hex[0] = '\0';

    memcpy(sim->mapName, sim->previousMapName, sizeof(sim->mapName));
    len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = (BYTE *)malloc(len);
    if (sim->cachedMapData) {
        memcpy(sim->cachedMapData, tempBuf, len);
        sim->cachedMapDataLen = len;
    } else {
        sim->cachedMapDataLen = 0;
    }

    /* The map that was displaced is back, so the file it was read from is
       the live one again and a scenario can be found beside it. Empty when
       that map came from bytes, which is the same answer as before. */
    SDL_strlcpy(sim->mapFilePath, sim->previousMapPath,
                sizeof(sim->mapFilePath));

    free(sim->previousMapData);
    sim->previousMapData = NULL;
    sim->previousMapDataLen = 0;
    sim->previousMapName[0] = '\0';
    sim->previousMapPath[0] = '\0';

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimRevertPreview: rolled back to '%s'", sim->mapName);
    serverSimApplyMapChange(sim);

    /* The map change above seated the template from scratch, which is what a
       map the host commits wants and not what one they backed out of wants:
       the seats are back at the template's counts and the host's trim is
       gone. Put their counts back. */
    if (sim->previousSeatsValid) {
        serverSimScenarioTrimSeatsTo(sim, sim->previousSeats);
    }
    sim->previousSeatsValid = false;
    memset(sim->previousSeats, 0, sizeof(sim->previousSeats));
    return TRUE;
}

void serverSimCommitPreview(ServerSim *sim) {
    if (!sim || !sim->previousMapData) return;
    free(sim->previousMapData);
    sim->previousMapData = NULL;
    sim->previousMapDataLen = 0;
    sim->previousMapName[0] = '\0';
    sim->previousMapPath[0] = '\0';
    /* The host keeps the previewed map, so the lobby the old one had is gone
       for good and there is nothing left to put back. */
    sim->previousSeatsValid = false;
    memset(sim->previousSeats, 0, sizeof(sim->previousSeats));
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimCommitPreview: committed '%s'", sim->mapName);
}

/* ────────────────────────────────────────────────────────────────
 * Map directory enumeration / search
 * ──────────────────────────────────────────────────────────────── */

static bool relPathIsSafe(const char *p) {
    if (!p) return true;
    if (p[0] == '/' || p[0] == '\\') return false;
    if (p[0] != '\0' && (p[1] == ':' || (p[2] == ':' && p[3] != '\0')))
        return false; /* "C:..." Windows drive */
    for (const char *s = p; *s;) {
        if (s[0] == '.' && s[1] == '.' &&
            (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
            return false;
        }
        while (*s && *s != '/' && *s != '\\') s++;
        while (*s == '/' || *s == '\\') s++;
    }
    return true;
}

/* Resolve a client-facing map relPath to an absolute filesystem path. The
 * virtual "Uploads" folder (and "Uploads/<name>") redirects to the configured
 * persist directory when the sim has one set; every other path — and the unset
 * case — resolves under the map-dir root as before. relPath must already have
 * passed relPathIsSafe. out holds at least FILENAME_MAX bytes.
 *
 * Not static: the lobby's set-map command and the upload preview's use-local
 * path name a map by the same relPath a listing gave, and each used to build
 * "<map root>/<relPath>" for itself. With a persist directory configured that
 * opens a different file from the one the client picked, so both come through
 * here instead. Declared in server_sim_shared.h. */
void serverSimResolveMapPath(const ServerSim *sim, const char *relPath,
                             char *out, size_t outSize) {
    const char *persist =
        (sim && sim->uploadPersistDir[0] != '\0') ? sim->uploadPersistDir : NULL;
    if (persist != NULL && relPath != NULL) {
        if (SDL_strcmp(relPath, "Uploads") == 0) {
            SDL_strlcpy(out, persist, outSize);
            return;
        }
        if (SDL_strncmp(relPath, "Uploads/", 8) == 0) {
            SDL_snprintf(out, outSize, "%s/%s", persist, relPath + 8);
            return;
        }
    }
    const char *root = serverSimGetMapDirRoot(sim);
    if (relPath == NULL || relPath[0] == '\0') {
        SDL_strlcpy(out, root, outSize);
    } else {
        SDL_snprintf(out, outSize, "%s/%s", root, relPath);
    }
}

int serverSimEnumerateMapDir(ServerSim *sim, const char *relPath,
                              ServerMapEntry *entries, int maxEntries) {
    if (!entries || maxEntries <= 0) return -1;
    if (!relPathIsSafe(relPath)) return -1;

    char fullPath[FILENAME_MAX];
    serverSimResolveMapPath(sim, relPath, fullPath, sizeof(fullPath));

    int count = 0;
    int globCount = 0;
    char **list = SDL_GlobDirectory(fullPath, NULL, 0, &globCount);
    if (!list) return 0;

    for (int i = 0; i < globCount && count < maxEntries; i++) {
        const char *name = list[i];
        if (!name || name[0] == '.') continue;

        char child[FILENAME_MAX];
        SDL_snprintf(child, sizeof(child), "%s/%s", fullPath, name);

        SDL_PathInfo info;
        if (!SDL_GetPathInfo(child, &info)) continue;
        bool isDir = (info.type == SDL_PATHTYPE_DIRECTORY);

        if (!isDir) {
            size_t nlen = SDL_strlen(name);
            if (nlen <= 4 ||
                SDL_strcasecmp(name + nlen - 4, ".map") != 0) {
                continue;
            }
        }

        ServerMapEntry *e = &entries[count++];
        SDL_strlcpy(e->name, name, sizeof(e->name));
        e->isFolder = isDir;
        e->modTime  = (int64_t)info.modify_time;
        e->size     = isDir ? 0 : (int64_t)info.size;
        /* A folder is never scripted; the question is about a map file, and
           `child` is the full path the answer needs. */
        e->scripted = isDir ? false
                            : serverSimScenarioMapIsScripted(sim, child);
    }
    SDL_free(list);

    /* Folders first; alphabetical within each group. */
    for (int i = 1; i < count; i++) {
        ServerMapEntry cur = entries[i];
        int j = i - 1;
        while (j >= 0) {
            const ServerMapEntry *a = &entries[j];
            bool aFirst;
            if (a->isFolder != cur.isFolder) aFirst = a->isFolder;
            else aFirst = SDL_strcasecmp(a->name, cur.name) <= 0;
            if (aFirst) break;
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = cur;
    }

    return count;
}

/* The public entry and the scenario library's hold the same three lengths.
   They are stated twice because a gui translation unit reads public/ and
   cannot reach scenario_api/; this file sees both, and is where a change to
   one without the other fails to build. */
BOLO_STATIC_ASSERT(SERVER_SCENARIO_FILE_LEN == SCN_DIR_FILE_LEN,
                   server_scenario_file_matches_the_directory_entry);
BOLO_STATIC_ASSERT(SERVER_SCENARIO_NAME_LEN == SCN_DIR_NAME_LEN,
                   server_scenario_name_matches_the_directory_entry);
BOLO_STATIC_ASSERT(SERVER_SCENARIO_DESC_LEN == SCN_DIR_DESC_LEN,
                   server_scenario_description_matches_the_directory_entry);

int serverSimEnumerateScenarioDir(ServerSim *sim,
                                  ServerScenarioEntry *entries,
                                  int maxEntries) {
    ScnDirEntry *dirRows;
    int          got;
    int          i;

    if (entries == NULL || maxEntries <= 0) return 0;

    /* Read into heap rather than a stack array: an entry carries a
       description, so a full listing runs to tens of kilobytes and this is
       called from a UI thread as readily as a server one. */
    dirRows = (ScnDirEntry *)calloc((size_t)maxEntries, sizeof(*dirRows));
    if (dirRows == NULL) return 0;

    got = serverSimScenarioListDir(sim, dirRows, maxEntries);
    for (i = 0; i < got; i++) {
        ServerScenarioEntry *e = &entries[i];
        SDL_strlcpy(e->file, dirRows[i].file, sizeof(e->file));
        SDL_strlcpy(e->name, dirRows[i].name, sizeof(e->name));
        SDL_strlcpy(e->description, dirRows[i].description,
                    sizeof(e->description));
        e->maxPlayers = dirRows[i].maxPlayers;
        e->bots       = dirRows[i].bots;
        e->bound      = dirRows[i].bound;
    }
    free(dirRows);
    return got;
}

static void searchDirRecursive(const char *fullRoot,
                                const char *subRel,
                                const char *queryLower,
                                size_t queryLen,
                                ServerMapEntry *entries,
                                int maxEntries,
                                int *count,
                                int depth) {
    const int kMaxDepth = 8;
    if (*count >= maxEntries) return;
    if (depth > kMaxDepth) return;

    char dirPath[FILENAME_MAX];
    if (subRel[0] == '\0') {
        SDL_strlcpy(dirPath, fullRoot, sizeof(dirPath));
    } else {
        SDL_snprintf(dirPath, sizeof(dirPath), "%s/%s",
                     fullRoot, subRel);
    }

    int globCount = 0;
    char **list = SDL_GlobDirectory(dirPath, NULL, 0, &globCount);
    if (!list) return;

    for (int i = 0; i < globCount && *count < maxEntries; i++) {
        const char *name = list[i];
        if (!name || name[0] == '.') continue;

        char childPath[FILENAME_MAX];
        SDL_snprintf(childPath, sizeof(childPath), "%s/%s",
                     dirPath, name);

        SDL_PathInfo info;
        if (!SDL_GetPathInfo(childPath, &info)) continue;
        bool isDir = (info.type == SDL_PATHTYPE_DIRECTORY);

        char rel[256];
        if (subRel[0] == '\0') {
            SDL_strlcpy(rel, name, sizeof(rel));
        } else {
            SDL_snprintf(rel, sizeof(rel), "%s/%s", subRel, name);
        }

        if (isDir) {
            searchDirRecursive(fullRoot, rel, queryLower, queryLen,
                               entries, maxEntries, count, depth + 1);
            continue;
        }

        size_t nlen = SDL_strlen(name);
        if (nlen <= 4 ||
            SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;

        bool match = false;
        for (size_t k = 0; k + queryLen <= nlen; k++) {
            size_t m;
            for (m = 0; m < queryLen; m++) {
                char hc = name[k + m];
                if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
                if (hc != queryLower[m]) break;
            }
            if (m == queryLen) { match = true; break; }
        }
        if (!match) continue;

        ServerMapEntry *e = &entries[(*count)++];
        SDL_strlcpy(e->name, rel, sizeof(e->name));
        e->isFolder = false;
        e->modTime  = (int64_t)info.modify_time;
        e->size     = (int64_t)info.size;
        /* Written rather than left alone: the caller's array is not zeroed,
           and the search's own results do not carry the flag. */
        e->scripted = false;
    }
    SDL_free(list);
}

int serverSimSearchMapDir(ServerSim *sim, const char *relPath,
                           const char *query,
                           ServerMapEntry *entries, int maxEntries) {
    if (!entries || maxEntries <= 0) return -1;
    if (!query || query[0] == '\0') return 0;
    if (!relPathIsSafe(relPath)) return -1;

    char fullRoot[FILENAME_MAX];
    serverSimResolveMapPath(sim, relPath, fullRoot, sizeof(fullRoot));

    char queryLower[128];
    size_t qlen = SDL_strlen(query);
    if (qlen >= sizeof(queryLower)) qlen = sizeof(queryLower) - 1;
    for (size_t i = 0; i < qlen; i++) {
        char c = query[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        queryLower[i] = c;
    }
    queryLower[qlen] = '\0';

    int count = 0;
    searchDirRecursive(fullRoot, "", queryLower, qlen,
                       entries, maxEntries, &count, 0);

    for (int i = 1; i < count; i++) {
        ServerMapEntry cur = entries[i];
        int j = i - 1;
        while (j >= 0 &&
               SDL_strcasecmp(entries[j].name, cur.name) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = cur;
    }
    return count;
}

/* Side-effects every server-side map change owes its audience:
 * refresh the per-client compressed-map blob on the wire helper,
 * publish the map-change control event so in-process subscribers
 * (SP host's ClientSim, bots, replay log writers) react, re-publish
 * settings so the mapName / pillCount / baseCount / startCount
 * fields in CTRL_LOBBY_SETTINGS reflect the new map, and clear
 * humans' ready state — which aborts any in-flight countdown via
 * lobbyAutoUnreadyOnChange. Called from every map-mutator at the
 * end of its success path. */
static void serverSimApplyMapChange(ServerSim *sim) {
    mpDiagLog("[srv] applyMapChange map='%.32s' cachedLen=%d random=%d",
              sim->mapName, sim->cachedMapDataLen,
              (int)sim->randomMapEnabled);

    /* The live map has just been replaced and every audience is about to be
     * handed the new one, so restart every slot's copy of the terrain from it.
     * Without this a slot's copy would still hold the previous map and the
     * checksum in its snapshot header would not match what it was sent. */
    serverSimShadowSeedAll(sim);
    /* A fill still owing squares was aimed at the map that has just gone, so
       it goes with it. This is the reload, the map-list pick, an uploaded map
       and the random regenerate: none of them reaches serverSimResetGameWorld,
       where the round starts drop theirs. */
    serverSimScenarioResetFill(sim);

    /* A reservation from the previous map can index past the new map's
     * start list, or sit on a side the slot's team may not use now the
     * starts have moved; drop those, then re-cluster every now-unassigned
     * slot into the new map's free starts. Slots whose reservation is
     * still valid keep it (reservations are not auto-moved otherwise). */
    {
        BYTE numStarts = startsGetNumStarts(&sim->sim.ss);
        BYTE before[MAX_TANKS];
        BYTE k;
        /* Snapshot reservations before reconcile so we can publish exactly
         * the slots whose reservation actually moved — covers both a
         * reassign and a stale drop to 0xFF that couldn't be re-picked
         * (more players than starts), without touching unchanged slots. */
        for (k = 0; k < MAX_TANKS; k++) {
            before[k] = sim->lobbyPlayers[k].startIdx;
        }
        for (k = 0; k < MAX_TANKS; k++) {
            if (!sim->playerConnected[k]) continue;
            if (sim->lobbyPlayers[k].startIdx == 0xFF) continue;
            if (sim->lobbyPlayers[k].startIdx < 1 ||
                sim->lobbyPlayers[k].startIdx > numStarts ||
                startsIsActive(&sim->sim.ss, sim->lobbyPlayers[k].startIdx) == FALSE) {
                sim->lobbyPlayers[k].startIdx = 0xFF;
            }
        }
        for (k = 0; k < MAX_TANKS; k++) {
            if (!sim->playerConnected[k]) continue;
            serverSimReleaseIneligibleStart(sim, k);
        }
        /* The backfill publishes every slot it hands a start to. A slot
         * dropped above that it could not place is still 0xFF, so it is
         * not among those; publish it here if it held a start before. */
        serverSimBackfillLobbyStarts(sim);
        for (k = 0; k < MAX_TANKS; k++) {
            if (!sim->playerConnected[k]) continue;
            if (sim->lobbyPlayers[k].startIdx == 0xFF && before[k] != 0xFF) {
                serverSimPublishLobbySlot(sim, k);
            }
        }
    }

    /* The map is loaded and the starts are reconciled, so this is the first
       point a seat can be given one on the new map. Whoever owns the
       scenario is told about the file first and the seating reads whatever
       template they leave; the settings publish below then carries a lobby
       that is already the new map's. */
    serverSimScenarioOnMapChanged(sim, sim->mapFilePath);

    /* Whoever owns the scenario has just attached the new map's or let the
       previous one go, so the identity above is the new map's and this is
       the point the lobby's own settings follow it. A server booting straight
       onto a scripted map makes the same call for itself, having had no
       commit to reach it through. */
    serverSimScenarioApplyLobbyRules(sim);

    transportUdpServerOnLobbyMapChange(sim);
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_MAP_CHANGE;
        serverSimPublishControl(sim, &evt);
    }
    serverSimPublishLobbySettings(sim);
    lobbyAutoUnreadyOnChange(sim);
    serverSimWbnLobbyUpdate(sim, FALSE);
}

bool serverSimReadMapFile(ServerSim *sim, const char *relPath,
                           uint8_t **outBytes, size_t *outLen) {
    if (!outBytes || !outLen) return false;
    *outBytes = NULL;
    *outLen   = 0;
    if (!relPath || !*relPath) return false;
    if (!relPathIsSafe(relPath)) return false;

    char fullPath[FILENAME_MAX];
    serverSimResolveMapPath(sim, relPath, fullPath, sizeof(fullPath));
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(fullPath, &info)) return false;
    if (info.type != SDL_PATHTYPE_FILE) return false;

    FILE *fp = fopen(fullPath, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return false; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return false; }

    /* A file inside the cap is read whole; one over it is read as a prefix
       that large. A whole plain map fits inside the cap, so the front of an
       over-cap file still holds its map however large whatever follows it
       is — which is how a map with a scenario chunk appended gets read at
       all, where sizing the read off the file would refuse it outright. */
    bool   overCap = (size_t)sz > LOBBY_MAP_UPLOAD_MAX_BYTES;
    size_t want    = overCap ? (size_t)LOBBY_MAP_UPLOAD_MAX_BYTES : (size_t)sz;

    uint8_t *buf = (uint8_t *)malloc(want);
    if (!buf) { fclose(fp); return false; }
    size_t got = fread(buf, 1, want, fp);
    fclose(fp);
    if (got != want) { free(buf); return false; }

    /* Where the map stops is where this read stops: a chunk appended after a
       map is the server's business rather than a client's, and the preview
       only ever wants the map.

       What is not a map is handed back as it was read. This function reads
       bytes for a caller and does not judge them, and a file that does not
       parse is a file with no chunk in it to keep back. The one exception is
       the over-cap file, which only ever got this far because its front
       might have been a map: with no map in it there is nothing to trim it
       to, so it is refused, as an over-cap file always has been. */
    size_t bodyLen = 0;
    if (boloMapBodyLength(buf, got, &bodyLen)) {
        if (bodyLen < got) {
            uint8_t *trimmed = (uint8_t *)realloc(buf, bodyLen);
            if (trimmed != NULL) buf = trimmed;
        }
        got = bodyLen;
    } else if (overCap) {
        free(buf);
        return false;
    }

    *outBytes = buf;
    *outLen   = got;
    return true;
}
