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

#include "lobby_bot_pools.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "bolo_rand.h"

/* Loose upper bound for "names in any one pool" — used to size the
 * uniform-pick scratch buffer in lobbyBotPoolPick.  Generous so a new
 * pool added below doesn't silently truncate. */
#define POOL_MAX_NAMES LOBBY_BOT_POOL_MAX_NAMES

/* Pool entry: a label + a fixed array of names. Names are mirrored
 * verbatim from the Layout A mockup (LobbyData.jsx BOT_NAME_POOLS). */
typedef struct {
    const char *label;
    const char *const *names;
    int         count;
} BotPool;

/* ── Pool data — order matters; index travels on the wire ───────── */

static const char *const s_namesClassic[] = {
    "HAL-9000", "GLaDOS", "Skynet", "Wintermute", "Cortana",
    "Marvin", "Bender", "TARS", "CASE", "Roy", "Sonny", "Ash"
};

static const char *const s_namesPainters[] = {
    "Van Gogh", "Picasso", "Monet", "Rembrandt", "Dali",
    "Vermeer", "Klimt", "Cezanne", "Matisse", "Kahlo",
    "Pollock", "Warhol"
};

static const char *const s_namesMusicians[] = {
    "Mozart", "Beethoven", "Hendrix", "Bowie", "Prince",
    "Aretha", "Coltrane", "Bach", "Miles", "Joplin",
    "Lennon", "Cash"
};

static const char *const s_namesMartialArts[] = {
    "Bruce Lee", "Jackie", "Jet Li", "Donnie Yen", "Mas Oyama",
    "Ip Man", "Funakoshi", "Ueshiba", "Helio", "Sonny Chiba",
    "Royce", "Khabib"
};

static const char *const s_namesScientists[] = {
    "Einstein", "Newton", "Curie", "Tesla", "Darwin",
    "Hawking", "Feynman", "Turing", "Hopper", "Lovelace",
    "Sagan", "Rosalind"
};

static const char *const s_namesPhilosophers[] = {
    "Socrates", "Plato", "Nietzsche", "Kant", "Sartre",
    "Aristotle", "Confucius", "Hume", "Camus", "Beauvoir",
    "Marcus", "Hypatia"
};

static const char *const s_namesGenerals[] = {
    "Hannibal", "Caesar", "Napoleon", "Patton", "Sun Tzu",
    "Zhukov", "Saladin", "Alexander", "Rommel", "Khan",
    "Boudica", "Shaka"
};

static const char *const s_namesCallsigns[] = {
    "Maverick", "Goose", "Iceman", "Viper", "Slider",
    "Hollywood", "Wolfman", "Merlin", "Sundown", "Stinger",
    "Ghost Rider", "Charlie"
};

/* Numeric pool — generated at module init (Bot-01..Bot-16). */
static char        s_namesNumericStorage[16][8];     /* "Bot-01\0" each */
static const char *s_namesNumeric[16];
static int         s_namesNumericInit = 0;

static void initNumericPool(void) {
    int i;
    for (i = 0; i < 16; i++) {
        snprintf(s_namesNumericStorage[i], sizeof(s_namesNumericStorage[i]),
                 "Bot-%02d", i + 1);
        s_namesNumeric[i] = s_namesNumericStorage[i];
    }
    s_namesNumericInit = 1;
}

#define POOL_ENTRY(label_, arr_) { (label_), (arr_), (int)(sizeof(arr_) / sizeof((arr_)[0])) }

/* Built-in themed pools — the fallback used when no custom set has
 * been installed (e.g. data/bot_names.json is missing).  The numeric
 * pool is NOT in here; it is always appended as the last index by the
 * accessors below. */
static const BotPool s_builtinPools[] = {
    POOL_ENTRY("Classic AI",       s_namesClassic),
    POOL_ENTRY("Famous Painters",  s_namesPainters),
    POOL_ENTRY("Famous Musicians", s_namesMusicians),
    POOL_ENTRY("Martial Artists",  s_namesMartialArts),
    POOL_ENTRY("Famous Scientists", s_namesScientists),
    POOL_ENTRY("Philosophers",     s_namesPhilosophers),
    POOL_ENTRY("Generals & Tacticians", s_namesGenerals),
    POOL_ENTRY("Pilot Callsigns",  s_namesCallsigns),
};

#define BUILTIN_THEMED_COUNT (int)(sizeof(s_builtinPools) / sizeof(s_builtinPools[0]))

