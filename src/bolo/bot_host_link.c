/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Bot Host link
 *Filename:      bot_host_link.c
 *Purpose:
 *  The ANNOUNCE, QUERY and INVITE bodies and the address
 *  lists of bot_host_link.h.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bot_host_link.h"
#include "brain_sync.h"   /* brainSyncNameSafe */

static void putU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t getU16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void putU64(uint8_t *p, uint64_t v) {
    int i;
    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static uint64_t getU64(const uint8_t *p) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}

/* A name on the wire: 1..cap-1 printable ASCII bytes. */
static bool nameOk(const uint8_t *p, size_t n, size_t cap) {
    size_t i;
    if (n == 0 || n >= cap) return false;
    for (i = 0; i < n; i++) {
        if (p[i] < 0x20 || p[i] > 0x7e) return false;
    }
    return true;
}

int botHostAnnounceEncode(const BotHostAnnounce *a, uint8_t *buf, size_t cap) {
    size_t pos = 0, n;
    int b, count;

    if (a == NULL || buf == NULL) return -1;
    n = strlen(a->name);
    if (n >= BOT_HOST_LINK_NAME_LEN) n = BOT_HOST_LINK_NAME_LEN - 1;
    if (cap < 9 + n + 1) return -1;
    buf[pos++] = BOT_HOST_LINK_PROTO;
    buf[pos++] = a->verMajor;
    buf[pos++] = a->verMinor;
    buf[pos++] = a->verRevision;
    putU16(buf + pos, a->listenPort);
    pos += 2;
    buf[pos++] = a->slots;
    buf[pos++] = a->freeSlots > a->slots ? a->slots : a->freeSlots;
    buf[pos++] = (uint8_t)n;
    memcpy(buf + pos, a->name, n);
    pos += n;
    count = a->brainCount;
    if (count > BOT_HOST_LINK_MAX_BRAINS) count = BOT_HOST_LINK_MAX_BRAINS;
    buf[pos++] = (uint8_t)count;
    for (b = 0; b < count; b++) {
        size_t bn = strlen(a->brains[b].name);
        if (bn >= BOT_HOST_LINK_BRAIN_LEN) bn = BOT_HOST_LINK_BRAIN_LEN - 1;
        if (pos + 1 + bn + 8 > cap) return -1;
        buf[pos++] = (uint8_t)bn;
        memcpy(buf + pos, a->brains[b].name, bn);
        pos += bn;
        putU64(buf + pos, a->brains[b].version);
        pos += 8;
    }
    return (int)pos;
}

