/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Bot Host link
 *Filename:      bot_host_link.h
 *Purpose:
 *  How a Bot Host and a server find each other.
 *
 *  A Bot Host is given a list of servers (--servers) and
 *  listens on a UDP port of its own (--listen). A server is
 *  given a list of the Bot Hosts it may use (-bothosts).
 *
 *    ANNOUNCE  host -> server, to the server's game port, from
 *              the host's listen port: "I am open to take
 *              bots": name, slots, free slots, listen port and
 *              the brain versions in the host's cache.
 *    QUERY     server -> host, to the host's listen port: "do
 *              you have free slots?" The host answers with an
 *              ANNOUNCE.
 *    INVITE    server -> host, the answer to an ANNOUNCE from a
 *              listed host with free slots when the server
 *              wants bots run: "connect now". The host then
 *              opens its control connection (a join with
 *              JOIN_FLAG_BOT_HOST), so the connection always
 *              starts from the host's side.
 *
 *  Each of the three is one datagram: the packet header, then
 *  the body below. Every body opens with BOT_HOST_LINK_PROTO.
 *
 *  This file holds the bodies' codecs and the address lists
 *  both programs read ("host[:port], host[:port] ..." or a
 *  file of them), so the unit tests can check them.
 *********************************************************/

#ifndef BOT_HOST_LINK_H
#define BOT_HOST_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOT_HOST_LINK_PROTO          1
/* The port a Bot Host listens on unless --listen says another. */
#define BOT_HOST_DEFAULT_LISTEN_PORT 27510
/* The port a server listens on unless the list entry says another. */
#define BOT_HOST_DEFAULT_SERVER_PORT 27500
/* Brain versions one ANNOUNCE lists at most. */
#define BOT_HOST_LINK_MAX_BRAINS     12
#define BOT_HOST_LINK_NAME_LEN       32   /* = JOIN_BOT_HOST_NAME_LEN */
#define BOT_HOST_LINK_BRAIN_LEN      64   /* = BOT_HOST_BRAIN_NAME_LEN */
/* Entries one address list holds at most. */
#define BOT_HOST_LINK_MAX_ADDRS      16
#define BOT_HOST_LINK_HOST_LEN       128

typedef struct {
    char     name[BOT_HOST_LINK_BRAIN_LEN];
    uint64_t version;
} BotHostLinkBrain;

typedef struct {
    uint8_t  verMajor, verMinor, verRevision;
    uint16_t listenPort;
    uint8_t  slots;        /* the host's whole pool */
    uint8_t  freeSlots;    /* not taken by any server */
    char     name[BOT_HOST_LINK_NAME_LEN];
    uint8_t  brainCount;
    BotHostLinkBrain brains[BOT_HOST_LINK_MAX_BRAINS];
} BotHostAnnounce;

/* ANNOUNCE body, big-endian:
 *   [proto 1][major 1][minor 1][revision 1][listenPort 2][slots 1][free 1]
 *   [nameLen 1][name][brainCount 1]
 *   per brain: [nameLen 1][name][version 8]
 * Returns the body length, or -1 when it does not fit in cap. */
int  botHostAnnounceEncode(const BotHostAnnounce *a, uint8_t *buf, size_t cap);
/* False for a short, long or damaged body, a wrong proto, free above slots,
 * or a brain name that is empty or not printable. */
bool botHostAnnounceDecode(const uint8_t *buf, size_t len, BotHostAnnounce *out);

/* QUERY and INVITE bodies: [proto 1]. */
#define BOT_HOST_LINK_SHORT_LEN 1
bool botHostLinkShortValid(const uint8_t *buf, size_t len);

/* One list entry as written. port is 0 when the entry gave none. */
typedef struct {
    char     host[BOT_HOST_LINK_HOST_LEN];
    uint16_t port;
} BotHostAddrEntry;

typedef struct {
    int              count;
    BotHostAddrEntry e[BOT_HOST_LINK_MAX_ADDRS];
} BotHostAddrList;

