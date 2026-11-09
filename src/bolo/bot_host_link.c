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
#include "bothost_record.h" /* BOTHOST_DECISION_* */
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

/* ---- Where a server places its bots ------------------------------------ */

#define BOT_HOST_PLACE_EPS 1e-9

double botHostPlaceEffSlots(int slots, int smallSlots, double bias) {
    double eff = (double)slots;
    if (slots <= smallSlots) eff -= bias;
    if (eff < 0.05) eff = 0.05;
    return eff;
}

static bool placeTakes(const BotHostPlaceHost *h) {
    if (!h->open || h->slots <= 0) return false;
    return h->unlimited || h->bots < h->slots;
}

/* True when a host other than skip and not unlimited has room for one more
 * bot (bots below its slots). While one has, an unlimited host (the server
 * itself) takes no bot past its slots and gives first when it is over
 * them. */
static bool placeRoomElsewhere(const BotHostPlaceHost *hosts, int n,
                               int skip) {
    int i;
    for (i = 0; i < n; i++) {
        if (i == skip || hosts[i].unlimited) continue;
        if (placeTakes(&hosts[i])) return true;
    }
    return false;
}

/* An unlimited host at or over its slots while another host has room. */
static bool placeUnlimitedFull(const BotHostPlaceHost *h, bool roomElsewhere) {
    return h->unlimited && roomElsewhere && h->bots >= h->slots;
}

void botHostLagSmooth(double *lagMs, bool *known, double rawMs,
                      int smoothTicks) {
    if (lagMs == NULL || known == NULL || !(rawMs >= 0.0)) return;
    if (!*known || smoothTicks <= 1) {
        *lagMs = rawMs;
        *known = true;
        return;
    }
    *lagMs += (rawMs - *lagMs) / (double)smoothTicks;
}

uint16_t botHostLagRaw(bool playerHost, int ownerPingMs, uint16_t hostPingMs) {
    if (playerHost && ownerPingMs > 0) {
        return (uint16_t)(ownerPingMs > 65534 ? 65534 : ownerPingMs);
    }
    return hostPingMs;
}

uint16_t botHostRowPing(bool playerHost, int ownerPingMs, bool lagKnown,
                        double lagMs) {
    double ms;
    if (playerHost && ownerPingMs > 0) {
        return (uint16_t)(ownerPingMs > 65534 ? 65534 : ownerPingMs);
    }
    if (!lagKnown || !(lagMs >= 0.0)) return 0xFFFFu;
    ms = lagMs + 0.5;
    if (ms < 1.0) ms = 1.0;
    if (ms > 65534.0) ms = 65534.0;
    return (uint16_t)ms;
}

/* True when the lag rules are on. */
static bool placeLagOn(const BotHostPlaceLag *lag) {
    return lag != NULL && lag->refMs > 0.0;
}

/* The lag a host counts: a lag not known counts refMs. */
static double placeLagOf(const BotHostPlaceHost *h,
                         const BotHostPlaceLag *lag) {
    return h->lagMs >= 0.0 ? h->lagMs : lag->refMs;
}

/* The factor a host's score is multiplied by: 1 with the lag rules off. */
static double placeLagFactor(const BotHostPlaceHost *h,
                             const BotHostPlaceLag *lag) {
    if (!placeLagOn(lag)) return 1.0;
    return 1.0 + placeLagOf(h, lag) / lag->refMs;
}

/* The host is under the lag ceiling (always, with no ceiling). */
static bool placeUnderCeiling(const BotHostPlaceHost *h,
                              const BotHostPlaceLag *lag) {
    if (!placeLagOn(lag) || !(lag->maxMs > 0.0)) return true;
    return placeLagOf(h, lag) <= lag->maxMs;
}

/* Index of the best host for one more bot, skipping skip (-1: none). */
static int placePickSkip(const BotHostPlaceHost *hosts, int n, int smallSlots,
                         double bias, const BotHostPlaceLag *lag, int skip,
                         bool skipBad, double *scoreOut) {
    int i, best = -1;
    double bestScore = 0.0;
    bool lowRoom = false;
    bool roomElsewhere = placeRoomElsewhere(hosts, n, skip);
    /* The ceiling: while a host under it has room (bots below its slots,
     * the server's own process too), no host over it takes a bot. */
    for (i = 0; i < n; i++) {
        if (i == skip || !placeTakes(&hosts[i])) continue;
        if (hosts[i].bots < hosts[i].slots &&
            placeUnderCeiling(&hosts[i], lag)) {
            lowRoom = true;
        }
    }
    for (i = 0; i < n; i++) {
        double score;
        if (i == skip || !placeTakes(&hosts[i])) continue;
        if (skipBad && hosts[i].bad) continue;
        if (lowRoom && !placeUnderCeiling(&hosts[i], lag)) continue;
        if (placeUnlimitedFull(&hosts[i], roomElsewhere)) continue;
        score = (double)(hosts[i].bots + 1) /
                botHostPlaceEffSlots(hosts[i].slots, smallSlots, bias) *
                placeLagFactor(&hosts[i], lag);
        if (best < 0 || score < bestScore - BOT_HOST_PLACE_EPS ||
            (score <= bestScore + BOT_HOST_PLACE_EPS &&
             hosts[i].slots > hosts[best].slots)) {
            best = i;
            bestScore = score;
        }
    }
    if (scoreOut != NULL) *scoreOut = bestScore;
    return best;
}

int botHostPlacePick(const BotHostPlaceHost *hosts, int n, int smallSlots,
                     double bias) {
    return botHostPlacePickLag(hosts, n, smallSlots, bias, NULL);
}

int botHostPlacePickLag(const BotHostPlaceHost *hosts, int n, int smallSlots,
                        double bias, const BotHostPlaceLag *lag) {
    if (hosts == NULL || n <= 0) return -1;
    return placePickSkip(hosts, n, smallSlots, bias, lag, -1, false,
                         NULL);
}

int botHostPlacePickOther(const BotHostPlaceHost *hosts, int n,
                          int smallSlots, double bias,
                          const BotHostPlaceLag *lag, int skip) {
    if (hosts == NULL || n <= 0) return -1;
    return placePickSkip(hosts, n, smallSlots, bias, lag, skip, false,
                         NULL);
}

