/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Bot Hosts on one computer, the game bot limit and the slot override
 * (bot_host_machine.h). */

#include "bot_host_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>  /* SDL_strcasecmp, SDL_strlcpy */

#include "global.h"   /* MAX_TANKS */

bool botHostAddrIsServerMachine(uint32_t ip, const uint32_t *own, int n) {
    const uint8_t *b = (const uint8_t *)&ip;
    int i;
    if (b[0] == 127) return true;   /* network order: first byte first */
    if (own == NULL) return false;
    for (i = 0; i < n; i++) {
        if (own[i] != 0 && own[i] == ip) return true;
    }
    return false;
}

/* True when host in is on the server's computer: by its address, or by
 * its answer and the server's hash. *how says which. */
static bool machineOnServer(const BotHostMachineIn *in,
                            const uint8_t *serverHash, uint8_t *how) {
    if (in->addrServer) {
        *how = BOT_HOST_MACHINE_HOW_ADDRESS;
        return true;
    }
    if (serverHash != NULL && in->hashHave &&
        memcmp(in->hash, serverHash, MACHINE_HASH_LEN) == 0) {
        *how = BOT_HOST_MACHINE_HOW_TOKEN;
        return true;
    }
    return false;
}

void botHostMachineGroup(const BotHostMachineIn *in, int n,
                         const uint8_t *serverHash, BotHostMachineOut *out) {
    bool done[64];
    int i, j;
    if (in == NULL || out == NULL || n <= 0) return;
    if (n > (int)(sizeof(done) / sizeof(done[0]))) {
        n = (int)(sizeof(done) / sizeof(done[0]));
    }
    for (i = 0; i < n; i++) {
        out[i].kind = BOT_HOST_MACHINE_ALONE;
        out[i].other = -1;
        out[i].how = BOT_HOST_MACHINE_HOW_NONE;
        out[i].zero = false;
        done[i] = !in[i].present;
    }
    /* The server's computer. */
    for (i = 0; i < n; i++) {
        uint8_t how = BOT_HOST_MACHINE_HOW_NONE;
        if (done[i]) continue;
        if (!machineOnServer(&in[i], serverHash, &how)) continue;
        out[i].how = how;
        out[i].kind = BOT_HOST_MACHINE_SERVER;
        out[i].zero = in[i].playerHost;
        done[i] = true;
    }
    /* Hosts only, by equal hashes. */
    for (i = 0; i < n; i++) {
        int members[64], count = 0, lead = -1, k;
        if (done[i] || !in[i].hashHave) continue;
        for (j = i; j < n; j++) {
            if (done[j] || !in[j].hashHave) continue;
            if (memcmp(in[j].hash, in[i].hash, MACHINE_HASH_LEN) != 0) {
                continue;
            }
            members[count++] = j;
        }
        for (k = 0; k < count; k++) done[members[k]] = true;
        if (count < 2) continue;
        /* The first-joined host that offers slots leads; with none, the
         * first-joined one. */
        for (k = 0; k < count; k++) {
            int m = members[k];
            if (in[m].offered <= 0) continue;
            if (lead < 0 || in[m].joinSeq < in[lead].joinSeq) lead = m;
        }
        if (lead < 0) {
            for (k = 0; k < count; k++) {
                int m = members[k];
                if (lead < 0 || in[m].joinSeq < in[lead].joinSeq) lead = m;
            }
        }
        for (k = 0; k < count; k++) {
            int m = members[k];
            out[m].how = BOT_HOST_MACHINE_HOW_TOKEN;
            if (m == lead) continue;
            out[m].kind = BOT_HOST_MACHINE_HOST;
            out[m].other = (int8_t)lead;
            out[m].zero = true;
        }
    }
}

