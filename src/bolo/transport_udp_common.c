/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Transport UDP Common
 *Filename:      transport_udp_common.c
 *Author:        John Morrison
 *Purpose:
 *  Shared serialization (pack/unpack) and socket helpers
 *  used by both client and server UDP transport code.
 *********************************************************/

/* Packet-information control messages. glibc declares struct in6_pktinfo only
 * under _GNU_SOURCE, and macOS declares IPV6_RECVPKTINFO and gives
 * IPV6_PKTINFO its RFC 3542 meaning only under __APPLE_USE_RFC_3542. Both
 * have to be set before the first system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(__APPLE_USE_RFC_3542)
#define __APPLE_USE_RFC_3542
#endif

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#endif

#include "transport_udp_internal.h"
#include "wire_codec.h"
#include "wire_messages.h"
#include "../common/wb_log.h"

#if defined(_WIN32)
#include <mswsock.h>   /* WSAID_WSARECVMSG, LPFN_WSARECVMSG */
#endif

/* ================================================================
 * Serialization helpers — pack/unpack structs to/from wire format
 * All multi-byte values use network byte order (big-endian).
 * ================================================================ */

void packU16(uint8_t *buf, uint16_t val) {
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
}

uint16_t unpackU16(const uint8_t *buf) {
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

void packU32(uint8_t *buf, uint32_t val) {
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val & 0xFF);
}

uint32_t unpackU32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8)  |
           (uint32_t)buf[3];
}

void packHeader(uint8_t *buf, uint8_t packetType, uint32_t sequence) {
    buf[0] = BOLO_NEW_MAGIC_0;
    buf[1] = BOLO_NEW_MAGIC_1;
    buf[2] = packetType;
    buf[3] = 0; /* reserved */
    packU32(buf + 4, sequence);
}

uint8_t getPacketType(const uint8_t *buf, int len) {
    if (len < PACKET_HEADER_SIZE) return 0;
    if (buf[0] != BOLO_NEW_MAGIC_0 || buf[1] != BOLO_NEW_MAGIC_1) return 0;
    return buf[2];
}

/* Packet-type -> debug name. The one declarative list every type/name
 * mapping derives from; the switch below expands it, and the unit test
 * re-expands it to pin every string. Types absent here resolve to
 * "UNKNOWN" (a logging helper only — not a wire field). */
