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

/* The pick, kept as the first entry of the script list. A caller with only
   a file name to give is every caller this has: the command arm that knows
   more than the name calls serverSimSetScriptList instead, and what this one
   writes leaves the manifest's name and the two flags empty, which is
   honest — it has not read the directory and does not know them. */
void serverSimSetSelectedScenario(ServerSim *sim, const char *file) {
    ScnDirEntry entry;

    if (sim == NULL) return;
    if (file == NULL || file[0] == '\0') {
        serverSimSetScriptList(sim, NULL, 0);
        return;
    }
    memset(&entry, 0, sizeof(entry));
    SDL_strlcpy(entry.file, file, sizeof(entry.file));
    serverSimSetScriptList(sim, &entry, 1);
}

/* Entry 0 and not the whole list, because what this answers is the one
   question it has always answered: which script the round is decided by.
   The entries behind it are mods, and whoever loads them reads the list.

   Entry 0 may now be the committed map's own script, where the host has put
   that row at the front of the list rather than leaving it off. That is the
   same answer said a different way — the map's script is what decides that
   round — and it is still a file name a caller can print or compare. */
const char *serverSimGetSelectedScenario(const ServerSim *sim) {
    if (sim == NULL || sim->scenarioScriptCount <= 0) return "";
    return sim->scenarioScripts[0].file;
}

void serverSimSetScriptList(ServerSim *sim, const ScnDirEntry *entries,
                            int count) {
    int i;

    if (sim == NULL) return;
    if (entries == NULL || count <= 0) {
        count = 0;
    } else if (count > LOBBY_SCRIPT_LIST_MAX) {
        count = LOBBY_SCRIPT_LIST_MAX;
    }
    /* The whole array and not the rows in use: a shorter list must not leave
       the tail of a longer one behind it, since anything reading past the
       count would then find a script nobody picked. */
    memset(sim->scenarioScripts, 0, sizeof(sim->scenarioScripts));
    for (i = 0; i < count; i++) {
        sim->scenarioScripts[i] = entries[i];
    }
    sim->scenarioScriptCount = count;
}

int serverSimGetScriptCount(const ServerSim *sim) {
    if (sim == NULL) return 0;
    return sim->scenarioScriptCount;
}

const ScnDirEntry *serverSimGetScript(const ServerSim *sim, int i) {
    if (sim == NULL || i < 0 || i >= sim->scenarioScriptCount) return NULL;
    return &sim->scenarioScripts[i];
}

/* Where the picks hold the map's own row, or -1 for a list that does not.
   bound is what says so and nothing else does: every pick is a file out of
   the scenarios directory and the command bus refuses a bound one, so the one
   bound row a list can carry is the row the map brought. A host who leaves it
   on the list is saying where on the list the map's own script goes; a host
   who takes it off is saying the map's script plays ahead of the picks, which
   is where it has always played. */
static int scriptListMapOwnAt(const ServerSim *sim) {
    int i;

    for (i = 0; i < sim->scenarioScriptCount; i++) {
        if (sim->scenarioScripts[i].bound) return i;
    }
    return -1;
}

void serverSimSetMapScript(ServerSim *sim, const ScnDirEntry *entry) {
    int at;

    if (sim == NULL) return;
    at = scriptListMapOwnAt(sim);
    /* The details belonged to the row this replaces. The caller sets the new
       row's with serverSimSetMapScriptDetails after this, so a row never
       answers with another script's details. */
    sim->scenarioMapScriptDetailsLen = 0;
    sim->scenarioMapScriptSettingsLen = 0;
    if (entry == NULL || entry->file[0] == '\0') {
        memset(&sim->scenarioMapScript, 0, sizeof(sim->scenarioMapScript));
        /* And the place the host kept for it, which now names a script no
           map brings. A row that stayed would draw the last map's scenario
           in the lobby list and would be handed to the compose again at the
           next pick, so the position goes with the script. The rows behind
           it close up, which keeps the order of everything the host chose
           for itself. */
        if (at >= 0) {
            int i;

            for (i = at; i + 1 < sim->scenarioScriptCount; i++) {
                sim->scenarioScripts[i] = sim->scenarioScripts[i + 1];
            }
            sim->scenarioScriptCount--;
            memset(&sim->scenarioScripts[sim->scenarioScriptCount], 0,
                   sizeof(sim->scenarioScripts[0]));
        }
        return;
    }
    sim->scenarioMapScript = *entry;
    /* Whatever the manifest said. A script that came with the map is tied to
       it in the one sense the lobby cares about: the host cannot take it off
       without changing the map, which is what a chooser reads this to know.
       An unbound script that happens to sit beside a map is still that map's
       for as long as the map is committed. */
    sim->scenarioMapScript.bound = true;
    /* And the list's own copy of the row, where the host gave the map's
       script a place. The two are one row said twice and they have to agree:
       a commit of another scripted map would otherwise leave the lobby
       drawing the old map's name at that position, and the next pick would
       hand the compose a row naming a file that is no longer anybody's. The
       place is the host's and stays; only what sits in it is replaced. */
    if (at >= 0) {
        sim->scenarioScripts[at] = sim->scenarioMapScript;
    }
}