void botHostMachineStep(const BotHostMachineIn *in, int n,
                        const uint8_t *serverHash, bool waiting,
                        BotHostMachineOut *out, bool *known) {
    int i;
    if (in == NULL || out == NULL || known == NULL || n <= 0) return;
    if (!waiting) {
        botHostMachineGroup(in, n, serverHash, out);
        for (i = 0; i < n; i++) known[i] = in[i].present;
        return;
    }
    for (i = 0; i < n; i++) {
        uint8_t how = BOT_HOST_MACHINE_HOW_NONE;
        if (!in[i].present) {
            known[i] = false;
            continue;
        }
        if (known[i]) continue;
        /* The server's computer needs no other host's answer, so it is
         * decided now. A group of hosts only waits for the answers. */
        if (!machineOnServer(&in[i], serverHash, &how)) continue;
        out[i].kind = BOT_HOST_MACHINE_SERVER;
        out[i].other = -1;
        out[i].how = how;
        out[i].zero = in[i].playerHost;
        known[i] = true;
    }
}

int botHostMachineSlots(bool known, const BotHostMachineOut *m, int offered) {
    if (!known || m == NULL || m->zero) return 0;
    return offered > 0 ? offered : 0;
}

bool botHostMachineTellSameServer(bool known, const BotHostMachineOut *m,
                                  bool sent) {
    if (!known || m == NULL || sent) return false;
    return m->kind == BOT_HOST_MACHINE_SERVER && m->zero;
}

int botHostGameBotLimit(int serverCap, const int *counted, int n) {
    int total, i, hosts = 0;
    if (serverCap == -1) return -1;
    total = serverCap >= 0 ? serverCap : 0;
    for (i = 0; counted != NULL && i < n; i++) {
        if (counted[i] > 0) hosts += counted[i];
    }
    if (serverCap == -2 && hosts == 0) return -2;
    total += hosts;
    if (total > MAX_TANKS) total = MAX_TANKS;
    return total;
}

int botHostSlotsEffective(int reported, int override) {
    if (reported < 0) reported = 0;
    if (override < 0) return reported;
    return override < reported ? override : reported;
}

bool botHostSlotsOverrideValid(int reported, int value, int *out) {
    if (out == NULL) return false;
    if (value == 0xFF) {
        *out = -1;
        return true;
    }
    if (value < 0 || value > reported || value > MAX_TANKS) return false;
    *out = value;
    return true;
}

bool botHostSlotsArgParse(const char *arg, char *name, size_t nameSz,
                          int *n, char *err, size_t errSz) {
    const char *eq;
    const char *p;
    size_t len;
    long v;
    char *end;
    if (err != NULL && errSz > 0) err[0] = '\0';
    if (arg == NULL || name == NULL || nameSz == 0 || n == NULL) {
        if (err) snprintf(err, errSz, "no value");
        return false;
    }
    eq = strrchr(arg, '=');
    if (eq == NULL) {
        if (err) snprintf(err, errSz, "\"%s\" has no '=' (use name=N)", arg);
        return false;
    }
    len = (size_t)(eq - arg);
    if (len == 0) {
        if (err) snprintf(err, errSz, "\"%s\" has no name before '='", arg);
        return false;
    }
    if (len >= nameSz) {
        if (err) snprintf(err, errSz, "the name in \"%s\" is too long "
                          "(at most %u characters)", arg,
                          (unsigned)(nameSz - 1));
        return false;
    }
    p = eq + 1;
    if (*p < '0' || *p > '9') {
        if (err) snprintf(err, errSz, "\"%s\" has no number after '='", arg);
        return false;
    }
    v = strtol(p, &end, 10);
    if (*end != '\0') {
        if (err) snprintf(err, errSz, "\"%s\": \"%s\" is not a number", arg,
                          p);
        return false;
    }
    if (v < 0 || v > MAX_TANKS) {
        if (err) snprintf(err, errSz, "\"%s\": the number must be 0 to %d",
                          arg, MAX_TANKS);
        return false;
    }
    memcpy(name, arg, len);
    name[len] = '\0';
    *n = (int)v;
    return true;
}