bool botHostPlaceRebalance(const BotHostPlaceHost *hosts, int n,
                           int smallSlots, double bias, int *from, int *to) {
    return botHostPlaceRebalanceLag(hosts, n, smallSlots, bias, NULL, from,
                                    to);
}

bool botHostPlaceRebalanceLag(const BotHostPlaceHost *hosts, int n,
                              int smallSlots, double bias,
                              const BotHostPlaceLag *lag, int *from,
                              int *to) {
    int i, src = -1;
    double srcLoad = 0.0;
    if (hosts == NULL || n <= 0 || from == NULL || to == NULL) return false;
    for (i = 0; i < n; i++) {
        double load;
        if (!hosts[i].movable || hosts[i].bots <= 0) continue;
        if (hosts[i].slots <= 0) {
            /* No slots but bots (a limit lowered to 0): it gives first.
             * The server itself only while a host has room. */
            if (hosts[i].unlimited && !placeRoomElsewhere(hosts, n, i)) {
                continue;
            }
            load = 1000.0 + (double)hosts[i].bots;
        } else {
            load = (double)hosts[i].bots /
                   botHostPlaceEffSlots(hosts[i].slots, smallSlots, bias) *
                   placeLagFactor(&hosts[i], lag);
            if (hosts[i].bots > hosts[i].slots &&
                (!hosts[i].unlimited ||
                 placeRoomElsewhere(hosts, n, i))) {
                /* Over its slots: it gives first. The server itself
                 * too, while a host has room. */
                load += 1000.0;
            }
        }
        if (src < 0 || load > srcLoad + BOT_HOST_PLACE_EPS) {
            src = i;
            srcLoad = load;
        }
    }
    if (src < 0) return false;
    {
        double dstScore = 0.0;
        int dst = placePickSkip(hosts, n, smallSlots, bias, lag, src, false,
                                &dstScore);
        if (dst < 0) return false;
        if (hosts[dst].unlimited && hosts[dst].bots >= hosts[dst].slots &&
            hosts[src].bots <= hosts[src].slots) {
            /* The server itself goes past its slots only to take a bot
             * from a host over its own slots, so a bot does not go back
             * and forth between a full host and a full server. */
            return false;
        }
        if (placeLagOn(lag)) {
            /* With lag: only a gain of at least lag->gain, so a lag that
             * goes up and down a little moves no bot. */
            double gain = lag->gain > 0.0 ? lag->gain : 0.0;
            if (dstScore > srcLoad * (1.0 - gain) + BOT_HOST_PLACE_EPS ||
                dstScore >= srcLoad - BOT_HOST_PLACE_EPS) {
                return false;
            }
        } else if (dstScore >= srcLoad - BOT_HOST_PLACE_EPS) {
            /* Only a strict gain: a bot never moves between two places
             * that are as crowded, so bots do not go back and forth. */
            return false;
        }
        *from = src;
        *to = dst;
        return true;
    }
}

/* ---- Keep bots where they run well ------------------------------------- */

/* Host i is over its slots as rule 1 counts it. */
static bool keepOverSlots(const BotHostPlaceHost *hosts, int n, int i) {
    const BotHostPlaceHost *h = &hosts[i];
    if (h->bots <= 0) return false;
    if (h->slots > 0 && h->bots <= h->slots) return false;
    /* The server itself only while another host has room. */
    return !h->unlimited || placeRoomElsewhere(hosts, n, i);
}

/* The server itself (an unlimited place other than src) while it is open,
 * not bad and below its slots, or -1. */
static int keepServerFirst(const BotHostPlaceHost *hosts, int n, int src) {
    int i;
    for (i = 0; i < n; i++) {
        const BotHostPlaceHost *h = &hosts[i];
        if (i == src || !h->unlimited) continue;
        if (h->open && !h->bad && h->slots > 0 && h->bots < h->slots) {
            return i;
        }
    }
    return -1;
}

/* Where a bot of src goes, or -1. anyHost: a bad host may take it when no
 * good one can (rule 1). */
static int keepTarget(const BotHostPlaceHost *hosts, int n, int smallSlots,
                      double bias, const BotHostPlaceLag *lag, int src,
                      bool anyHost) {
    int dst = keepServerFirst(hosts, n, src);
    if (dst >= 0) return dst;
    dst = placePickSkip(hosts, n, smallSlots, bias, lag, src, true, NULL);
    if (dst < 0 && anyHost) {
        dst = placePickSkip(hosts, n, smallSlots, bias, lag, src, false, NULL);
    }
    if (dst < 0) return -1;
    if (hosts[dst].unlimited && hosts[dst].bots >= hosts[dst].slots &&
        hosts[src].slots > 0 && hosts[src].bots <= hosts[src].slots) {
        /* The server itself goes past its slots only to take a bot from a
         * host over its own slots. */
        return -1;
    }
    return dst;
}

bool botHostPlaceRebalanceKeep(const BotHostPlaceHost *hosts, int n,
                               int smallSlots, double bias,
                               const BotHostPlaceLag *lag, bool even,
                               int *from, int *to, BotHostSpreadWhy *why) {
    BotHostSpreadWhy local;
    int i, best = -1, excess = 0, dst;
    if (why == NULL) why = &local;
    *why = BOT_HOST_SPREAD_NONE;
    if (hosts == NULL || n <= 0 || from == NULL || to == NULL) return false;
    if (even) {
        if (!botHostPlaceRebalanceLag(hosts, n, smallSlots, bias, lag, from,
                                      to)) {
            return false;
        }
        *why = keepOverSlots(hosts, n, *from) ? BOT_HOST_SPREAD_OVER_SLOTS
                                              : BOT_HOST_SPREAD_EVEN;
        return true;
    }
    /* 1. Over its slots: the most over first, then the first in the
     * table. */
    for (i = 0; i < n; i++) {
        int e;
        if (!hosts[i].movable || !keepOverSlots(hosts, n, i)) continue;
        e = hosts[i].slots > 0 ? hosts[i].bots - hosts[i].slots
                               : 1000 + hosts[i].bots;
        if (best < 0 || e > excess) {
            best = i;
            excess = e;
        }
    }
    if (best >= 0) {
        dst = keepTarget(hosts, n, smallSlots, bias, lag, best, true);
        if (dst >= 0) {
            *from = best;
            *to = dst;
            *why = BOT_HOST_SPREAD_OVER_SLOTS;
            return true;
        }
    }
    /* 2. Bad: the first in the table that has a good place to go. */
    for (i = 0; i < n; i++) {
        if (!hosts[i].movable || !hosts[i].bad || hosts[i].badHold ||
            hosts[i].bots <= 0) {
            continue;
        }
        dst = keepTarget(hosts, n, smallSlots, bias, lag, i, false);
        if (dst < 0 || hosts[dst].bad) continue;
        *from = i;
        *to = dst;
        *why = BOT_HOST_SPREAD_BAD;
        return true;
    }
    return false;
}

