/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          wbn_serverlist
 * Filename:      wbn_serverlist.c
 * Purpose:
 *   Parses the WinBolo.net public game list (GET
 *   /api/v1/games). wbnServerListParse is pure cJSON;
 *   wbnFetchServerList drives the transport via
 *   wbn_api_get_public in http.c.
 *********************************************************/

#include "wbn_serverlist.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "cJSON.h"
#include "http.h"
#include "view_policy.h"   /* the view-policy defaults for absent fields */
#include "server_voice_mode.h"  /* serverVoiceOn — the default for an absent "voice" */

/* Copy a JSON string item into a fixed buffer, truncating to fit.
 * Non-string / NULL items leave dst as an empty string. */
static void copyStringField(const cJSON *item, char *dst, size_t dstSize) {
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        snprintf(dst, dstSize, "%s", item->valuestring);
    } else {
        dst[0] = '\0';
    }
}

/* Read a JSON boolean field. Accepts true/false and tolerates a
 * numeric 0/1. Missing / other types default to false. */
static bool readBoolField(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? true : false;
    }
    if (cJSON_IsNumber(item)) {
        return item->valueint != 0;
    }
    return false;
}

/* Read a JSON integer field via valueint. Missing / non-number -> 0. */
static int readIntField(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return 0;
}

/* Read a JSON integer field, falling back to `def` when the field is
 * missing or not a number. Used by the view policies, whose "absent"
 * value is not 0 for every category. */
static int readIntFieldDef(const cJSON *obj, const char *name, int def) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return def;
}

/* Read a JSON string field into a fixed buffer. Missing -> "". */
static void readStringField(const cJSON *obj, const char *name,
                            char *dst, size_t dstSize) {
    copyStringField(cJSON_GetObjectItemCaseSensitive(obj, name), dst, dstSize);
}

/* True when s is NULL, empty, or all whitespace. */
static bool isBlank(const char *s) {
    if (s == NULL) {
        return true;
    }
    for (; *s != '\0'; s++) {
        if (!isspace((unsigned char)*s)) {
            return false;
        }
    }
    return true;
}

static void parseServerEntry(const cJSON *src, WbnServerListEntry *dst) {
    readStringField(src, "address",  dst->address, sizeof(dst->address));
    dst->port = readIntField(src, "port");
    readStringField(src, "server_key", dst->serverKey, sizeof(dst->serverKey));
    readStringField(src, "map",      dst->map,      sizeof(dst->map));
    readStringField(src, "map_md5",  dst->mapMd5,   sizeof(dst->mapMd5));
    readStringField(src, "version",  dst->version,  sizeof(dst->version));
    readStringField(src, "country",  dst->country,  sizeof(dst->country));

    dst->gameType = readIntField(src, "game_type");
    dst->ai       = readIntField(src, "ai");

    /* A tracker row missing these fields is read the way an INFO packet
     * missing them is — meaning B in view_policy.h, not the
     * VIEW_POLICY_STOCK_* set. */
    dst->pillView = readIntFieldDef(src, "pillview", viewPolicyAlways);
    dst->baseView = readIntFieldDef(src, "baseview", viewPolicyOff);
    dst->allyView = readIntFieldDef(src, "allyview", viewPolicyAlways);

    /* Decay seconds mean something only where the matching policy is
     * viewPolicyDecay; a tracker that has not learned the fields yet
     * leaves the server's own default standing. */
    dst->pillViewDecay = readIntFieldDef(src, "pillviewdecay",
                                         VIEW_DECAY_DEFAULT_SECS);
    dst->baseViewDecay = readIntFieldDef(src, "baseviewdecay",
                                         VIEW_DECAY_DEFAULT_SECS);
    dst->allyViewDecay = readIntFieldDef(src, "allyviewdecay",
                                         VIEW_DECAY_DEFAULT_SECS);

    /* A tracker that has not learned this field yet leaves the mode at on,
     * which is what every server did before the field existed. */
    dst->voiceMode = readIntFieldDef(src, "voice", serverVoiceOn);

    /* Same for these two: a tracker that has not learned them leaves the
     * expanded overview window with nothing blocking sight. readIntFieldDef
     * hands back whatever number it finds, so each is clamped here — a row
     * must not carry a mode this build has no name for, the same rule the
     * INFO packet and the LAN record apply to their own copies. */
    dst->overviewWindow = readIntFieldDef(src, "overviewwindow",
                                          overviewWindowExpanded);
    if (dst->overviewWindow < 0 || dst->overviewWindow >= OVERVIEW_WINDOW_COUNT) {
        dst->overviewWindow = overviewWindowExpanded;
    }
    dst->lineOfSight = readIntFieldDef(src, "lineofsight", lineOfSightOff);
    if (dst->lineOfSight < 0 || dst->lineOfSight >= LINE_OF_SIGHT_COUNT) {
        dst->lineOfSight = lineOfSightOff;
    }

    dst->mines           = readBoolField(src, "mines");
    dst->password        = readBoolField(src, "password");
    dst->randomMap       = readBoolField(src, "random_map");
    dst->ranked          = readBoolField(src, "ranked");
    dst->inLobby         = readBoolField(src, "in_lobby");
    dst->hasLobby        = readBoolField(src, "has_lobby");
    dst->allowNewPlayers = readBoolField(src, "allow_new_players");
    dst->autoLock        = readBoolField(src, "auto_lock");
    dst->allowSpectators = readBoolField(src, "allow_spectators");
    dst->classicMode     = readBoolField(src, "classicmode");
    dst->alliesInTrees   = readBoolField(src, "alliesintrees");
    /* Absent reads as false, which here means smart pings are allowed —
     * the behaviour of every server that predates the key. */
    dst->smartPingsOff   = readBoolField(src, "smartpingsoff");
    dst->spectatorCount  = readIntField(src, "spectator_count");

    dst->timeLimit   = readBoolField(src, "time_limit");
    dst->timeMinutes = readIntField(src, "time_minutes");
    dst->freeBases   = readIntField(src, "free_bases");
    dst->freePills   = readIntField(src, "free_pills");
    dst->numBases    = readIntField(src, "num_bases");
    dst->numPills    = readIntField(src, "num_pills");

    dst->numPlayers = readIntField(src, "num_players");
    dst->numHumans  = readIntField(src, "num_humans");
    dst->numBots    = readIntField(src, "num_bots");

    /* players: array of usernames; skip blanks, clamp to MAX_TANKS. */
    dst->numPlayerNames = 0;
    const cJSON *players = cJSON_GetObjectItemCaseSensitive(src, "players");
    if (cJSON_IsArray(players)) {
        const cJSON *name = NULL;
        cJSON_ArrayForEach(name, players) {
            if (dst->numPlayerNames >= MAX_TANKS) {
                break;
            }
            if (!cJSON_IsString(name) || isBlank(name->valuestring)) {
                continue;
            }
            snprintf(dst->players[dst->numPlayerNames], PLAYER_NAME_LEN,
                     "%s", name->valuestring);
            dst->numPlayerNames++;
        }
    }
}

