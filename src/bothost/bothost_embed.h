/*
 * The Bot Host inside a WinBolo client.
 *
 * Each player's client (the game and WinBoloHeadless) runs a Bot Host of
 * its own while it is in a game on a dedicated server, so the server can
 * move its bots' brains onto the players' computers. The host is the
 * WinBoloBotHost program's code (bothost_main.c, built a second time by
 * bothost_embed.c), run on a thread of its own inside the client. It serves
 * the one server the player is on, opens no listen port, and joins with the
 * ticket the server sent the player (CTRL_BOT_HOST_TICKET) in place of a key.
 * The server takes it only while the player is connected and drops it, with
 * every bot on it going back to the server, when the player leaves.
 *
 * botHostEmbedService (bothost_embed_service.c) is what the clients call
 * once a frame: it starts the host when a ticket comes and stops it when
 * the player leaves.
 */
#ifndef BOTHOST_EMBED_H
#define BOTHOST_EMBED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

typedef struct {
    const char    *serverHost;   /* the server's address, as the player joined */
    uint16_t       serverPort;
    uint8_t        player;       /* the player's number */
    const uint8_t *ticket;       /* BOT_HOST_TICKET_LEN bytes */
    const char    *brain;        /* the server's bot brain, for its capacity */
    const char    *name;         /* the host's name: the player's name */
    const char    *cacheDir;     /* where the server's brain files are kept */
    int            slots;        /* > 0: offer this many; 0: the capacity */
    double         recordMinFreeGb; /* 0: the default (50) */
} BotHostEmbedConfig;

typedef struct {
    bool running;    /* the host's thread runs */
    bool connected;  /* the server took the host */
    bool refused;    /* the server refused it */
    int  slots;      /* the bots it offers to run */
    int  bots;       /* the bots it runs now */
    bool sameServer; /* the server is on this computer: the host is
                      * stopped for this join (CTRL_BOT_HOST_SAME_SERVER) */
    bool consentAsking; /* the server wants to send its own bot code and
                         * waits for the player's answer
                         * (botHostEmbedConsentAnswer) */
} BotHostEmbedStatus;

/* The player's answer, for one join, to a server that wants to send its
 * own bot code (brain files that differ from the install). Asked only by a
 * client built without WINBOLO_BOTHOST_OFFICIAL_ONLY. Until the player
 * says yes, nothing is downloaded, no brain code runs and the host offers
 * 0 slots. */
#define BOT_HOST_EMBED_CONSENT_NONE   0
#define BOT_HOST_EMBED_CONSENT_ASKING 1
#define BOT_HOST_EMBED_CONSENT_YES    2
#define BOT_HOST_EMBED_CONSENT_NO     3

/* Start the host (a running one stops first). Main thread only. */
bool botHostEmbedStart(const BotHostEmbedConfig *cfg);
/* Stop the host and wait for its thread. tellServer: each bot connection
 * and the host itself send their quit, so the server takes the bots back at
 * once; false only for a program that ends next (a crash test). */
void botHostEmbedStop(bool tellServer);
bool botHostEmbedRunning(void);
/* Remove the brain session folders under cacheDir that an earlier run left
 * (marked, and no running client holds their lock). */
void botHostEmbedSweepSessions(const char *cacheDir);
void botHostEmbedGetStatus(BotHostEmbedStatus *out);
/* on: the host offers the server 0 slots (CTRL_BOT_HOST_SLOTS), so the
 * server takes its bots back, and it takes no new seat; it stays connected.
 * off: it offers its slots again. It holds across a start and a stop; the
 * clients' side (botHostEmbedService) sets it each frame from the lobby's
 * one-game opt-out. Main thread only. */
void botHostEmbedSetWithhold(bool on);
/* The player's answer while BotHostEmbedStatus.consentAsking: yes, the
 * host takes the server's code for this join; no, it offers 0 slots for
 * this join. Main thread. */
void botHostEmbedConsentAnswer(bool yes);
/* on (-bothost-record): the host may record its bots' brains for a
 * -brain-debug server. Off by default. Read when the host starts. */