bool botHostAnnounceDecode(const uint8_t *buf, size_t len, BotHostAnnounce *out) {
    size_t pos = 0, n;
    int b, count;

    if (buf == NULL || out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (len < 10 || buf[0] != BOT_HOST_LINK_PROTO) return false;
    pos = 1;
    out->verMajor = buf[pos++];
    out->verMinor = buf[pos++];
    out->verRevision = buf[pos++];
    out->listenPort = getU16(buf + pos);
    pos += 2;
    out->slots = buf[pos++];
    out->freeSlots = buf[pos++];
    if (out->listenPort == 0 || out->freeSlots > out->slots) goto bad;
    n = buf[pos++];
    if (pos + n + 1 > len || !nameOk(buf + pos, n, BOT_HOST_LINK_NAME_LEN)) {
        goto bad;
    }
    memcpy(out->name, buf + pos, n);
    pos += n;
    count = buf[pos++];
    if (count > BOT_HOST_LINK_MAX_BRAINS) goto bad;
    for (b = 0; b < count; b++) {
        if (pos + 1 > len) goto bad;
        n = buf[pos++];
        if (pos + n + 8 > len || !nameOk(buf + pos, n, BOT_HOST_LINK_BRAIN_LEN)) {
            goto bad;
        }
        memcpy(out->brains[b].name, buf + pos, n);
        pos += n;
        out->brains[b].version = getU64(buf + pos);
        pos += 8;
    }
    if (pos != len) goto bad;
    out->brainCount = (uint8_t)count;
    return true;
bad:
    memset(out, 0, sizeof(*out));
    return false;
}

bool botHostLinkShortValid(const uint8_t *buf, size_t len) {
    return buf != NULL && len == BOT_HOST_LINK_SHORT_LEN &&
           buf[0] == BOT_HOST_LINK_PROTO;
}

static void setErr(char *err, size_t errSz, const char *what, const char *tok) {
    if (err == NULL || errSz == 0) return;
    if (tok != NULL) {
        SDL_snprintf(err, errSz, "%s: \"%s\"", what, tok);
    } else {
        SDL_strlcpy(err, what, errSz);
    }
}

/* One "host[:port]" token. */
static bool parseEntry(const char *tok, BotHostAddrEntry *e, char *err,
                       size_t errSz) {
    const char *colon = strrchr(tok, ':');
    size_t hostLen = colon != NULL ? (size_t)(colon - tok) : strlen(tok);

    memset(e, 0, sizeof(*e));
    if (hostLen == 0) {
        setErr(err, errSz, "an entry has no host", tok);
        return false;
    }
    if (hostLen >= sizeof(e->host)) {
        setErr(err, errSz, "a host name is too long", tok);
        return false;
    }
    if (colon != NULL && strchr(tok, ':') != colon) {
        setErr(err, errSz, "IPv6 addresses are not taken", tok);
        return false;
    }
    memcpy(e->host, tok, hostLen);
    e->host[hostLen] = '\0';
    if (colon != NULL) {
        const char *p = colon + 1;
        unsigned long v = 0;
        if (*p == '\0') {
            setErr(err, errSz, "an entry has no port after ':'", tok);
            return false;
        }
        for (; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                setErr(err, errSz, "a port is not a number", tok);
                return false;
            }
            v = v * 10 + (unsigned long)(*p - '0');
            if (v > 65535) break;
        }
        if (v < 1 || v > 65535) {
            setErr(err, errSz, "a port is not 1 to 65535", tok);
            return false;
        }
        e->port = (uint16_t)v;
    }
    return true;
}

bool botHostAddrListParse(const char *text, BotHostAddrList *out,
                          char *err, size_t errSz) {
    const char *p = text;
    if (err != NULL && errSz > 0) err[0] = '\0';
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (text == NULL) {
        setErr(err, errSz, "the list is empty", NULL);
        return false;
    }
    while (*p != '\0') {
        char tok[BOT_HOST_LINK_HOST_LEN + 16];
        size_t n = 0;
        if (*p == '#') {
            while (*p != '\0' && *p != '\n') p++;
            continue;
        }
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
            continue;
        }
        while (*p != '\0' && *p != ',' && *p != ' ' && *p != '\t' &&
               *p != '\r' && *p != '\n' && *p != '#') {
            if (n + 1 >= sizeof(tok)) {
                setErr(err, errSz, "an entry is too long", NULL);
                return false;
            }
            tok[n++] = *p++;
        }
        tok[n] = '\0';
        if (out->count >= BOT_HOST_LINK_MAX_ADDRS) {
            char what[64];
            SDL_snprintf(what, sizeof(what), "more than %d entries",
                         BOT_HOST_LINK_MAX_ADDRS);
            setErr(err, errSz, what, NULL);
            return false;
        }
        if (!parseEntry(tok, &out->e[out->count], err, errSz)) return false;
        out->count++;
    }
    if (out->count == 0) {
        setErr(err, errSz, "the list is empty", NULL);
        return false;
    }
    return true;
}

bool botHostAddrListLoad(const char *arg, BotHostAddrList *out,
                         char *err, size_t errSz) {
    const char *path = NULL;
    SDL_PathInfo info;

    if (arg == NULL) return botHostAddrListParse(NULL, out, err, errSz);
    if (arg[0] == '@') {
        path = arg + 1;
    } else if (SDL_GetPathInfo(arg, &info) && info.type == SDL_PATHTYPE_FILE) {
        path = arg;
    }
    if (path != NULL) {
        size_t sz = 0;
        char *text = (char *)SDL_LoadFile(path, &sz);
        bool ok;
        if (text == NULL) {
            setErr(err, errSz, "cannot read the list file", path);
            if (out != NULL) memset(out, 0, sizeof(*out));
            return false;
        }
        ok = botHostAddrListParse(text, out, err, errSz);
        SDL_free(text);
        return ok;
    }
    return botHostAddrListParse(arg, out, err, errSz);
}

