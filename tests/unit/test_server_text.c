/*
 * The short name and the longer description a host gives a server, which
 * the game finder shows in place of the server's address.
 *
 * run_server_text_sanitize — serverTextSanitize (server_text.c):
 *   - plain text passes unchanged; NULL gives "";
 *   - tabs, newlines, NEL, no-break and line/paragraph separators become one
 *     space, runs collapse, and the ends are trimmed;
 *   - C0, DEL and C1 controls, zero-width and bidi marks are dropped;
 *   - bytes that are not UTF-8 (a stray continuation, a lead byte with no
 *     tail, overlong forms, a surrogate, past U+10FFFF, F5..FF) are dropped;
 *   - soft hyphen, U+061C, U+180E, U+FFF9..FFFB, tag characters and
 *     noncharacters are dropped; an emoji is kept;
 *   - NFC, leading combining marks dropped, stacked marks capped;
 *   - the cut never falls inside a character or between a letter and its
 *     marks, and the output buffer size is honoured below maxBytes.
 *
 * run_server_text_sim — the sim's setters sanitise and cut, and the getters
 * give "" for a NULL sim.
 *
 * run_server_text_tail_golden — the writer (buildInfoServerText through
 * buildInfoScriptTail, udp_server_query.c) against literal bytes:
 *   - no name and no description writes nothing after the mods, so a plain
 *     server's reply is byte for byte what it was before the field;
 *   - a name and a description follow the mods;
 *   - a description alone writes a zero name length;
 *   - behind the largest script tail, the name goes whole and the
 *     description is cut to what is left of INFO_REPLY_TAIL_CAP (34 bytes),
 *     which infoReplyServerDescRoom reports; 2 bytes of room write nothing
 *     and 3 write a one-byte name and an empty description.
 *
 * run_server_text_tail_read — the reader (discoveryReadScriptTail,
 * discovery.c) against literal buffers:
 *   - the old layout, which stops after the mods: true, name empty;
 *   - a name and a description: read;
 *   - a name length of 33, a description length of 201, and a name length
 *     past the end: the scripts are kept and the server text stays empty,
 *     except that a good name before a bad description is kept;
 *   - an empty [0][0] pair: accepted;
 *   - control characters in the bytes are cleaned on read.
 */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"          /* lobbyScenarioMod, lobbyScenarioNone */
#include "netpacks.h"               /* INFO_SCRIPT_TAIL_MAX, INFO_REPLY_TAIL_CAP */
#include "server_sim.h"
#include "server_sim_internal.h"    /* serverSimSetScriptList */
#include "server_sim_lifecycle.h"   /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"    /* serverSimSetScenarioIdentity */
#include "scenario_defs.h"          /* ScnDirEntry */
#include "transport_udp_server_internal.h" /* buildInfoScriptTail */
#include "discovery.h"              /* discoveryReadScriptTail */
#include "server_text.h"
#include "everard_map.h"
#include "test_harness.h"

/* Sanitise in with the given caps and compare to want. */
static int stCheck(const char *in, size_t outSize, size_t maxBytes,
                   const char *want, const char *what) {
    char   out[512];
    size_t n;

    memset(out, 0xAB, sizeof(out));
    n = serverTextSanitize(in, out, outSize, maxBytes);
    UT_ASSERT_MSG(strcmp(out, want) == 0, "%s: got '%s', expected '%s'",
                  what, out, want);
    UT_ASSERT_MSG(n == strlen(want), "%s: returned %u, expected %u", what,
                  (unsigned)n, (unsigned)strlen(want));
    return 0;
}