#define PACKET_NAME_TABLE(X) \
    X(PACKET_INPUT, "INPUT") \
    X(PACKET_JOIN_REQUEST, "JOIN_REQUEST") \
    X(PACKET_CHAT_MESSAGE, "CHAT_MESSAGE") \
    X(PACKET_PING, "PING") \
    X(PACKET_QUIT, "QUIT") \
    X(PACKET_STATE_SNAPSHOT, "STATE_SNAPSHOT") \
    X(PACKET_JOIN_ACCEPT, "JOIN_ACCEPT") \
    X(PACKET_JOIN_REJECT, "JOIN_REJECT") \
    X(PACKET_PLAYER_JOINED, "PLAYER_JOINED") \
    X(PACKET_PLAYER_LEFT, "PLAYER_LEFT") \
    X(PACKET_CHAT_BROADCAST, "CHAT_BROADCAST") \
    X(PACKET_FULL_STATE, "FULL_STATE") \
    X(PACKET_MAP_DELTA, "MAP_DELTA") \
    X(PACKET_BASE_STATE, "BASE_STATE") \
    X(PACKET_PILL_STATE, "PILL_STATE") \
    X(PACKET_PONG, "PONG") \
    X(PACKET_GAME_EVENT, "GAME_EVENT") \
    X(PACKET_MAP_RESYNC_REQUEST, "MAP_RESYNC_REQUEST") \
    X(PACKET_NAME_CHANGE, "NAME_CHANGE") \
    X(PACKET_ALLIANCE_REQUEST, "ALLIANCE_REQUEST") \
    X(PACKET_ALLIANCE_ACCEPT, "ALLIANCE_ACCEPT") \
    X(PACKET_ALLIANCE_LEAVE, "ALLIANCE_LEAVE") \
    X(PACKET_ALLIANCE_UPDATE, "ALLIANCE_UPDATE") \
    X(PACKET_LOCK_TOGGLE, "LOCK_TOGGLE") \
    X(PACKET_SERVER_SHUTDOWN, "SERVER_SHUTDOWN") \
    X(PACKET_LOBBY_TEAM_SET, "LOBBY_TEAM_SET") \
    X(PACKET_LOBBY_READY, "LOBBY_READY") \
    X(PACKET_LOBBY_ADD_BOT, "LOBBY_ADD_BOT") \
    X(PACKET_LOBBY_REMOVE_BOT, "LOBBY_REMOVE_BOT") \
    X(PACKET_LOBBY_UPDATE, "LOBBY_UPDATE") \
    X(PACKET_LOBBY_SETTINGS, "LOBBY_SETTINGS") \
    X(PACKET_LOBBY_SET_SETTING, "LOBBY_SET_SETTING") \
    X(PACKET_LOBBY_SETTING_CHG, "LOBBY_SETTING_CHG") \
    X(PACKET_LOBBY_OPEN_HOST, "LOBBY_OPEN_HOST") \
    X(PACKET_LOBBY_OPEN_HOST_CHG, "LOBBY_OPEN_HOST_CHG") \
    X(PACKET_LOBBY_TEAM_META, "LOBBY_TEAM_META") \
    X(PACKET_LOBBY_TEAM_META_CHG, "LOBBY_TEAM_META_CHG") \
    X(PACKET_LOBBY_TEAM_CLEAR, "LOBBY_TEAM_CLEAR") \
    X(PACKET_LOBBY_BOT_CONFIG, "LOBBY_BOT_CONFIG") \
    X(PACKET_LOBBY_BOT_CONFIG_CHG, "LOBBY_BOT_CONFIG_CHG") \
    X(PACKET_LOBBY_KICK, "LOBBY_KICK") \
    X(PACKET_KICKED, "KICKED") \
    X(PACKET_LOBBY_SET_BOT_BRAIN, "LOBBY_SET_BOT_BRAIN") \
    X(PACKET_LOBBY_BOT_BRAIN_CHG, "LOBBY_BOT_BRAIN_CHG") \
    X(PACKET_LOBBY_BRAIN_LIST, "LOBBY_BRAIN_LIST") \
    X(PACKET_LOBBY_SET_MAP, "LOBBY_SET_MAP") \
    X(PACKET_SET_SCRIPT_LIST, "SET_SCRIPT_LIST") \
    X(PACKET_SET_SCRIPT_SETTING, "SET_SCRIPT_SETTING") \
    X(PACKET_LOBBY_SET_PASSWORD, "LOBBY_SET_PASSWORD") \
    X(PACKET_LOBBY_MAP_LIST_REQ, "LOBBY_MAP_LIST_REQ") \
    X(PACKET_LOBBY_MAP_LIST_RSP, "LOBBY_MAP_LIST_RSP") \
    X(PACKET_LOBBY_MAP_SEARCH_REQ, "LOBBY_MAP_SEARCH_REQ") \
    X(PACKET_LOBBY_MAP_SEARCH_RSP, "LOBBY_MAP_SEARCH_RSP") \
    X(PACKET_LOBBY_MAP_UPLOAD_BEGIN, "LOBBY_MAP_UPLOAD_BEGIN") \
    X(PACKET_LOBBY_MAP_UPLOAD_ACK, "LOBBY_MAP_UPLOAD_ACK") \
    X(PACKET_LOBBY_MAP_UPLOAD_DONE, "LOBBY_MAP_UPLOAD_DONE") \
    X(PACKET_LOBBY_MAP_USE_LOCAL, "LOBBY_MAP_USE_LOCAL") \
    X(PACKET_LOBBY_MAP_USE_LOCAL_NACK, "LOBBY_MAP_USE_LOCAL_NACK") \
    X(PACKET_LOBBY_MAP_PREVIEW_REQ, "LOBBY_MAP_PREVIEW_REQ") \
    X(PACKET_LOBBY_MAP_PREVIEW_ERR, "LOBBY_MAP_PREVIEW_ERR") \
    X(PACKET_LOBBY_PREVIEW_CANCEL, "LOBBY_PREVIEW_CANCEL") \
    X(PACKET_LOBBY_RELOAD_SCENARIO, "LOBBY_RELOAD_SCENARIO") \
    X(PACKET_LOBBY_SCENARIO_LIST_REQ, "LOBBY_SCENARIO_LIST_REQ") \
    X(PACKET_LOBBY_SCENARIO_LIST_RSP, "LOBBY_SCENARIO_LIST_RSP") \
    X(PACKET_LOBBY_SCENARIO_DETAILS_REQ, "LOBBY_SCENARIO_DETAILS_REQ") \
    X(PACKET_LOBBY_BRAIN_DOCS_REQ, "LOBBY_BRAIN_DOCS_REQ") \
    X(PACKET_LOBBY_BOT_POOL_REQ, "LOBBY_BOT_POOL_REQ") \
    X(PACKET_LOBBY_SCRIPT_FETCH_REQ, "LOBBY_SCRIPT_FETCH_REQ") \
    X(PACKET_LOBBY_PREVIEW_COMMIT, "LOBBY_PREVIEW_COMMIT") \
    X(PACKET_LOBBY_PREVIEW_RANDOM, "LOBBY_PREVIEW_RANDOM") \
    X(PACKET_LOBBY_CLAIM_START, "LOBBY_CLAIM_START") \
    X(PACKET_COUNTDOWN, "COUNTDOWN") \
    X(PACKET_GAME_START, "GAME_START") \
    X(PACKET_GAME_OVER, "GAME_OVER") \
    X(PACKET_LOBBY_MAP_CHANGE, "LOBBY_MAP_CHANGE") \
    X(PACKET_WBN_REAUTH, "WBN_REAUTH") \
    X(PACKET_WBN_REKEY, "WBN_REKEY") \
    X(PACKET_BALANCE_REQUEST, "BALANCE_REQUEST") \
    X(PACKET_BALANCE_PROPOSAL, "BALANCE_PROPOSAL") \
    X(PACKET_BALANCE_APPLY, "BALANCE_APPLY") \
    X(PACKET_BALANCE_DISMISS, "BALANCE_DISMISS") \
    X(PACKET_MAP_SKIP_VOTE, "MAP_SKIP_VOTE") \
    X(PACKET_MAP_SKIP_STATE, "MAP_SKIP_STATE") \
    X(PACKET_GAME_VOTE_TOGGLE, "GAME_VOTE_TOGGLE") \
    X(PACKET_GAME_VOTE_STATE, "GAME_VOTE_STATE") \
    X(PACKET_PUNCH_REQUEST, "PUNCH_REQUEST") \
    X(PACKET_PUNCH_NOTIFY, "PUNCH_NOTIFY") \
    X(PACKET_PUNCH_REQUEST_ACK, "PUNCH_REQUEST_ACK") \
    X(PACKET_PUNCH_PROBE_REQUEST, "PUNCH_PROBE_REQUEST") \
    X(PACKET_PUNCH_PROBE_REPLY, "PUNCH_PROBE_REPLY") \
    X(PACKET_PUNCH_REQUEST6, "PUNCH_REQUEST6") \
    X(PACKET_PUNCH_NOTIFY6, "PUNCH_NOTIFY6") \
    X(PACKET_PUNCH_PROBE_REPLY6, "PUNCH_PROBE_REPLY6") \
    X(PACKET_COMMAND_TICK, "COMMAND_TICK") \
    X(PACKET_COMMAND_ACK, "COMMAND_ACK") \
    X(PACKET_COMMAND_REJECTED, "COMMAND_REJECTED") \
    X(PACKET_BALANCE_FAILED, "BALANCE_FAILED") \
    X(PACKET_MAP_DL_READY, "MAP_DL_READY") \
    X(PACKET_PLAYER_MUTE, "PLAYER_MUTE") \
    X(PACKET_VOICE_STATE, "VOICE_STATE") \
    X(PACKET_MAP_PING, "MAP_PING") \
    X(PACKET_PLAYER_PING_MUTE, "PLAYER_PING_MUTE")

const char *packetTypeName(uint8_t type) {
    switch (type) {
#define X(sym, str) case sym: return str;
    PACKET_NAME_TABLE(X)
#undef X
    default: return "UNKNOWN";
    }
}

/* The punch address body: [family 1][address 16][port 2, big endian]. A plain
 * or mapped IPv4 address goes as family 4 with its 4 bytes first and the other
 * 12 zero; any other IPv6 address goes as family 6. No scope id is carried, so
 * a link-local address, which needs one, is refused, as are loopback and
 * unspecified addresses, which name no host the tracker can reach. */