/* ── Active (possibly custom) themed pools ──────────────────────────
 * When s_useDyn is set, the themed pools come from the heap-allocated
 * s_dyn[] (installed via lobbyBotPoolsInstall); otherwise they come
 * from s_builtinPools[].  Either way the generated "Numbered Bots"
 * pool is appended as the last index. */
typedef struct {
    char  *label;
    char **names;   /* count owned strings */
    int    count;
} DynPool;

static DynPool *s_dyn = NULL;
static int      s_dynCount = 0;
static int      s_useDyn = 0;

static int themedCount(void) {
    return s_useDyn ? s_dynCount : BUILTIN_THEMED_COUNT;
}

#define NUMERIC_POOL_IDX (themedCount())

/* ── Public API ─────────────────────────────────────────────────── */

int lobbyBotPoolCount(void) {
    return themedCount() + 1;   /* +1 for the numbered pool */
}

const char *lobbyBotPoolLabel(int poolIdx) {
    int themed = themedCount();
    if (poolIdx < 0 || poolIdx > themed) return "Unknown";
    if (poolIdx == themed) return "Numbered Bots";
    return s_useDyn ? s_dyn[poolIdx].label : s_builtinPools[poolIdx].label;
}

int lobbyBotPoolNameCount(int poolIdx) {
    int themed = themedCount();
    if (poolIdx < 0 || poolIdx > themed) return 0;
    if (poolIdx == themed) return 16;   /* numbered pool */
    return s_useDyn ? s_dyn[poolIdx].count : s_builtinPools[poolIdx].count;
}

const char *lobbyBotPoolName(int poolIdx, int nameIdx) {
    int themed = themedCount();
    if (poolIdx < 0 || poolIdx > themed) return NULL;

    /* Numeric pool: lazy-init the storage on first read. */
    if (poolIdx == themed) {
        if (nameIdx < 0 || nameIdx >= 16) return NULL;
        if (!s_namesNumericInit) initNumericPool();
        return s_namesNumeric[nameIdx];
    }

    if (s_useDyn) {
        if (nameIdx < 0 || nameIdx >= s_dyn[poolIdx].count) return NULL;
        return s_dyn[poolIdx].names[nameIdx];
    }
    if (nameIdx < 0 || nameIdx >= s_builtinPools[poolIdx].count) return NULL;
    return s_builtinPools[poolIdx].names[nameIdx];
}

static int isUsed(const char *name, const char **used, int usedCount) {
    int i;
    for (i = 0; i < usedCount; i++) {
        if (used[i] && strcmp(used[i], name) == 0) return 1;
    }
    return 0;
}

char *lobbyBotPoolPick(int poolIdx,
                       const char **used, int usedCount,
                       char *outBuf, int outBufLen) {
    int n, i, overflow;
    int unusedIdx[POOL_MAX_NAMES];
    int unusedCount = 0;
    const char *cand;
    const char *label;

    if (!outBuf || outBufLen <= 0) return outBuf;
    outBuf[0] = '\0';

    if (poolIdx < 0 || poolIdx >= lobbyBotPoolCount()) {
        /* Unknown pool — fall back to classic. */
        poolIdx = 0;
    }

    n = lobbyBotPoolNameCount(poolIdx);

    /* Collect every unused candidate then pick one uniformly at
     * random.  The previous "first unused wins" pick made reroll
     * ping-pong between two names (current-name went into the
     * used-list, so reroll always picked the same alternative). */
    for (i = 0; i < n && unusedCount < POOL_MAX_NAMES; i++) {
        cand = lobbyBotPoolName(poolIdx, i);
        if (!cand) continue;
        if (!isUsed(cand, used, usedCount)) {
            unusedIdx[unusedCount++] = i;
        }
    }
    if (unusedCount > 0) {
        int pick = (int)bolo_rand_below((uint32_t)unusedCount);
        cand = lobbyBotPoolName(poolIdx, unusedIdx[pick]);
        strncpy(outBuf, cand, (size_t)outBufLen - 1);
        outBuf[outBufLen - 1] = '\0';
        return outBuf;
    }

    /* Pool exhausted — overflow into "<label> N", finding the
     * lowest N that's also unused. */
    label = lobbyBotPoolLabel(poolIdx);
    for (overflow = 1; overflow < 1000; overflow++) {
        snprintf(outBuf, (size_t)outBufLen, "%s %d", label, overflow);
        if (!isUsed(outBuf, used, usedCount)) return outBuf;
    }

    /* Catastrophic fallback if even 999 overflow names collide. */
    strncpy(outBuf, "Bot", (size_t)outBufLen - 1);
    outBuf[outBufLen - 1] = '\0';
    return outBuf;
}