int botHostPlaceBadBatch(BotHostPlaceHost *hosts, int n, int smallSlots,
                         double bias, const BotHostPlaceLag *lag, int src,
                         int want, const int *room, int *to) {
    int given = 0;
    if (hosts == NULL || to == NULL || src < 0 || src >= n) return 0;
    while (given < want && hosts[src].bots > 0) {
        int dst = keepTarget(hosts, n, smallSlots, bias, lag, src, false);
        int take, freeSlots;
        if (dst < 0 || hosts[dst].bad) break;
        take = room != NULL && room[dst] > 1 ? room[dst] : 1;
        freeSlots = hosts[dst].slots - hosts[dst].bots;
        if (take > freeSlots) take = freeSlots;
        if (take > want - given) take = want - given;
        if (take > hosts[src].bots) take = hosts[src].bots;
        /* It takes no more of this batch: its free CPU check waits for a
         * reading with these bots. */
        hosts[dst].open = false;
        while (take-- > 0) {
            to[given++] = dst;
            hosts[src].bots--;
            hosts[dst].bots++;
        }
    }
    return given;
}

/* ---- No quick way back ------------------------------------------------- */

void botHostTrailReset(BotHostTrail *t) {
    int s;
    if (t == NULL) return;
    for (s = 0; s < BOT_HOST_TRAIL_SEATS; s++) {
        t->leftPlace[s] = -1;
        t->leftTick[s] = 0;
    }
}

void botHostTrailClear(BotHostTrail *t, int s) {
    if (t == NULL || s < 0 || s >= BOT_HOST_TRAIL_SEATS) return;
    t->leftPlace[s] = -1;
    t->leftTick[s] = 0;
}

void botHostTrailNote(BotHostTrail *t, int s, int fromPlace, uint32_t now) {
    if (t == NULL || s < 0 || s >= BOT_HOST_TRAIL_SEATS) return;
    t->leftPlace[s] = fromPlace;
    t->leftTick[s] = now;
}

int botHostTrailBackTo(const BotHostTrail *t, int s, uint32_t now,
                       uint32_t stayTicks) {
    if (t == NULL || s < 0 || s >= BOT_HOST_TRAIL_SEATS) return -1;
    if (t->leftPlace[s] < 0 || stayTicks == 0) return -1;
    if ((uint32_t)(now - t->leftTick[s]) >= stayTicks) return -1;
    return t->leftPlace[s];
}

int botHostTrailPickSeat(const BotHostTrail *t, const int *seats,
                         int nSeats, int toPlace, uint32_t now,
                         uint32_t stayTicks, bool forced) {
    int k;
    if (seats == NULL || nSeats <= 0) return -1;
    for (k = 0; k < nSeats; k++) {
        if (botHostTrailBackTo(t, seats[k], now, stayTicks) != toPlace) {
            return seats[k];
        }
    }
    return forced ? seats[0] : -1;
}

/* ---- Bot overruns on a host --------------------------------------------- */

void botHostOverrunCfgDefaults(BotHostOverrunCfg *cfg) {
    if (cfg == NULL) return;
    cfg->pct = BOT_HOST_OVR_PCT_DEFAULT;
    cfg->emergencyPct = BOT_HOST_OVR_EMERGENCY_PCT_DEFAULT;
    cfg->gain = BOT_HOST_OVR_GAIN_DEFAULT;
    cfg->staySecs = BOT_HOST_OVR_STAY_SECS_DEFAULT;
    cfg->stayFactor = BOT_HOST_OVR_STAY_FACTOR_DEFAULT;
    cfg->windowSecs = BOT_HOST_OVR_WINDOW_SECS_DEFAULT;
    cfg->stepSecs = BOT_HOST_OVR_STEP_SECS_DEFAULT;
    cfg->stepMinSecs = BOT_HOST_OVR_STEP_MIN_SECS_DEFAULT;
    cfg->freeBatch = BOT_HOST_FREE_BATCH_DEFAULT;
    cfg->freeSampleSecs = BOT_HOST_FREE_SAMPLE_SECS_DEFAULT;
    cfg->freeHistorySecs = BOT_HOST_FREE_HISTORY_SECS_DEFAULT;
}

/* A count that lets a new state step at once, as soon as its window shows
 * it. */
#define OVR_LONG_AGO (1 << 30)

static void ovrClearProbes(BotHostOverrunState *st) {
    memset(st->probeThinks, 0, sizeof(st->probeThinks));
    memset(st->probeOverruns, 0, sizeof(st->probeOverruns));
    memset(st->probeBots, 0, sizeof(st->probeBots));
    st->probeCount = 0;
    st->probeNext = 0;
    /* The next free CPU reading starts again too: the probes so far ran
     * with the bots before the step. */
    st->accOverruns = 0;
    st->accThinks = 0;
    st->accProbes = 0;
}

void botHostOverrunReset(BotHostOverrunState *st) {
    if (st == NULL) return;
    memset(st, 0, sizeof(*st));
    st->limit = -1;
    st->sinceChange = OVR_LONG_AGO;
    st->sinceRefuse = OVR_LONG_AGO;
}