int packPunchAddr(uint8_t *out, const BoloAddr *addr) {
    uint8_t v4[4];
    const uint8_t *v6;

    if (out == NULL || boloAddrFamily(addr) == 0 ||
        boloAddrIsUnspecified(addr) || boloAddrIsLoopback(addr) ||
        boloAddrIsLinkLocal(addr)) {
        return 0;
    }
    memset(out, 0, PUNCH_ADDR_WIRE_SIZE);
    if (boloAddrV4Bytes(addr, v4)) {
        out[0] = PUNCH_ADDR_FAMILY_V4;
        memcpy(out + 1, v4, 4);
    } else {
        v6 = boloAddrV6Bytes(addr);
        if (v6 == NULL) {
            return 0;
        }
        out[0] = PUNCH_ADDR_FAMILY_V6;
        memcpy(out + 1, v6, 16);
    }
    packU16(out + 17, boloAddrPort(addr));
    return PUNCH_ADDR_WIRE_SIZE;
}

bool unpackPunchAddr(const uint8_t *in, int len, BoloAddr *out) {
    BoloAddr tmp;
    uint16_t port;

    if (in == NULL || out == NULL || len < PUNCH_ADDR_WIRE_SIZE) {
        return false;
    }
    port = unpackU16(in + 17);
    if (in[0] == PUNCH_ADDR_FAMILY_V4) {
        struct in_addr v4;
        memcpy(&v4, in + 1, 4);
        boloAddrFromV4(v4, port, &tmp);
    } else if (in[0] == PUNCH_ADDR_FAMILY_V6) {
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        memcpy(&sin6.sin6_addr, in + 1, 16);
        sin6.sin6_port = htons(port);
        if (!boloAddrFromSockaddr((const struct sockaddr *)&sin6,
                                  (socklen_t)sizeof(sin6), &tmp)) {
            return false;
        }
    } else {
        return false;
    }
    if (boloAddrIsUnspecified(&tmp)) {
        return false;
    }
    *out = tmp;
    return true;
}

int buildPunchRequest(uint8_t *buf, int cap, const BoloAddr *target,
                      uint32_t sequence) {
    uint8_t v4[4];

    if (buf == NULL || cap < PUNCH_REQUEST_MAX_LEN ||
        boloAddrFamily(target) == 0 || boloAddrIsUnspecified(target) ||
        boloAddrIsLoopback(target) || boloAddrIsLinkLocal(target)) {
        return 0;
    }
    if (boloAddrV4Bytes(target, v4)) {
        packHeader(buf, PACKET_PUNCH_REQUEST, sequence);
        memcpy(buf + PACKET_HEADER_SIZE, v4, 4);
        packU16(buf + PACKET_HEADER_SIZE + 4, boloAddrPort(target));
        return PACKET_HEADER_SIZE + 6;
    }
    packHeader(buf, PACKET_PUNCH_REQUEST6, sequence);
    if (packPunchAddr(buf + PACKET_HEADER_SIZE, target) !=
        PUNCH_ADDR_WIRE_SIZE) {
        return 0;
    }
    return PACKET_HEADER_SIZE + PUNCH_ADDR_WIRE_SIZE;
}

/* Serialize one InputPacket into buf. Returns bytes written (21). */
int packInputPacket(uint8_t *buf, const InputPacket *pkt) {
    packU32(buf, pkt->tick);
    buf[4] = pkt->playerNum;
    buf[5] = pkt->buttons;
    buf[6] = pkt->actions;
    buf[7] = pkt->buildAction;
    buf[8] = pkt->buildX;
    buf[9] = pkt->buildY;
    buf[10] = pkt->flags;
    packU32(buf + 11, pkt->mapEventAck);
    packU16(buf + 15, pkt->pingMs);
    packU32(buf + 17, pkt->viewTick);
    return 21;
}

void unpackInputPacket(const uint8_t *buf, InputPacket *pkt) {
    pkt->tick = unpackU32(buf);
    pkt->playerNum = buf[4];
    pkt->buttons = buf[5];
    pkt->actions = buf[6];
    pkt->buildAction = buf[7];
    pkt->buildX = buf[8];
    pkt->buildY = buf[9];
    pkt->flags = buf[10];
    pkt->mapEventAck = unpackU32(buf + 11);
    pkt->pingMs = unpackU16(buf + 15);
    pkt->viewTick = unpackU32(buf + 17);
}

/* Variable-length tank entry: a 1-byte stub when playerNum carries
 * TANK_SNAPSHOT_HIDDEN_FLAG, otherwise an 11-byte core plus the present groups.
 * Generated from TANK_SNAPSHOT_FIELDS in wire_messages.h. */
DEFINE_WIRE_CODEC_MASKED(TankSnapshot, "tank_snapshot", playerNum,
                         TANK_SNAPSHOT_HIDDEN_FLAG, TANK_SNAPSHOT_FIELDS)

/* Flat fixed-layout leaf snapshot codecs, generated from the field lists in
 * wire_messages.h. pack returns bytes written; unpack returns bytes consumed
 * or 0 if `avail` is too short for a field. */
