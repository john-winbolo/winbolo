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

/********************************************************
 *Name:          server_address_parse
 *Filename:      server_address_parse.h
 *Purpose:
 *  Splits a pasted server address into host + optional port. Used by the
 *  manual Internet/UDP join dialog so a value pasted into the address field
 *  ("host", "host:port", or "winbolo://host:port[/path]") is routed into the
 *  right input boxes.
 *
 *  Header-only (static inline) and free of SDL / ImGui / stdio so it can be
 *  shared between the GUI dialog and the unit-test binary, and pulled into a
 *  plain C translation unit, without a new CMake object or link dependency.
 *********************************************************/

#ifndef SERVER_ADDRESS_PARSE_H
#define SERVER_ADDRESS_PARSE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Bounded, always-NUL-terminating string copy (a tiny strlcpy so the header
 * stays SDL-free). No-op when dstSz is 0. */
static inline void wbAddrCopyBounded(char *dst, size_t dstSz, const char *src) {
    if (dstSz == 0) {
        return;
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < dstSz; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* True when s is non-empty, all decimal digits, and its value is in
 * 1..65535. */
static inline int wbAddrIsValidPort(const char *s) {
    if (s[0] == '\0') {
        return 0;
    }
    for (const char *q = s; *q; q++) {
        if (*q < '0' || *q > '9') {
            return 0;
        }
    }
    unsigned long val = strtoul(s, NULL, 10);
    return val >= 1 && val <= 65535;
}

/* Parses a pasted server address into host + optional decimal port string.
 * Accepts:  host  |  host:port  |  ip  |  ip:port  |  [ipv6]  |  [ipv6]:port
 *           |  ipv6  |  scheme://<any of these>[/...]
 * (e.g. "winbolo://1.2.3.4:5000", "winbolo://[2001:db8::1]:27500"). Leading
 * scheme and any trailing /path or surrounding whitespace are stripped.
 * - A bracketed host is returned without its brackets. A ":port" after the
 *   closing bracket is split off when it is a valid port; anything else after
 *   the bracket is dropped and the port left empty.
 * - Text with two or more ':' and no brackets is a bare IPv6 literal and is
 *   kept whole as the host, with no port split from it.
 * - Otherwise a single trailing ":port" is only split off when it is digits
 *   in the 1..65535 range; else the input is kept verbatim as the host.
 * An IPv6 %scope suffix (fe80::1%en0) stays part of the host.
 * Returns 1 and fills outHost (and outPort, empty if none) on success; 0 when
 * there is no usable host (or on a NULL argument), leaving outPort empty. */
static inline int parseServerAddressPaste(const char *in,
                                          char *outHost, size_t hostSz,
                                          char *outPort, size_t portSz) {
    /* Clear the port up front so a failed parse never leaves a stale value in
     * the caller's buffer — even when 'in' itself is NULL. */
    if (outPort && portSz != 0) {
        outPort[0] = '\0';
    }
    if (!in || !outHost || hostSz == 0 || !outPort || portSz == 0) {
        return 0;
    }

    /* Skip leading whitespace. */
    while (*in == ' ' || *in == '\t') {
        in++;
    }

    /* Strip a "scheme://" prefix (winbolo://, http://, ...). */
    const char *p = in;
    const char *sep = strstr(p, "://");
    if (sep) {
        p = sep + 3;
    }

    /* Copy host[:port] up to a path slash / whitespace / end. */
    char work[512];
    size_t n = 0;
    while (p[n] && p[n] != '/' && p[n] != ' ' && p[n] != '\t' &&
           p[n] != '\r' && p[n] != '\n' && n + 1 < sizeof(work)) {
        work[n] = p[n];
        n++;
    }
    work[n] = '\0';
    if (work[0] == '\0') {
        return 0;
    }

    /* "[host]" or "[host]:port": the host is what lies inside the brackets. */
    if (work[0] == '[') {
        char *closeBracket = strchr(work, ']');
        if (closeBracket) {
            *closeBracket = '\0';
            if (work[1] == '\0') {
                return 0;
            }
            if (closeBracket[1] == ':' && wbAddrIsValidPort(closeBracket + 2)) {
                wbAddrCopyBounded(outPort, portSz, closeBracket + 2);
            }
            wbAddrCopyBounded(outHost, hostSz, work + 1);
            return 1;
        }
    }

    /* Two or more ':' without brackets is a bare IPv6 literal, which cannot
     * carry a port: keep it whole. */
    char *colon = strrchr(work, ':');
    if (colon && colon != strchr(work, ':')) {
        wbAddrCopyBounded(outHost, hostSz, work);
        return 1;
    }

    /* Split a single trailing ":port" only if it is a valid port number. */
    if (colon && wbAddrIsValidPort(colon + 1)) {
        wbAddrCopyBounded(outPort, portSz, colon + 1);
        *colon = '\0';
    }
    if (work[0] == '\0') {
        return 0;
    }

    wbAddrCopyBounded(outHost, hostSz, work);
    return 1;
}

/* Appends src to dst at *len, bounded by dstSz, keeping dst NUL-terminated. */
static inline void wbAddrAppend(char *dst, size_t dstSz, size_t *len,
                                const char *src) {
    while (*src && *len + 1 < dstSz) {
        dst[(*len)++] = *src++;
    }
    dst[*len] = '\0';
}

/* Writes the text form of a server address: "host:port", or "[host]:port"
 * when host contains a ':' (an IPv6 literal). With port 0 only the host is
 * written, still bracketed when it contains a ':'. Output is truncated to
 * outSz and always NUL-terminated; no-op when outSz is 0. */
static inline void formatServerAddressHostPort(const char *host, unsigned port,
                                               char *out, size_t outSz) {
    if (!out || outSz == 0) {
        return;
    }
    out[0] = '\0';
    if (!host) {
        return;
    }
    size_t len = 0;
    int bracket = strchr(host, ':') != NULL;
    if (bracket) {
        wbAddrAppend(out, outSz, &len, "[");
    }
    wbAddrAppend(out, outSz, &len, host);
    if (bracket) {
        wbAddrAppend(out, outSz, &len, "]");
    }
    if (port != 0) {
        char digits[12];
        char text[13];
        int nd = 0;
        unsigned v = port;
        while (v != 0 && nd < (int)sizeof(digits)) {
            digits[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
        text[0] = ':';
        for (int i = 0; i < nd; i++) {
            text[1 + i] = digits[nd - 1 - i];
        }
        text[1 + nd] = '\0';
        wbAddrAppend(out, outSz, &len, text);
    }
}

#endif /* SERVER_ADDRESS_PARSE_H */