bool wbnServerListParse(const char *json, WbnServerList *out) {
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (json == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    /* "servers" must be present and an array. */
    cJSON *servers = cJSON_GetObjectItemCaseSensitive(root, "servers");
    if (!cJSON_IsArray(servers)) {
        cJSON_Delete(root);
        return false;
    }

    /* tversion: missing -> 0; any value is accepted (forward-compat). */
    out->tversion = readIntField(root, "tversion");

    /* motd: array of strings, capped at WBN_MOTD_LINES. */
    cJSON *motd = cJSON_GetObjectItemCaseSensitive(root, "motd");
    if (cJSON_IsArray(motd)) {
        const cJSON *line = NULL;
        cJSON_ArrayForEach(line, motd) {
            if (out->numMotd >= WBN_MOTD_LINES) {
                break;
            }
            if (!cJSON_IsString(line) || line->valuestring == NULL) {
                continue;
            }
            snprintf(out->motd[out->numMotd], WBN_MOTD_LINE_LEN, "%s",
                     line->valuestring);
            out->numMotd++;
        }
    }

    int n = cJSON_GetArraySize(servers);
    if (n > 0) {
        out->servers = calloc((size_t)n, sizeof(WbnServerListEntry));
        if (out->servers == NULL) {
            cJSON_Delete(root);
            memset(out, 0, sizeof(*out));
            return false;
        }
        out->count = n;
        int i = 0;
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, servers) {
            if (i >= n) {
                break;
            }
            parseServerEntry(entry, &out->servers[i]);
            i++;
        }
    }

    cJSON_Delete(root);
    return true;
}

bool wbnFetchServerList(WbnServerList *out) {
    char *resp = NULL;
    int code = wbn_api_get_public("games", &resp);
    if (code < 200 || code >= 300) {
        free(resp);
        if (out != NULL) {
            memset(out, 0, sizeof(*out));
        }
        return false;
    }
    bool ok = wbnServerListParse(resp, out);
    free(resp);
    return ok;
}

void wbnServerListFree(WbnServerList *out) {
    if (out == NULL) {
        return;
    }
    free(out->servers);
    out->servers = NULL;
    out->count = 0;
}