/* Parse "host[:port]" entries split by commas, spaces, tabs or new lines.
 * '#' starts a comment to the end of the line. False, with a reason in err,
 * for an empty list, a bad port, a host that is too long, or too many
 * entries. */
bool botHostAddrListParse(const char *text, BotHostAddrList *out,
                          char *err, size_t errSz);

/* The same for a command-line value: "@path" or the path of a file that
 * exists reads that file; anything else is the list itself. */
bool botHostAddrListLoad(const char *arg, BotHostAddrList *out,
                         char *err, size_t errSz);

/* A resolved entry: IPv4 in network order, port in host order (0: any). */
typedef struct {
    uint32_t ip;
    uint16_t port;
} BotHostAllow;

/* The index of the first entry with this ip whose port is 0 or equal to
 * port, or -1. */
int  botHostAllowFind(const BotHostAllow *list, int count, uint32_t ip,
                      uint16_t port);

/* 16 hex digits to a version. False for anything else. */
bool botHostLinkParseVersion(const char *hex, uint64_t *out);

/* The brain versions in a Bot Host cache (<root>/<brain>/<version>/), up to
 * max of them. Returns how many were found. */
int  botHostLinkListCache(const char *root, BotHostLinkBrain *out, int max);

/* ---- Where a server places its bots ---------------------------------
 *
 * Each host, the server's own process among them, is filled in proportion
 * to the bots it can run, so all are about equally full. A host with few
 * slots (smallSlots or fewer: a weaker computer) counts bias slots fewer,
 * so it fills a little later than the others. A new bot goes to the host
 * with the lowest (bots + 1) / effective slots; ties go to the host with
 * more slots, then to the one that came first in the list (the server's
 * own process first, then the hosts in the order they joined). No host but
 * an unlimited one takes more bots than its slots. */
#define BOT_HOST_SMALL_SLOTS_DEFAULT 2
#define BOT_HOST_SMALL_BIAS_DEFAULT  0.25

typedef struct {
    int  slots;      /* bots it can run */
    int  bots;       /* bots on it now */
    bool open;       /* it may take a bot now */
    bool unlimited;  /* it takes bots past its slots (the server itself) */
    bool movable;    /* one of its bots may be moved now */
    double lagMs;    /* the server's smoothed round trip to it, in ms;
                      * below 0 when not known (the server itself: 0) */
} BotHostPlaceHost;

/* Lag in the placement (botHostPlacePickLag, botHostPlaceRebalanceLag).
 *
 * refMs: a host's score is multiplied by (1 + lagMs / refMs), so a host
 * refMs away fills at half the rate of one with no lag. A host whose lag
 * is not known counts refMs. 0 (or below) turns all lag rules off, and
 * the placement is then exactly the one without lag.
 *
 * maxMs: a host with more lag than this takes no new bot while a host
 * with at most this lag has room (bots below its slots). 0: no ceiling.
 *
 * gain: a bot moves only when its score at the new place is at least this
 * part lower than its score now (0.33: 33% lower), so small changes in
 * lag do not move bots back and forth. */
#define BOT_HOST_LAG_REF_MS_DEFAULT 100
#define BOT_HOST_LAG_MAX_MS_DEFAULT 250
#define BOT_HOST_LAG_GAIN_DEFAULT   0.33

typedef struct {
    double refMs;
    double maxMs;
    double gain;
} BotHostPlaceLag;

/* slots, less bias when slots is smallSlots or fewer; never under 0.05. */
double botHostPlaceEffSlots(int slots, int smallSlots, double bias);
/* The host a new bot goes to, or -1 when none may take it. */
int  botHostPlacePick(const BotHostPlaceHost *hosts, int n, int smallSlots,
                      double bias);
/* One move toward the proportional spread: a bot of *from (a movable host)
 * to *to, made only when the bot would be strictly less crowded there than
 * it is now. The host most crowded now gives first. False when no move
 * helps. */