int run_server_text_sanitize(void) {
    if (stCheck("Andy's Den", 64, 32, "Andy's Den", "plain")) return 1;
    if (stCheck(NULL, 64, 32, "", "NULL")) return 1;
    if (stCheck("", 64, 32, "", "empty")) return 1;
    if (stCheck("   ", 64, 32, "", "spaces only")) return 1;

    /* Whitespace of every kind becomes one space; the ends go. */
    if (stCheck("  a\tb\r\nc  ", 64, 32, "a b c", "C0 spaces")) return 1;
    if (stCheck("a \xC2\x85 b", 64, 32, "a b", "NEL")) return 1;
    if (stCheck("a\xC2\xA0" "b", 64, 32, "a b", "no-break space")) return 1;
    if (stCheck("a\xE2\x80\xA8" "b\xE2\x80\xA9", 64, 32, "a b",
                "line and paragraph separators")) return 1;

    /* Controls and invisible marks go with no space left behind. */
    if (stCheck("a\x01" "b\x1B[31mc\x7F" "d", 64, 32, "ab[31mcd",
                "C0 and DEL")) return 1;
    if (stCheck("a\xC2\x9B" "b", 64, 32, "ab", "C1")) return 1;
    if (stCheck("a\xE2\x80\x8B" "b\xE2\x80\xAE" "c\xE2\x81\xA6" "d\xEF\xBB\xBF",
                64, 32, "abcd", "zero-width and bidi marks")) return 1;

    /* Bytes that are not UTF-8 are dropped one at a time. */
    if (stCheck("a\x80" "b", 64, 32, "ab", "stray continuation")) return 1;
    if (stCheck("a\xC3", 64, 32, "a", "lead byte at the end")) return 1;
    if (stCheck("a\xC0\xAF" "b", 64, 32, "ab", "overlong slash")) return 1;
    if (stCheck("a\xED\xA0\x80" "b", 64, 32, "ab", "surrogate")) return 1;
    if (stCheck("caf\xC3\xA9 \xE2\x9C\x93", 64, 32, "caf\xC3\xA9 \xE2\x9C\x93",
                "accents and symbols kept")) return 1;

    /* The cut: 4 bytes of room, "abc" then a two-byte é: é does not fit
       whole, so it goes. */
    if (stCheck("abc\xC3\xA9", 64, 4, "abc", "cut before a character"))
        return 1;
    if (stCheck("abcd\xC3\xA9", 64, 4, "abcd", "cut at the cap")) return 1;
    /* A space that would sit at the end of the cut is not written. */
    if (stCheck("abcd efg", 64, 5, "abcd", "no trailing space at the cut"))
        return 1;
    /* An output buffer smaller than maxBytes wins. */
    if (stCheck("abcdefgh", 4, 32, "abc", "small buffer")) return 1;

    /* A four-byte character is kept. */
    if (stCheck("hi \xF0\x9F\x98\x80", 64, 32, "hi \xF0\x9F\x98\x80", "emoji"))
        return 1;

    /* More bytes that are not UTF-8: overlong four- and three-byte forms, a
       code point past U+10FFFF, and the lead bytes F5..FF that never start
       a character. */
    if (stCheck("a\xF0\x80\x80\x80" "b", 64, 32, "ab", "overlong F0 80 80 80"))
        return 1;
    if (stCheck("a\xE0\x80\x80" "b", 64, 32, "ab", "overlong E0 80 80"))
        return 1;
    if (stCheck("a\xF4\x90\x80\x80" "b", 64, 32, "ab", "past U+10FFFF"))
        return 1;
    if (stCheck("a\xF5\x80\x80\x80" "b\xF8\xFB\xFC\xFD\xFE\xFF" "c", 64, 32,
                "abc", "lead bytes F5..FF")) return 1;

    /* The 32-byte cap falling inside a two-byte character: 31 letters and
       an é would be 33 bytes, so the é goes. */
    {
        char in[40];
        char want[40];
        memset(in, 'a', 31);
        in[31] = (char)0xC3;
        in[32] = (char)0xA9;
        in[33] = '\0';
        memcpy(want, in, 31);
        want[31] = '\0';
        if (stCheck(in, SERVER_NAME_LEN, SERVER_NAME_MAX, want,
                    "cap inside a two-byte character")) return 1;
    }

    /* More characters that hide or reorder text: U+061C, U+00AD, U+180E,
       U+FFF9, a tag character U+E0001, the noncharacters U+FDD0, U+FFFE and
       U+1FFFF. */
    if (stCheck("a\xD8\x9C" "b\xC2\xAD" "c\xE1\xA0\x8E" "d\xEF\xBF\xB9"
                "e\xF3\xA0\x80\x81" "f\xEF\xB7\x90" "g\xEF\xBF\xBE"
                "h\xF0\x9F\xBF\xBF" "i",
                64, 32, "abcdefghi", "hidden and noncharacters")) return 1;

    /* Combining marks. NFC folds e + U+0301 into the one é it shows as. */
    if (stCheck("e\xCC\x81", 64, 32, "\xC3\xA9", "NFC")) return 1;
    /* A mark with nothing to draw on goes: at the start, and after a
       space. */
    if (stCheck("\xCC\x81\xCC\x88" "abc", 64, 32, "abc", "leading marks"))
        return 1;
    if (stCheck("a \xCC\x81" "b", 64, 32, "a b", "mark after a space"))
        return 1;
    /* A letter keeps at most SERVER_TEXT_MARKS_MAX marks; x has no
       precomposed form, so NFC leaves all six for the cap to cut. */
    if (stCheck("x\xCC\x81\xCC\x81\xCC\x81\xCC\x81\xCC\x81\xCC\x81" "y", 64, 32,
                "x\xCC\x81\xCC\x81\xCC\x81" "y", "stacked marks")) return 1;
    /* The cut keeps a letter with its marks: x and its mark need 3 bytes
       and only 2 are left after "ab", so x goes too, and the space before
       it with it. */
    if (stCheck("abx\xCC\x81", 64, 4, "ab", "cut before a letter's mark"))
        return 1;
    if (stCheck("ab x\xCC\x81", 64, 5, "ab", "cut drops the space too"))
        return 1;
    if (stCheck("ab x\xCC\x81", 64, 6, "ab x\xCC\x81", "letter and mark fit"))
        return 1;
    return 0;
}