void botHostOverrunNewGame(BotHostOverrunState *st) {
    if (st == NULL) return;
    st->limit = -1;
    st->cut = false;
    ovrClearProbes(st);
    st->sinceChange = OVR_LONG_AGO;
    st->sinceRefuse = OVR_LONG_AGO;
}

int botHostOverrunLimit(const BotHostOverrunState *st, int slots) {
    if (st == NULL || st->limit < 0 || st->limit >= slots) return slots;
    return st->limit;
}

static int ovrClamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int ovrStep(const BotHostOverrunCfg *cfg) {
    return cfg->stepSecs > 0 ? cfg->stepSecs : 1;
}

bool botHostOverrunScaled(const BotHostOverrunCfg *cfg) {
    return cfg != NULL && cfg->stepMinSecs < ovrStep(cfg);
}

double botHostOverrunWaitSecs(const BotHostOverrunCfg *cfg, double ratePct) {
    double step, lo, wait;
    if (cfg == NULL) return (double)BOT_HOST_OVR_STEP_SECS_DEFAULT;
    step = (double)ovrStep(cfg);
    if (!botHostOverrunScaled(cfg) || !(cfg->pct > 0.0) ||
        !(ratePct > cfg->pct)) {
        return step;
    }
    lo = cfg->stepMinSecs > 1 ? (double)cfg->stepMinSecs : 1.0;
    /* stepSecs / severity, severity = ratePct / pct. */
    wait = step * cfg->pct / ratePct;
    return wait < lo ? lo : wait > step ? step : wait;
}

static int ovrWindow(const BotHostOverrunCfg *cfg) {
    return ovrClamp(cfg->windowSecs, 1, BOT_HOST_OVR_MAX_PROBES);
}

static int ovrSample(const BotHostOverrunCfg *cfg) {
    return ovrClamp(cfg->freeSampleSecs, 1, BOT_HOST_OVR_MAX_PROBES);
}

static int ovrHistory(const BotHostOverrunCfg *cfg) {
    return ovrClamp(cfg->freeHistorySecs / ovrSample(cfg), 1,
                    BOT_HOST_FREE_MAX_READINGS);
}

/* The window's sums. True when the rate is known: a full window, bots in
 * each of its probes, and thinks. With the scaled wait, after a move or a
 * raise (the probes started again) the probes since that change count
 * too, once there are stepMinSecs of them and at least
 * BOT_HOST_OVR_SHORT_MIN_THINKS thinks. */
static bool ovrWindowRate(const BotHostOverrunState *st,
                          const BotHostOverrunCfg *cfg, double *pct) {
    int i, window = ovrWindow(cfg);
    uint64_t thinks = 0, overruns = 0;
    bool part = st->probeCount < window;
    *pct = 0.0;
    if (part && (!botHostOverrunScaled(cfg) ||
                 st->sinceChange >= OVR_LONG_AGO ||
                 st->probeCount < (cfg->stepMinSecs > 1 ? cfg->stepMinSecs
                                                        : 1))) {
        return false;
    }
    for (i = 0; i < st->probeCount && i < BOT_HOST_OVR_MAX_PROBES; i++) {
        if (!st->probeBots[i]) return false;
        thinks += st->probeThinks[i];
        overruns += st->probeOverruns[i];
    }
    if (thinks == 0) return false;
    if (part && thinks < BOT_HOST_OVR_SHORT_MIN_THINKS) return false;
    *pct = (double)overruns * 100.0 / (double)thinks;
    return true;
}

bool botHostOverrunRate(const BotHostOverrunState *st,
                        const BotHostOverrunCfg *cfg, double *pct) {
    double local;
    if (pct == NULL) pct = &local;
    *pct = 0.0;
    if (st == NULL || cfg == NULL) return false;
    return ovrWindowRate(st, cfg, pct);
}

/* The reading k places back (0: the newest). */
static const BotHostFreeReading *ovrReading(const BotHostOverrunState *st,
                                            const BotHostOverrunCfg *cfg,
                                            int k) {
    int cap = ovrHistory(cfg);
    return &st->reading[((st->readingNext - 1 - k) % cap + cap) % cap];
}

/* Fills the free CPU numbers of rep from the history. */
static void ovrFreeNumbers(const BotHostOverrunState *st,
                           const BotHostOverrunCfg *cfg,
                           BotHostOverrunReport *rep) {
    int k, recent, known = 0;
    double sum = 0.0;
    recent = (ovrWindow(cfg) + ovrSample(cfg) - 1) / ovrSample(cfg);
    rep->freeKnown = false;
    rep->freeAvg = 0.0;
    rep->lowestClean = -1;
    rep->highestBad = -1;
    for (k = 0; k < st->readingCount; k++) {
        const BotHostFreeReading *r = ovrReading(st, cfg, k);
        if (r->freePct == BOT_HOST_FREE_UNKNOWN) continue;
        if (k < recent) {
            sum += r->freePct;
            known++;
        }
        if (r->bad) {
            if (r->freePct > rep->highestBad) rep->highestBad = r->freePct;
        } else if (r->ran && r->bots > 0) {
            /* Clean evidence only from readings where bots thought. */
            if (rep->lowestClean < 0 || r->freePct < rep->lowestClean) {
                rep->lowestClean = r->freePct;
            }
        }
    }
    if (known > 0) {
        rep->freeKnown = true;
        rep->freeAvg = sum / known;
    }
    rep->costKnown = st->costKnown;
    rep->oneBotCost = st->oneBotCost;
    rep->predicted = rep->freeAvg - rep->oneBotCost;
}

static void ovrReportInit(BotHostOverrunReport *rep) {
    memset(rep, 0, sizeof(*rep));
    rep->newLimit = -1;
    rep->lowestClean = -1;
    rep->highestBad = -1;
    rep->why = "";
}

/* b and c of the raise rule. */
static bool ovrCleanEvidence(const BotHostOverrunReport *rep) {
    return rep->lowestClean >= 0 &&
           (double)rep->lowestClean <= rep->predicted;
}