const ScnDirEntry *serverSimGetMapScript(const ServerSim *sim) {
    if (sim == NULL || sim->scenarioMapScript.file[0] == '\0') return NULL;
    return &sim->scenarioMapScript;
}

void serverSimSetMapScriptDetails(ServerSim *sim, const uint8_t *details,
                                  size_t len) {
    if (sim == NULL) return;
    sim->scenarioMapScriptDetailsLen = 0;
    if (details == NULL || len == 0 || sim->scenarioMapScript.file[0] == '\0' ||
        len > sizeof(sim->scenarioMapScriptDetails)) {
        return;
    }
    memcpy(sim->scenarioMapScriptDetails, details, len);
    sim->scenarioMapScriptDetailsLen = (uint16_t)len;
}

void serverSimSetMapScriptSettings(ServerSim *sim, const uint8_t *settings,
                                   size_t len) {
    if (sim == NULL) return;
    sim->scenarioMapScriptSettingsLen = 0;
    if (settings == NULL || len == 0 ||
        sim->scenarioMapScript.file[0] == '\0' ||
        len > sizeof(sim->scenarioMapScriptSettings)) {
        return;
    }
    memcpy(sim->scenarioMapScriptSettings, settings, len);
    sim->scenarioMapScriptSettingsLen = (uint16_t)len;
}

/* The two together, which is the list the lobby is told and a chooser draws:
   the map's own row where it belongs, then the picks in order.

   Where it belongs is the host's answer where the host has given one. A list
   that carries the map's row has already said where that row goes, and the
   picks alone are the whole of the list — prepending a second copy would draw
   the same script twice and a chooser would offer to remove one of them. A
   list that does not carry it gets it at the front, which is where the round
   composes it for a host who never said otherwise and is what every list
   built before the row could be moved looks like.

   The front, rather than the back, because that is where the round loads it:
   a chooser draws the rows in the order it is given them and the order is
   load order.

   Held at LOBBY_SCRIPT_LIST_MAX, which is what a client can take: one that is
   sent more rows than that keeps the list it had. The picks are what the cap
   takes off, because the map's row is not the host's to lose. A host who has
   picked the full ten and then commits a map with a script of its own
   therefore sees the last pick drop out of the list, and the round composes
   the same ten. */
int serverSimGetLobbyScriptCount(const ServerSim *sim) {
    int n;

    if (sim == NULL) return 0;
    n = sim->scenarioScriptCount;
    if (sim->scenarioMapScript.file[0] != '\0' &&
        scriptListMapOwnAt(sim) < 0) {
        n++;
    }
    if (n > LOBBY_SCRIPT_LIST_MAX) n = LOBBY_SCRIPT_LIST_MAX;
    return n;
}

const ScnDirEntry *serverSimGetLobbyScript(const ServerSim *sim, int i) {
    if (sim == NULL || i < 0 || i >= serverSimGetLobbyScriptCount(sim)) {
        return NULL;
    }
    if (sim->scenarioMapScript.file[0] != '\0' &&
        scriptListMapOwnAt(sim) < 0) {
        if (i == 0) return &sim->scenarioMapScript;
        i--;
    }
    if (i >= sim->scenarioScriptCount) return NULL;
    return &sim->scenarioScripts[i];
}