/* ── Runtime-loadable pools ─────────────────────────────────────── */

static void freeDyn(DynPool *pools, int count) {
    int i, j;
    if (!pools) return;
    for (i = 0; i < count; i++) {
        if (pools[i].names) {
            for (j = 0; j < pools[i].count; j++) free(pools[i].names[j]);
            free(pools[i].names);
        }
        free(pools[i].label);
    }
    free(pools);
}

void lobbyBotPoolsReset(void) {
    if (s_dyn) freeDyn(s_dyn, s_dynCount);
    s_dyn = NULL;
    s_dynCount = 0;
    s_useDyn = 0;
}

/* Number of bytes in a UTF-8 string (just strlen — names are stored
 * verbatim; the player-name validator does the heavy normalisation
 * when the name is actually used). */
static int nameTooLong(const char *s) {
    return (int)strlen(s) > LOBBY_BOT_POOL_MAX_NAME_BYTES;
}

static int nameDupInPool(char **names, int count, const char *cand) {
    int i;
    for (i = 0; i < count; i++) {
#if defined(_WIN32)
        if (_stricmp(names[i], cand) == 0) return 1;
#else
        if (strcasecmp(names[i], cand) == 0) return 1;
#endif
    }
    return 0;
}

static char *dupStr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

int lobbyBotPoolsInstall(const LobbyBotPoolDef *defs, int defCount,
                         LobbyBotPoolLoadStats *stats) {
    LobbyBotPoolLoadStats st;
    DynPool *out;
    int outCount = 0;
    int i, j;

    memset(&st, 0, sizeof(st));
    st.poolsIn = defCount < 0 ? 0 : defCount;

    if (!defs || defCount <= 0) {
        if (stats) *stats = st;
        return 0;   /* nothing offered — leave current pools alone */
    }

    out = (DynPool *)calloc((size_t)defCount, sizeof(DynPool));
    if (!out) {
        if (stats) *stats = st;
        return 0;
    }

    for (i = 0; i < defCount; i++) {
        const LobbyBotPoolDef *d = &defs[i];
        DynPool *p;
        char labelBuf[LOBBY_BOT_POOL_MAX_LABEL_BYTES + 1];

        if (outCount >= LOBBY_BOT_POOL_MAX_POOLS) {
            st.poolsDropped++;   /* over the wire-index cap */
            continue;
        }
        if (d->nameCount <= 0 || !d->names) {
            st.poolsDropped++;
            continue;
        }

        p = &out[outCount];
        p->names = (char **)calloc((size_t)d->nameCount, sizeof(char *));
        if (!p->names) { st.poolsDropped++; continue; }
        p->count = 0;

        for (j = 0; j < d->nameCount; j++) {
            const char *nm = d->names[j];
            st.namesIn++;
            if (!nm || nm[0] == '\0') { st.namesDropped++; continue; }
            if (nameTooLong(nm))       { st.namesDropped++; continue; }
            if (p->count >= LOBBY_BOT_POOL_MAX_NAMES) { st.namesDropped++; continue; }
            if (nameDupInPool(p->names, p->count, nm)) { st.namesDropped++; continue; }
            p->names[p->count] = dupStr(nm);
            if (!p->names[p->count]) { st.namesDropped++; continue; }
            p->count++;
            st.namesKept++;
        }

        if (p->count == 0) {
            /* No usable names — drop the whole pool. */
            free(p->names);
            p->names = NULL;
            st.poolsDropped++;
            continue;
        }

        /* Label: use given, default to "Pool N", clamp length. */
        if (d->label && d->label[0]) {
            snprintf(labelBuf, sizeof(labelBuf), "%s", d->label);
        } else {
            snprintf(labelBuf, sizeof(labelBuf), "Pool %d", outCount + 1);
        }
        p->label = dupStr(labelBuf);
        if (!p->label) {
            for (j = 0; j < p->count; j++) free(p->names[j]);
            free(p->names);
            p->names = NULL;
            st.poolsDropped++;
            continue;
        }

        outCount++;
        st.poolsKept++;
    }

    if (outCount == 0) {
        /* Nothing usable — keep whatever was active before. */
        freeDyn(out, defCount);
        if (stats) *stats = st;
        return 0;
    }

    /* Swap in the new set. */
    lobbyBotPoolsReset();
    s_dyn = out;
    s_dynCount = outCount;
    s_useDyn = 1;

    if (stats) *stats = st;
    return outCount;
}