static bool ovrAboveBad(const BotHostOverrunReport *rep) {
    return rep->highestBad < 0 || rep->predicted > (double)rep->highestBad;
}

BotHostOverrunAction botHostOverrunProbe(BotHostOverrunState *st,
                                         const BotHostOverrunCfg *cfg,
                                         const BotHostOverrunProbe *in,
                                         BotHostOverrunReport *rep) {
    BotHostOverrunReport local;
    int window;
    bool over, spaced;

    if (rep == NULL) rep = &local;
    ovrReportInit(rep);
    if (st == NULL || cfg == NULL || in == NULL) return BOT_HOST_OVR_NONE;
    if (!(cfg->pct > 0.0)) {
        st->limit = -1;
        return BOT_HOST_OVR_NONE;
    }
    window = ovrWindow(cfg);
    if (st->sinceChange < OVR_LONG_AGO) st->sinceChange++;
    if (st->sinceRefuse < OVR_LONG_AGO) st->sinceRefuse++;

    /* This probe, into the window. */
    rep->ratePct = in->thinks > 0
        ? (double)in->overruns * 100.0 / (double)in->thinks : 0.0;
    if (st->probeNext >= window) st->probeNext = 0;
    st->probeThinks[st->probeNext] = in->thinks;
    st->probeOverruns[st->probeNext] = in->overruns;
    st->probeBots[st->probeNext] = in->bots > 0;
    st->probeNext = (st->probeNext + 1) % window;
    if (st->probeCount < window) st->probeCount++;

    /* One bot's CPU: the host's bot CPU over its bots, kept while it has
     * none. */
    if (in->bots > 0) {
        if (in->botCpu <= 100) {
            st->oneBotCost = (double)in->botCpu / (double)in->bots;
            st->costKnown = true;
        } else if (in->botCpu == 0xFEu) {  /* BOT_HOST_CPU_UNDER_ONE */
            st->oneBotCost = 0.5 / (double)in->bots;
            st->costKnown = true;
        }
    }

    /* The free CPU reading, each freeSampleSecs, paired with the overrun
     * rate over the same seconds. */
    st->accOverruns += in->overruns;
    st->accThinks += in->thinks;
    if (++st->accProbes >= ovrSample(cfg)) {
        int cap = ovrHistory(cfg);
        BotHostFreeReading *r;
        if (st->readingNext >= cap) st->readingNext = 0;
        r = &st->reading[st->readingNext];
        r->freePct = in->freePct <= 100 ? in->freePct
                                        : (uint8_t)BOT_HOST_FREE_UNKNOWN;
        r->bots = (uint8_t)ovrClamp(in->bots, 0, 255);
        r->botCpu = in->botCpu;
        r->ran = st->accThinks > 0;
        r->bad = st->accThinks > 0 &&
                 (double)st->accOverruns * 100.0 / (double)st->accThinks >
                 cfg->pct;
        st->readingNext = (st->readingNext + 1) % cap;
        if (st->readingCount < cap) st->readingCount++;
        st->accOverruns = 0;
        st->accThinks = 0;
        st->accProbes = 0;
        rep->sampled = true;
        rep->sample = *r;
    }

    /* A host that now reports no more slots than the limit needs none. */
    if (st->limit >= 0 && st->limit >= in->slots) st->limit = -1;

    rep->probes = st->probeCount;
    rep->windowKnown = ovrWindowRate(st, cfg, &rep->windowPct);
    ovrFreeNumbers(st, cfg, rep);
    rep->limitBefore = st->limit;
    rep->newLimit = st->limit;

    over = rep->windowKnown && rep->windowPct > cfg->pct;
    spaced = st->sinceChange >= cfg->stepSecs;

    if (over) {
        /* One move each wait: shorter the further the rate is over. */
        rep->severity = rep->windowPct / cfg->pct;
        rep->waitSecs = botHostOverrunWaitSecs(cfg, rep->windowPct);
        if ((double)st->sinceChange < rep->waitSecs) return BOT_HOST_OVR_NONE;
        rep->why = "overruns";
        return BOT_HOST_OVR_OVER;
    }

    /* A raise: only with a full window since the last change. */
    if (st->limit < 0 || !spaced || st->probeCount < window) {
        return BOT_HOST_OVR_NONE;
    }
    if (!rep->freeKnown) {
        rep->why = "no free CPU data";
    } else if (!rep->costKnown) {
        rep->why = "the CPU of one bot is not known";
    } else if (!ovrCleanEvidence(rep)) {
        rep->why = "no clean reading with that little free CPU";
    } else if (!ovrAboveBad(rep)) {
        rep->why = "a bad reading had as much free CPU";
    } else {
        st->limit++;
        if (st->limit >= in->slots) st->limit = -1;
        rep->newLimit = st->limit;
        ovrClearProbes(st);
        st->sinceChange = 0;
        rep->why = "free CPU";
        return BOT_HOST_OVR_RAISE;
    }
    if (st->sinceRefuse < cfg->stepSecs) return BOT_HOST_OVR_NONE;
    st->sinceRefuse = 0;
    return BOT_HOST_OVR_REFUSE;
}

int botHostOverrunMoved(BotHostOverrunState *st, int bots) {
    return botHostOverrunMovedN(st, bots, 1);
}

int botHostOverrunMovedN(BotHostOverrunState *st, int bots, int n) {
    if (st == NULL) return -1;
    st->limit = bots - (n > 1 ? n : 1);
    if (st->limit < 0) st->limit = 0;
    st->cut = true;
    ovrClearProbes(st);
    st->sinceChange = 0;
    return st->limit;
}

void botHostOverrunGave(BotHostOverrunState *st) {
    if (st == NULL) return;
    ovrClearProbes(st);
    st->sinceChange = 0;
}

void botHostOverrunHeld(BotHostOverrunState *st) {
    if (st == NULL) return;
    st->sinceChange = 0;
}

/* The free CPU check's reasons that rest on known numbers. */
static const char OVR_WHY_NO_CLEAN[] =
    "no clean reading with that little free CPU";
