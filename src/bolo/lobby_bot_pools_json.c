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
 *Name:          Lobby Bot Naming Pools — JSON loader
 *Filename:      lobby_bot_pools_json.c
 *Purpose:
 *  Parses a bot-names JSON file and installs it as the active set
 *  via lobbyBotPoolsInstall().  Kept separate from
 *  lobby_bot_pools.c so the core (which lives in bolo_static and is
 *  shared with the wasm / iOS builds) carries no cJSON dependency.
 *  Only linked into targets that need runtime loading — the desktop
 *  client, the dedicated server, and the unit tests.
 *
 *  File format:
 *    { "pools": [
 *        { "label": "Classic AI", "names": ["HAL-9000", "GLaDOS"] },
 *        ...
 *    ] }
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cJSON.h"
#include "lobby_bot_pools.h"
#include "../common/wb_log.h"

/* Read an entire file into a malloc'd, NUL-terminated buffer. Returns
 * NULL on any error. Caller frees. */
static char *readWholeFile(const char *path) {
    FILE *f;
    long len;
    size_t got;
    char *buf;

    f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    len = ftell(f);
    if (len < 0 || len > (4L * 1024 * 1024)) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

bool lobbyBotPoolsLoadFromFile(const char *path,
                               LobbyBotPoolLoadStats *stats) {
    char *text;
    cJSON *root, *poolsArr, *poolItem;
    LobbyBotPoolDef *defs = NULL;
    const char ***nameArrays = NULL;   /* per-pool array of name pointers */
    int poolCap, poolN = 0;
    int installed = 0;
    int i;

    if (!path || !path[0]) return false;

    text = readWholeFile(path);
    if (!text) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET,
                    "lobbyBotPoolsLoadFromFile: cannot read '%s'", path);
        return false;
    }

    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET,
                    "lobbyBotPoolsLoadFromFile: invalid JSON in '%s'", path);
        return false;
    }

    poolsArr = cJSON_GetObjectItemCaseSensitive(root, "pools");
    if (!cJSON_IsArray(poolsArr)) {
        WB_LOG_WARN(WB_LOG_CAT_ASSET,
                    "lobbyBotPoolsLoadFromFile: '%s' has no \"pools\" array",
                    path);
        cJSON_Delete(root);
        return false;
    }

    poolCap = cJSON_GetArraySize(poolsArr);
    if (poolCap <= 0) {
        cJSON_Delete(root);
        return false;
    }

    defs = (LobbyBotPoolDef *)calloc((size_t)poolCap, sizeof(*defs));
    nameArrays = (const char ***)calloc((size_t)poolCap, sizeof(*nameArrays));
    /* The free casts below avoid MSVC C4090 for pointers to const strings.
     * Only the allocated pointer arrays are freed; cJSON owns the strings. */
    if (!defs || !nameArrays) {
        free(defs);
        free((void *)nameArrays);
        cJSON_Delete(root);
        return false;
    }

    /* Build LobbyBotPoolDef[] pointing at the cJSON-owned strings.
     * lobbyBotPoolsInstall copies everything, so these stay valid only
     * until cJSON_Delete below — which is fine. */
    cJSON_ArrayForEach(poolItem, poolsArr) {
        cJSON *labelNode, *namesNode, *nameNode;
        const char **names;
        int nameCap, nameN = 0;

        if (!cJSON_IsObject(poolItem)) continue;
        namesNode = cJSON_GetObjectItemCaseSensitive(poolItem, "names");
        if (!cJSON_IsArray(namesNode)) continue;
        nameCap = cJSON_GetArraySize(namesNode);
        if (nameCap <= 0) continue;

        names = (const char **)calloc((size_t)nameCap, sizeof(*names));
        if (!names) continue;

        cJSON_ArrayForEach(nameNode, namesNode) {
            if (cJSON_IsString(nameNode) && nameNode->valuestring) {
                names[nameN++] = nameNode->valuestring;
            }
        }
        if (nameN == 0) { free((void *)names); continue; }

        labelNode = cJSON_GetObjectItemCaseSensitive(poolItem, "label");
        defs[poolN].label =
            (cJSON_IsString(labelNode) ? labelNode->valuestring : NULL);
        defs[poolN].names = names;
        defs[poolN].nameCount = nameN;
        nameArrays[poolN] = names;
        poolN++;
    }

    if (poolN > 0) {
        installed = lobbyBotPoolsInstall(defs, poolN, stats);
    }

    for (i = 0; i < poolN; i++) free((void *)nameArrays[i]);
    free((void *)nameArrays);
    free(defs);
    cJSON_Delete(root);

    if (installed > 0) {
        WB_LOG_DEBUG(WB_LOG_CAT_ASSET,
                    "lobbyBotPoolsLoadFromFile: loaded %d pool(s) from '%s'",
                    installed, path);
        return true;
    }
    WB_LOG_WARN(WB_LOG_CAT_ASSET,
                "lobbyBotPoolsLoadFromFile: no usable pools in '%s'", path);
    return false;
}

bool lobbyBotPoolsLoadDefault(LobbyBotPoolLoadStats *stats) {
    const char *base;
    char path[1024];

    /* Next to the executable first (installed / dev build layout). */
    base = SDL_GetBasePath();
    if (base && base[0]) {
        snprintf(path, sizeof(path), "%sdata/bot_names.json", base);
        if (lobbyBotPoolsLoadFromFile(path, stats)) return true;
    }

    /* Fall back to the CWD-relative path. */
    if (lobbyBotPoolsLoadFromFile("data/bot_names.json", stats)) return true;

    return false;
}
