/*
 * The lobby's one-game opt-out from running bots on this computer: "Don't
 * run bots on this computer for this game". The Bot Host inside each client
 * runs the server's bots by default; a player can turn it off for the game
 * they are in, and only for that game.
 *
 * The state is never saved and is in no config file. It belongs to one
 * game: the ClientSim and the CTRL_BOT_HOST_TICKET ticket it was turned on
 * with. The server sends a new ticket on each new connection (a rejoin, the
 * next round of a map rotation), so a new ticket, another ClientSim or no
 * game at all turns it off again.
 *
 * Plain C with no other part of the program, so the unit tests can drive
 * it. bothost_embed_service.c keeps the one copy the client uses.
 */
#ifndef BOTHOST_OPTOUT_H
#define BOTHOST_OPTOUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* == BOT_HOST_TICKET_LEN (control_event.h); bothost_embed_service.c
 * checks it. */
#define BOT_HOST_OPTOUT_TICKET_LEN 16

typedef struct {
    bool        on;
    const void *game;   /* the ClientSim it was turned on in */
    uint8_t     ticket[BOT_HOST_OPTOUT_TICKET_LEN];
} BotHostOptOut;

/* Off, for no game. */
void botHostOptOutInit(BotHostOptOut *o);

/* The player turns it on or off in game (a ClientSim) with that game's
 * ticket. On with no game or no ticket stays off. */
void botHostOptOutSet(BotHostOptOut *o, bool on, const void *game,
                      const uint8_t *ticket);

/* Whether it is on for game with ticket (NULL: no ticket now). When game
 * or ticket is not the one it was turned on with, it goes off first, so it
 * never carries over to another game. */
bool botHostOptOutCheck(BotHostOptOut *o, const void *game,
                        const uint8_t *ticket);

#ifdef __cplusplus
}
#endif

#endif /* BOTHOST_OPTOUT_H */