void botHostEmbedSetRecord(bool on);
/* True when this client was built with WINBOLO_BOTHOST_OFFICIAL_ONLY: its
 * host runs only the brains of its own install. */
bool botHostEmbedOfficialOnly(void);
/* The slots offered before the brain's own answer is known. */
int botHostEmbedDefaultSlots(void);

/* ---- The host on a thread of this program (bothost_main.c) ------------
 *
 * The functions above that start, stop, read or tell the host choose
 * between a child process (WinBoloBotHost --embed-child, see
 * src/bothost_child/bothost_child.h) and the host on a thread of this
 * program. These are the thread's. The child runs them too, in its own
 * process. */
bool botHostInprocStart(const BotHostEmbedConfig *cfg);
void botHostInprocStop(bool tellServer);
bool botHostInprocRunning(void);
void botHostInprocGetStatus(BotHostEmbedStatus *out);
void botHostInprocSetWithhold(bool on);
void botHostInprocConsentAnswer(bool yes);
void botHostInprocSetRecord(bool on);
/* on: run only the install's brains, as a client built with
 * WINBOLO_BOTHOST_OFFICIAL_ONLY does (the child's --official-only). It can
 * only add the rule. Before the host starts. */
void botHostInprocSetOfficialOnly(bool on);

/* ---- The clients' side (bothost_embed_service.c) ---------------------- */

typedef struct {
    bool        disabled;    /* never host (a test option) */
    int         slots;       /* > 0: a fixed slot count (a test option) */
    const char *cacheDir;    /* NULL: the default under the prefs folder */
    /* Run the host on a thread of this program, not as a child process
     * (WinBoloBotHost --embed-child). Off by default: the child runs, and
     * the thread is used only when the child cannot start. */
    bool        inProcess;
    /* The child's limits (bothost_child.h): < 0 the default, 0 off. */
    int         childMemMb;
    int         childCpuPct; /* Windows only */
    /* Run the child with no file sandbox and with its own network (the
     * config's "Child File Sandbox" = No, --bothost-no-sandbox). Off by
     * default: in the sandbox, and with no sandbox no bots. */
    bool        childNoSandbox;
} BotHostEmbedSettings;

/* Settings with every default: hosting on, as a child, default limits. */
void botHostEmbedSettingsDefaults(BotHostEmbedSettings *settings);
void botHostEmbedSetSettings(const BotHostEmbedSettings *settings);
/* The game's config file keys (section "BOT HOST"):
 *   "Run As Child Process"     Yes (default) / No
 *   "Child Memory Limit MiB"   4096 (default); 0 off
 *   "Child CPU Limit Percent"  75 (default), a part of the whole machine,
 *                              Windows only; 0 or 100 off
 *   "Child File Sandbox"       Yes (default) / No: the child's OS file
 *                              sandbox and no network (bothost_child.h)
 * Read once, after the prefs file is open. */
void botHostEmbedLoadPrefs(void);

/* Once a frame on the main thread, with the player's ClientSim of a
 * network game, or NULL when there is none (the menu, a local game). It
 * starts the host when the server's ticket comes and stops it when the
 * player leaves the game, or when a new ticket replaces the old one. */
void botHostEmbedService(struct ClientSim *cs);

/* Stop at once: the player quits the game. */
void botHostEmbedLeave(const char *why);

/* The lobby's one-game opt-out: "Don't run bots on this computer for this
 * game". It is never saved. It goes off again when the player is in
 * another game: a new connection (a new ticket), another ClientSim, or no
 * game at all. botHostEmbedService sends it to the host each frame. */

/* True while the server lets this computer run bots in the game of cs
 * (its ticket came, hosting is not off, the server did not refuse the
 * host, and the server is not on this computer): the lobby shows the
 * opt-out only then. */
bool botHostEmbedOffered(struct ClientSim *cs);
/* Turn the opt-out on or off for the game of cs. */
void botHostEmbedSetOptOut(struct ClientSim *cs, bool on);
/* The opt-out for the game of cs; false for any other game. */
bool botHostEmbedOptOut(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif /* BOTHOST_EMBED_H */