int botHostAllowFind(const BotHostAllow *list, int count, uint32_t ip,
                     uint16_t port) {
    int i;
    if (list == NULL) return -1;
    for (i = 0; i < count; i++) {
        if (list[i].ip == ip && (list[i].port == 0 || list[i].port == port)) {
            return i;
        }
    }
    return -1;
}

bool botHostLinkParseVersion(const char *hex, uint64_t *out) {
    uint64_t v = 0;
    int i;
    if (hex == NULL) return false;
    for (i = 0; i < 16; i++) {
        char c = hex[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | (uint64_t)d;
    }
    if (hex[16] != '\0') return false;
    if (out != NULL) *out = v;
    return true;
}

/* Folder names under one folder, for botHostLinkListCache. */
typedef struct {
    char names[64][BOT_HOST_LINK_BRAIN_LEN];
    int  count;
} LinkDirNames;

static SDL_EnumerationResult SDLCALL linkCollectName(void *userdata,
                                                     const char *dirname,
                                                     const char *fname) {
    LinkDirNames *dn = (LinkDirNames *)userdata;
    (void)dirname;
    if (dn->count >= (int)(sizeof(dn->names) / sizeof(dn->names[0]))) {
        return SDL_ENUM_SUCCESS;
    }
    if (strlen(fname) >= BOT_HOST_LINK_BRAIN_LEN) return SDL_ENUM_CONTINUE;
    SDL_strlcpy(dn->names[dn->count++], fname, BOT_HOST_LINK_BRAIN_LEN);
    return SDL_ENUM_CONTINUE;
}

int botHostLinkListCache(const char *root, BotHostLinkBrain *out, int max) {
    LinkDirNames *brains, *vers;
    int found = 0, b, v;
    char path[1024];
    SDL_PathInfo info;

    if (root == NULL || out == NULL || max <= 0) return 0;
    brains = (LinkDirNames *)calloc(1, sizeof(*brains));
    vers = (LinkDirNames *)calloc(1, sizeof(*vers));
    if (brains == NULL || vers == NULL) {
        free(brains);
        free(vers);
        return 0;
    }
    (void)SDL_EnumerateDirectory(root, linkCollectName, brains);
    for (b = 0; b < brains->count && found < max; b++) {
        if (!brainSyncNameSafe(brains->names[b])) continue;
        SDL_snprintf(path, sizeof(path), "%s/%s", root, brains->names[b]);
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_DIRECTORY) {
            continue;
        }
        memset(vers, 0, sizeof(*vers));
        (void)SDL_EnumerateDirectory(path, linkCollectName, vers);
        for (v = 0; v < vers->count && found < max; v++) {
            uint64_t ver;
            char init[1200];
            if (!botHostLinkParseVersion(vers->names[v], &ver)) continue;
            /* Only a version that holds its init.lua (at the top or in
             * opt/); a folder still being filled may not. */
            SDL_snprintf(init, sizeof(init), "%s/%s/init.lua", path,
                         vers->names[v]);
            if (!SDL_GetPathInfo(init, &info) || info.type != SDL_PATHTYPE_FILE) {
                SDL_snprintf(init, sizeof(init), "%s/%s/opt/init.lua", path,
                             vers->names[v]);
                if (!SDL_GetPathInfo(init, &info) ||
                    info.type != SDL_PATHTYPE_FILE) {
                    continue;
                }
            }
            SDL_strlcpy(out[found].name, brains->names[b],
                        sizeof(out[found].name));
            out[found].version = ver;
            found++;
        }
    }
    free(brains);
    free(vers);
    return found;
}