static const char OVR_WHY_BAD_AS_MUCH[] = "a bad reading had as much free CPU";
static const char OVR_WHY_TOO_LITTLE[] = "too little free CPU";

static bool ovrFreeRule(BotHostOverrunReport *rep);

bool botHostOverrunMayTake(const BotHostOverrunState *st,
                           const BotHostOverrunCfg *cfg, int bots,
                           uint8_t latestFree, BotHostOverrunReport *rep) {
    BotHostOverrunReport local;
    const BotHostFreeReading *newest;

    if (rep == NULL) rep = &local;
    ovrReportInit(rep);
    if (st == NULL || cfg == NULL || !(cfg->pct > 0.0)) return true;
    ovrFreeNumbers(st, cfg, rep);
    if (!rep->costKnown && st->costFallbackKnown) {
        /* No measured one-bot cost yet: the full-budget estimate, the most
         * one bot can use while the runners keep to their budget. */
        rep->costKnown = true;
        rep->costFromBudget = true;
        rep->oneBotCost = st->costFallback;
        rep->predicted = rep->freeAvg - rep->oneBotCost;
    }
    rep->limitBefore = st->limit;
    rep->newLimit = st->limit;
    if (bots <= 0) {
        rep->why = "its first bot";
        return true;
    }
    if (latestFree > 100 && !rep->freeKnown) {
        /* No free CPU data: the slots (and a lowered limit) only. */
        rep->why = "no free CPU data";
        return true;
    }
    if (st->readingCount == 0) {
        rep->why = "no reading yet";
        return false;
    }
    newest = ovrReading(st, cfg, 0);
    if ((int)newest->bots < bots || !newest->ran) {
        rep->why = "no reading counts its newest bot yet";
        return false;
    }
    if (!rep->costKnown) {
        rep->why = "the CPU of one bot is not known";
        return false;
    }
    if (!rep->freeKnown) {
        rep->why = "no free CPU average";
        return false;
    }
    return ovrFreeRule(rep);
}

/* The free CPU check's rules on known numbers (rep's predicted free CPU,
 * one-bot cost and readings), with the reason in rep->why. */
static bool ovrFreeRule(BotHostOverrunReport *rep) {
    if (rep->highestBad >= 0) {
        if (!ovrCleanEvidence(rep)) {
            rep->why = OVR_WHY_NO_CLEAN;
            return false;
        }
        if (!ovrAboveBad(rep)) {
            rep->why = OVR_WHY_BAD_AS_MUCH;
            return false;
        }
        rep->why = "clean evidence";
        return true;
    }
    if (rep->predicted >= rep->oneBotCost) {
        rep->why = "room for one more bot";
        return true;
    }
    if (ovrCleanEvidence(rep)) {
        rep->why = "clean evidence";
        return true;
    }
    rep->why = OVR_WHY_TOO_LITTLE;
    return false;
}

bool botHostOverrunFreeLow(const BotHostOverrunState *st,
                           const BotHostOverrunCfg *cfg, int bots,
                           uint8_t latestFree, BotHostOverrunReport *rep) {
    BotHostOverrunReport local;
    if (rep == NULL) rep = &local;
    if (bots <= 0) {
        ovrReportInit(rep);
        return false;
    }
    if (botHostOverrunMayTake(st, cfg, bots - 1, latestFree, rep)) {
        return false;
    }
    return rep->why == OVR_WHY_NO_CLEAN || rep->why == OVR_WHY_BAD_AS_MUCH ||
           rep->why == OVR_WHY_TOO_LITTLE;
}

int botHostOverrunBatchSize(const BotHostOverrunCfg *cfg, int shortBy,
                            bool fresh) {
    if (cfg == NULL || !cfg->freeBatch || !fresh || shortBy <= 0) return 0;
    return shortBy;
}

int botHostOverrunFreeRoom(const BotHostOverrunState *st,
                           const BotHostOverrunCfg *cfg, int bots,
                           uint8_t latestFree, int max) {
    BotHostOverrunReport rep;
    int room;
    if (max <= 0) return 0;
    if (!botHostOverrunMayTake(st, cfg, bots, latestFree, &rep)) return 0;
    if (st == NULL || cfg == NULL || !(cfg->pct > 0.0) ||
        st->readingCount == 0 || !rep.freeKnown || !rep.costKnown ||
        !(rep.oneBotCost > 0.0)) {
        return 1;
    }
    /* The first bot passed the check above; each bot after it takes the
     * one-bot cost off the free CPU average. */
    for (room = 1; room < max; room++) {
        BotHostOverrunReport r = rep;
        r.freeAvg = rep.freeAvg - (double)room * rep.oneBotCost;
        r.predicted = r.freeAvg - r.oneBotCost;
        if (!ovrFreeRule(&r)) break;
    }
    return room;
}

int botHostOverrunFreeShort(const BotHostOverrunState *st,
                            const BotHostOverrunCfg *cfg, int bots,
                            uint8_t latestFree, BotHostOverrunReport *rep) {
    BotHostOverrunReport local;
    int k;
    if (rep == NULL) rep = &local;
    if (!botHostOverrunFreeLow(st, cfg, bots, latestFree, rep)) return 0;
    /* A bad reading and no clean one: no free CPU level is known to be
     * enough, so the numbers cannot count the bots. */
    if (rep->highestBad >= 0 && rep->lowestClean < 0) return 1;
    for (k = 1; k < bots; k++) {
        BotHostOverrunReport r = *rep;
        /* The newest of the bots - k left is the one the check is for. */
        if (bots - k - 1 <= 0) return k;
        r.freeAvg = rep->freeAvg + (double)k * rep->oneBotCost;
        r.predicted = r.freeAvg - r.oneBotCost;
        if (ovrFreeRule(&r)) return k;
    }
    return bots > 1 ? bots - 1 : 1;
}

/* ---- The overrun move -------------------------------------------------- */

const char *botHostOvrRuleName(BotHostOvrRule rule) {
    switch (rule) {
    case BOT_HOST_OVR_RULE_NORMAL:    return "normal";
    case BOT_HOST_OVR_RULE_EMERGENCY: return "emergency";
    case BOT_HOST_OVR_RULE_TRIAL:     return "trial";
    default:                          return "none";
    }
}

