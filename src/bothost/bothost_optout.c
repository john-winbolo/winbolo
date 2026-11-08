/*
 * The lobby's one-game opt-out from running bots on this computer
 * (bothost_optout.h).
 */
#include <string.h>

#include "bothost_optout.h"

void botHostOptOutInit(BotHostOptOut *o) {
    if (o == NULL) return;
    memset(o, 0, sizeof(*o));
}

void botHostOptOutSet(BotHostOptOut *o, bool on, const void *game,
                      const uint8_t *ticket) {
    if (o == NULL) return;
    botHostOptOutInit(o);
    if (!on || game == NULL || ticket == NULL) return;
    o->on = true;
    o->game = game;
    memcpy(o->ticket, ticket, sizeof(o->ticket));
}

bool botHostOptOutCheck(BotHostOptOut *o, const void *game,
                        const uint8_t *ticket) {
    if (o == NULL || !o->on) return false;
    if (game == NULL || game != o->game || ticket == NULL ||
        memcmp(ticket, o->ticket, sizeof(o->ticket)) != 0) {
        botHostOptOutInit(o);
        return false;
    }
    return true;
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
