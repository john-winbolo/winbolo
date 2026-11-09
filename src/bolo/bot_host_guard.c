/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* The Bot Host guard rules (bot_host_guard.h). */

#include "bot_host_guard.h"

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>  /* SDL_strlcpy, SDL_strcmp */

/* ---- Init tokens ------------------------------------------------------- */

static bool guardName(const char *s) {
    size_t n = 0;
    if (s == NULL || *s == '\0') return false;
    for (; *s != '\0'; s++, n++) {
        char c = *s;
        if (n >= BOT_HOST_TOKEN_NAME_MAX) return false;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

/* One decimal integer lo..hi of at most 3 digits, the whole of [s, end). */
static bool guardInt(const char *s, const char *end, int lo, int hi) {
    int v = 0, n = 0;
    if (s >= end) return false;
    for (; s < end; s++, n++) {
        if (*s < '0' || *s > '9' || n >= 3) return false;
        v = v * 10 + (*s - '0');
    }
    return v >= lo && v <= hi;
}

/* minParts..maxParts integers lo..hi joined by '/'. */
static bool guardIntList(const char *s, int minParts, int maxParts, int lo,
                         int hi) {
    int parts = 0;
    for (;;) {
        const char *slash = strchr(s, '/');
        const char *end = slash != NULL ? slash : s + strlen(s);
        if (!guardInt(s, end, lo, hi)) return false;
        parts++;
        if (parts > maxParts) return false;
        if (slash == NULL) break;
        s = slash + 1;
    }
    return parts >= minParts;
}

/* 0 < x <= 10: digits with at most one '.', at most 8 characters. */
static bool guardRefuel(const char *s) {
    size_t n = strlen(s);
    int dots = 0;
    size_t i;
    double v;
    if (n == 0 || n > 8) return false;
    for (i = 0; i < n; i++) {
        if (s[i] == '.') {
            if (++dots > 1) return false;
        } else if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    if (s[0] == '.' || s[n - 1] == '.') return false;
    v = atof(s);
    return v > 0.0 && v <= 10.0;
}

static const char *const guardWords[] = {
    "survivor", "horde", "normal", "ammoless", "noammo", "suicider",
    "nosuicider", "noblitz", "blitzonly", "noclaimdead",
};

static bool guardIsWord(const char *s) {
    size_t i;
    for (i = 0; i < sizeof(guardWords) / sizeof(guardWords[0]); i++) {
        if (strcmp(s, guardWords[i]) == 0) return true;
    }
    return false;
}

/* tok with no spaces. */
static bool guardTokenBare(const char *tok) {
    const char *eq = strchr(tok, '=');
    char key[BOT_HOST_TOKEN_NAME_MAX + 1];
    const char *val;
    size_t kl;

    if (eq == NULL) return guardIsWord(tok);
    kl = (size_t)(eq - tok);
    if (kl == 0 || kl > BOT_HOST_TOKEN_NAME_MAX) return false;
    memcpy(key, tok, kl);
    key[kl] = '\0';
    val = eq + 1;
    if (strcmp(key, "mode") == 0 || strcmp(key, "difficulty") == 0 ||
        strcmp(key, "preset") == 0) {
        return guardName(val);
    }
    if (strcmp(key, "portfolio") == 0) return guardIntList(val, 3, 4, 0, 100);
    if (strcmp(key, "blitz") == 0) {
        return strcmp(val, "default") == 0 || guardIntList(val, 1, 2, 1, 16);
    }
    if (strcmp(key, "blitzsuiciders") == 0) {
        return guardInt(val, val + strlen(val), 0, 16);
    }
    if (strcmp(key, "refuel") == 0) return guardRefuel(val);
    if (strcmp(key, "peace") == 0) {
        return guardIntList(val, 1, 16, 0, 15);
    }
    return false;
}

bool botHostInitTokenAllowed(const char *tok) {
    char buf[160];
    size_t n = 0;
    if (tok == NULL) return false;
    for (; *tok != '\0'; tok++) {
        if (*tok == ' ' || *tok == '\t' || *tok == '\r' || *tok == '\n' ||
            *tok == '\f' || *tok == '\v') {
            continue;
        }
        if (*tok == ';' || *tok == ',') return false;
        if (n + 1 >= sizeof(buf)) return false;
        buf[n++] = *tok;
    }
    buf[n] = '\0';
    /* An empty token (two separators in a row) is nothing to the brain. */
    if (n == 0) return true;
    return guardTokenBare(buf);
}

bool botHostInitArgAllowed(const char *arg) {
    char tok[160];
    const char *p = arg;
    if (arg == NULL) return true;
    for (;;) {
        const char *end = p;
        size_t n;
        while (*end != '\0' && *end != ';' && *end != ',') end++;
        n = (size_t)(end - p);
        if (n >= sizeof(tok)) return false;
        memcpy(tok, p, n);
        tok[n] = '\0';
        if (!botHostInitTokenAllowed(tok)) return false;
        if (*end == '\0') break;
        p = end + 1;
    }
    return true;
}

bool botHostInitTableAllowed(const ScnTable *t) {
    int i;
    if (t == NULL) return true;
    for (i = 0; i < t->count && i < SCN_TABLE_MAX; i++) {
        const char *k = t->kv[i].key;
        const char *v = t->kv[i].value;
        char tok[SCN_TABLE_KEY_LEN + SCN_TABLE_VALUE_LEN + 2];
        if (k[0] == '\0') continue;
        if (guardIsWord(k)) {
            if (strcmp(v, "1") == 0 || v[0] == '\0' ||
                strcmp(v, "true") == 0 || strcmp(v, "0") == 0 ||
                strcmp(v, "false") == 0) {
                continue;   /* the bare word, or nothing */
            }
        } else if (strcmp(v, "false") == 0) {
            continue;       /* a key written false: nothing */
        }
        SDL_snprintf(tok, sizeof(tok), "%s=%s", k, v);
        if (!botHostInitArgAllowed(tok)) return false;
    }
    return true;
}

/* ---- Placement --------------------------------------------------------- */

bool botHostSeatMayRunOn(const BotHostSeatFit *f) {
    if (f == NULL) return false;
    if (f->playerHost && !f->culled && !f->fullMapOk) return false;
    if (f->officialOnly && !f->brainInPlace) return false;
    if (f->playerHost && f->officialOnly && !f->tokensAllowed) return false;
    return true;
}

bool botHostPackReqAllInstall(const uint8_t *bits, size_t bitsLen,
                              uint16_t fileCount, uint16_t fromInstall) {
    size_t i;
    if (bits == NULL || fileCount == 0) return false;
    for (i = 0; i < bitsLen; i++) {
        if (bits[i] != 0) return false;
    }
    return fromInstall == fileCount;
}

/* ---- Brain files from a server ----------------------------------------- */

bool botHostSyncTakesData(const BotHostSyncTake *s, int kind, uint32_t gen) {
    if (s == NULL) return false;
    if (s->officialOnly || s->installOnly || s->refused) return false;
    if (!s->haveManifest || s->ready || gen != s->gen) return false;
    if (kind == BOT_HOST_SYNC_DATA_PACK) return s->packMode;
    return kind == BOT_HOST_SYNC_DATA_CHUNK;
}

/* ---- Same-computer salts ----------------------------------------------- */

void botHostSaltLimitInit(BotHostSaltLimit *l) {
    if (l == NULL) return;
    l->answered = 0;
    l->lastMs = 0;
}

bool botHostSaltLimitTake(BotHostSaltLimit *l, uint32_t nowMs) {
    if (l == NULL) return false;
    if (l->answered >= BOT_HOST_SALT_MAX_PER_CONN) return false;
    if (l->answered > 0 && nowMs - l->lastMs < BOT_HOST_SALT_MIN_GAP_MS) {
        return false;
    }
    l->answered++;
    l->lastMs = nowMs;
    return true;
}

/* ---- Download resets -------------------------------------------------- */

void botHostResetLimitInit(BotHostResetLimit *l) {
    if (l == NULL) return;
    l->resets = 0;
}

bool botHostResetLimitTake(BotHostResetLimit *l) {
    if (l == NULL) return false;
    if (l->resets >= BOT_HOST_RESET_MAX_PER_LIST) return false;
    l->resets++;
    return true;
}

/* ---- Recording uploads ------------------------------------------------- */

void botHostRecAskedInit(BotHostRecAsked *a) {
    if (a != NULL) memset(a, 0, sizeof(*a));
}

void botHostRecAskedNote(BotHostRecAsked *a, const char *block,
                         const char *host, bool playerHost) {
    BotHostRecAskedEntry *e;
    if (a == NULL || block == NULL || host == NULL || block[0] == '\0') return;
    if (botHostRecAskedHas(a, block, host, playerHost)) return;
    if (a->count < BOT_HOST_REC_ASKED_MAX) {
        e = &a->e[a->count++];
    } else {
        e = &a->e[a->next];
        a->next = (a->next + 1) % BOT_HOST_REC_ASKED_MAX;
    }
    SDL_strlcpy(e->block, block, sizeof(e->block));
    SDL_strlcpy(e->host, host, sizeof(e->host));
    e->playerHost = playerHost;
}

bool botHostRecAskedHas(const BotHostRecAsked *a, const char *block,
                        const char *host, bool playerHost) {
    int i;
    if (a == NULL || block == NULL || host == NULL) return false;
    for (i = 0; i < a->count; i++) {
        const BotHostRecAskedEntry *e = &a->e[i];
        if (e->playerHost == playerHost && strcmp(e->block, block) == 0 &&
            strcmp(e->host, host) == 0) {
            return true;
        }
    }
    return false;
}