int run_server_text_sim(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island", gameOpen,
                                               false, 0, -1);
    char       longName[80];
    char       longDesc[300];

    UT_ASSERT(sim != NULL);
    UT_ASSERT(serverSimGetServerName(sim)[0] == '\0');
    UT_ASSERT(serverSimGetServerDescription(sim)[0] == '\0');

    serverSimSetServerName(sim, "  Andy's\tDen\n");
    UT_ASSERT_MSG(strcmp(serverSimGetServerName(sim), "Andy's Den") == 0,
                  "the name read back as '%s'", serverSimGetServerName(sim));

    memset(longName, 'n', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    serverSimSetServerName(sim, longName);
    UT_ASSERT_MSG(strlen(serverSimGetServerName(sim)) == SERVER_NAME_MAX,
                  "a long name kept %u bytes",
                  (unsigned)strlen(serverSimGetServerName(sim)));

    memset(longDesc, 'd', sizeof(longDesc) - 1);
    longDesc[sizeof(longDesc) - 1] = '\0';
    serverSimSetServerDescription(sim, longDesc);
    UT_ASSERT_MSG(strlen(serverSimGetServerDescription(sim)) == SERVER_DESC_MAX,
                  "a long description kept %u bytes",
                  (unsigned)strlen(serverSimGetServerDescription(sim)));

    serverSimSetServerName(sim, NULL);
    serverSimSetServerDescription(sim, "");
    UT_ASSERT(serverSimGetServerName(sim)[0] == '\0');
    UT_ASSERT(serverSimGetServerDescription(sim)[0] == '\0');

    UT_ASSERT(serverSimGetServerName(NULL)[0] == '\0');
    UT_ASSERT(serverSimGetServerDescription(NULL)[0] == '\0');
    serverSimDestroy(sim);
    return 0;
}

/* Every byte of got against want, naming the first that differs. */
static int stBytesEqual(const uint8_t *got, size_t gotLen,
                        const uint8_t *want, size_t wantLen,
                        const char *what) {
    size_t i;
    UT_ASSERT_MSG(gotLen == wantLen, "%s: %u bytes written, expected %u",
                  what, (unsigned)gotLen, (unsigned)wantLen);
    for (i = 0; i < wantLen; i++) {
        UT_ASSERT_MSG(got[i] == want[i],
                      "%s: byte %u is 0x%02x, expected 0x%02x",
                      what, (unsigned)i, (unsigned)got[i], (unsigned)want[i]);
    }
    return 0;
}

static void stRow(ScnDirEntry *row, const char *file, const char *name,
                  bool keepsWinCondition) {
    memset(row, 0, sizeof(*row));
    snprintf(row->file, sizeof(row->file), "%s", file);
    snprintf(row->name, sizeof(row->name), "%s", name);
    row->keepsWinCondition = keepsWinCondition;
}

