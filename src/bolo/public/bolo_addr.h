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
*Name:          Bolo Address
*Filename:      bolo_addr.h
*Purpose:
*  One network address type for IPv4 and IPv6 (BoloAddr),
*  the helpers that compare, classify, format and parse it,
*  and the resolver that turns a host string into one.
*
*  An IPv4-mapped IPv6 address (::ffff:a.b.c.d) compares
*  equal to the same plain IPv4 address and always prints
*  as plain IPv4. A link-local IPv6 address keeps its scope
*  id, which takes part in comparisons and prints as a
*  %scope suffix.
*
*  Header-only (static inline) so every target that sees
*  src/bolo/public/ can use it without a library to link.
*********************************************************/

#ifndef BOLO_ADDR_H
#define BOLO_ADDR_H

#include <stdint.h>
#include <stdio.h>   /* snprintf */
#include <stdlib.h>  /* strtoul */
#include <string.h>  /* memset, memcpy, memcmp, strchr, strlen */

#include "platform_net.h"

/* Longest "[v6%scope]:port" text plus terminator. A link-local address is at
 * most 39 characters, its scope at most 16 with the '%', and the brackets and
 * ":65535" 8 more; any other address is at most 45 + 8. */
#define BOLO_ADDR_STR_LEN 64
/* Canonical key size: family 1 + address 16 + scope id 4. */
#define BOLO_ADDR_KEY_LEN 21

typedef struct {
    struct sockaddr_storage ss;
    socklen_t               len;   /* 0 means "no address" */
} BoloAddr;

/* Flags for the Apple getaddrinfo call in bolo_resolve_addr. AI_DEFAULT is
 * declared only in the full Darwin C level; fall back to AI_ADDRCONFIG. */
#if defined(__APPLE__)
#  if defined(AI_DEFAULT)
#    define BOLO_ADDR_AI_DEFAULT AI_DEFAULT
#  elif defined(AI_ADDRCONFIG)
#    define BOLO_ADDR_AI_DEFAULT AI_ADDRCONFIG
#  else
#    define BOLO_ADDR_AI_DEFAULT 0
#  endif
#endif

#if defined(AI_ADDRCONFIG)
#  define BOLO_ADDR_AI_ADDRCONFIG AI_ADDRCONFIG
#else
#  define BOLO_ADDR_AI_ADDRCONFIG 0
#endif

/* Zeroes the address, leaving it empty. */
static inline void boloAddrClear(BoloAddr *a) {
    if (a != NULL) {
        memset(a, 0, sizeof(*a));
    }
}

/* Copies an AF_INET or AF_INET6 sockaddr into *out. Returns 1 on success, 0
 * for another family, a short length or a NULL argument (*out untouched). */
static inline int boloAddrFromSockaddr(const struct sockaddr *sa, socklen_t len,
                                       BoloAddr *out) {
    if (sa == NULL || out == NULL) {
        return 0;
    }
    if (sa->sa_family == AF_INET &&
        (size_t)len >= sizeof(struct sockaddr_in)) {
        boloAddrClear(out);
        memcpy(&out->ss, sa, sizeof(struct sockaddr_in));
        out->len = (socklen_t)sizeof(struct sockaddr_in);
        return 1;
    }
    if (sa->sa_family == AF_INET6 &&
        (size_t)len >= sizeof(struct sockaddr_in6)) {
        boloAddrClear(out);
        memcpy(&out->ss, sa, sizeof(struct sockaddr_in6));
        out->len = (socklen_t)sizeof(struct sockaddr_in6);
        return 1;
    }
    return 0;
}

/* AF_INET, AF_INET6, or 0 when the address is empty. */
static inline int boloAddrFamily(const BoloAddr *a) {
    if (a == NULL || a->len == 0) {
        return 0;
    }
    if (a->ss.ss_family == AF_INET || a->ss.ss_family == AF_INET6) {
        return (int)a->ss.ss_family;
    }
    return 0;
}

/* The port in host byte order; 0 for an empty address. */
static inline uint16_t boloAddrPort(const BoloAddr *a) {
    switch (boloAddrFamily(a)) {
    case AF_INET:
        return (uint16_t)ntohs(((const struct sockaddr_in *)&a->ss)->sin_port);
    case AF_INET6:
        return (uint16_t)ntohs(((const struct sockaddr_in6 *)&a->ss)->sin6_port);
    default:
        return 0;
    }
}

