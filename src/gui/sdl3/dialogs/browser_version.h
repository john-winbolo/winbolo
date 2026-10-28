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

/*
 * The game browser's server version test.
 *
 * The browser hides servers older than BROWSER_MIN_VERSION_* (1.1.9). A
 * server's version text comes in two forms:
 *   - the old "M.mr" form, one digit each for minor and revision, which
 *     servers built before 2.1.0 still send ("1.19" is 1,1,9 and "2.10" is
 *     2,1,0);
 *   - the "M.m.r" form ("2.1.0", "2.1.10").
 * Either form may end in a letter suffix ("2.10a", "2.1.0a"), which is
 * ignored for the comparison. Text in any other shape is not parsed, and the
 * server stays hidden.
 *
 * Plain C in a header with no ImGui includes, so imgui_gamebrowser.cpp and
 * test_browser_version.c use the same code without another source file in
 * every target's source list.
 */

#ifndef BROWSER_VERSION_H
#define BROWSER_VERSION_H

#include <stdbool.h>
#include <stddef.h>

#define BROWSER_MIN_VERSION_MAJOR    1u
#define BROWSER_MIN_VERSION_MINOR    1u
#define BROWSER_MIN_VERSION_REVISION 9u

/* Reads a run of decimal digits at *p into *out, moving *p past it.
 * Returns false when there is no digit at *p or the value is over 255. */
static inline bool browserVersionReadField(const char **p, unsigned *out) {
    const char *s = *p;
    unsigned v = 0;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned)(*s - '0');
        if (v > 255u) return false;
        s++;
    }
    *out = v;
    *p = s;
    return true;
}

/* Parses ver into out[0..2] (major, minor, revision). Returns false for
 * NULL, empty text, or text that is not one of the forms above. */
static inline bool browserVersionParse(const char *ver, unsigned out[3]) {
    const char *p = ver;
    const char *minorStart;
    unsigned major, minor, revision;

    if (p == NULL) return false;
    if (!browserVersionReadField(&p, &major)) return false;
    if (*p != '.') return false;
    p++;
    minorStart = p;
    if (!browserVersionReadField(&p, &minor)) return false;

    if (*p == '.') {
        /* "M.m.r" */
        p++;
        if (!browserVersionReadField(&p, &revision)) return false;
    } else if (p - minorStart == 2) {
        /* "M.mr": the two digits are the minor and the revision. */
        minor = (unsigned)(minorStart[0] - '0');
        revision = (unsigned)(minorStart[1] - '0');
    } else {
        return false;
    }

    /* An optional suffix of letters only, then the end of the text. */
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) p++;
    if (*p != '\0') return false;

    out[0] = major;
    out[1] = minor;
    out[2] = revision;
    return true;
}

/* True when ver parses and is at least 1.1.9. */
static inline bool browserVersionAllowed(const char *ver) {
    unsigned v[3];
    if (!browserVersionParse(ver, v)) return false;
    if (v[0] != BROWSER_MIN_VERSION_MAJOR) return v[0] > BROWSER_MIN_VERSION_MAJOR;
    if (v[1] != BROWSER_MIN_VERSION_MINOR) return v[1] > BROWSER_MIN_VERSION_MINOR;
    return v[2] >= BROWSER_MIN_VERSION_REVISION;
}

#endif /* BROWSER_VERSION_H */