void serverSimSetUploadPersistDir(ServerSim *sim, const char *dir) {
    if (sim == NULL) return;
    if (dir != NULL) {
        SDL_strlcpy(sim->uploadPersistDir, dir, sizeof(sim->uploadPersistDir));
    } else {
        sim->uploadPersistDir[0] = '\0';
    }
}

void serverSimSetWorkshopMapDir(ServerSim *sim, const char *dir) {
    if (sim == NULL) return;
    if (dir != NULL) {
        SDL_strlcpy(sim->workshopMapDir, dir, sizeof(sim->workshopMapDir));
    } else {
        sim->workshopMapDir[0] = '\0';
    }
}

const char *serverSimGetWorkshopMapDir(const ServerSim *sim) {
    if (sim == NULL) return "";
    return sim->workshopMapDir;
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
     * decoder touches them. mapRead's run decoder, which the .map
     * branch below reaches, only writes cells encoded in the new
     * file — any tile NOT included in the new map's runs would
     * otherwise keep the previous map's value. */
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
     * (zlib over a bases/pills/starts struct dump + the map), so feeding it
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

/* Resolve a client-facing map relPath to an absolute filesystem path. Two
 * virtual folders live outside the map root: "Uploads" (and "Uploads/<name>")
 * redirects to the configured persist directory, and "Workshop" (and
 * "Workshop/<name>") to the directory the host copies its Workshop items to.
 * Each redirects only when the sim has that directory set; every other path —
 * and the unset case — resolves under the map-dir root as before. "Workshop"
 * also does not redirect when the map root holds a real folder of that name:
 * the root listing shows that folder in place of the Workshop directory, and
 * the lobby's chooser opens it, so the path leads to the folder that was
 * listed. relPath must already have passed relPathIsSafe. out holds at least
 * FILENAME_MAX bytes.
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
    const char *workshop =
        (sim && sim->workshopMapDir[0] != '\0') ? sim->workshopMapDir : NULL;
    if (workshop != NULL && relPath != NULL &&
        (SDL_strcmp(relPath, "Workshop") == 0 ||
         SDL_strncmp(relPath, "Workshop/", 9) == 0)) {
        char         realFolder[FILENAME_MAX];
        SDL_PathInfo info;

        SDL_snprintf(realFolder, sizeof(realFolder), "%s/Workshop", root);
        if (!(SDL_GetPathInfo(realFolder, &info) &&
              info.type == SDL_PATHTYPE_DIRECTORY)) {
            if (relPath[8] == '\0') {
                SDL_strlcpy(out, workshop, outSize);
            } else {
                SDL_snprintf(out, outSize, "%s/%s", workshop, relPath + 9);
            }
            return;
        }
    }
    if (relPath == NULL || relPath[0] == '\0') {
        SDL_strlcpy(out, root, outSize);
    } else {
        SDL_snprintf(out, outSize, "%s/%s", root, relPath);
    }
}

void serverSimGetUploadsDir(const ServerSim *sim, char *out, size_t outLen) {
    if (out == NULL || outLen == 0) {
        return;
    }
    out[0] = '\0';
    if (sim == NULL) {
        return;
    }
    /* The virtual folder's own name, resolved the way any map path under it
       is, so the prefix a caller compares against is the one the resolve
       would have produced. */
    serverSimResolveMapPath(sim, "Uploads", out, outLen);
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

    /* The root also offers the Workshop directory as a folder, the way the
       resolve above reaches it. Left out when the directory is not there yet
       (nothing subscribed), when the listing is full, and when the map root
       already holds a real folder of that name in any case, so the chooser
       never draws two rows called Workshop. A real "Workshop" row opens the
       real folder: the resolve does not redirect while that folder is
       there. */
    if ((relPath == NULL || relPath[0] == '\0') && sim != NULL &&
        sim->workshopMapDir[0] != '\0' && count < maxEntries) {
        SDL_PathInfo info;
        bool present = false;
        for (int i = 0; i < count; i++) {
            if (SDL_strcasecmp(entries[i].name, "Workshop") == 0) {
                present = true;
                break;
            }
        }
        if (!present && SDL_GetPathInfo(sim->workshopMapDir, &info) &&
            info.type == SDL_PATHTYPE_DIRECTORY) {
            ServerMapEntry *e = &entries[count++];
            SDL_strlcpy(e->name, "Workshop", sizeof(e->name));
            e->isFolder = true;
            e->modTime  = (int64_t)info.modify_time;
            e->size     = 0;
            e->scripted = false;
        }
    }

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
BOLO_STATIC_ASSERT(SERVER_SCENARIO_SOURCE_SERVER == SCN_DIR_SOURCE_SERVER,
                   server_scenario_source_server_matches_the_directory_entry);
BOLO_STATIC_ASSERT(SERVER_SCENARIO_SOURCE_UPLOAD == SCN_DIR_SOURCE_UPLOAD,
                   server_scenario_source_upload_matches_the_directory_entry);
BOLO_STATIC_ASSERT(SERVER_SCENARIO_SOURCE_WORKSHOP == SCN_DIR_SOURCE_WORKSHOP,
                   server_scenario_source_workshop_matches_the_directory_entry);

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
        e->keepsWinCondition = dirRows[i].keepsWinCondition;
        e->source     = dirRows[i].source;
        e->workshopId = dirRows[i].workshopId;
        e->workshopAuthor = dirRows[i].workshopAuthor;
    }
    free(dirRows);
    return got;
}