/* Sets the port (host byte order). No effect on an empty address. */
static inline void boloAddrSetPort(BoloAddr *a, uint16_t port) {
    switch (boloAddrFamily(a)) {
    case AF_INET:
        ((struct sockaddr_in *)&a->ss)->sin_port = htons(port);
        break;
    case AF_INET6:
        ((struct sockaddr_in6 *)&a->ss)->sin6_port = htons(port);
        break;
    default:
        break;
    }
}

/* The 16 address bytes of an AF_INET6 address, or NULL for any other. */
static inline const uint8_t *boloAddrV6Bytes(const BoloAddr *a) {
    if (boloAddrFamily(a) != AF_INET6) {
        return NULL;
    }
    return (const uint8_t *)&((const struct sockaddr_in6 *)&a->ss)->sin6_addr;
}

/* True for an AF_INET6 address of the form ::ffff:a.b.c.d. */
static inline int boloAddrIsMappedV4(const BoloAddr *a) {
    static const uint8_t prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    const uint8_t *b = boloAddrV6Bytes(a);
    return b != NULL && memcmp(b, prefix, sizeof(prefix)) == 0;
}

/* Writes the 4 IPv4 bytes of a plain or mapped IPv4 address to out and
 * returns 1; returns 0 for any other address. */
static inline int boloAddrV4Bytes(const BoloAddr *a, uint8_t out[4]) {
    if (boloAddrFamily(a) == AF_INET) {
        memcpy(out, &((const struct sockaddr_in *)&a->ss)->sin_addr, 4);
        return 1;
    }
    if (boloAddrIsMappedV4(a)) {
        memcpy(out, boloAddrV6Bytes(a) + 12, 4);
        return 1;
    }
    return 0;
}

/* True for AF_INET and for an IPv4-mapped IPv6 address. */
static inline int boloAddrIsV4(const BoloAddr *a) {
    uint8_t v4[4];
    return boloAddrV4Bytes(a, v4);
}

/* Writes the IPv4 address of a plain or mapped IPv4 address to *out and
 * returns 1; returns 0 with *out zeroed for any other address. For the 4-byte
 * address fields on the wire and in the brain API. */
static inline int boloAddrV4(const BoloAddr *a, struct in_addr *out) {
    uint8_t v4[4];
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!boloAddrV4Bytes(a, v4)) {
        return 0;
    }
    memcpy(out, v4, 4);
    return 1;
}

/* Builds an AF_INET address from a 4-byte IPv4 address (network order) and a
 * port (host order), as the 4-byte address fields from the tracker carry. */
static inline void boloAddrFromV4(struct in_addr v4, uint16_t port,
                                  BoloAddr *out) {
    struct sockaddr_in sin;
    if (out == NULL) {
        return;
    }
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr = v4;
    sin.sin_port = htons(port);
    boloAddrClear(out);
    memcpy(&out->ss, &sin, sizeof(sin));
    out->len = (socklen_t)sizeof(sin);
}

/* Converts an address for sending on a socket of `family`. For AF_INET6 an
 * AF_INET address becomes the mapped form ::ffff:a.b.c.d with the same port,
 * and an AF_INET6 address is copied. For AF_INET an AF_INET address is copied
 * and a mapped IPv6 address becomes plain AF_INET; any other IPv6 address
 * cannot be sent on that socket and gives 0. Returns 1 on success, 0 on
 * failure (an empty address or another family included) with *out cleared.
 * in and out may be the same object. */
static inline int boloAddrToFamily(const BoloAddr *in, int family,
                                   BoloAddr *out) {
    BoloAddr tmp;
    uint8_t v4[4];
    int have;
    if (out == NULL) {
        return 0;
    }
    boloAddrClear(&tmp);
    have = boloAddrFamily(in);
    if (have == 0) {
        boloAddrClear(out);
        return 0;
    }
    if (family == AF_INET6) {
        if (have == AF_INET6) {
            tmp = *in;
        } else {
            struct sockaddr_in6 sin6;
            uint8_t *b = (uint8_t *)&sin6.sin6_addr;
            memset(&sin6, 0, sizeof(sin6));
            sin6.sin6_family = AF_INET6;
            sin6.sin6_port = htons(boloAddrPort(in));
            memcpy(v4, &((const struct sockaddr_in *)&in->ss)->sin_addr, 4);
            b[10] = 0xff;
            b[11] = 0xff;
            memcpy(b + 12, v4, 4);
            memcpy(&tmp.ss, &sin6, sizeof(sin6));
            tmp.len = (socklen_t)sizeof(sin6);
        }
    } else if (family == AF_INET) {
        if (have == AF_INET) {
            tmp = *in;
        } else if (boloAddrIsMappedV4(in)) {
            struct in_addr in4;
            memcpy(&in4, boloAddrV6Bytes(in) + 12, 4);
            boloAddrFromV4(in4, boloAddrPort(in), &tmp);
        } else {
            boloAddrClear(out);
            return 0;
        }
    } else {
        boloAddrClear(out);
        return 0;
    }
    *out = tmp;
    return 1;
}