DEFINE_WIRE_CODEC(ShellSnapshot,       "shell_snapshot",        SHELL_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC(TkExplosionSnapshot, "tk_explosion_snapshot", TK_EXPLOSION_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC(BaseSnapshot,        "base_snapshot",         BASE_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC(PillSnapshot,        "pill_snapshot",         PILL_SNAPSHOT_FIELDS)

/* Tie the generated wire size to the hand-maintained *_WIRE_SIZE constants that
 * the server's pre-pack buffer guards still use, so a field added to a list
 * without bumping the constant fails the build rather than under-counting. */
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(SHELL_SNAPSHOT_FIELDS)        == SHELL_SNAPSHOT_WIRE_SIZE,        shell_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(TK_EXPLOSION_SNAPSHOT_FIELDS) == TK_EXPLOSION_SNAPSHOT_WIRE_SIZE, tk_explosion_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(BASE_SNAPSHOT_FIELDS)         == BASE_SNAPSHOT_WIRE_SIZE,         base_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_SIZE_OF(PILL_SNAPSHOT_FIELDS)         == PILL_SNAPSHOT_WIRE_SIZE,         pill_wire_size_drift);
BOLO_STATIC_ASSERT(WIRE_MASKED_SIZE_OF(TANK_SNAPSHOT_FIELDS)  == TANK_SNAPSHOT_WIRE_SIZE,         tank_wire_size_drift);

/* Serialize one GameEvent into buf. Returns bytes written (1 + dataSize). */
int packGameEvent(uint8_t *buf, const GameEvent *ev) {
    int dataLen = gameEventDataSize(ev->type);
    buf[0] = ev->type;
    memcpy(buf + 1, ev->data, dataLen);
    return 1 + dataLen;
}

/* Deserialize one GameEvent from buf, reading at most `avail` bytes. Returns
 * bytes consumed (1 + dataSize), or 0 if the buffer is too short for the type
 * byte or its data — mirroring the generated unpack* codecs so a truncated
 * trailing event can't over-read the datagram. */
int unpackGameEvent(const uint8_t *buf, size_t avail, GameEvent *ev) {
    int dataLen;
    if (avail < 1) return 0;
    ev->type = buf[0];
    dataLen = gameEventDataSize(ev->type);
    if (avail < (size_t)(1 + dataLen)) return 0;
    memset(ev->data, 0, sizeof(ev->data));
    memcpy(ev->data, buf + 1, dataLen);
    return 1 + dataLen;
}

/* Splice channel-delivered game events into `events` ahead of the map-tail
 * events already staged at [tailStart, tailStart+tailCount).  The `chanCount`
 * events in `chan` are placed at tailStart and the existing tail is shifted up
 * behind them, preserving the game-then-map order the snapshot game tail used.
 * Counts are clamped to `cap` (placing channel events first, truncating the
 * tail if room runs out) so the splice can never index past `events[cap]`.
 * Returns the new total event count. */
int spliceGameEventsBeforeTail(GameEvent *events, int tailStart, int tailCount,
                               const GameEvent *chan, int chanCount, int cap) {
    int room;
    if (chanCount <= 0) {
        return tailStart + tailCount;
    }
    if (chanCount > cap - tailStart) {
        chanCount = cap - tailStart;
    }
    if (chanCount < 0) {
        chanCount = 0;
    }
    room = cap - (tailStart + chanCount);
    if (tailCount > room) {
        tailCount = room;
    }
    if (tailCount < 0) {
        tailCount = 0;
    }
    memmove(&events[tailStart + chanCount], &events[tailStart],
            (size_t)tailCount * sizeof(GameEvent));
    memcpy(&events[tailStart], chan, (size_t)chanCount * sizeof(GameEvent));
    return tailStart + chanCount + tailCount;
}

/* ================================================================
 * Socket helpers
 * ================================================================ */

#if WB_ENABLE_NETIMPAIR
/* Set by udpTestForceIPv4Socket: createUdpSocket skips the AF_INET6 attempt. */
static SDL_AtomicInt s_udpForceIPv4;

void udpTestForceIPv4Socket(bool on) {
    SDL_SetAtomicInt(&s_udpForceIPv4, on ? 1 : 0);
}
#endif

/* Opens an AF_INET6 UDP socket with IPV6_V6ONLY cleared, so IPv4 peers reach
 * it as mapped addresses. The option is cleared before any bind: Windows
 * turns it on by default and Linux follows the bindv6only sysctl. Returns
 * INVALID_SOCKET when the open fails or the option cannot be cleared. The web
 * build sends through Emscripten's IPv4 socket emulation and never tries. */
static SOCKET udpOpenDualStackSocket(void) {
#if defined(__EMSCRIPTEN__) || !defined(IPV6_V6ONLY)
    return INVALID_SOCKET;
#else
    SOCKET sock;
    int v6only = 0;
    int rc;

#if WB_ENABLE_NETIMPAIR
    if (SDL_GetAtomicInt(&s_udpForceIPv4) != 0) {
        return INVALID_SOCKET;
    }
#endif
    sock = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
#ifdef _WIN32
    rc = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
                    (const char *)&v6only, (int)sizeof(v6only));
#else
    rc = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
                    &v6only, (socklen_t)sizeof(v6only));
#endif
    if (rc != 0) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
#endif
}

/* Create a non-blocking UDP socket.
 *
 * The socket is AF_INET6 with IPV6_V6ONLY cleared where that works, so one
 * socket serves IPv6 peers and IPv4 peers (as ::ffff:a.b.c.d). When the
 * AF_INET6 open fails or the option cannot be cleared, as on a machine with
 * no IPv6 stack, it is AF_INET as before. *outFamily receives the family
 * opened (AF_INET6 or AF_INET); it is left alone on failure.
 *
 * exclusive=true: caller wants a hard failure on bind() if the port is
 *   already in use (server case — a second listen-server host on the
 *   same machine must NOT silently share the port with the first). On
 *   Windows that means SO_EXCLUSIVEADDRUSE and skipping SO_REUSEADDR;
 *   SO_REUSEADDR there is permissive enough that two binds to the same
 *   port both succeed and the OS dispatches packets to one of them.
 * exclusive=false: ephemeral / client socket — no options. The kernel
 *   picks the port on the first send, so there is no fixed port to
 *   rebind. SO_REUSEADDR and SO_REUSEPORT used to be set here, and they
 *   let the kernel give the client a port number another process's
 *   server already held on 127.0.0.1. Datagrams to 127.0.0.1:port then
 *   went to that server, the more specific bind, and the client never
 *   heard its own server's replies. */
SOCKET createUdpSocket(bool exclusive, int *outFamily) {
    SOCKET sock;
    int family = AF_INET6;
    unsigned long nonBlock = 1;
#ifdef _WIN32
    int reuse = 1;
#endif

    sock = udpOpenDualStackSocket();
    if (sock == INVALID_SOCKET) {
        family = AF_INET;
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    }
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    if (outFamily != NULL) {
        *outFamily = family;
    }
#ifdef _WIN32
    if (exclusive) {
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&reuse, sizeof(reuse));
    }
#else
    (void)exclusive;
#endif
    ioctlsocket(sock, FIONBIO, &nonBlock);
    return sock;
}

#if WB_ENABLE_NETIMPAIR
/* Largest length handed to udpSendTo since the last reset. Atomic because the
 * client and the server transports in one test process both send through
 * here. */
static SDL_AtomicInt s_udpLargestSend;

int udpTestLargestSend(void) {
    return SDL_GetAtomicInt(&s_udpLargestSend);
}

void udpTestResetLargestSend(void) {
    SDL_SetAtomicInt(&s_udpLargestSend, 0);
}
#endif

/* Count of sends udpSendTo refused because the destination could not be
 * converted to the socket's family. The error is logged for the first
 * UDP_SEND_REFUSED_LOG_FIRST refusals and then once every
 * UDP_SEND_REFUSED_LOG_EVERY, so a peer the game keeps sending to cannot
 * flood the log. Atomic because client and server send from different
 * threads in one process. */