bool botHostPlaceRebalance(const BotHostPlaceHost *hosts, int n,
                           int smallSlots, double bias, int *from, int *to);
/* Ticks over which a host's lag is smoothed: about 10 s at 50 ticks a
 * second. */
#define BOT_HOST_LAG_SMOOTH_TICKS 500
/* One tick of the lag smoothing: *lagMs moves 1 / smoothTicks of the way
 * to rawMs; the first sample (*known false) is taken as it is. */
void botHostLagSmooth(double *lagMs, bool *known, double rawMs,
                      int smoothTicks);
/* The two above with the lag rules (BotHostPlaceLag; NULL: no lag). The
 * score of a host is (bots + 1) / effective slots x (1 + lagMs / refMs). */
int  botHostPlacePickLag(const BotHostPlaceHost *hosts, int n, int smallSlots,
                         double bias, const BotHostPlaceLag *lag);
bool botHostPlaceRebalanceLag(const BotHostPlaceHost *hosts, int n,
                              int smallSlots, double bias,
                              const BotHostPlaceLag *lag, int *from, int *to);

/* The best host for one more bot other than host skip, by the same rules
 * as botHostPlacePickLag, or -1 when no other host may take it. */
int  botHostPlacePickOther(const BotHostPlaceHost *hosts, int n,
                           int smallSlots, double bias,
                           const BotHostPlaceLag *lag, int skip);

/* ---- Bot overruns on a host ------------------------------------------
 *
 * Each host (the server's own process too) is probed once a second. One
 * probe is the host's overrun rate in that second: new overruns / new
 * thinks over all its bots. A probe is bad when the rate is above pct; a
 * probe with no thinks is not bad. When at least badShare percent of the
 * probes in the last windowSecs are bad, one bot moves off the host and the
 * host's limit becomes its bot count less 1 (never below 0). That happens
 * at most once each stepSecs, and the probes are cleared after it, so the
 * next step needs a full new window.
 *
 * The free CPU history: each freeSampleSecs the host's last reading of its
 * machine's free CPU (all cores, 0..100, BOT_HOST_FREE_UNKNOWN when not
 * known) is kept with the host's bot count and whether its overrun rate
 * over those seconds was above pct (bad). freeHistorySecs of readings are
 * kept.
 *
 * The limit goes up by 1 (at most once each stepSecs, never above the
 * host's slots) only when all of these hold:
 *   a. the trigger is not met over a full window since the last change;
 *   b. one reading in the history ran bots with no overruns above pct and
 *      had at most the predicted free CPU: the average free CPU of the
 *      last windowSecs less the CPU of one bot (the host's bot CPU byte /
 *      its bots; the last known value while it has no bots);
 *   c. the predicted free CPU is above the highest free CPU of any bad
 *      reading in the history (true when there is none).
 * With no free CPU data or no one-bot cost the limit stays where it is
 * until the game ends.
 *
 * The same free CPU evidence decides each new bot for a host
 * (botHostOverrunMayTake), in the spread, at the start and in each
 * rebalance: a host with no bots may take its first one; after each new
 * bot it takes no more until a reading counts that bot (so the CPU of one
 * bot is measured); then it takes one more only when the one-bot cost is
 * known and, with a bad reading in the history, b and c above hold, or,
 * with no bad reading, the predicted free CPU is at least one more
 * one-bot cost or b holds. A host with no free CPU data takes bots by its
 * slots only until its first step down.
 *
 * The limit is for one game: the server clears it, the probes and the
 * step down at a new game and a new round (botHostOverrunNewGame), and
 * all of the state when a host joins again (botHostOverrunReset). */