/* The any address of `family` (:: for AF_INET6, 0.0.0.0 for AF_INET) with
 * `port` (host order). Any other family leaves *out empty. */
static inline void boloAddrAny(int family, uint16_t port, BoloAddr *out) {
    if (out == NULL) {
        return;
    }
    boloAddrClear(out);
    if (family == AF_INET6) {
        struct sockaddr_in6 sin6;
        memset(&sin6, 0, sizeof(sin6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(port);
        memcpy(&out->ss, &sin6, sizeof(sin6));
        out->len = (socklen_t)sizeof(sin6);
    } else if (family == AF_INET) {
        struct in_addr any;
        memset(&any, 0, sizeof(any));
        boloAddrFromV4(any, port, out);
    }
}

/* True for an empty address, 0.0.0.0, :: and ::ffff:0.0.0.0. */
static inline int boloAddrIsUnspecified(const BoloAddr *a) {
    static const uint8_t zero[16] = { 0 };
    uint8_t v4[4];
    const uint8_t *b;
    if (boloAddrFamily(a) == 0) {
        return 1;
    }
    if (boloAddrV4Bytes(a, v4)) {
        return memcmp(v4, zero, 4) == 0;
    }
    b = boloAddrV6Bytes(a);
    return b != NULL && memcmp(b, zero, 16) == 0;
}

/* True for 127.0.0.0/8 (plain or mapped) and ::1. */
static inline int boloAddrIsLoopback(const BoloAddr *a) {
    static const uint8_t v6Loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0, 0, 0, 1 };
    uint8_t v4[4];
    const uint8_t *b;
    if (boloAddrV4Bytes(a, v4)) {
        return v4[0] == 127;
    }
    b = boloAddrV6Bytes(a);
    return b != NULL && memcmp(b, v6Loopback, 16) == 0;
}

/* True for an IPv6 address in fe80::/10. */
static inline int boloAddrIsV6LinkLocal(const BoloAddr *a) {
    const uint8_t *b = boloAddrV6Bytes(a);
    return b != NULL && b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
}

/* True for fe80::/10, and for 169.254.0.0/16 plain or mapped. */
static inline int boloAddrIsLinkLocal(const BoloAddr *a) {
    uint8_t v4[4];
    if (boloAddrV4Bytes(a, v4)) {
        return v4[0] == 169 && v4[1] == 254;
    }
    return boloAddrIsV6LinkLocal(a);
}

/* Canonical key: byte 0 is 4 or 6 (0 and all zero for an empty address).
 * IPv4 and mapped IPv4 put the 4 address bytes in bytes 1..4. IPv6 puts the
 * 16 address bytes in 1..16 and, for a link-local address only, the scope id
 * in network byte order in 17..20. The port is not part of the key. */
static inline void boloAddrKey(const BoloAddr *a, uint8_t out[BOLO_ADDR_KEY_LEN]) {
    uint8_t v4[4];
    const uint8_t *b;
    memset(out, 0, BOLO_ADDR_KEY_LEN);
    if (boloAddrV4Bytes(a, v4)) {
        out[0] = 4;
        memcpy(out + 1, v4, 4);
        return;
    }
    b = boloAddrV6Bytes(a);
    if (b == NULL) {
        return;
    }
    out[0] = 6;
    memcpy(out + 1, b, 16);
    if (boloAddrIsV6LinkLocal(a)) {
        uint32_t scope =
            (uint32_t)((const struct sockaddr_in6 *)&a->ss)->sin6_scope_id;
        out[17] = (uint8_t)((scope >> 24) & 0xff);
        out[18] = (uint8_t)((scope >> 16) & 0xff);
        out[19] = (uint8_t)((scope >> 8) & 0xff);
        out[20] = (uint8_t)(scope & 0xff);
    }
}

/* The key with the last 8 address bytes of an IPv6 address zeroed, so every
 * address in one /64 gives the same key. IPv4 keeps the whole address. */
static inline void boloAddrPrefixKey(const BoloAddr *a,
                                     uint8_t out[BOLO_ADDR_KEY_LEN]) {
    boloAddrKey(a, out);
    if (out[0] == 6) {
        memset(out + 9, 0, 8);
    }
}

/* True when both addresses name the same host (address and scope id). */
static inline int boloAddrSameHost(const BoloAddr *a, const BoloAddr *b) {
    uint8_t ka[BOLO_ADDR_KEY_LEN];
    uint8_t kb[BOLO_ADDR_KEY_LEN];
    boloAddrKey(a, ka);
    boloAddrKey(b, kb);
    return memcmp(ka, kb, BOLO_ADDR_KEY_LEN) == 0;
}

/* True when the host and the port both match. Two empty addresses are equal. */
static inline int boloAddrEqual(const BoloAddr *a, const BoloAddr *b) {
    return boloAddrSameHost(a, b) && boloAddrPort(a) == boloAddrPort(b);
}

/* Writes the numeric host with no port and no brackets. A mapped IPv4 address
 * prints as plain dotted IPv4. A link-local IPv6 address with a non-zero scope
 * id gets a %scope suffix: the interface name where the platform reports one,
 * else the number. Returns 1 on success; 0 on failure, with out set to the
 * empty string when outSz > 0. */
static inline int boloAddrFormatHost(const BoloAddr *a, char *out, size_t outSz) {
    char text[BOLO_ADDR_STR_LEN];
    char scope[BOLO_ADDR_STR_LEN];
    uint8_t v4[4];
    int ok = 0;

    if (out == NULL || outSz == 0) {
        return 0;
    }
    out[0] = '\0';
    text[0] = '\0';
    scope[0] = '\0';

    if (boloAddrV4Bytes(a, v4)) {
        struct in_addr in4;
        memcpy(&in4, v4, 4);
        ok = inet_ntop(AF_INET, &in4, text, sizeof(text)) != NULL;
    } else if (boloAddrFamily(a) == AF_INET6) {
        struct sockaddr_in6 sin6;
        memcpy(&sin6, &a->ss, sizeof(sin6));
        ok = inet_ntop(AF_INET6, &sin6.sin6_addr, text, sizeof(text)) != NULL;
        if (ok && boloAddrIsV6LinkLocal(a) && sin6.sin6_scope_id != 0) {
            /* getnameinfo prints "addr%scope", naming the interface where it
             * can; only the part after the '%' is taken from it. */
            char full[BOLO_ADDR_STR_LEN];
            const char *pct = NULL;
            full[0] = '\0';
            if (getnameinfo((const struct sockaddr *)&sin6,
                            (socklen_t)sizeof(sin6), full,
                            (unsigned int)sizeof(full), NULL, 0,
                            NI_NUMERICHOST) == 0) {
                pct = strchr(full, '%');
            }
            if (pct != NULL && pct[1] != '\0') {
                ok = snprintf(scope, sizeof(scope), "%s", pct) <
                     (int)sizeof(scope);
            } else {
                ok = snprintf(scope, sizeof(scope), "%%%lu",
                              (unsigned long)sin6.sin6_scope_id) <
                     (int)sizeof(scope);
            }
        }
    }

    if (ok) {
        int n = snprintf(out, outSz, "%s%s", text, scope);
        if (n < 0 || (size_t)n >= outSz) {
            ok = 0;
        }
    }
    if (!ok) {
        out[0] = '\0';
    }
    return ok;
}

/* Writes host and port: "1.2.3.4:27500", "[2001:db8::1]:27500",
 * "[fe80::1%en0]:27500". A mapped IPv4 address prints as "1.2.3.4:27500".
 * Same return convention as boloAddrFormatHost. */
static inline int boloAddrFormat(const BoloAddr *a, char *out, size_t outSz) {
    char host[BOLO_ADDR_STR_LEN];
    int n;
    if (out == NULL || outSz == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!boloAddrFormatHost(a, host, sizeof(host))) {
        return 0;
    }
    if (boloAddrIsV4(a)) {
        n = snprintf(out, outSz, "%s:%u", host, (unsigned)boloAddrPort(a));
    } else {
        n = snprintf(out, outSz, "[%s]:%u", host, (unsigned)boloAddrPort(a));
    }
    if (n < 0 || (size_t)n >= outSz) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Parses a numeric host with no brackets and no port: a dotted-quad IPv4
 * address, or an IPv6 literal with an optional %scope suffix (an interface
 * name or a number). Never looks a name up. The port is set to port. Returns
 * 1 on success, 0 otherwise (*out untouched). */
static inline int boloAddrParseNumericHost(const char *host, uint16_t port,
                                           BoloAddr *out) {
    BoloAddr tmp;
    if (host == NULL || host[0] == '\0' || out == NULL) {
        return 0;
    }
    boloAddrClear(&tmp);
    if (strchr(host, ':') == NULL) {
        /* inet_pton accepts only the full dotted-quad form, unlike
         * getaddrinfo, which on some platforms also reads "127.1" or "1". */
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        if (inet_pton(AF_INET, host, &sin.sin_addr) != 1) {
            return 0;
        }
        if (!boloAddrFromSockaddr((const struct sockaddr *)&sin,
                                  (socklen_t)sizeof(sin), &tmp)) {
            return 0;
        }
    } else {
        /* getaddrinfo with AI_NUMERICHOST reads the %scope suffix, by name or
         * number, on every platform and never queries DNS. */
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        int ok;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET6;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_flags = AI_NUMERICHOST;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
            if (res != NULL) {
                freeaddrinfo(res);
            }
            return 0;
        }
        ok = boloAddrFromSockaddr(res->ai_addr, (socklen_t)res->ai_addrlen,
                                  &tmp);
        freeaddrinfo(res);
        if (!ok || boloAddrFamily(&tmp) != AF_INET6) {
            return 0;
        }
    }
    boloAddrSetPort(&tmp, port);
    *out = tmp;
    return 1;
}

/* Reads a decimal port of 1 to 5 digits in 0..65535. The text must be all
 * digits. Returns 1 and sets *port on success, 0 otherwise. */
static inline int boloAddrParsePortText(const char *text, uint16_t *port) {
    size_t i;
    unsigned long val;
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    for (i = 0; text[i] != '\0'; i++) {
        if (text[i] < '0' || text[i] > '9' || i >= 5) {
            return 0;
        }
    }
    val = strtoul(text, NULL, 10);
    if (val > 65535UL) {
        return 0;
    }
    *port = (uint16_t)val;
    return 1;
}

/* Parses numeric address text, never looking a name up. Accepts "1.2.3.4",
 * "1.2.3.4:port", "2001:db8::1", "[2001:db8::1]", "[2001:db8::1]:port", a
 * %scope suffix on an IPv6 literal with or without brackets, and
 * "::ffff:1.2.3.4" (kept as AF_INET6). Text with two or more ':' and no
 * brackets is read whole as an IPv6 literal, so it carries no port. With no
 * port the port is defaultPort. Returns 1 on success; 0 on failure with *out
 * cleared. */
static inline int boloAddrParse(const char *text, uint16_t defaultPort,
                                BoloAddr *out) {
    char host[BOLO_ADDR_STR_LEN];
    const char *hostStart;
    const char *portText = NULL;
    size_t hostLen;
    uint16_t port = defaultPort;

    if (out == NULL) {
        return 0;
    }
    boloAddrClear(out);
    if (text == NULL || text[0] == '\0') {
        return 0;
    }

    if (text[0] == '[') {
        const char *closeBracket = strchr(text, ']');
        if (closeBracket == NULL) {
            return 0;
        }
        hostStart = text + 1;
        hostLen = (size_t)(closeBracket - hostStart);
        if (closeBracket[1] == ':') {
            portText = closeBracket + 2;
        } else if (closeBracket[1] != '\0') {
            return 0;
        }
    } else {
        const char *first = strchr(text, ':');
        hostStart = text;
        if (first != NULL && strchr(first + 1, ':') == NULL) {
            /* Exactly one ':' — host:port. */
            hostLen = (size_t)(first - text);
            portText = first + 1;
        } else {
            hostLen = strlen(text);
        }
    }

    if (hostLen == 0 || hostLen >= sizeof(host)) {
        return 0;
    }
    memcpy(host, hostStart, hostLen);
    host[hostLen] = '\0';

    if (portText != NULL && !boloAddrParsePortText(portText, &port)) {
        return 0;
    }
    if (!boloAddrParseNumericHost(host, port, out)) {
        boloAddrClear(out);
        return 0;
    }
    return 1;
}

/* Picks the first result of family `want` from a getaddrinfo list, or NULL. */
static inline const struct addrinfo *boloAddrPickFamily(const struct addrinfo *res,
                                                        int want) {
    const struct addrinfo *ai;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == want && ai->ai_addr != NULL) {
            return ai;
        }
    }
    return NULL;
}