#define UDP_SEND_REFUSED_LOG_FIRST 5
#define UDP_SEND_REFUSED_LOG_EVERY 1000
static SDL_AtomicInt s_udpSendRefused;

/* The part of a send that udpSendTo and udpSendToFrom share: converts addr to
 * sockFamily into *dest, logs a refusal, and records len for the test-only
 * largest-send counter. Returns 1 when the datagram may go out, 0 when it is
 * refused. */
static int udpPrepareSend(int sockFamily, int len, const BoloAddr *addr,
                          BoloAddr *dest) {
    if (!boloAddrToFamily(addr, sockFamily, dest)) {
        int n = SDL_AddAtomicInt(&s_udpSendRefused, 1);
        if (n < UDP_SEND_REFUSED_LOG_FIRST ||
            (n % UDP_SEND_REFUSED_LOG_EVERY) == 0) {
            char text[BOLO_ADDR_STR_LEN];
            if (!boloAddrFormat(addr, text, sizeof(text))) {
                snprintf(text, sizeof(text), "%s",
                         boloAddrFamily(addr) == 0 ? "(no address)"
                                                   : "(unprintable)");
            }
            WB_LOG_ERROR(WB_LOG_CAT_NET,
                "udp send refused: %s cannot be reached from an %s socket "
                "(%d refused so far)",
                text, sockFamily == AF_INET6 ? "IPv6" : "IPv4", n + 1);
        }
        return 0;
    }
#if WB_ENABLE_NETIMPAIR
    {
        int seen = SDL_GetAtomicInt(&s_udpLargestSend);
        while (len > seen &&
               !SDL_CompareAndSwapAtomicInt(&s_udpLargestSend, seen, len)) {
            seen = SDL_GetAtomicInt(&s_udpLargestSend);
        }
    }
#else
    (void)len;
#endif
    return 1;
}

/* Send a buffer via UDP to a specific address, converted to sockFamily (an
 * IPv4 destination on an AF_INET6 socket goes as ::ffff:a.b.c.d). An empty
 * address, or an IPv6 destination on an AF_INET socket, is not sent. */
void udpSendTo(SOCKET sock, int sockFamily, const uint8_t *buf, int len,
               const BoloAddr *addr) {
    BoloAddr dest;

    if (!udpPrepareSend(sockFamily, len, addr, &dest)) {
        return;
    }
    sendto(sock, (const char *)buf, len, 0,
           (const struct sockaddr *)&dest.ss, dest.len);
}

/* ================================================================
 * The local address a datagram arrived on, and sending from it
 *
 * A server socket bound to the any address leaves the source of each reply
 * to the operating system. On a host with more than one address of a family
 * (an IPv6 host with a stable and a temporary address) it can pick one the
 * peer did not send to, and a router firewall in front of the peer drops a
 * reply from an address the peer never contacted. So the server asks for the
 * destination address of each datagram it receives and names it as the
 * source of what it sends back.
 *
 * Which option carries what:
 *   - IPv6 datagrams: IPV6_RECVPKTINFO (Windows: IPV6_PKTINFO) on receive,
 *     an IPV6_PKTINFO control message on send.
 *   - IPv4 datagrams on an AF_INET socket: IP_PKTINFO where the platform has
 *     it (Linux, Windows, macOS), else IP_RECVDSTADDR on receive and
 *     IP_SENDSRCADDR on send (FreeBSD and the other BSDs).
 *   - IPv4 datagrams on a dual-stack AF_INET6 socket: macOS reports them
 *     through IPV6_RECVPKTINFO as a mapped address. Linux does the same,
 *     and also reports IP_PKTINFO when that option is set. Windows reports
 *     them only through IP_PKTINFO, so the option is set on Linux and
 *     Windows. On send, a mapped destination takes an IP_PKTINFO control
 *     message where the platform has IP_PKTINFO: macOS hands such a send to
 *     its IPv4 path, which reads only IPv4 control messages, and Windows
 *     expects IP_PKTINFO for it too.
 * ================================================================ */

#if !defined(__EMSCRIPTEN__)
#  if defined(_WIN32)
#    define UDP_V6_RECV_OPT IPV6_PKTINFO
#  elif defined(IPV6_RECVPKTINFO)
#    define UDP_V6_RECV_OPT IPV6_RECVPKTINFO
#  endif
#  if defined(IPV6_PKTINFO)
#    define UDP_V6_PKTINFO 1
#  endif
#  if defined(IP_PKTINFO)
#    define UDP_V4_PKTINFO 1
#  elif defined(IP_RECVDSTADDR)
#    define UDP_V4_DSTADDR 1
#  endif
#endif

/* Room for every control message a receive can carry here: one IPv6 and one
 * IPv4 packet-information message, with headers and padding. */
#define UDP_CMSG_BUF_LEN 256

#if !defined(__EMSCRIPTEN__)

/* Set once each, so a platform without the options logs one line. */
static SDL_AtomicInt s_udpRecvLocalWarned;
static SDL_AtomicInt s_udpSendFromWarned;

#if defined(_WIN32)
/* WSARecvMsg is reached through a pointer the provider hands out. It is
 * fetched by udpEnableRecvLocalAddr, which the server calls before its
 * receive thread starts, and only read after that. NULL means
 * udpRecvFromLocal falls back to udpRecvFrom. */
static LPFN_WSARECVMSG s_udpWsaRecvMsg = NULL;
static int             s_udpWsaRecvMsgTried = 0;

static void udpFetchWsaRecvMsg(SOCKET sock) {
    GUID guid = WSAID_WSARECVMSG;
    LPFN_WSARECVMSG fn = NULL;
    DWORD bytes = 0;

    if (s_udpWsaRecvMsgTried) {
        return;
    }
    s_udpWsaRecvMsgTried = 1;
    if (WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid,
                 (DWORD)sizeof(guid), &fn, (DWORD)sizeof(fn), &bytes,
                 NULL, NULL) == 0 && fn != NULL) {
        s_udpWsaRecvMsg = fn;
    }
}
#endif

/* Turns one boolean socket option on. Returns 1 on success. */
static int udpSetBoolOpt(SOCKET sock, int level, int name) {
#if defined(_WIN32)
    DWORD on = 1;
    return setsockopt(sock, level, name, (const char *)&on,
                      (int)sizeof(on)) == 0;
#else
    int on = 1;
    return setsockopt(sock, level, name, &on, (socklen_t)sizeof(on)) == 0;
#endif
}

