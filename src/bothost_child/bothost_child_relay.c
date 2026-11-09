/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The game's side of the relay (bothost_child.h): the sandboxed child has
 * no network, so the game sends and receives its datagrams for it.
 *
 * Two threads:
 *   - the reader takes the child's frames. OPEN makes a real UDP socket
 *     for one of the child's sockets, SEND sends a datagram from it, but
 *     only to the server the game knows (another address is refused and
 *     counted, with one log line), CLOSE marks the socket to close;
 *   - the poller waits on the open sockets (5 ms at a time), passes on
 *     each datagram from the server as a RECV frame and drops the rest,
 *     and closes the marked sockets. Only it closes sockets, so no socket
 *     closes while it waits on it.
 * The reader ends when the child's end of the pipe closes (the child
 * ended or was killed); the poller ends when told to, or when a write to
 * the child fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bothost_child.h"
#include "platform_net.h"

#ifdef _WIN32
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

enum { SLOT_FREE = 0, SLOT_OPEN, SLOT_CLOSING };

typedef struct {
    int           state;
    bolo_socket_t sock;
} RelaySlot;

struct BotHostRelay {
    BotHostChildProc *proc;
    uint32_t          serverIp;   /* network order */
    uint16_t          serverPort; /* network order */
    SDL_Mutex        *lock;
    RelaySlot         slots[BOT_HOST_RELAY_MAX_SOCKS];
    SDL_Thread       *reader;
    SDL_Thread       *poller;
    SDL_AtomicInt     stop;
    SDL_AtomicInt     toServer;
    SDL_AtomicInt     fromServer;
    SDL_AtomicInt     refused;
    bool              refusedSaid;
};

static bolo_socket_t relayOpenSocket(void) {
    struct sockaddr_in a;
    bolo_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    u_long nb = 1;
#endif
    if (s == BOLO_INVALID_SOCKET) return s;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        closesocket(s);
        return BOLO_INVALID_SOCKET;
    }
#ifdef _WIN32
    ioctlsocket(s, FIONBIO, &nb);
    {
        /* An ICMP "port unreachable" must not end the receives. */
        BOOL off = FALSE;
        DWORD got = 0;
        (void)WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0,
                       &got, NULL, NULL);
    }
#else
    (void)fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
    return s;
}

static void relayRefused(BotHostRelay *r, const char *what) {
    SDL_AddAtomicInt(&r->refused, 1);
    if (!r->refusedSaid) {
        r->refusedSaid = true;
        fprintf(stderr, "[bothost] the relay refused the child's %s (not the "
                        "game's server); it says this once\n", what);
    }
}

static int SDLCALL relayReaderFn(void *arg) {
    BotHostRelay *r = (BotHostRelay *)arg;
    uint8_t hdr[BOT_HOST_RELAY_HDR];
    static uint8_t body[BOT_HOST_RELAY_ADDR + BOT_HOST_RELAY_MAX_DATAGRAM];
    for (;;) {
        uint8_t type, id;
        uint16_t len;
        if (!botHostChildRelayRead(r->proc, hdr, sizeof(hdr))) break;
        if (!botHostRelayParseHeader(hdr, &type, &id, &len)) {
            fprintf(stderr, "[bothost] the relay got a bad frame from the "
                            "child: it stops\n");
            break;
        }
        if (len > 0 && !botHostChildRelayRead(r->proc, body, len)) break;
        if (type == BOT_HOST_RELAY_OPEN) {
            int waited = 0;
            bolo_socket_t s;
            /* The poller may still close the slot's old socket. */
            for (;;) {
                int st;
                SDL_LockMutex(r->lock);
                st = r->slots[id].state;
                SDL_UnlockMutex(r->lock);
                if (st != SLOT_CLOSING || waited >= 100) break;
                SDL_Delay(1);
                waited++;
            }
            s = relayOpenSocket();
            SDL_LockMutex(r->lock);
            if (r->slots[id].state != SLOT_FREE) {
                /* Opened twice, or still closing: the old one goes. */
                if (r->slots[id].sock != BOLO_INVALID_SOCKET) {
                    closesocket(r->slots[id].sock);
                }
            }
            r->slots[id].sock = s;
            r->slots[id].state = s != BOLO_INVALID_SOCKET ? SLOT_OPEN
                                                           : SLOT_FREE;
            SDL_UnlockMutex(r->lock);
        } else if (type == BOT_HOST_RELAY_CLOSE) {
            SDL_LockMutex(r->lock);
            if (r->slots[id].state == SLOT_OPEN) {
                r->slots[id].state = SLOT_CLOSING;
            }
            SDL_UnlockMutex(r->lock);
        } else if (type == BOT_HOST_RELAY_SEND) {
            uint32_t ip;
            uint16_t port;
            const uint8_t *data;
            size_t n;
            bolo_socket_t s = BOLO_INVALID_SOCKET;
            if (!botHostRelayParseBody(body, len, &ip, &port, &data, &n)) {
                continue;
            }
            if (ip != r->serverIp || port != r->serverPort) {
                relayRefused(r, "datagram to another address");
                continue;
            }
            SDL_LockMutex(r->lock);
            if (r->slots[id].state == SLOT_OPEN) s = r->slots[id].sock;
            if (s != BOLO_INVALID_SOCKET) {
                struct sockaddr_in to;
                memset(&to, 0, sizeof(to));
                to.sin_family = AF_INET;
                to.sin_addr.s_addr = ip;
                to.sin_port = port;
                /* Under the lock, so the poller cannot close it now. */
                (void)sendto(s, (const char *)data, (int)n, 0,
                             (const struct sockaddr *)&to, sizeof(to));
                SDL_AddAtomicInt(&r->toServer, 1);
            }
            SDL_UnlockMutex(r->lock);
        }
        /* A RECV from the child is not valid: ignored. */
    }
    return 0;
}

