/*
 * A Bot Host's slots: the player's Max slots of the Bot Host inside a
 * client, what a host offers a server, and the stop of the built-in host
 * on the server's own computer.
 *
 * Max slots is the lobby's "Max slots" box on the player's own Bot Host
 * row, saved in the client config ("BOT HOST", "Max Slots"). 0 runs no
 * bot on this computer (it replaces the old one-game opt-out); 1 to 16 is
 * the most bots; "Auto" (BOT_HOST_MAX_AUTO) is the bot brain's own answer
 * for this computer. With dynamic slots the server starts the host at a
 * few bots and raises it up to Max while the computer has free CPU.
 *
 * Plain C with no other part of the program, so the unit tests can drive
 * it. The Bot Host program uses the offer rule too.
 */
#ifndef BOTHOST_OPTOUT_H
#define BOTHOST_OPTOUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/* Max slots: the brain's own answer. */
#define BOT_HOST_MAX_AUTO   (-1)
/* The most Max slots (== BOT_HOST_MAX_SLOTS, bot_host_link.h). */
#define BOT_HOST_MAX_TOP    16
/* The client config's place for Max slots. */
#define BOT_HOST_MAX_PREFS_SECTION "BOT HOST"
#define BOT_HOST_MAX_PREFS_KEY     "Max Slots"

/* Max slots from its saved text: "0" to "16" (a larger number is 16), and
 * BOT_HOST_MAX_AUTO for NULL, "", "Auto" or any other text. */
int  botHostMaxParse(const char *text);
/* Its saved text: "Auto" or the number. */
void botHostMaxFormat(int max, char *out, size_t outSize);
/* The most bots the host runs (its ceiling): max, or capacity (the
 * brain's answer) for BOT_HOST_MAX_AUTO; 0 to BOT_HOST_MAX_TOP. */
int  botHostMaxCeiling(int max, int capacity);
/* The slots a host offers a server: the ceiling to a server that sent the
 * dynamic slots hello (CTRL_BOT_HOST_LIMITS), else at most its start, so
 * an older server never counts the ceiling as the host's slots. */
int  botHostMaxOffer(int ceiling, int start, bool hello);
/* The Bot Host program's ceiling: slotsMax (--slots-max) when given (above
 * 0), else slots with --keel, else the larger of slots and cpus - 1; at
 * most BOT_HOST_MAX_TOP. */
int  botHostSlotsMaxDefault(int slotsMax, int slots, int cpus, bool keel);

/* The built-in Bot Host on the server's own computer. When the server
 * finds that this computer is its own (CTRL_BOT_HOST_SAME_SERVER), the
 * host stops for this join: it takes no brain files (and drops the ones on
 * the way), reads no CPU, offers 0 slots and takes no bot. It stays
 * connected, so the lobby keeps its row. The next join (a new start of the
 * host) clears it, and the server checks again. It does not change Max
 * slots. */
#define BOT_HOST_RUN_SAME_SERVER_KIND 1   /* == BOT_HOST_MACHINE_SERVER */

typedef struct {
    bool sameServer;   /* stopped for this join */
} BotHostRun;

/* A new join: the host runs as normal. */
void botHostRunJoin(BotHostRun *r);
/* The server's CTRL_BOT_HOST_SAME_SERVER with its kind. True when the host
 * stops now (only for the server's computer, and only the first time). */
bool botHostRunSameServer(BotHostRun *r, uint8_t kind);
/* The host is stopped for this join. */
bool botHostRunStopped(const BotHostRun *r);
/* The slots the host offers: 0 while it is stopped. */
int  botHostRunSlots(const BotHostRun *r, int slots);

#ifdef __cplusplus
}
#endif

#endif /* BOTHOST_OPTOUT_H */