#endif /* !__EMSCRIPTEN__ */

void udpEnableRecvLocalAddr(SOCKET sock, int sockFamily) {
#if defined(__EMSCRIPTEN__)
    (void)sock;
    (void)sockFamily;
#else
    int ok = 0;

    if (sockFamily == AF_INET6) {
#if defined(UDP_V6_RECV_OPT)
        ok = udpSetBoolOpt(sock, IPPROTO_IPV6, UDP_V6_RECV_OPT);
#endif
#if defined(UDP_V4_PKTINFO) && (defined(__linux__) || defined(_WIN32))
        /* IPv4 datagrams on this dual-stack socket. Not needed for the
         * result on a current Linux kernel, so a failure is not reported. */
        (void)udpSetBoolOpt(sock, IPPROTO_IP, IP_PKTINFO);
#endif
    } else if (sockFamily == AF_INET) {
#if defined(UDP_V4_PKTINFO)
        ok = udpSetBoolOpt(sock, IPPROTO_IP, IP_PKTINFO);
#elif defined(UDP_V4_DSTADDR)
        ok = udpSetBoolOpt(sock, IPPROTO_IP, IP_RECVDSTADDR);
#endif
    }
#if defined(_WIN32)
    if (ok) {
        udpFetchWsaRecvMsg(sock);
        ok = s_udpWsaRecvMsg != NULL;
    }
#endif
    if (!ok && SDL_CompareAndSwapAtomicInt(&s_udpRecvLocalWarned, 0, 1)) {
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "the %s server socket cannot report the address each datagram "
            "arrived on; replies go out from the address the system picks",
            sockFamily == AF_INET6 ? "IPv6" : "IPv4");
    }
#endif
}

#if !defined(__EMSCRIPTEN__)

/* What the control messages of one received datagram said about its
 * destination. */
typedef struct {
    int      haveV6;
    uint8_t  v6[16];
    uint32_t v6IfIndex;
    int      haveV4;
    uint8_t  v4[4];
} UdpLocalSeen;

/* Reads one control message into *seen when it is one of the
 * packet-information kinds; ignores any other. */
static void udpNoteLocalCmsg(int level, int type, const unsigned char *data,
                             size_t dataLen, UdpLocalSeen *seen) {
#if defined(UDP_V6_PKTINFO)
    if (level == IPPROTO_IPV6 && type == IPV6_PKTINFO &&
        dataLen >= sizeof(struct in6_pktinfo)) {
        struct in6_pktinfo pi;
        memcpy(&pi, data, sizeof(pi));
        memcpy(seen->v6, &pi.ipi6_addr, 16);
        seen->v6IfIndex = (uint32_t)pi.ipi6_ifindex;
        seen->haveV6 = 1;
        return;
    }
#endif
#if defined(UDP_V4_PKTINFO)
    if (level == IPPROTO_IP && type == IP_PKTINFO &&
        dataLen >= sizeof(struct in_pktinfo)) {
        struct in_pktinfo pi;
        memcpy(&pi, data, sizeof(pi));
#if defined(_WIN32)
        memcpy(seen->v4, &pi.ipi_addr, 4);
#else
        /* ipi_spec_dst is the local address a reply should come from, which
         * Linux fills in and which differs from the header destination
         * ipi_addr for a broadcast; macOS leaves it zero. */
        if (pi.ipi_spec_dst.s_addr != 0) {
            memcpy(seen->v4, &pi.ipi_spec_dst, 4);
        } else {
            memcpy(seen->v4, &pi.ipi_addr, 4);
        }
#endif
        seen->haveV4 = 1;
        return;
    }
#elif defined(UDP_V4_DSTADDR)
    if (level == IPPROTO_IP && type == IP_RECVDSTADDR &&
        dataLen >= sizeof(struct in_addr)) {
        memcpy(seen->v4, data, 4);
        seen->haveV4 = 1;
        return;
    }
#endif
    (void)level;
    (void)type;
    (void)data;
    (void)dataLen;
    (void)seen;
}

/* True for an address a datagram can be sent from: not unspecified, not
 * multicast, and for IPv4 not the limited broadcast or the reserved block
 * above the multicast one. */
static int udpLocalAddrUsable(const BoloAddr *a) {
    uint8_t v4[4];
    const uint8_t *b;
    if (boloAddrIsUnspecified(a)) {
        return 0;
    }
    if (boloAddrV4Bytes(a, v4)) {
        return v4[0] < 224;
    }
    b = boloAddrV6Bytes(a);
    return b != NULL && b[0] != 0xff;
}

/* Writes the destination the control messages named into *out, port 0, in
 * `family` (the socket's family, so IPv4 on a dual-stack socket is mapped).
 * An IPv4 report wins over an IPv6 one, because on Linux the IPv4 report
 * carries the reply address rather than the header destination. *out is
 * left empty when nothing usable was reported. */
static void udpLocalFinish(const UdpLocalSeen *seen, int family, BoloAddr *out) {
    BoloAddr tmp;

    boloAddrClear(out);
    boloAddrClear(&tmp);
    if (seen->haveV4) {
        struct in_addr in4;
        memcpy(&in4, seen->v4, 4);
        boloAddrFromV4(in4, 0, &tmp);
    } else if (seen->haveV6) {
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        memcpy(&sin6.sin6_addr, seen->v6, 16);
        if (!boloAddrFromSockaddr((const struct sockaddr *)&sin6,
                                  (socklen_t)sizeof(sin6), &tmp)) {
            return;
        }
        if (boloAddrIsV6LinkLocal(&tmp)) {
            ((struct sockaddr_in6 *)&tmp.ss)->sin6_scope_id = seen->v6IfIndex;
        }
    } else {
        return;
    }
    if (!udpLocalAddrUsable(&tmp)) {
        return;
    }
    if (!boloAddrToFamily(&tmp, family, out)) {
        boloAddrClear(out);
    }
}

#endif /* !__EMSCRIPTEN__ */

