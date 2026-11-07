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
} BotHostPlaceHost;

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

#endif /* BOT_HOST_LINK_H */