double botHostOvrStayNeed(const BotHostOverrunCfg *cfg, int moves) {
    double need;
    int k;
    if (cfg == NULL || moves <= 0) return 0.0;
    need = cfg->staySecs > 0 ? (double)cfg->staySecs : 0.0;
    /* Held at about a day, so many moves give a large number, not an
     * infinity. */
    for (k = 1; k < moves && need < 86400.0; k++) need *= cfg->stayFactor;
    return need > 86400.0 ? 86400.0 : need;
}

/* gain^(moves + 1): the part of the source rate rule c allows. */
static double ovrGainPow(const BotHostOverrunCfg *cfg, int moves) {
    double f = 1.0;
    int k;
    for (k = 0; k <= moves && k < 64; k++) f *= cfg->gain;
    return f;
}

BotHostOvrRule botHostOvrMoveRule(const BotHostOverrunCfg *cfg, double srcPct,
                                  const BotHostOvrHost *dst, int moves,
                                  double staySecs, const char **why) {
    const char *local;
    bool trial, emergency;
    if (why == NULL) why = &local;
    *why = "";
    if (cfg == NULL || dst == NULL || !(cfg->pct > 0.0)) {
        *why = "the check is off";
        return BOT_HOST_OVR_RULE_NONE;
    }
    if (!(srcPct > cfg->pct)) {           /* a */
        *why = "the source is not over the target rate";
        return BOT_HOST_OVR_RULE_NONE;
    }
    if (!dst->mayTake) {                  /* e */
        *why = "the target may not take a bot";
        return BOT_HOST_OVR_RULE_NONE;
    }
    trial = dst->bots <= 0;
    if (!trial && !dst->rateKnown) {
        *why = "the target's rate is not known yet";
        return BOT_HOST_OVR_RULE_NONE;
    }
    emergency = cfg->emergencyPct > 0.0 && srcPct >= cfg->emergencyPct;
    if (emergency) {
        if (!trial && !(dst->ratePct < cfg->pct) &&
            dst->ratePct > srcPct * BOT_HOST_OVR_EMERGENCY_SHARE +
                               BOT_HOST_PLACE_EPS) {
            *why = "the target is not below the target rate or half the "
                   "source rate";
            return BOT_HOST_OVR_RULE_NONE;
        }
        return BOT_HOST_OVR_RULE_EMERGENCY;
    }
    if (!trial && !(dst->ratePct < cfg->pct)) {          /* b */
        *why = "the target is not below the target rate";
        return BOT_HOST_OVR_RULE_NONE;
    }
    if (!trial && dst->ratePct > srcPct * ovrGainPow(cfg, moves) +
                                     BOT_HOST_PLACE_EPS) {  /* c */
        *why = "the target is not low enough for this bot's moves";
        return BOT_HOST_OVR_RULE_NONE;
    }
    if (moves > 0 &&
        staySecs + BOT_HOST_PLACE_EPS < botHostOvrStayNeed(cfg, moves)) {
        *why = "the bot has not stayed long enough";     /* d */
        return BOT_HOST_OVR_RULE_NONE;
    }
    return trial ? BOT_HOST_OVR_RULE_TRIAL : BOT_HOST_OVR_RULE_NORMAL;
}

/* The lag of a target for the order: lag off counts all as 0; a lag not
 * known counts refMs, as in the spread. */
static double ovrLagOf(const BotHostOvrHost *h, const BotHostPlaceLag *lag) {
    if (!placeLagOn(lag)) return 0.0;
    return h->lagMs >= 0.0 ? h->lagMs : lag->refMs;
}

/* True when target a goes before target b (rule 10). */
static bool ovrBetter(const BotHostOvrHost *hosts, int a, int b,
                      const BotHostPlaceLag *lag) {
    const BotHostOvrHost *ha = &hosts[a], *hb = &hosts[b];
    bool ta = ha->bots <= 0, tb = hb->bots <= 0;
    double la, lb;
    if (ta != tb) return tb;              /* a host with a rate first */
    if (!ta) {
        if (ha->ratePct < hb->ratePct - BOT_HOST_OVR_RATE_TIE_PCT) return true;
        if (ha->ratePct > hb->ratePct + BOT_HOST_OVR_RATE_TIE_PCT) {
            return false;
        }
    }
    la = ovrLagOf(ha, lag);
    lb = ovrLagOf(hb, lag);
    if (la < lb - BOT_HOST_PLACE_EPS) return true;
    if (la > lb + BOT_HOST_PLACE_EPS) return false;
    if (ha->slots != hb->slots) return ha->slots > hb->slots;
    return a < b;
}