int run_server_text_tail_golden(void) {
    BYTE       emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island", gameOpen,
                                               false, 0, -1);
    uint8_t    buf[INFO_REPLY_TAIL_CAP];
    size_t     n;

    UT_ASSERT(sim != NULL);
    serverSimSetLobbyEnabled(sim, true);

    /* No name, no description: the plain round's four bytes and nothing
       more, the same reply an older server sends. */
    {
        static const uint8_t want[4] = { 0x00, 0x00, 0x00, 0x00 };
        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        if (stBytesEqual(buf, n, want, sizeof(want), "no server text")) {
            return 1;
        }
    }

    /* A name and a description after the plain round's four bytes. */
    {
        static const uint8_t want[] = {
            0x00, 0x00, 0x00, 0x00,                         /* 0..3 scripts */
            0x03, 'D', 'e', 'n',                            /* 4..7 name */
            0x07, 'C', 'o', 'm', 'e', ' ', 'i', 'n'         /* 8..15 desc */
        };
        serverSimSetServerName(sim, "Den");
        serverSimSetServerDescription(sim, "Come in");
        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        if (stBytesEqual(buf, n, want, sizeof(want), "name and description")) {
            return 1;
        }
    }

    /* A description alone: a zero name length leads it. */
    {
        static const uint8_t want[] = {
            0x00, 0x00, 0x00, 0x00,
            0x00,                                           /* 4 name */
            0x02, 'H', 'i'                                  /* 5..7 desc */
        };
        serverSimSetServerName(sim, "");
        serverSimSetServerDescription(sim, "Hi");
        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        if (stBytesEqual(buf, n, want, sizeof(want), "description alone")) {
            return 1;
        }
    }

    /* The largest script tail: a 63-byte scenario name, a 200-byte
       description and nine 63-byte mods, INFO_SCRIPT_TAIL_MAX bytes. Behind
       it the 32-byte name goes whole and the 200-byte description is cut to
       the room that is left, so the whole reply is exactly 1024 bytes. */
    {
        ScnDirEntry rows[10];
        char        scnName[64];
        char        scnDesc[WBN_SCENARIO_DESC_MAX + 1];
        char        modName[64];
        char        name[SERVER_NAME_MAX + 1];
        char        desc[SERVER_DESC_MAX + 1];
        size_t      room = INFO_REPLY_TAIL_CAP - INFO_SCRIPT_TAIL_MAX;
        size_t      descKept = room - 1 - SERVER_NAME_MAX - 1;
        int         i;

        memset(scnName, 's', 63);
        scnName[63] = '\0';
        memset(scnDesc, 'x', WBN_SCENARIO_DESC_MAX);
        scnDesc[WBN_SCENARIO_DESC_MAX] = '\0';
        serverSimSetScenarioIdentity(sim, lobbyScenarioMod, scnName, "s.lua",
                                     scnDesc, false, false, false, false,
                                     false);
        stRow(&rows[0], "s.lua", scnName, false);
        for (i = 0; i < 9; i++) {
            char file[16];
            memset(modName, 'a' + i, 63);
            modName[63] = '\0';
            snprintf(file, sizeof(file), "m%d.lua", i);
            stRow(&rows[1 + i], file, modName, true);
        }
        serverSimSetScriptList(sim, rows, 10);

        memset(name, 'N', SERVER_NAME_MAX);
        name[SERVER_NAME_MAX] = '\0';
        memset(desc, 'D', SERVER_DESC_MAX);
        desc[SERVER_DESC_MAX] = '\0';
        serverSimSetServerName(sim, name);
        serverSimSetServerDescription(sim, desc);

        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        UT_ASSERT_MSG(n == INFO_REPLY_TAIL_CAP, "%u bytes written, expected %u",
                      (unsigned)n, (unsigned)INFO_REPLY_TAIL_CAP);
        UT_ASSERT_MSG(sizeof(INFO_PACKET) + n == MAX_UDPPACKET_SIZE,
                      "the reply is %u bytes",
                      (unsigned)(sizeof(INFO_PACKET) + n));
        UT_ASSERT_MSG(buf[INFO_SCRIPT_TAIL_MAX] == SERVER_NAME_MAX,
                      "name length %u", (unsigned)buf[INFO_SCRIPT_TAIL_MAX]);
        UT_ASSERT(memcmp(buf + INFO_SCRIPT_TAIL_MAX + 1, name,
                         SERVER_NAME_MAX) == 0);
        UT_ASSERT_MSG(buf[INFO_SCRIPT_TAIL_MAX + 1 + SERVER_NAME_MAX] ==
                          descKept,
                      "description length %u, expected %u",
                      (unsigned)buf[INFO_SCRIPT_TAIL_MAX + 1 + SERVER_NAME_MAX],
                      (unsigned)descKept);

        /* The room the DS's startup note is worked out from. */
        UT_ASSERT_MSG(infoReplyServerDescRoom(sim) == descKept,
                      "infoReplyServerDescRoom is %u, expected %u",
                      (unsigned)infoReplyServerDescRoom(sim),
                      (unsigned)descKept);

        /* Two bytes of room would hold only two empty lengths: nothing. */
        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, INFO_SCRIPT_TAIL_MAX + 2);
        UT_ASSERT_MSG(n == INFO_SCRIPT_TAIL_MAX,
                      "with 2 bytes of room %u bytes were written", (unsigned)n);
        /* Three bytes: one byte of name and an empty description. */
        n = buildInfoScriptTail(sim, buf, INFO_SCRIPT_TAIL_MAX + 3);
        UT_ASSERT_MSG(n == INFO_SCRIPT_TAIL_MAX + 3,
                      "with 3 bytes of room %u bytes were written", (unsigned)n);
        UT_ASSERT(buf[INFO_SCRIPT_TAIL_MAX] == 1);
        UT_ASSERT(buf[INFO_SCRIPT_TAIL_MAX + 1] == 'N');
        UT_ASSERT(buf[INFO_SCRIPT_TAIL_MAX + 2] == 0);

        /* The reader takes it all back. */
        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        {
            DiscoveryScripts s;
            memset(&s, 0xAB, sizeof(s));
            UT_ASSERT(discoveryReadScriptTail(buf, n, &s));
            UT_ASSERT(s.modCount == 9);
            UT_ASSERT(strcmp(s.serverName, name) == 0);
            UT_ASSERT(strlen(s.serverDescription) == descKept);
        }
    }

    serverSimSetScriptList(sim, NULL, 0);
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false, false);
    serverSimDestroy(sim);
    return 0;
}