/* The familyHint every game lookup passes to bolo_resolve_addr. Names
 * resolve IPv4-first because every server released before IPv6 support
 * listens on IPv4 only, so a name with both record types must reach it. The
 * resolver falls back to the name's IPv6 address when it has no IPv4 one,
 * which is the case on an IPv6-only network. A numeric address is used as
 * written whatever the hint. */
#define BOLO_RESOLVE_PREFER AF_INET

/*
 * bolo_resolve_addr() — The one place a host string becomes a BoloAddr.
 *
 *   `host` is a numeric address (IPv4, or IPv6 with an optional %scope, with
 *   or without brackets, no port) or a DNS name. `familyHint` is AF_UNSPEC,
 *   AF_INET or AF_INET6: among the results the first of the hinted family is
 *   taken, else the first of the other family; with AF_UNSPEC the first
 *   AF_INET or AF_INET6 result in the order getaddrinfo returned them.
 *
 *   Off Apple platforms a numeric host is parsed directly and returned as
 *   written, whatever familyHint says; only a name reaches getaddrinfo.
 *
 *   On macOS and iOS getaddrinfo is always called with AF_UNSPEC and
 *   AI_DEFAULT, even for an IPv4 literal. On an IPv6-only network with NAT64
 *   that call is what turns "1.2.3.4" into the synthesised IPv6 address the
 *   gateway forwards; a direct parse would give an IPv4 address with no route.
 *
 *   When the getaddrinfo call fails with AI_ADDRCONFIG set it is retried once
 *   without that flag, because AI_ADDRCONFIG can fail "localhost" on a machine
 *   with no network interface up.
 *
 *   Returns 0 on success with the port written into *out; non-zero on failure
 *   with *out unchanged.
 */