bool botHostOvrPickMove(const BotHostOverrunCfg *cfg,
                        const BotHostOvrHost *hosts, int n, int src,
                        const BotHostOvrBot *bots, int nBots,
                        const BotHostPlaceLag *lag, BotHostOvrMove *out) {
    BotHostOvrMove local;
    int order[BOT_HOST_OVR_SEATS];
    int i, j, k, count = 0;
    double srcPct;

    if (out == NULL) out = &local;
    memset(out, 0, sizeof(*out));
    out->seat = -1;
    out->to = -1;
    out->why = "";
    if (cfg == NULL || hosts == NULL || src < 0 || src >= n) {
        out->why = "no host";
        return false;
    }
    srcPct = hosts[src].ratePct;
    out->srcPct = srcPct;
    if (!(cfg->pct > 0.0) || !hosts[src].rateKnown || !(srcPct > cfg->pct)) {
        out->why = "the source is not over the target rate";
        return false;
    }
    if (bots == NULL || nBots <= 0) {
        out->why = "no bot may move now";
        return false;
    }
    /* The bots in order: fewest moves, then the longest stay, then the
     * lowest seat. */
    for (i = 0; i < nBots && count < BOT_HOST_OVR_SEATS; i++) {
        order[count++] = i;
    }
    for (i = 1; i < count; i++) {
        int v = order[i];
        for (j = i; j > 0; j--) {
            const BotHostOvrBot *a = &bots[v], *b = &bots[order[j - 1]];
            bool before = a->moves < b->moves ||
                          (a->moves == b->moves &&
                           (a->staySecs > b->staySecs ||
                            (a->staySecs == b->staySecs && a->seat < b->seat)));
            if (!before) break;
            order[j] = order[j - 1];
        }
        order[j] = v;
    }
    out->why = "no better host";
    for (k = 0; k < count; k++) {
        const BotHostOvrBot *bot = &bots[order[k]];
        int best = -1;
        BotHostOvrRule bestRule = BOT_HOST_OVR_RULE_NONE;
        for (i = 0; i < n; i++) {
            BotHostOvrRule rule;
            if (i == src) continue;
            rule = botHostOvrMoveRule(cfg, srcPct, &hosts[i], bot->moves,
                                      bot->staySecs, NULL);
            if (rule == BOT_HOST_OVR_RULE_NONE) continue;
            if (bot->back && i == bot->backTo &&
                rule != BOT_HOST_OVR_RULE_EMERGENCY) {
                /* It left that place within the stay time. */
                continue;
            }
            if (best < 0 || ovrBetter(hosts, i, best, lag)) {
                best = i;
                bestRule = rule;
            }
        }
        if (best < 0) continue;
        out->seat = bot->seat;
        out->to = best;
        out->moves = bot->moves;
        out->staySecs = bot->staySecs;
        out->needStaySecs = bestRule == BOT_HOST_OVR_RULE_EMERGENCY
                                ? 0.0 : botHostOvrStayNeed(cfg, bot->moves);
        out->dstKnown = hosts[best].bots > 0;
        out->dstPct = out->dstKnown ? hosts[best].ratePct : 0.0;
        out->rule = bestRule;
        out->why = "";
        return true;
    }
    return false;
}

void botHostOvrMovesReset(BotHostOvrMoves *mv) {
    if (mv == NULL) return;
    memset(mv, 0, sizeof(*mv));
}

void botHostOvrMovesSeat(BotHostOvrMoves *mv, int s, bool isBot,
                         uint32_t now) {
    if (mv == NULL || s < 0 || s >= BOT_HOST_OVR_SEATS) return;
    if (!isBot) {
        mv->moves[s] = 0;
        mv->lastTick[s] = 0;
        mv->seen[s] = false;
        return;
    }
    if (!mv->seen[s]) {
        mv->seen[s] = true;
        mv->startTick[s] = now;
    }
}

void botHostOvrMovesNote(BotHostOvrMoves *mv, int s, BotHostMoveKind kind,
                         uint32_t now) {
    if (mv == NULL || s < 0 || s >= BOT_HOST_OVR_SEATS) return;
    /* A forced move has no penalty: m and the stay stay as they are. */
    if (kind != BOT_HOST_MOVE_VOLUNTARY) return;
    mv->moves[s]++;
    mv->lastTick[s] = now;
}

int botHostOvrMovesCount(const BotHostOvrMoves *mv, int s) {
    if (mv == NULL || s < 0 || s >= BOT_HOST_OVR_SEATS) return 0;
    return mv->moves[s];
}

double botHostOvrMovesStay(const BotHostOvrMoves *mv, int s, uint32_t now,
                           int ticksPerSec) {
    uint32_t from;
    if (mv == NULL || s < 0 || s >= BOT_HOST_OVR_SEATS || ticksPerSec <= 0) {
        return 0.0;
    }
    from = mv->moves[s] > 0 ? mv->lastTick[s]
                            : (mv->seen[s] ? mv->startTick[s] : now);
    return (double)(uint32_t)(now - from) / (double)ticksPerSec;
}

uint32_t botHostSeatsToGiveBack(const int *seatHost, int n, int h,
                                int newSlots) {
    uint32_t mask = 0;
    int s;
    if (seatHost == NULL || h < 0 || newSlots > 0) return 0;
    if (n > 32) n = 32;
    for (s = 0; s < n; s++) {
        if (seatHost[s] == h) mask |= (uint32_t)1u << s;
    }
    return mask;
}

/* ---- No bot moves while a game runs (-bothost-nomove) ------------------ */

void botHostNoMoveInit(BotHostNoMove *nm, bool on) {
    if (nm == NULL) return;
    memset(nm, 0, sizeof(*nm));
    nm->on = on;
}

void botHostNoMoveNewGame(BotHostNoMove *nm) {
    bool on;
    if (nm == NULL) return;
    on = nm->on;
    botHostNoMoveInit(nm, on);
}

void botHostNoMoveSeat(BotHostNoMove *nm, int s, bool isBot, bool onHost,
                       uint32_t now) {
    if (nm == NULL || s < 0 || s >= BOT_HOST_NOMOVE_SEATS) return;
    if (!isBot) {
        nm->seen[s] = false;
        nm->wasOnHost[s] = false;
        return;
    }
    if (!nm->seen[s]) {
        nm->seen[s] = true;
        nm->seenTick[s] = now;
    }
    if (onHost) nm->wasOnHost[s] = true;
}

bool botHostNoMoveMayPlace(const BotHostNoMove *nm, int s, bool running,
                           uint32_t now, uint32_t placeTicks) {
    if (nm == NULL || !nm->on || !running) return true;
    if (s < 0 || s >= BOT_HOST_NOMOVE_SEATS) return false;
    if (!nm->seen[s] || nm->wasOnHost[s]) return false;
    return now - nm->seenTick[s] < placeTicks;
}

bool botHostNoMoveMayMove(const BotHostNoMove *nm, bool running) {
    return nm == NULL || !nm->on || !running;
}

int botHostNoMoveOverrunDecision(const BotHostNoMove *nm, bool running) {
    return botHostNoMoveMayMove(nm, running) ? BOTHOST_DECISION_MOVE
                                             : BOTHOST_DECISION_WOULD_MOVE;
}

bool botHostNoMoveGiveBack(const BotHostNoMove *nm, bool running, int bots,
                           int slots) {
    if (botHostNoMoveMayMove(nm, running)) return false;
    return bots > 0 && bots > (slots < 0 ? 0 : slots);
}