bool botHostSlotFlagsAdd(BotHostSlotFlags *f, const char *arg, char *err,
                         size_t errSz) {
    char name[BOT_HOST_SLOT_NAME_LEN];
    int n, i;
    if (f == NULL) return false;
    if (!botHostSlotsArgParse(arg, name, sizeof(name), &n, err, errSz)) {
        return false;
    }
    for (i = 0; i < f->count; i++) {
        if (SDL_strcasecmp(f->flag[i].name, name) == 0) {
            if (err) snprintf(err, errSz, "\"%s\" is named by -bothost-slots "
                              "before; give each host once", name);
            return false;
        }
    }
    if (f->count >= BOT_HOST_SLOT_FLAGS_MAX) {
        if (err) snprintf(err, errSz, "more than %d -bothost-slots flags",
                          BOT_HOST_SLOT_FLAGS_MAX);
        return false;
    }
    SDL_strlcpy(f->flag[f->count].name, name, sizeof(f->flag[f->count].name));
    f->flag[f->count].n = n;
    f->count++;
    return true;
}

int botHostSlotFlagsFind(const BotHostSlotFlags *f, const char *hostName,
                         const char *playerName) {
    int i;
    if (f == NULL) return -1;
    for (i = 0; i < f->count; i++) {
        if ((hostName != NULL && hostName[0] != '\0' &&
             SDL_strcasecmp(f->flag[i].name, hostName) == 0) ||
            (playerName != NULL && playerName[0] != '\0' &&
             SDL_strcasecmp(f->flag[i].name, playerName) == 0)) {
            return f->flag[i].n;
        }
    }
    return -1;
}

void botHostSlotOverrideJoin(BotHostSlotOverride *o, const BotHostSlotFlags *f,
                             const char *hostName, const char *playerName) {
    int n;
    if (o == NULL) return;
    n = botHostSlotFlagsFind(f, hostName, playerName);
    o->override = n;
    o->locked = n >= 0;
}

bool botHostSlotSetAllowed(bool playerHost, int owner, int player) {
    return playerHost && owner >= 0 && player == owner;
}

int botHostDynamicForRun(int dynamic, bool seed, bool luaSeed,
                         bool noBudgetKill) {
    if (seed && luaSeed && noBudgetKill) return 0;
    return dynamic;
}

bool botHostParsePercent(const char *text, int *out) {
    int v = 0;
    const char *p = text;
    if (p == NULL || *p == '\0') return false;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 100) return false;
        p++;
    }
    if (*p != '\0') return false;
    if (out != NULL) *out = v;
    return true;
}

void botHostDynamicFlagsInit(BotHostDynamicFlags *f) {
    if (f == NULL) return;
    f->dynamic = f->serverMax = f->clientStart = -1;
    f->raiseReservePct = f->clientReservePct = f->raiseLagMaxMs = -1;
    f->raiseStep = f->raiseAfterCutSecs = f->carryLimit = -1;
    f->reserveRecentSecs = -1;
}

void botHostDynamicFlagsKeel(BotHostDynamicFlags *f) {
    if (f == NULL) return;
    f->dynamic = 0;
    f->serverMax = 0;          /* the ceiling is the start */
    f->clientStart = 0;        /* a player's computer: its slots */
    f->raiseReservePct = 0;
    f->clientReservePct = 0;
    f->raiseLagMaxMs = 0;
    f->raiseStep = 1;
    f->raiseAfterCutSecs = 0;
    f->carryLimit = 0;
    f->reserveRecentSecs = 0;
}

bool botHostSlotOverrideSet(BotHostSlotOverride *o, int reported, int value) {
    int v;
    if (o == NULL || o->locked) return false;
    if (!botHostSlotsOverrideValid(reported, value, &v)) return false;
    o->override = v;
    return true;
}

void botHostSlotOverrideNewLobby(BotHostSlotOverride *o) {
    if (o == NULL || o->locked) return;
    o->override = -1;
}