#define BOT_HOST_OVR_PCT_DEFAULT           1.0
#define BOT_HOST_OVR_BAD_SHARE_DEFAULT     80
#define BOT_HOST_OVR_WINDOW_SECS_DEFAULT   30
#define BOT_HOST_OVR_STEP_SECS_DEFAULT     30
#define BOT_HOST_FREE_SAMPLE_SECS_DEFAULT  5
#define BOT_HOST_FREE_HISTORY_SECS_DEFAULT 300
/* The most probes a window and the most readings a history hold. */
#define BOT_HOST_OVR_MAX_PROBES   600
#define BOT_HOST_FREE_MAX_READINGS 600
/* A free CPU reading that is not known. */
#define BOT_HOST_FREE_UNKNOWN     0xFFu

typedef struct {
    double pct;            /* a probe is bad above this percent; 0: off */
    int    badShare;       /* percent of a window's probes that trigger */
    int    windowSecs;
    int    stepSecs;
    int    freeSampleSecs;
    int    freeHistorySecs;
} BotHostOverrunCfg;

typedef struct {
    uint8_t freePct;       /* 0..100 or BOT_HOST_FREE_UNKNOWN */
    uint8_t bots;          /* the host's bots at the reading */
    uint8_t botCpu;        /* its CTRL_BOT_HOST_CPU byte at the reading */
    bool    ran;           /* its bots thought in those seconds */
    bool    bad;           /* the overrun rate was above pct */
} BotHostFreeReading;

typedef struct {
    int      limit;        /* the lowered limit, -1: none */
    bool     cut;          /* a step down was made this game */
    uint8_t  probe[BOT_HOST_OVR_MAX_PROBES];   /* 1: bad */
    int      probeCount, probeNext;
    int      sinceChange;  /* probes since the last step down or up */
    int      sinceRefuse;  /* probes since the last refused raise */
    uint32_t accOverruns, accThinks;
    int      accProbes;
    BotHostFreeReading reading[BOT_HOST_FREE_MAX_READINGS];
    int      readingCount, readingNext;
    double   oneBotCost;   /* the last known CPU percent of one bot */
    bool     costKnown;
} BotHostOverrunState;

typedef struct {
    uint32_t overruns;     /* new overruns in this second */
    uint32_t thinks;       /* new thinks in this second */
    int      bots;         /* bots on the host now */
    int      slots;        /* the slots the host reports */
    uint8_t  botCpu;       /* its CTRL_BOT_HOST_CPU byte */
    uint8_t  freePct;      /* its last free CPU reading */
    bool     canMove;      /* a bot may move off it now to another place */
} BotHostOverrunProbe;

typedef enum {
    BOT_HOST_OVR_NONE = 0,
    BOT_HOST_OVR_WAIT,     /* the trigger is met but no bot may move now */
    BOT_HOST_OVR_LOWER,    /* move one bot off; the limit is lowered */
    BOT_HOST_OVR_RAISE,    /* the limit goes up by 1 */
    BOT_HOST_OVR_REFUSE    /* a raise was looked at and refused */
} BotHostOverrunAction;

typedef struct {
    int    badProbes;      /* bad probes in the window */
    int    probes;         /* probes in the window */
    double ratePct;        /* this probe's overrun rate */
    int    limitBefore;    /* the limit before this probe, -1: none */
    int    newLimit;       /* the limit after this probe, -1: none */
    bool   sampled;        /* this probe took a free CPU reading */
    BotHostFreeReading sample;  /* that reading */
    bool   freeKnown;      /* a free CPU average of the window is known */
    double freeAvg;
    bool   costKnown;
    double oneBotCost;
    double predicted;      /* freeAvg - oneBotCost */
    int    lowestClean;    /* lowest free of a clean reading with bots, -1 */
    int    highestBad;     /* highest free of a bad reading, -1 */
    const char *why;       /* the reason, for the log */
} BotHostOverrunReport;

void botHostOverrunCfgDefaults(BotHostOverrunCfg *cfg);
/* Clears the state: no limit, no probes, no history. */
void botHostOverrunReset(BotHostOverrunState *st);
/* A new game or round: no limit, no probes, no step down; the free CPU
 * history and the one-bot cost stay. */
