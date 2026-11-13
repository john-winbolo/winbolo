/*
 * A Bot Host's slots: Max slots, the offer, and the same-computer stop
 * (bothost_optout.h).
 */
#include <stdio.h>
#include <string.h>

#include "bothost_optout.h"

int botHostMaxParse(const char *text) {
    long v = 0;
    const char *p = text;
    if (p == NULL || *p == '\0') return BOT_HOST_MAX_AUTO;
    while (*p >= '0' && *p <= '9') {
        if (v < 1000) v = v * 10 + (*p - '0');
        p++;
    }
    if (p == text || *p != '\0') return BOT_HOST_MAX_AUTO;
    return v > BOT_HOST_MAX_TOP ? BOT_HOST_MAX_TOP : (int)v;
}

void botHostMaxFormat(int max, char *out, size_t outSize) {
    if (out == NULL || outSize == 0) return;
    if (max < 0) {
        snprintf(out, outSize, "Auto");
    } else {
        snprintf(out, outSize, "%d", max > BOT_HOST_MAX_TOP ? BOT_HOST_MAX_TOP
                                                            : max);
    }
}

int botHostMaxCeiling(int max, int capacity) {
    int c = max >= 0 ? max : capacity;
    if (c < 0) c = 0;
    return c > BOT_HOST_MAX_TOP ? BOT_HOST_MAX_TOP : c;
}

int botHostSlotsMaxDefault(int slotsMax, int slots, int cpus, bool keel) {
    int v = slotsMax;
    if (v <= 0) v = keel ? slots : (cpus - 1 > slots ? cpus - 1 : slots);
    if (v > BOT_HOST_MAX_TOP) v = BOT_HOST_MAX_TOP;
    return v < 0 ? 0 : v;
}

int botHostMaxOffer(int ceiling, int start, bool hello) {
    if (ceiling < 0) ceiling = 0;
    if (hello) return ceiling;
    if (start < 0) start = 0;
    return start < ceiling ? start : ceiling;
}

void botHostRunJoin(BotHostRun *r) {
    if (r == NULL) return;
    memset(r, 0, sizeof(*r));
}

bool botHostRunSameServer(BotHostRun *r, uint8_t kind) {
    if (r == NULL || kind != BOT_HOST_RUN_SAME_SERVER_KIND) return false;
    if (r->sameServer) return false;
    r->sameServer = true;
    return true;
}

bool botHostRunStopped(const BotHostRun *r) {
    return r != NULL && r->sameServer;
}

int botHostRunSlots(const BotHostRun *r, int slots) {
    if (botHostRunStopped(r) || slots < 0) return 0;
    return slots;
}
