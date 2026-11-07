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