int run_server_text_tail_read(void) {
    DiscoveryScripts s;

    /* The old layout: one mod and nothing after it. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00,
            0x01, 0x02, 'O', 'k'
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT_MSG(discoveryReadScriptTail(buf, sizeof(buf), &s),
                      "the old layout was refused");
        UT_ASSERT(s.modCount == 1);
        UT_ASSERT(s.serverName[0] == '\0');
        UT_ASSERT(s.serverDescription[0] == '\0');
    }

    /* A name and a description, then a byte left for a later field. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00, 0x00,
            0x03, 'D', 'e', 'n',
            0x02, 'H', 'i',
            0xEE
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT_MSG(strcmp(s.serverName, "Den") == 0,
                      "the name read as '%s'", s.serverName);
        UT_ASSERT_MSG(strcmp(s.serverDescription, "Hi") == 0,
                      "the description read as '%s'", s.serverDescription);
    }

    /* A name length of 33: the scripts stay, the server text does not. */
    {
        uint8_t buf[3 + 1 + 3 + 1 + 33 + 1];
        memset(buf, 0, sizeof(buf));
        buf[3] = 1;                      /* modCount */
        buf[4] = 2;                      /* mod 0 nameLen */
        buf[5] = 'O';
        buf[6] = 'k';
        buf[7] = 33;                     /* name length */
        memset(buf + 8, 'n', 33);        /* 8..40 */
        buf[41] = 0;                     /* description length */
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT(s.modCount == 1);
        UT_ASSERT(strcmp(s.modNames[0], "Ok") == 0);
        UT_ASSERT(s.serverName[0] == '\0');
        UT_ASSERT(s.serverDescription[0] == '\0');
    }

    /* A description length of 201. */
    {
        uint8_t buf[4 + 2 + 201];
        memset(buf, 0, sizeof(buf));
        buf[4] = 0;                      /* name length */
        buf[5] = 201;                    /* description length */
        memset(buf + 6, 'd', 201);
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT(s.serverName[0] == '\0');
        UT_ASSERT(s.serverDescription[0] == '\0');
    }

    /* A section cut short in the description: the name is kept. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00, 0x00,
            0x03, 'D', 'e', 'n',
            0x05, 'H', 'i'
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT(s.hasScriptInfo);
        UT_ASSERT_MSG(strcmp(s.serverName, "Den") == 0,
                      "the name read as '%s'", s.serverName);
        UT_ASSERT(s.serverDescription[0] == '\0');
    }

    /* A name length larger than the bytes left. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00, 0x00,
            0x05, 'D', 'e'
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT(s.hasScriptInfo);
        UT_ASSERT(s.serverName[0] == '\0');
        UT_ASSERT(s.serverDescription[0] == '\0');
    }

    /* An empty pair, [0][0]: accepted, both empty. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT(s.hasScriptInfo);
        UT_ASSERT(s.serverName[0] == '\0');
        UT_ASSERT(s.serverDescription[0] == '\0');
    }

    /* Controls on the wire are cleaned on read. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00, 0x00,
            0x05, ' ', 'D', 0x1B, 'e', 'n',
            0x04, 'H', '\n', '\n', 'i'
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT(discoveryReadScriptTail(buf, sizeof(buf), &s));
        UT_ASSERT_MSG(strcmp(s.serverName, "Den") == 0,
                      "the name read as '%s'", s.serverName);
        UT_ASSERT_MSG(strcmp(s.serverDescription, "H i") == 0,
                      "the description read as '%s'", s.serverDescription);
    }
    return 0;
}
