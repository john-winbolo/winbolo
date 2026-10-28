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
 * The game browser's server version test (browser_version.h): the old
 * "M.mr" form and the "M.m.r" form both parse, with or without a letter
 * suffix, and are compared as numbers against 1.1.9; NULL, empty and
 * malformed text is hidden.
 *
 * Also the protocol version bytes cmake/bolo_version.cmake derives from
 * WINBOLO_VERSION: one byte per field, 2, 1, 0 for this release.
 */

#include <stdio.h>
#include <string.h>

#include "gui/sdl3/dialogs/browser_version.h"
#include "test_harness.h"

typedef struct {
    const char *text;
    bool parses;
    unsigned major, minor, revision;
    bool allowed;
} VersionCase;

static const VersionCase s_cases[] = {
    /* The old "M.mr" form. */
    { "1.19",    true,  1, 1, 9,   true  },
    { "2.10",    true,  2, 1, 0,   true  },
    { "2.02",    true,  2, 0, 2,   true  },
    { "1.18",    true,  1, 1, 8,   false },
    { "1.09",    true,  1, 0, 9,   false },
    { "0.99",    true,  0, 9, 9,   false },
    /* The "M.m.r" form. */
    { "2.1.0",   true,  2, 1, 0,   true  },
    { "2.1.10",  true,  2, 1, 10,  true  },
    { "1.1.9",   true,  1, 1, 9,   true  },
    { "1.1.8",   true,  1, 1, 8,   false },
    { "1.2.0",   true,  1, 2, 0,   true  },
    { "10.0.0",  true,  10, 0, 0,  true  },
    { "255.255.255", true, 255, 255, 255, true },
    /* Either form with a letter suffix, ignored for the comparison. */
    { "2.10a",   true,  2, 1, 0,   true  },
    { "2.1.0a",  true,  2, 1, 0,   true  },
    { "2.1.0rc", true,  2, 1, 0,   true  },
    { "1.18a",   true,  1, 1, 8,   false },
    { "1.1.8b",  true,  1, 1, 8,   false },
    /* Text that does not parse, so the server stays hidden. */
    { "",        false, 0, 0, 0,   false },
    { "2",       false, 0, 0, 0,   false },
    { "2.",      false, 0, 0, 0,   false },
    { "2.1",     false, 0, 0, 0,   false },
    { "2.100",   false, 0, 0, 0,   false },
    { "2.1.",    false, 0, 0, 0,   false },
    { ".1.0",    false, 0, 0, 0,   false },
    { "2..0",    false, 0, 0, 0,   false },
    { "2.1.0.0", false, 0, 0, 0,   false },
    { "2.1.256", false, 0, 0, 0,   false },
    { "256.1.0", false, 0, 0, 0,   false },
    { "2.1.0 ",  false, 0, 0, 0,   false },
    { " 2.1.0",  false, 0, 0, 0,   false },
    { "2.1.0-1", false, 0, 0, 0,   false },
    { "2.1a.0",  false, 0, 0, 0,   false },
    { "-2.1.0",  false, 0, 0, 0,   false },
    { "v2.1.0",  false, 0, 0, 0,   false },
    { "abc",     false, 0, 0, 0,   false },
    { "1.19!",   false, 0, 0, 0,   false },
    { "99999999999.1.0", false, 0, 0, 0, false },
};

int run_browser_version_parse(void) {
    size_t i;
    int failures = 0;
    unsigned v[3];

    for (i = 0; i < sizeof(s_cases) / sizeof(s_cases[0]); i++) {
        const VersionCase *c = &s_cases[i];
        bool parsed;
        bool allowed;

        v[0] = v[1] = v[2] = 999u;
        parsed = browserVersionParse(c->text, v);
        allowed = browserVersionAllowed(c->text);

        if (parsed != c->parses) {
            fprintf(stderr, "browser_version: \"%s\" parsed=%d, want %d\n",
                    c->text, (int)parsed, (int)c->parses);
            failures++;
            continue;
        }
        if (parsed && (v[0] != c->major || v[1] != c->minor ||
                       v[2] != c->revision)) {
            fprintf(stderr, "browser_version: \"%s\" gave %u.%u.%u, "
                    "want %u.%u.%u\n", c->text, v[0], v[1], v[2],
                    c->major, c->minor, c->revision);
            failures++;
        }
        if (allowed != c->allowed) {
            fprintf(stderr, "browser_version: \"%s\" allowed=%d, want %d\n",
                    c->text, (int)allowed, (int)c->allowed);
            failures++;
        }
    }

    /* NULL is hidden and does not parse. */
    if (browserVersionParse(NULL, v)) {
        fprintf(stderr, "browser_version: NULL parsed\n");
        failures++;
    }
    if (browserVersionAllowed(NULL)) {
        fprintf(stderr, "browser_version: NULL allowed\n");
        failures++;
    }

    return failures == 0 ? 0 : 1;
}

int run_version_protocol_bytes(void) {
    unsigned v[3];
    char rebuilt[32];

    /* The bytes are the three fields of WINBOLO_VERSION, one each. */
    UT_ASSERT_MSG(browserVersionParse(WINBOLO_VERSION, v),
                  "WINBOLO_VERSION \"%s\" did not parse", WINBOLO_VERSION);
    UT_ASSERT_MSG(v[0] == BOLO_VERSION_MAJOR && v[1] == BOLO_VERSION_MINOR &&
                  v[2] == BOLO_VERSION_REVISION,
                  "WINBOLO_VERSION \"%s\" but bytes %u,%u,%u", WINBOLO_VERSION,
                  (unsigned)BOLO_VERSION_MAJOR, (unsigned)BOLO_VERSION_MINOR,
                  (unsigned)BOLO_VERSION_REVISION);
    snprintf(rebuilt, sizeof(rebuilt), "%u.%u.%u",
             (unsigned)BOLO_VERSION_MAJOR, (unsigned)BOLO_VERSION_MINOR,
             (unsigned)BOLO_VERSION_REVISION);
    UT_ASSERT_MSG(strcmp(rebuilt, WINBOLO_VERSION) == 0,
                  "bytes give \"%s\", WINBOLO_VERSION is \"%s\"", rebuilt,
                  WINBOLO_VERSION);

    /* The display version is WINBOLO_VERSION with the suffix after it. */
    UT_ASSERT_MSG(strncmp(WINBOLO_DISPLAY_VERSION, WINBOLO_VERSION,
                          strlen(WINBOLO_VERSION)) == 0,
                  "WINBOLO_DISPLAY_VERSION \"%s\" does not start with \"%s\"",
                  WINBOLO_DISPLAY_VERSION, WINBOLO_VERSION);

    /* This release: 2.1.0 sends 2, 1, 0, the same bytes 2.1 builds send, so
     * the two connect. Update this when the version changes. */
    UT_ASSERT_MSG(BOLO_VERSION_MAJOR == 2 && BOLO_VERSION_MINOR == 1 &&
                  BOLO_VERSION_REVISION == 0,
                  "protocol bytes %u,%u,%u, want 2,1,0",
                  (unsigned)BOLO_VERSION_MAJOR, (unsigned)BOLO_VERSION_MINOR,
                  (unsigned)BOLO_VERSION_REVISION);
    return 0;
}
