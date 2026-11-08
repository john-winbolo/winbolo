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
 * an unlimited one takes more bots than its slots, and an unlimited one
 * goes past its slots only when no other host has room. */
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
/* The round trip the lag smoothing takes each tick. A player's own Bot
 * Host (playerHost) counts its player's game ping, ownerPingMs, the value
 * the player list shows; with no player ping yet (ownerPingMs 0 or below)
 * it, like every listed Bot Host, counts its own measured hostPingMs. */
uint16_t botHostLagRaw(bool playerHost, int ownerPingMs, uint16_t hostPingMs);
/* The ping a Bot Host row shows, in whole ms (at least 1), or 0xFFFF
 * (BOT_HOST_LAG_UNKNOWN) when there is none. A player's own Bot Host shows
 * its player's game ping (ownerPingMs above 0), the same value and so the
 * same colour as the player list; else the smoothed lag (lagKnown,
 * lagMs). */
uint16_t botHostRowPing(bool playerHost, int ownerPingMs, bool lagKnown,
                        double lagMs);
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
 * Each host (the server's own process too) is probed once a second: its
 * bots' new thinks and new overruns in that second. The host's rate is
 * the sum of its overruns over the sum of its thinks in the last
 * windowSecs probes. The rate is known only when the host had bots in
 * each of those probes and they thought; a host with no bots has no rate.
 *
 * The overrun move (botHostOvrPickMove). A host whose rate is above pct
 * (the target) may give one bot to a better host, at most once each
 * stepSecs. Each extra bot on a host is taken to run at that host's rate
 * while the free CPU check passes. For a bot with m voluntary moves in
 * this game, a normal move needs all of these:
 *   a. the source rate is above pct;
 *   b. the target's rate is below pct, or the target has no bots (a
 *      trial: its rate is known 30 s later);
 *   c. the target's rate is at most the source rate x gain^(m + 1);
 *   d. the bot stayed at least staySecs x stayFactor^(m - 1) since its
 *      last voluntary move (no stay rule while m is 0);
 *   e. the target passes the free CPU check and has slot room.
 * With the source rate at or above emergencyPct, c and d do not apply;
 * the target must then be below pct, or have at most half the source
 * rate, or have no bots, and e still holds. Lag only breaks ties: the
 * lag gain and the lag ceiling of the spread do not stop this move. Of
 * the targets that pass, the lowest rate goes first (a trial after every
 * host with a rate), then the lowest lag, then more slots, then the first
 * in the table. After the move the source's limit becomes its bots less 1
 * for this game, so the spread does not take the bot back, and its probes
 * start again.
 *
 * Forced moves (a host leaves, times out or loses the bot, a fall-back,
 * changed brain files, fewer slots, a host on the server's computer) do
 * not count in m. m starts at 0 again in the lobby (BotHostOvrMoves).
 *
 * The free CPU history: each freeSampleSecs the host's last reading of its
 * machine's free CPU (all cores, 0..100, BOT_HOST_FREE_UNKNOWN when not
 * known) is kept with the host's bot count and whether its overrun rate
 * over those seconds was above pct (bad). freeHistorySecs of readings are
 * kept.
 *
 * The limit goes up by 1 (at most once each stepSecs, never above the
 * host's slots) only when all of these hold:
 *   a. the rate is not above pct over a full window since the last change;
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
 * (botHostOverrunMayTake), in the spread, at the start, in each
 * rebalance and for the overrun move: a host with no bots may take its
 * first one; after each new bot it takes no more until a reading counts
 * that bot (so the CPU of one bot is measured); then it takes one more
 * only when the one-bot cost is known and, with a bad reading in the
 * history, b and c above hold, or, with no bad reading, the predicted
 * free CPU is at least one more one-bot cost or b holds. A host with no
 * free CPU data takes bots by its slots only until its first step down.
 *
 * The limit is for one game: the server clears it, the probes and the
 * step down at a new game and a new round (botHostOverrunNewGame), and
 * all of the state when a host joins again (botHostOverrunReset). */
#define BOT_HOST_OVR_PCT_DEFAULT           1.0
#define BOT_HOST_OVR_EMERGENCY_PCT_DEFAULT 5.0
#define BOT_HOST_OVR_GAIN_DEFAULT          0.5
#define BOT_HOST_OVR_STAY_SECS_DEFAULT     30
#define BOT_HOST_OVR_STAY_FACTOR_DEFAULT   2.0
#define BOT_HOST_OVR_WINDOW_SECS_DEFAULT   30
#define BOT_HOST_OVR_STEP_SECS_DEFAULT     30
#define BOT_HOST_FREE_SAMPLE_SECS_DEFAULT  5
#define BOT_HOST_FREE_HISTORY_SECS_DEFAULT 300
/* In an emergency the target may have at most this part of the source
 * rate (when it is not below pct). */
#define BOT_HOST_OVR_EMERGENCY_SHARE 0.5
/* Two rates closer than this (in percent points) are the same rate when
 * the targets are put in order, so the lag decides between them. */
#define BOT_HOST_OVR_RATE_TIE_PCT    0.1
/* The most probes a window and the most readings a history hold. */
#define BOT_HOST_OVR_MAX_PROBES   600
#define BOT_HOST_FREE_MAX_READINGS 600
/* A free CPU reading that is not known. */
#define BOT_HOST_FREE_UNKNOWN     0xFFu

typedef struct {
    double pct;            /* the target rate in percent; 0: the check is off */
    double emergencyPct;   /* at or above it: the emergency rule; 0: none */
    double gain;           /* rule c: the target at most rate x gain^(m+1) */
    int    staySecs;       /* rule d: the stay after the first move */
    double stayFactor;     /* rule d: each later move multiplies the stay */
    int    windowSecs;     /* the probes a rate sums */
    int    stepSecs;       /* at most one move or raise per host in this */
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
    /* The window: each probe's thinks and overruns, and whether the host
     * had bots in it. */
    uint32_t probeThinks[BOT_HOST_OVR_MAX_PROBES];
    uint32_t probeOverruns[BOT_HOST_OVR_MAX_PROBES];
    bool     probeBots[BOT_HOST_OVR_MAX_PROBES];
    int      probeCount, probeNext;
    int      sinceChange;  /* probes since the last move, step down or up */
    int      sinceRefuse;  /* probes since the last refused raise */
    uint32_t accOverruns, accThinks;
    int      accProbes;
    BotHostFreeReading reading[BOT_HOST_FREE_MAX_READINGS];
    int      readingCount, readingNext;
    double   oneBotCost;   /* the last known CPU percent of one bot */
    bool     costKnown;
    /* The one-bot cost the free CPU check uses while no measured cost is
     * known: the host's full-budget estimate (botHostCpuMilliBudget, as a
     * percent). The caller sets it before each check; the probe and the
     * raise rule do not use it. */
    double   costFallback;
    bool     costFallbackKnown;
} BotHostOverrunState;

typedef struct {
    uint32_t overruns;     /* new overruns in this second */
    uint32_t thinks;       /* new thinks in this second */
    int      bots;         /* bots on the host now */
    int      slots;        /* the slots the host reports */
    uint8_t  botCpu;       /* its CTRL_BOT_HOST_CPU byte */
    uint8_t  freePct;      /* its last free CPU reading */
} BotHostOverrunProbe;

typedef enum {
    BOT_HOST_OVR_NONE = 0,
    BOT_HOST_OVR_OVER,     /* the rate is above pct and a move may come
                            * now: the caller looks for one
                            * (botHostOvrPickMove) */
    BOT_HOST_OVR_RAISE,    /* the limit goes up by 1 */
    BOT_HOST_OVR_REFUSE    /* a raise was looked at and refused */
} BotHostOverrunAction;

typedef struct {
    double ratePct;        /* this probe's (one second's) overrun rate */
    bool   windowKnown;    /* the window rate is known */
    double windowPct;      /* the window rate */
    int    probes;         /* probes in the window */
    int    limitBefore;    /* the limit before this probe, -1: none */
    int    newLimit;       /* the limit after this probe, -1: none */
    bool   sampled;        /* this probe took a free CPU reading */
    BotHostFreeReading sample;  /* that reading */
    bool   freeKnown;      /* a free CPU average of the window is known */
    double freeAvg;
    bool   costKnown;
    bool   costFromBudget; /* oneBotCost is the full-budget fallback */
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
/* The host's window rate in percent (*pct), when it is known. */
bool botHostOverrunRate(const BotHostOverrunState *st,
                        const BotHostOverrunCfg *cfg, double *pct);
/* A bot moved off the host by the overrun move; bots is its count before
 * the move. The limit becomes bots - 1 (never below 0) for this game, the
 * probes start again and the next move or raise waits stepSecs. Returns
 * the new limit. */
int  botHostOverrunMoved(BotHostOverrunState *st, int bots);
/* A move was found but not made (-bothost-nomove): the next one waits
 * stepSecs; the limit and the probes stay. */
void botHostOverrunHeld(BotHostOverrunState *st);

/* What the overrun move knows of one place in the host table. */
typedef struct {
    bool   rateKnown;
    double ratePct;
    int    bots;
    bool   mayTake;    /* the free CPU check passes and it has slot room */
    double lagMs;      /* below 0: not known */
    int    slots;
} BotHostOvrHost;

/* A bot the overrun move may move off its host. */
typedef struct {
    int    seat;
    int    moves;      /* m: its voluntary moves in this game */
    double staySecs;   /* since its last voluntary move */
} BotHostOvrBot;

typedef enum {
    BOT_HOST_OVR_RULE_NONE = 0,
    BOT_HOST_OVR_RULE_NORMAL,     /* rules a to e */
    BOT_HOST_OVR_RULE_EMERGENCY,  /* the source at or above emergencyPct */
    BOT_HOST_OVR_RULE_TRIAL       /* a normal move to a host with no bots */
} BotHostOvrRule;

typedef struct {
    int    seat;           /* -1: no move */
    int    to;             /* the target's place in the table, -1: none */
    int    moves;          /* the bot's m the rules used */
    double staySecs;
    double needStaySecs;   /* the stay rule d asked for (0: none) */
    double srcPct;
    bool   dstKnown;       /* the target has a rate (not a trial) */
    double dstPct;
    BotHostOvrRule rule;
    const char *why;       /* why no move, for the log */
} BotHostOvrMove;

/* "normal", "emergency", "trial" or "none". */
const char *botHostOvrRuleName(BotHostOvrRule rule);
/* The stay rule d asks of a bot with moves voluntary moves: 0 for 0,
 * else staySecs x stayFactor^(moves - 1). */
double botHostOvrStayNeed(const BotHostOverrunCfg *cfg, int moves);
/* The rule that lets a bot with moves voluntary moves and staySecs since
 * the last one go from a host at srcPct to dst, or BOT_HOST_OVR_RULE_NONE
 * with the reason in *why (may be NULL). */
BotHostOvrRule botHostOvrMoveRule(const BotHostOverrunCfg *cfg, double srcPct,
                                  const BotHostOvrHost *dst, int moves,
                                  double staySecs, const char **why);
/* The overrun move off host src: of its movable bots (fewest moves first,
 * then the longest stay, then the lowest seat) the first that has a
 * target that passes, to the best such target. lag gives the lag of each
 * host (NULL or off: lag does not count). False, with out->why, when no
 * bot may move. */
bool botHostOvrPickMove(const BotHostOverrunCfg *cfg,
                        const BotHostOvrHost *hosts, int n, int src,
                        const BotHostOvrBot *bots, int nBots,
                        const BotHostPlaceLag *lag, BotHostOvrMove *out);

/* Each seat's voluntary moves (m) and the tick of its last one. */
#define BOT_HOST_OVR_SEATS 32

typedef enum {
    BOT_HOST_MOVE_VOLUNTARY = 0,  /* the overrun move */
    BOT_HOST_MOVE_FORCED          /* a fall-back: no penalty, m stays */
} BotHostMoveKind;

typedef struct {
    int      moves[BOT_HOST_OVR_SEATS];
    uint32_t lastTick[BOT_HOST_OVR_SEATS];
    uint32_t startTick[BOT_HOST_OVR_SEATS]; /* when the seat was first seen */
    bool     seen[BOT_HOST_OVR_SEATS];
} BotHostOvrMoves;

/* The lobby (the game ended): every m is 0. */
void botHostOvrMovesReset(BotHostOvrMoves *mv);
/* Seat s at tick now: a seat with no bot starts again. */
void botHostOvrMovesSeat(BotHostOvrMoves *mv, int s, bool isBot,
                         uint32_t now);
/* Seat s moved at tick now. Only a voluntary move counts in m and starts
 * its stay again. */
void botHostOvrMovesNote(BotHostOvrMoves *mv, int s, BotHostMoveKind kind,
                         uint32_t now);
int  botHostOvrMovesCount(const BotHostOvrMoves *mv, int s);
/* The seconds since seat s's last voluntary move (since it was first seen
 * while it has none), at ticksPerSec ticks a second. */
double botHostOvrMovesStay(const BotHostOvrMoves *mv, int s, uint32_t now,
                           int ticksPerSec);

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