static int SDLCALL relayPollerFn(void *arg) {
    BotHostRelay *r = (BotHostRelay *)arg;
    static uint8_t buf[BOT_HOST_RELAY_MAX_DATAGRAM];
    static uint8_t frame[BOT_HOST_RELAY_MAX_FRAME];
    while (!SDL_GetAtomicInt(&r->stop)) {
        fd_set rd;
        struct timeval tv;
        bolo_socket_t socks[BOT_HOST_RELAY_MAX_SOCKS];
        int ids[BOT_HOST_RELAY_MAX_SOCKS];
        int n = 0, i, ready;
        bolo_socket_t maxFd = 0;

        FD_ZERO(&rd);
        SDL_LockMutex(r->lock);
        for (i = 0; i < BOT_HOST_RELAY_MAX_SOCKS; i++) {
            if (r->slots[i].state == SLOT_CLOSING) {
                closesocket(r->slots[i].sock);
                r->slots[i].sock = BOLO_INVALID_SOCKET;
                r->slots[i].state = SLOT_FREE;
            } else if (r->slots[i].state == SLOT_OPEN) {
                socks[n] = r->slots[i].sock;
                ids[n] = i;
                FD_SET(socks[n], &rd);
                if (socks[n] > maxFd) maxFd = socks[n];
                n++;
            }
        }
        SDL_UnlockMutex(r->lock);
        if (n == 0) {
            SDL_Delay(5);
            continue;
        }
        tv.tv_sec = 0;
        tv.tv_usec = 5000;
        ready = select((int)maxFd + 1, &rd, NULL, NULL, &tv);
        if (ready <= 0) continue;
        for (i = 0; i < n; i++) {
            if (!FD_ISSET(socks[i], &rd)) continue;
            for (;;) {
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                int got = (int)recvfrom(socks[i], (char *)buf, sizeof(buf), 0,
                                        (struct sockaddr *)&from, &fl);
                size_t fn;
                if (got < 0) break;
                if (from.sin_addr.s_addr != r->serverIp ||
                    from.sin_port != r->serverPort) {
                    relayRefused(r, "datagram from another address");
                    continue;
                }
                fn = botHostRelayEncode(BOT_HOST_RELAY_RECV, (uint8_t)ids[i],
                                        from.sin_addr.s_addr, from.sin_port,
                                        buf, (size_t)got, frame,
                                        sizeof(frame));
                if (fn == 0) continue;
                if (!botHostChildRelayWrite(r->proc, frame, fn)) {
                    /* The child is gone. */
                    return 0;
                }
                SDL_AddAtomicInt(&r->fromServer, 1);
            }
        }
    }
    return 0;
}

BotHostRelay *botHostRelayStart(BotHostChildProc *p, const char *serverHost,
                                uint16_t serverPort) {
    BotHostRelay *r;
    struct in_addr a;
    int i;
    if (p == NULL || !botHostChildHasRelay(p) || serverHost == NULL) {
        return NULL;
    }
    if (bolo_net_init() != 0) return NULL;
    if (inet_pton(AF_INET, serverHost, &a) != 1) {
        fprintf(stderr, "[bothost] the relay needs the server's IPv4 "
                        "address, not %s\n", serverHost);
        return NULL;
    }
    r = (BotHostRelay *)calloc(1, sizeof(*r));
    if (r == NULL) return NULL;
    r->proc = p;
    r->serverIp = a.s_addr;
    r->serverPort = htons(serverPort);
    for (i = 0; i < BOT_HOST_RELAY_MAX_SOCKS; i++) {
        r->slots[i].sock = BOLO_INVALID_SOCKET;
    }
    r->lock = SDL_CreateMutex();
    if (r->lock == NULL) {
        free(r);
        return NULL;
    }
    r->reader = SDL_CreateThread(relayReaderFn, "bothost-relay-in", r);
    r->poller = SDL_CreateThread(relayPollerFn, "bothost-relay-out", r);
    if (r->reader == NULL || r->poller == NULL) {
        /* With no relay the child cannot run: it ends, and so does a
         * reader that started. */
        fprintf(stderr, "[bothost] the relay's threads did not start\n");
        botHostChildKill(p);
        botHostRelayStop(r);
        return NULL;
    }
    return r;
}

void botHostRelayStop(BotHostRelay *r) {
    int i;
    if (r == NULL) return;
    SDL_SetAtomicInt(&r->stop, 1);
    /* The reader ends when the child's end of the pipe closed. */
    if (r->reader != NULL) SDL_WaitThread(r->reader, NULL);
    if (r->poller != NULL) SDL_WaitThread(r->poller, NULL);
    for (i = 0; i < BOT_HOST_RELAY_MAX_SOCKS; i++) {
        if (r->slots[i].state != SLOT_FREE &&
            r->slots[i].sock != BOLO_INVALID_SOCKET) {
            closesocket(r->slots[i].sock);
        }
    }
    if (r->lock != NULL) SDL_DestroyMutex(r->lock);
    free(r);
}

void botHostRelayCounts(BotHostRelay *r, unsigned long *toServer,
                        unsigned long *fromServer, unsigned long *refused) {
    if (toServer != NULL) {
        *toServer = r != NULL ? (unsigned long)SDL_GetAtomicInt(&r->toServer)
                              : 0ul;
    }
    if (fromServer != NULL) {
        *fromServer =
            r != NULL ? (unsigned long)SDL_GetAtomicInt(&r->fromServer) : 0ul;
    }
    if (refused != NULL) {
        *refused = r != NULL ? (unsigned long)SDL_GetAtomicInt(&r->refused)
                             : 0ul;
    }
}