/* ── Wire catalog (serialize / compress / install) ──────────────── */

static void putU16BE(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}
static void putU32BE(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}
static unsigned getU16BE(const unsigned char *p) {
    return ((unsigned)p[0] << 8) | (unsigned)p[1];
}
static unsigned long getU32BE(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

/* Pack the active themed pools into `raw` (uncompressed), dropping
 * trailing pools that would push past LOBBY_BOT_CATALOG_MAX_BYTES.
 * Returns the payload length (>=1; first byte is the pool count). */
static int serializeRaw(unsigned char *raw) {
    int themed = themedCount();
    int pos = 1;          /* reserve byte 0 for the final pool count */
    int written = 0;
    int i, j;

    if (themed > LOBBY_BOT_POOL_MAX_POOLS) themed = LOBBY_BOT_POOL_MAX_POOLS;

    for (i = 0; i < themed; i++) {
        const char *label = lobbyBotPoolLabel(i);
        int nameCount = lobbyBotPoolNameCount(i);
        int labelLen = (int)strlen(label);
        int start = pos;

        if (labelLen > LOBBY_BOT_POOL_MAX_LABEL_BYTES)
            labelLen = LOBBY_BOT_POOL_MAX_LABEL_BYTES;

        /* label */
        if (pos + 1 + labelLen + 2 > (int)LOBBY_BOT_CATALOG_MAX_BYTES) break;
        raw[pos++] = (unsigned char)labelLen;
        memcpy(raw + pos, label, (size_t)labelLen);
        pos += labelLen;

        /* nameCount placeholder; fill after counting fits */
        {
            int countPos = pos;
            int packed = 0;
            pos += 2;
            for (j = 0; j < nameCount; j++) {
                const char *nm = lobbyBotPoolName(i, j);
                int nl;
                if (!nm) continue;
                nl = (int)strlen(nm);
                if (nl > LOBBY_BOT_POOL_MAX_NAME_BYTES) continue;
                if (pos + 1 + nl > (int)LOBBY_BOT_CATALOG_MAX_BYTES) break;
                raw[pos++] = (unsigned char)nl;
                memcpy(raw + pos, nm, (size_t)nl);
                pos += nl;
                packed++;
            }
            if (packed == 0) {
                /* Pool didn't fit at all — roll back and stop. */
                pos = start;
                break;
            }
            putU16BE(raw + countPos, (unsigned)packed);
        }
        written++;
    }

    raw[0] = (unsigned char)written;
    return pos;
}

/* The id of an uncompressed catalogue: its CRC-32, never 0 (0 means "no
 * catalogue"). Taken over the payload rather than the compressed blob, so two
 * builds whose zlib compresses differently still agree on the same pools. */
static uint32_t catalogIdOf(const unsigned char *raw, int rawLen) {
    uint32_t id = (uint32_t)crc32(0L, raw, (uInt)rawLen);
    return id != 0 ? id : 1u;
}

int lobbyBotPoolsSerializeWithId(unsigned char *out, int outCap,
                                 uint32_t *outId) {
    unsigned char *raw;
    int rawLen;
    uLongf compCap, compLen;
    int total;

    if (outId) *outId = 0;
    if (!out || outCap <= 4) return -1;
    if (themedCount() <= 0) return 0;

    raw = (unsigned char *)malloc(LOBBY_BOT_CATALOG_MAX_BYTES);
    if (!raw) return -1;
    rawLen = serializeRaw(raw);
    if (rawLen <= 1) { free(raw); return 0; }   /* nothing usable */

    compCap = compressBound((uLong)rawLen);
    if ((int)compCap > outCap - 4) { free(raw); return -1; }
    compLen = compCap;
    if (compress2(out + 4, &compLen, raw, (uLong)rawLen, 6) != Z_OK) {
        free(raw);
        return -1;
    }
    putU32BE(out, (unsigned long)rawLen);
    if (outId) *outId = catalogIdOf(raw, rawLen);
    free(raw);

    total = 4 + (int)compLen;
    return total;
}

int lobbyBotPoolsSerialize(unsigned char *out, int outCap) {
    return lobbyBotPoolsSerializeWithId(out, outCap, NULL);
}

uint32_t lobbyBotPoolsCatalogId(void) {
    unsigned char *raw;
    int rawLen;
    uint32_t id;

    if (themedCount() <= 0) return 0;
    raw = (unsigned char *)malloc(LOBBY_BOT_CATALOG_MAX_BYTES);
    if (!raw) return 0;
    rawLen = serializeRaw(raw);
    id = (rawLen > 1) ? catalogIdOf(raw, rawLen) : 0;
    free(raw);
    return id;
}

/* Parse an uncompressed payload into LobbyBotPoolDef[] backed by heap
 * strings, install it, and free the temporaries. Returns installed
 * count, or -1 on malformed input. Only fully-built pools are kept in
 * defs[0..builtPools); any partially-built pool is freed before the
 * cleanup loop runs, so cleanup is uniform. */
static int parseInstallRaw(const unsigned char *raw, int len,
                           LobbyBotPoolLoadStats *stats) {
    int poolCount, pos = 0, installed = -1, p, k;
    LobbyBotPoolDef *defs = NULL;
    int builtPools = 0;

    if (len < 1) return -1;
    poolCount = raw[pos++];
    if (poolCount == 0 || poolCount > LOBBY_BOT_POOL_MAX_POOLS) return -1;

    defs = (LobbyBotPoolDef *)calloc((size_t)poolCount, sizeof(*defs));
    if (!defs) return -1;

    for (p = 0; p < poolCount; p++) {
        int labelLen, nameCount, n;
        char *label = NULL;
        char **names = NULL;

        if (pos + 1 > len) goto cleanup;
        labelLen = raw[pos++];
        if (labelLen > len - pos) goto cleanup;
        label = (char *)malloc((size_t)labelLen + 1);
        if (!label) goto cleanup;
        memcpy(label, raw + pos, (size_t)labelLen);
        label[labelLen] = '\0';
        pos += labelLen;

        if (pos + 2 > len) { free(label); goto cleanup; }
        nameCount = (int)getU16BE(raw + pos);
        pos += 2;
        if (nameCount > LOBBY_BOT_POOL_MAX_NAMES) { free(label); goto cleanup; }

        names = (char **)calloc((size_t)(nameCount > 0 ? nameCount : 1),
                                sizeof(char *));
        if (!names) { free(label); goto cleanup; }

        for (n = 0; n < nameCount; n++) {
            int nl;
            if (pos + 1 > len) goto pool_fail;
            nl = raw[pos++];
            if (nl > len - pos) goto pool_fail;
            names[n] = (char *)malloc((size_t)nl + 1);
            if (!names[n]) goto pool_fail;
            memcpy(names[n], raw + pos, (size_t)nl);
            names[n][nl] = '\0';
            pos += nl;
            continue;
        pool_fail:
            /* Free this pool's partial strings + arrays, then bail. */
            for (k = 0; k < n; k++) free(names[k]);
            free(names);
            free(label);
            goto cleanup;
        }

        defs[p].label = label;     /* install copies; freed in cleanup */
        defs[p].names = (const char *const *)names;
        defs[p].nameCount = nameCount;
        builtPools++;
    }

    installed = lobbyBotPoolsInstall(defs, builtPools, stats);

cleanup:
    for (p = 0; p < builtPools; p++) {
        free((void *)defs[p].label);
        for (k = 0; k < defs[p].nameCount; k++)
            free((void *)defs[p].names[k]);
        free((void *)defs[p].names);
    }
    free(defs);
    return installed;
}

int lobbyBotPoolsDeserializeInstall(const unsigned char *blob, int len,
                                    LobbyBotPoolLoadStats *stats) {
    unsigned long rawLen;
    unsigned char *raw;
    uLongf outLen;
    int result;

    if (!blob || len < 5) return -1;
    rawLen = getU32BE(blob);
    if (rawLen == 0 || rawLen > LOBBY_BOT_CATALOG_MAX_BYTES) return -1;

    raw = (unsigned char *)malloc(rawLen);
    if (!raw) return -1;
    outLen = (uLongf)rawLen;
    if (uncompress(raw, &outLen, blob + 4, (uLong)(len - 4)) != Z_OK ||
        outLen != rawLen) {
        free(raw);
        return -1;
    }

    result = parseInstallRaw(raw, (int)rawLen, stats);
    free(raw);
    return result;
}