int serverSimScenarioDetails(ServerSim *sim, const char *file, uint8_t *out,
                             size_t cap) {
    if (sim == NULL || file == NULL || file[0] == '\0' || out == NULL) {
        return -1;
    }
    /* The map's own script first. It is not in the scenarios directory, so
       the row the attach published is the only place that knows it, and the
       name matched is the one that row gives it: the same name the lobby's
       script list carries for it. */
    if (sim->scenarioMapScript.file[0] != '\0' &&
        strcmp(sim->scenarioMapScript.file, file) == 0) {
        if (sim->scenarioMapScriptDetailsLen > cap) return -1;
        memcpy(out, sim->scenarioMapScriptDetails,
               sim->scenarioMapScriptDetailsLen);
        return (int)sim->scenarioMapScriptDetailsLen;
    }
    if (sim->scenarioDetailsReader == NULL) return -1;
    return sim->scenarioDetailsReader(sim->scenarioDetailsReaderCtx,
                                      serverSimGetScenarioDir(sim), file, out,
                                      cap);
}

int serverSimScenarioSettingsDecl(ServerSim *sim, const char *file,
                                  uint8_t *out, size_t cap) {
    if (sim == NULL || file == NULL || file[0] == '\0' || out == NULL) {
        return -1;
    }
    /* The map's own script first, for the reason serverSimScenarioDetails
       looks there first. */
    if (sim->scenarioMapScript.file[0] != '\0' &&
        strcmp(sim->scenarioMapScript.file, file) == 0) {
        if (sim->scenarioMapScriptSettingsLen > cap) return -1;
        memcpy(out, sim->scenarioMapScriptSettings,
               sim->scenarioMapScriptSettingsLen);
        return (int)sim->scenarioMapScriptSettingsLen;
    }
    if (sim->scenarioSettingsReader == NULL) return -1;
    return sim->scenarioSettingsReader(sim->scenarioSettingsReaderCtx,
                                       serverSimGetScenarioDir(sim), file,
                                       out, cap);
}