void botHostOverrunNewGame(BotHostOverrunState *st);
/* The host may take one more bot now by the free CPU evidence (it has
 * bots now; latestFree is its last free CPU reading). Slots and the limit
 * are checked by the placement, not here. rep (may be NULL) gets the
 * numbers and the reason. */
bool botHostOverrunMayTake(const BotHostOverrunState *st,
                           const BotHostOverrunCfg *cfg, int bots,
                           uint8_t latestFree, BotHostOverrunReport *rep);
/* The slots placement counts for a host that reports slots. */
int  botHostOverrunLimit(const BotHostOverrunState *st, int slots);
/* One probe (one second). rep may be NULL. */
BotHostOverrunAction botHostOverrunProbe(BotHostOverrunState *st,
                                         const BotHostOverrunCfg *cfg,
                                         const BotHostOverrunProbe *in,
                                         BotHostOverrunReport *rep);

/* The seats a host gives back at once when it reports newSlots
 * (CTRL_BOT_HOST_SLOTS): every seat on host h when newSlots is 0, as when
 * the host leaves, and none otherwise (a host with fewer slots than bots
 * gives them back one at a time through the rebalance). A player who
 * turns off "run bots on this computer" for a game sends 0. seatHost[s]
 * is the host that runs seat s, below 0 for a seat in-process; n seats,
 * at most 32. Bit s of the answer is seat s. */
uint32_t botHostSeatsToGiveBack(const int *seatHost, int n, int h,
                                int newSlots);

/* ---- No bot moves while a game runs (-bothost-nomove) ------------------ */

/* With the option on, each bot stays where it was placed for the whole
 * game. In the lobby the placement works as without it. While the game
 * runs, a bot is placed once: a bot there at the game start, or one added
 * during the game, may go from the server to a host only in its first
 * placeTicks ticks, and only when it was not on a host before in this game.
 * No bot moves from a host to another place (no spread, no lag move, no
 * overrun move). A fall-back (the host leaves, times out, loses the bot's
 * connection, changes its brain files or offers fewer slots) still sends
 * the bot to the server, and the bot then stays there. */
#define BOT_HOST_NOMOVE_SEATS 32

typedef struct {
    bool     on;
    bool     seen[BOT_HOST_NOMOVE_SEATS];      /* a bot in this game */
    bool     wasOnHost[BOT_HOST_NOMOVE_SEATS]; /* it was on a host */
    uint32_t seenTick[BOT_HOST_NOMOVE_SEATS];  /* when it was first seen */
} BotHostNoMove;

void botHostNoMoveInit(BotHostNoMove *nm, bool on);
/* A new game or round (the lobby or the game start): every seat starts
 * again. */
void botHostNoMoveNewGame(BotHostNoMove *nm);
/* Seat s at tick now: isBot, and onHost when a host has it (bound or
 * thinking there). Call it for each seat each placement step. */
void botHostNoMoveSeat(BotHostNoMove *nm, int s, bool isBot, bool onHost,
                       uint32_t now);
/* The server may offer seat s to a host now. Always true with the option
 * off or outside a running game. */
bool botHostNoMoveMayPlace(const BotHostNoMove *nm, int s, bool running,
                           uint32_t now, uint32_t placeTicks);
/* A bot may move from where it is now (the spread, the lag rule and the
 * overrun check). False only with the option on while the game runs. */
bool botHostNoMoveMayMove(const BotHostNoMove *nm, bool running);
/* What the overrun check records for a LOWER step: BOTHOST_DECISION_MOVE
 * (2) when the bot may move, BOTHOST_DECISION_WOULD_MOVE (7) when it may
 * not. */
int  botHostNoMoveOverrunDecision(const BotHostNoMove *nm, bool running);
/* With the option on while the game runs, a host with more bots than the
 * slots it offers gives one back to the server (a fall-back, not a move). */
bool botHostNoMoveGiveBack(const BotHostNoMove *nm, bool running, int bots,
                           int slots);

#endif /* BOT_HOST_LINK_H */
