/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
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

/* Parses a pasted server address into host + optional decimal port string.
 * Accepts:  host  |  host:port  |  ip  |  ip:port  |  scheme://host[:port][/...]
 * (e.g. "winbolo://1.2.3.4:5000"). Leading scheme and any trailing /path or
 * surrounding whitespace are stripped. A trailing ":port" is only split off
 * when it is digits in the 1..65535 range — otherwise the input is kept
 * verbatim as the host. WinBolo is IPv4-only, so a lone ':' is unambiguous.
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

    /* Split a trailing ":port" only if it is a valid port number. */
    char *colon = strrchr(work, ':');
    if (colon && colon[1] != '\0') {
        int digits = 1;
        for (const char *q = colon + 1; *q; q++) {
            if (*q < '0' || *q > '9') {
                digits = 0;
                break;
            }
        }
        unsigned long val = digits ? strtoul(colon + 1, NULL, 10) : 0;
        if (digits && val >= 1 && val <= 65535) {
            wbAddrCopyBounded(outPort, portSz, colon + 1);
            *colon = '\0';
        }
    }
    if (work[0] == '\0') {
        return 0;
    }

    wbAddrCopyBounded(outHost, hostSz, work);
    return 1;
}

#endif /* SERVER_ADDRESS_PARSE_H */
