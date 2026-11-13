/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * UDP socket hooks.
 *
 * A program with no network of its own (the Bot Host child in its sandbox,
 * src/bothost_child/bothost_child.h) sends its datagrams through the game,
 * over a pipe. With hooks set, createUdpSocket(false), udpSendTo,
 * udpRecvFrom and udpCloseSocket (transport_udp_common.c) call them instead
 * of the system. A listening socket (createUdpSocket(true)) cannot be made
 * through the hooks. Set them once, before any socket is made.
 */
#ifndef WINBOLO_UDP_SOCKET_HOOKS_H
#define WINBOLO_UDP_SOCKET_HOOKS_H

#include <stdbool.h>
#include <stdint.h>

#include "platform_net.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bolo_socket_t (*create)(void);
    void (*send)(bolo_socket_t sock, const uint8_t *buf, int len,
                 const struct sockaddr_in *addr);
    /* One datagram, or -1 when there is none. */
    int  (*recv)(bolo_socket_t sock, uint8_t *buf, int maxLen,
                 struct sockaddr_in *fromAddr);
    void (*close)(bolo_socket_t sock);
} UdpSocketHooks;

/* NULL: the system's sockets. */
void udpSetSocketHooks(const UdpSocketHooks *hooks);
/* True while hooks are set: a socket is then not a system socket, and no
 * system call (getsockname, sendto) may be given it. */
bool udpSocketHooksOn(void);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_UDP_SOCKET_HOOKS_H */
