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

#include <stdio.h>
#include <string.h>

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

static const BotPool s_pools[] = {
    POOL_ENTRY("Classic AI",       s_namesClassic),
    POOL_ENTRY("Famous Painters",  s_namesPainters),
    POOL_ENTRY("Famous Musicians", s_namesMusicians),
    POOL_ENTRY("Martial Artists",  s_namesMartialArts),
    POOL_ENTRY("Famous Scientists", s_namesScientists),
    POOL_ENTRY("Philosophers",     s_namesPhilosophers),
    POOL_ENTRY("Generals & Tacticians", s_namesGenerals),
    POOL_ENTRY("Pilot Callsigns",  s_namesCallsigns),
    /* numeric pool wired up in lobbyBotPoolNameCount/Name when first read */
    { "Numbered Bots", NULL, 16 },
};

#define POOL_COUNT (int)(sizeof(s_pools) / sizeof(s_pools[0]))
#define NUMERIC_POOL_IDX (POOL_COUNT - 1)

/* ── Public API ─────────────────────────────────────────────────── */

int lobbyBotPoolCount(void) {
    return POOL_COUNT;
}

const char *lobbyBotPoolLabel(int poolIdx) {
    if (poolIdx < 0 || poolIdx >= POOL_COUNT) return "Unknown";
    return s_pools[poolIdx].label;
}

int lobbyBotPoolNameCount(int poolIdx) {
    if (poolIdx < 0 || poolIdx >= POOL_COUNT) return 0;
    return s_pools[poolIdx].count;
}

const char *lobbyBotPoolName(int poolIdx, int nameIdx) {
    const BotPool *p;
    if (poolIdx < 0 || poolIdx >= POOL_COUNT) return NULL;
    p = &s_pools[poolIdx];
    if (nameIdx < 0 || nameIdx >= p->count) return NULL;

    /* Numeric pool: lazy-init the storage on first read. */
    if (poolIdx == NUMERIC_POOL_IDX) {
        if (!s_namesNumericInit) initNumericPool();
        return s_namesNumeric[nameIdx];
    }

    return p->names[nameIdx];
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
    const char *cand;
    const char *label;

    if (!outBuf || outBufLen <= 0) return outBuf;
    outBuf[0] = '\0';

    if (poolIdx < 0 || poolIdx >= POOL_COUNT) {
        /* Unknown pool — fall back to classic. */
        poolIdx = 0;
    }

    n = lobbyBotPoolNameCount(poolIdx);

    /* Try every name in the pool first; first unused wins. */
    for (i = 0; i < n; i++) {
        cand = lobbyBotPoolName(poolIdx, i);
        if (!cand) continue;
        if (!isUsed(cand, used, usedCount)) {
            strncpy(outBuf, cand, (size_t)outBufLen - 1);
            outBuf[outBufLen - 1] = '\0';
            return outBuf;
        }
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