int udpRecvFromLocal(SOCKET sock, uint8_t *buf, int maxLen, BoloAddr *fromAddr,
                     BoloAddr *localAddr) {
#if defined(__EMSCRIPTEN__)
    boloAddrClear(localAddr);
    return udpRecvFrom(sock, buf, maxLen, fromAddr);
#elif defined(_WIN32)
    struct sockaddr_storage ss;
    union {
        WSACMSGHDR align;
        char       bytes[UDP_CMSG_BUF_LEN];
    } ctrl;
    WSAMSG msg;
    WSABUF wbuf;
    WSACMSGHDR *cm = NULL;
    DWORD got = 0;
    UdpLocalSeen seen;

    boloAddrClear(localAddr);
    if (s_udpWsaRecvMsg == NULL) {
        return udpRecvFrom(sock, buf, maxLen, fromAddr);
    }
    memset(&ss, 0, sizeof(ss));
    memset(&ctrl, 0, sizeof(ctrl));
    memset(&msg, 0, sizeof(msg));
    memset(&seen, 0, sizeof(seen));
    wbuf.buf = (CHAR *)buf;
    wbuf.len = (ULONG)maxLen;
    msg.name = (LPSOCKADDR)&ss;
    msg.namelen = (INT)sizeof(ss);
    msg.lpBuffers = &wbuf;
    msg.dwBufferCount = 1;
    msg.Control.buf = ctrl.bytes;
    msg.Control.len = (ULONG)sizeof(ctrl.bytes);
    msg.dwFlags = 0;
    if (s_udpWsaRecvMsg(sock, &msg, &got, NULL, NULL) == SOCKET_ERROR) {
        return -1;
    }
    if (!boloAddrFromSockaddr((const struct sockaddr *)&ss,
                              (socklen_t)msg.namelen, fromAddr)) {
        return -1;
    }
    for (cm = WSA_CMSG_FIRSTHDR(&msg); cm != NULL;
         cm = WSA_CMSG_NXTHDR(&msg, cm)) {
        size_t hdr = (size_t)WSA_CMSG_LEN(0);
        if ((size_t)cm->cmsg_len < hdr) {
            break;
        }
        udpNoteLocalCmsg(cm->cmsg_level, cm->cmsg_type,
                         (const unsigned char *)WSA_CMSG_DATA(cm),
                         (size_t)cm->cmsg_len - hdr, &seen);
    }
    udpLocalFinish(&seen, boloAddrFamily(fromAddr), localAddr);
    return (int)got;
#else
    struct sockaddr_storage ss;
    union {
        struct cmsghdr align;
        unsigned char  bytes[UDP_CMSG_BUF_LEN];
    } ctrl;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *cm = NULL;
    ssize_t ret;
    UdpLocalSeen seen;

    boloAddrClear(localAddr);
    memset(&ss, 0, sizeof(ss));
    memset(&ctrl, 0, sizeof(ctrl));
    memset(&msg, 0, sizeof(msg));
    memset(&seen, 0, sizeof(seen));
    iov.iov_base = buf;
    iov.iov_len = (size_t)maxLen;
    msg.msg_name = &ss;
    msg.msg_namelen = (socklen_t)sizeof(ss);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.bytes;
    msg.msg_controllen = sizeof(ctrl.bytes);
    ret = recvmsg(sock, &msg, 0);
    if (ret < 0) {
        return -1;
    }
    if (!boloAddrFromSockaddr((const struct sockaddr *)&ss, msg.msg_namelen,
                              fromAddr)) {
        return -1;
    }
    for (cm = CMSG_FIRSTHDR(&msg); cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {
        size_t hdr = (size_t)CMSG_LEN(0);
        if ((size_t)cm->cmsg_len < hdr) {
            break;
        }
        udpNoteLocalCmsg(cm->cmsg_level, cm->cmsg_type, CMSG_DATA(cm),
                         (size_t)cm->cmsg_len - hdr, &seen);
    }
    udpLocalFinish(&seen, boloAddrFamily(fromAddr), localAddr);
    return (int)ret;
#endif
}

#if !defined(__EMSCRIPTEN__)

/* Outcomes of udpSendFromSource. */
#define UDP_SRC_SENT     0   /* sent, or the socket would block */
#define UDP_SRC_UNUSABLE 1   /* no control message names this source here */
#define UDP_SRC_FAILED   2   /* the send failed; *err holds the error number */

/* One sendmsg / WSASendMsg of buf to dest with a single control message. */
static int udpSendWithCmsg(SOCKET sock, const uint8_t *buf, int len,
                           BoloAddr *dest, int level, int type,
                           const void *data, size_t dataLen, int *err) {
#if defined(_WIN32)
    union {
        WSACMSGHDR align;
        char       bytes[UDP_CMSG_BUF_LEN];
    } ctrl;
    WSAMSG msg;
    WSABUF wbuf;
    WSACMSGHDR *cm = NULL;
    DWORD sent = 0;

    memset(&ctrl, 0, sizeof(ctrl));
    memset(&msg, 0, sizeof(msg));
    wbuf.buf = (CHAR *)buf;
    wbuf.len = (ULONG)len;
    msg.name = (LPSOCKADDR)&dest->ss;
    msg.namelen = (INT)dest->len;
    msg.lpBuffers = &wbuf;
    msg.dwBufferCount = 1;
    msg.Control.buf = ctrl.bytes;
    msg.Control.len = (ULONG)WSA_CMSG_SPACE(dataLen);
    cm = (WSACMSGHDR *)ctrl.bytes;
    cm->cmsg_level = level;
    cm->cmsg_type = type;
    cm->cmsg_len = WSA_CMSG_LEN(dataLen);
    memcpy(WSA_CMSG_DATA(cm), data, dataLen);
    if (WSASendMsg(sock, &msg, 0, &sent, NULL, NULL) == 0) {
        return UDP_SRC_SENT;
    }
    *err = WSAGetLastError();
    if (*err == WSAEWOULDBLOCK) {
        return UDP_SRC_SENT;
    }
    return UDP_SRC_FAILED;
#else
    union {
        struct cmsghdr align;
        unsigned char  bytes[UDP_CMSG_BUF_LEN];
    } ctrl;
    struct msghdr msg;
    struct iovec iov;
    struct cmsghdr *cm = NULL;

    memset(&ctrl, 0, sizeof(ctrl));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = (void *)buf;
    iov.iov_len = (size_t)len;
    msg.msg_name = &dest->ss;
    msg.msg_namelen = dest->len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctrl.bytes;
    msg.msg_controllen = CMSG_SPACE(dataLen);
    cm = CMSG_FIRSTHDR(&msg);
    if (cm == NULL) {
        return UDP_SRC_UNUSABLE;
    }
    cm->cmsg_level = level;
    cm->cmsg_type = type;
    cm->cmsg_len = CMSG_LEN(dataLen);
    memcpy(CMSG_DATA(cm), data, dataLen);
    if (sendmsg(sock, &msg, 0) >= 0) {
        return UDP_SRC_SENT;
    }
    *err = errno;
    if (*err == EAGAIN || *err == EWOULDBLOCK) {
        return UDP_SRC_SENT;
    }
    return UDP_SRC_FAILED;
#endif
}

#if defined(UDP_V6_PKTINFO)
/* An IPV6_PKTINFO send naming the 16 address bytes as the source. The
 * interface index is set only for a link-local source. */
static int udpSendFromV6(SOCKET sock, const uint8_t *buf, int len,
                         BoloAddr *dest, const BoloAddr *srcAddr, int *err) {
    struct in6_pktinfo pi;
    const uint8_t *b = boloAddrV6Bytes(srcAddr);

    if (b == NULL) {
        return UDP_SRC_UNUSABLE;
    }
    memset(&pi, 0, sizeof(pi));
    memcpy(&pi.ipi6_addr, b, 16);
    if (boloAddrIsV6LinkLocal(srcAddr)) {
        pi.ipi6_ifindex =
            ((const struct sockaddr_in6 *)&srcAddr->ss)->sin6_scope_id;
    }
    return udpSendWithCmsg(sock, buf, len, dest, IPPROTO_IPV6, IPV6_PKTINFO,
                           &pi, sizeof(pi), err);
}
#endif

/* Sends buf to dest (already in the socket's family) naming srcAddr as the
 * source. */
static int udpSendFromSource(SOCKET sock, int sockFamily, const uint8_t *buf,
                             int len, BoloAddr *dest, const BoloAddr *srcAddr,
                             int *err) {
    if (boloAddrIsV4(dest)) {
        uint8_t v4[4];
        if (!boloAddrV4Bytes(srcAddr, v4)) {
            return UDP_SRC_UNUSABLE;
        }
#if defined(UDP_V4_PKTINFO)
        {
            struct in_pktinfo pi;
            memset(&pi, 0, sizeof(pi));
#if defined(_WIN32)
            memcpy(&pi.ipi_addr, v4, 4);
#else
            memcpy(&pi.ipi_spec_dst, v4, 4);
#endif
            (void)sockFamily;
            return udpSendWithCmsg(sock, buf, len, dest, IPPROTO_IP,
                                   IP_PKTINFO, &pi, sizeof(pi), err);
        }
#else
        if (sockFamily == AF_INET6) {
#if defined(UDP_V6_PKTINFO)
            BoloAddr mapped;
            if (!boloAddrToFamily(srcAddr, AF_INET6, &mapped)) {
                return UDP_SRC_UNUSABLE;
            }
            return udpSendFromV6(sock, buf, len, dest, &mapped, err);
#else
            return UDP_SRC_UNUSABLE;
#endif
        }
#if defined(UDP_V4_DSTADDR) && defined(IP_SENDSRCADDR)
        {
            struct in_addr src;
            memcpy(&src, v4, 4);
            return udpSendWithCmsg(sock, buf, len, dest, IPPROTO_IP,
                                   IP_SENDSRCADDR, &src, sizeof(src), err);
        }
#else
        return UDP_SRC_UNUSABLE;
#endif
#endif
    }
#if defined(UDP_V6_PKTINFO)
    if (boloAddrFamily(srcAddr) != AF_INET6 || boloAddrIsMappedV4(srcAddr)) {
        return UDP_SRC_UNUSABLE;
    }
    return udpSendFromV6(sock, buf, len, dest, srcAddr, err);
#else
    (void)sock;
    (void)sockFamily;
    (void)buf;
    (void)len;
    (void)err;
    return UDP_SRC_UNUSABLE;
#endif
}

#endif /* !__EMSCRIPTEN__ */

void udpSendToFrom(SOCKET sock, int sockFamily, const uint8_t *buf, int len,
                   const BoloAddr *addr, const BoloAddr *localAddr) {
#if defined(__EMSCRIPTEN__)
    (void)localAddr;
    udpSendTo(sock, sockFamily, buf, len, addr);
#else
    BoloAddr dest;
    int err = 0;
    int rc;

    if (localAddr == NULL || boloAddrIsUnspecified(localAddr)) {
        udpSendTo(sock, sockFamily, buf, len, addr);
        return;
    }
    if (!udpPrepareSend(sockFamily, len, addr, &dest)) {
        return;
    }
    rc = udpSendFromSource(sock, sockFamily, buf, len, &dest, localAddr, &err);
    if (rc == UDP_SRC_SENT) {
        return;
    }
    if (rc == UDP_SRC_FAILED &&
        SDL_CompareAndSwapAtomicInt(&s_udpSendFromWarned, 0, 1)) {
        char destText[BOLO_ADDR_STR_LEN];
        char srcText[BOLO_ADDR_STR_LEN];
        if (!boloAddrFormat(&dest, destText, sizeof(destText))) {
            snprintf(destText, sizeof(destText), "(unprintable)");
        }
        if (!boloAddrFormatHost(localAddr, srcText, sizeof(srcText))) {
            snprintf(srcText, sizeof(srcText), "(unprintable)");
        }
        WB_LOG_WARN(WB_LOG_CAT_NET,
            "udp send to %s from %s failed (error %d); sent again without a "
            "source address (later failures of this kind are not logged)",
            destText, srcText, err);
    }
    /* A reply from an address the peer did not contact is better than no
     * reply at all. */
    sendto(sock, (const char *)buf, len, 0,
           (const struct sockaddr *)&dest.ss, dest.len);
#endif
}

/* Receive a UDP datagram. Returns bytes received, or -1 if none available or
 * the source is neither IPv4 nor IPv6. *fromAddr is the source as the socket
 * reported it: on a dual-stack socket an IPv4 peer is a mapped address, so
 * callers compare through boloAddrEqual and the keys, never the raw bytes. */
int udpRecvFrom(SOCKET sock, uint8_t *buf, int maxLen, BoloAddr *fromAddr) {
    struct sockaddr_storage ss;
    socklen_t addrLen = (socklen_t)sizeof(ss);
    int ret;

    memset(&ss, 0, sizeof(ss));
    ret = recvfrom(sock, (char *)buf, maxLen, 0,
                   (struct sockaddr *)&ss, &addrLen);
    if (ret == SOCKET_ERROR) {
        return -1;
    }
    if (!boloAddrFromSockaddr((const struct sockaddr *)&ss, addrLen,
                              fromAddr)) {
        return -1;
    }
    return ret;
}