/* Where file's value for id is kept, or -1. */
static int scriptSettingAt(const ServerSim *sim, const char *file,
                           const char *id) {
    int i;

    for (i = 0; i < sim->scriptSettingValueCount; i++) {
        if (strcmp(sim->scriptSettingValues[i].file, file) == 0 &&
            strcmp(sim->scriptSettingValues[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

bool serverSimGetScriptSetting(const ServerSim *sim, const char *file,
                               const char *id, int32_t *out) {
    int at;

    if (sim == NULL || file == NULL || id == NULL) return false;
    at = scriptSettingAt(sim, file, id);
    if (at < 0) return false;
    if (out != NULL) *out = sim->scriptSettingValues[at].value;
    return true;
}

bool serverSimSetScriptSetting(ServerSim *sim, const char *file,
                               const char *id, int32_t value,
                               int32_t *resolved) {
    uint8_t           blob[SCN_SETTINGS_BLOB_MAX];
    ScnSetting        rows[SCN_SETTINGS_MAX];
    const ScnSetting *decl;
    ControlEvent      evt;
    int               len;
    int               n;
    int               at;
    int32_t           v;

    if (sim == NULL || file == NULL || id == NULL || file[0] == '\0' ||
        strlen(file) >= LOBBY_SCENARIO_FILE_LEN || !scnSettingIdOk(id)) {
        return false;
    }
    /* The declaration is read again for every change rather than trusted
       from the client, so a value is only ever held against the file the
       server would run. */
    len = serverSimScenarioSettingsDecl(sim, file, blob, sizeof(blob));
    if (len <= 0) return false;
    n = scnSettingsBlobRead(blob, (size_t)len, rows, SCN_SETTINGS_MAX);
    if (n <= 0) return false;
    decl = scnSettingFind(rows, n, id);
    if (decl == NULL) return false;
    /* On or off has no nearest entry to clamp to: anything else is not a
       value the host's dropdown sends. */
    if (decl->type == SCN_SETTING_TYPE_BOOL && value != 0 && value != 1) {
        return false;
    }
    /* Nor has a list of words: a choice takes the index of one of its
       words and nothing else. */
    if (decl->type == SCN_SETTING_TYPE_CHOICE &&
        scnSettingChoiceText(decl, value) == NULL) {
        return false;
    }

    v  = scnSettingClamp(decl, value);
    at = scriptSettingAt(sim, file, id);
    if (v == decl->def) {
        /* The default is what a missing value means, so it is not kept. */
        if (at >= 0) {
            sim->scriptSettingValues[at] =
                sim->scriptSettingValues[sim->scriptSettingValueCount - 1];
            sim->scriptSettingValueCount--;
        }
    } else {
        if (at < 0) {
            if (sim->scriptSettingValueCount >=
                SERVER_SCRIPT_SETTING_VALUES_MAX) {
                return false;
            }
            at = sim->scriptSettingValueCount++;
            SDL_strlcpy(sim->scriptSettingValues[at].file, file,
                        sizeof(sim->scriptSettingValues[at].file));
            SDL_strlcpy(sim->scriptSettingValues[at].id, id,
                        sizeof(sim->scriptSettingValues[at].id));
        }
        sim->scriptSettingValues[at].value = v;
    }
    if (resolved != NULL) *resolved = v;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_SET;
    SDL_strlcpy(evt.u.lobbyScriptSetting.file, file,
                sizeof(evt.u.lobbyScriptSetting.file));
    SDL_strlcpy(evt.u.lobbyScriptSetting.id, id,
                sizeof(evt.u.lobbyScriptSetting.id));
    evt.u.lobbyScriptSetting.value = v;
    serverSimPublishControl(sim, &evt);
    return true;
}

void serverSimReplayScriptSettings(
    const ServerSim *sim, void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    ControlEvent evt;
    int          i;

    if (sim == NULL || deliver == NULL) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SCRIPT_SETTING;
    evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_CLEAR;
    deliver(ctx, &evt);
    for (i = 0; i < sim->scriptSettingValueCount; i++) {
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_SCRIPT_SETTING;
        evt.u.lobbyScriptSetting.op = LOBBY_SCRIPT_SETTING_SET;
        SDL_strlcpy(evt.u.lobbyScriptSetting.file,
                    sim->scriptSettingValues[i].file,
                    sizeof(evt.u.lobbyScriptSetting.file));
        SDL_strlcpy(evt.u.lobbyScriptSetting.id,
                    sim->scriptSettingValues[i].id,
                    sizeof(evt.u.lobbyScriptSetting.id));
        evt.u.lobbyScriptSetting.value = sim->scriptSettingValues[i].value;
        deliver(ctx, &evt);
    }
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

    /* Teams 1 and 2 still on the default pair the lobby opened with follow
     * the new map's shape: north/south for a tall or square map, east/west
     * for a wide one. A side the host chose is kept. Before the reconcile
     * below, so each reservation is checked against the sides it will
     * start on. */
    serverSimRefreshDefaultTeamSides(sim);

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
    /* And the list the lobby draws, because the map commit has just changed
       it: the row for the map's own script is the committed map's, and the
       decision above is what set or cleared it. The picks are untouched by a
       commit, so until the list carried the map's own row there was nothing
       here for a commit to publish. */
    serverSimPublishScriptList(sim);

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