static inline int bolo_resolve_addr(const char *host, uint16_t port,
                                    int familyHint, BoloAddr *out) {
    char name[256];
    size_t len;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    const struct addrinfo *pick = NULL;
    BoloAddr tmp;
    int flags;
    int rc;

    if (host == NULL || host[0] == '\0' || out == NULL) {
        return -1;
    }
    len = strlen(host);
    if (host[0] == '[') {
        if (len < 3 || host[len - 1] != ']') {
            return -1;
        }
        len -= 2;
        if (len >= sizeof(name)) {
            return -1;
        }
        memcpy(name, host + 1, len);
    } else {
        if (len >= sizeof(name)) {
            return -1;
        }
        memcpy(name, host, len);
    }
    name[len] = '\0';

#if !defined(__APPLE__)
    if (boloAddrParseNumericHost(name, port, &tmp)) {
        *out = tmp;
        return 0;
    }
    flags = BOLO_ADDR_AI_ADDRCONFIG;
#else
    flags = BOLO_ADDR_AI_DEFAULT;
#endif

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = flags;
    rc = getaddrinfo(name, NULL, &hints, &res);
    if ((rc != 0 || res == NULL) && (flags & BOLO_ADDR_AI_ADDRCONFIG) != 0) {
        if (res != NULL) {
            freeaddrinfo(res);
            res = NULL;
        }
        hints.ai_flags = flags & ~BOLO_ADDR_AI_ADDRCONFIG;
        rc = getaddrinfo(name, NULL, &hints, &res);
    }
    if (rc != 0 || res == NULL) {
        if (res != NULL) {
            freeaddrinfo(res);
        }
        return -1;
    }

    if (familyHint == AF_INET || familyHint == AF_INET6) {
        pick = boloAddrPickFamily(res, familyHint);
        if (pick == NULL) {
            pick = boloAddrPickFamily(res, familyHint == AF_INET ? AF_INET6
                                                                 : AF_INET);
        }
    } else {
        const struct addrinfo *ai;
        for (ai = res; ai != NULL; ai = ai->ai_next) {
            if ((ai->ai_family == AF_INET || ai->ai_family == AF_INET6) &&
                ai->ai_addr != NULL) {
                pick = ai;
                break;
            }
        }
    }

    rc = -1;
    if (pick != NULL &&
        boloAddrFromSockaddr(pick->ai_addr, (socklen_t)pick->ai_addrlen, &tmp)) {
        boloAddrSetPort(&tmp, port);
        *out = tmp;
        rc = 0;
    }
    freeaddrinfo(res);
    return rc;
}

#endif /* BOLO_ADDR_H */
