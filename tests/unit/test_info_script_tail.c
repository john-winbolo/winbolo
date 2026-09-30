/*
 * The script bytes the reply to an info request carries after the
 * INFO_PACKET: the scenario that decides the round, its description and
 * human cap, and the names of the mods that run.
 *
 * buildInfoScriptTail (udp_server_query.c) writes them from the sim, and
 * discoveryReadScriptTail (discovery.c) reads them. Each side is checked
 * against bytes written out here at literal offsets, never against the other,
 * so a change to the layout on one side fails here rather than passing
 * because the other side moved with it.
 *
 * run_info_script_tail_golden — the writer:
 *   - a plain round writes exactly 00 00 00 00;
 *   - a scenario, its description and cap, and two mods, byte for byte;
 *   - a 210-byte description whose byte 200 falls inside a two-byte
 *     character is cut to 199 bytes;
 *   - a buffer smaller than INFO_SCRIPT_TAIL_MAX gets nothing.
 *
 * run_info_script_tail_read — the reader:
 *   - the plain round, zero mods with a scenario, and nine mods;
 *   - a mod count of 10, a name length of 64 (scenario and mod) and a
 *     description length of 201: each false and zeroed;
 *   - a tail cut one byte short: false;
 *   - bytes after the last mod: accepted.
 *
 * No scenario host is registered, so the identity and the list a case sets
 * by hand are what the writer reads.
 */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"          /* lobbyScenarioMod, lobbyScenarioNone */
#include "netpacks.h"               /* INFO_SCRIPT_TAIL_MAX, WBN_SCENARIO_DESC_MAX */
#include "server_sim.h"
#include "server_sim_internal.h"    /* serverSimSetScriptList */
#include "server_sim_lifecycle.h"   /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"    /* serverSimSetScenarioIdentity,
                                       serverSimSetScenarioLobbyTemplate */
#include "scenario_defs.h"          /* ScnDirEntry, ScnLobbyTemplate */
#include "transport_udp_server_internal.h" /* buildInfoScriptTail */
#include "discovery.h"              /* discoveryReadScriptTail */
#include "everard_map.h"
#include "test_harness.h"

/* A lobby nobody is in, on the inbuilt map. */
static ServerSim *istLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

static void istRow(ScnDirEntry *row, const char *file, const char *name,
                   bool keepsWinCondition) {
    memset(row, 0, sizeof(*row));
    snprintf(row->file, sizeof(row->file), "%s", file);
    snprintf(row->name, sizeof(row->name), "%s", name);
    row->keepsWinCondition = keepsWinCondition;
}

/* Every byte of got against want, naming the first that differs. */
static int istBytesEqual(const uint8_t *got, size_t gotLen,
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

int run_info_script_tail_golden(void) {
    ServerSim        *sim = istLobbySim();
    ScnDirEntry       rows[3];
    ScnLobbyTemplate  t;
    uint8_t           buf[INFO_SCRIPT_TAIL_MAX];
    size_t            n;

    UT_ASSERT(sim != NULL);

    /* A plain round: no name, no description, no cap, no mods. */
    {
        static const uint8_t want[4] = { 0x00, 0x00, 0x00, 0x00 };
        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        if (istBytesEqual(buf, n, want, sizeof(want), "plain round")) return 1;
    }

    /* A scenario with a cap of 6 and two mods behind it. The scenario's own
       row keeps nothing, so it is not among the mods. */
    {
        uint8_t want[40];
        memset(want, 0, sizeof(want));
        want[0]  = 8;                           /* scenarioNameLen */
        memcpy(want + 1, "Survival", 8);        /* 1..8 */
        want[9]  = 9;                           /* scenarioDescLen */
        memcpy(want + 10, "Last tank", 9);      /* 10..18 */
        want[19] = 6;                           /* maxPlayers */
        want[20] = 2;                           /* modCount */
        want[21] = 9;                           /* mod 0 nameLen */
        memcpy(want + 22, "Infection", 9);      /* 22..30 */
        want[31] = 8;                           /* mod 1 nameLen */
        memcpy(want + 32, "Pill Tag", 8);       /* 32..39 */

        memset(&t, 0, sizeof(t));
        t.maxPlayers = 6;
        serverSimSetScenarioLobbyTemplate(sim, &t);
        serverSimSetScenarioIdentity(sim, lobbyScenarioMod, "Survival",
                                     "survival.lua", "Last tank", false,
                                     false, false, false);
        istRow(&rows[0], "survival.lua", "Survival", false);
        istRow(&rows[1], "infection.lua", "Infection", true);
        istRow(&rows[2], "pilltag.lua", "Pill Tag", true);
        serverSimSetScriptList(sim, rows, 3);

        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        if (istBytesEqual(buf, n, want, sizeof(want), "scenario and mods")) {
            return 1;
        }
    }

    /* A 210-byte description: 199 ASCII bytes, then an é at 199..200, then
       ASCII to the end. The cut at 200 would split the é, so it goes whole
       and 199 bytes are sent. No mods this time. */
    {
        char    desc[211];
        uint8_t want[1 + 1 + 1 + 199 + 1 + 1];

        memset(desc, 'd', 199);
        desc[199] = (char)0xC3;
        desc[200] = (char)0xA9;
        memset(desc + 201, 'e', 9);
        desc[210] = '\0';
        serverSimSetScenarioIdentity(sim, lobbyScenarioMod, "S", "s.lua", desc,
                                     false, false, false, false);
        serverSimSetScriptList(sim, NULL, 0);

        memset(want, 0, sizeof(want));
        want[0] = 1;                            /* scenarioNameLen */
        want[1] = 'S';
        want[2] = 199;                          /* scenarioDescLen */
        memset(want + 3, 'd', 199);             /* 3..201 */
        want[202] = 6;                          /* maxPlayers, still 6 */
        want[203] = 0;                          /* modCount */

        memset(buf, 0xAB, sizeof(buf));
        n = buildInfoScriptTail(sim, buf, sizeof(buf));
        if (istBytesEqual(buf, n, want, sizeof(want), "long description")) {
            return 1;
        }
    }

    /* A buffer that could not hold the largest tail gets nothing. */
    memset(buf, 0xAB, sizeof(buf));
    n = buildInfoScriptTail(sim, buf, sizeof(buf) - 1);
    UT_ASSERT_MSG(n == 0, "a short buffer was written %u bytes", (unsigned)n);
    UT_ASSERT_MSG(buf[0] == 0xAB, "a short buffer was written to");

    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false);
    serverSimDestroy(sim);
    return 0;
}

/* True when every byte of s is zero. */
static bool istScriptsZeroed(const DiscoveryScripts *s) {
    DiscoveryScripts zero;
    memset(&zero, 0, sizeof(zero));
    return memcmp(s, &zero, sizeof(zero)) == 0;
}

/* Reads buf, expecting it refused and out left zeroed. */
static int istRefused(const uint8_t *buf, size_t len, const char *what) {
    DiscoveryScripts s;
    memset(&s, 0xAB, sizeof(s));
    UT_ASSERT_MSG(!discoveryReadScriptTail(buf, len, &s),
                  "%s was accepted", what);
    UT_ASSERT_MSG(istScriptsZeroed(&s), "%s left the result not zeroed", what);
    return 0;
}

int run_info_script_tail_read(void) {
    DiscoveryScripts s;

    /* The plain round. */
    {
        static const uint8_t buf[4] = { 0x00, 0x00, 0x00, 0x00 };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT_MSG(discoveryReadScriptTail(buf, sizeof(buf), &s),
                      "the plain round was refused");
        UT_ASSERT(s.hasScriptInfo);
        UT_ASSERT(s.scenarioName[0] == '\0');
        UT_ASSERT(s.scenarioDescription[0] == '\0');
        UT_ASSERT(s.scenarioMaxPlayers == 0);
        UT_ASSERT(s.modCount == 0);
    }

    /* A scenario and no mods. */
    {
        static const uint8_t buf[] = {
            0x03, 'W', 'a', 'r',        /* 0..3 name */
            0x02, 'H', 'i',             /* 4..6 description */
            0x04,                       /* 7 cap */
            0x00                        /* 8 modCount */
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT_MSG(discoveryReadScriptTail(buf, sizeof(buf), &s),
                      "a scenario with no mods was refused");
        UT_ASSERT(s.hasScriptInfo);
        UT_ASSERT_MSG(strcmp(s.scenarioName, "War") == 0,
                      "the name read as '%s'", s.scenarioName);
        UT_ASSERT_MSG(strcmp(s.scenarioDescription, "Hi") == 0,
                      "the description read as '%s'", s.scenarioDescription);
        UT_ASSERT_MSG(s.scenarioMaxPlayers == 4, "the cap read as %u",
                      (unsigned)s.scenarioMaxPlayers);
        UT_ASSERT(s.modCount == 0);

        /* The same bytes one short: the count byte is missing. */
        if (istRefused(buf, sizeof(buf) - 1, "a tail one byte short")) return 1;
    }

    /* Nine mods, each a one-letter name. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00,           /* 0..2 no scenario */
            0x09,                       /* 3 modCount */
            0x01, 'A', 0x01, 'B', 0x01, 'C', 0x01, 'D', 0x01, 'E',
            0x01, 'F', 0x01, 'G', 0x01, 'H', 0x01, 'I'   /* 4..21 */
        };
        int i;
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT_MSG(discoveryReadScriptTail(buf, sizeof(buf), &s),
                      "nine mods were refused");
        UT_ASSERT_MSG(s.modCount == 9, "%u mods read, expected 9",
                      (unsigned)s.modCount);
        for (i = 0; i < 9; i++) {
            UT_ASSERT_MSG(s.modNames[i][0] == 'A' + i && s.modNames[i][1] == '\0',
                          "mod %d read as '%s'", i, s.modNames[i]);
        }

        /* The last name one byte short. */
        if (istRefused(buf, sizeof(buf) - 1, "nine mods one byte short")) {
            return 1;
        }
    }

    /* A mod count of 10. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00,
            0x0A,
            0x01, 'A', 0x01, 'B', 0x01, 'C', 0x01, 'D', 0x01, 'E',
            0x01, 'F', 0x01, 'G', 0x01, 'H', 0x01, 'I', 0x01, 'J'
        };
        if (istRefused(buf, sizeof(buf), "a mod count of 10")) return 1;
    }

    /* A scenario name length of 64, with all 64 bytes present. */
    {
        uint8_t buf[1 + 64 + 3];
        memset(buf, 0, sizeof(buf));
        buf[0] = 64;
        memset(buf + 1, 'n', 64);        /* 1..64 */
        if (istRefused(buf, sizeof(buf), "a name length of 64")) return 1;
    }

    /* A mod name length of 64. */
    {
        uint8_t buf[4 + 1 + 64];
        memset(buf, 0, sizeof(buf));
        buf[3] = 1;                      /* modCount */
        buf[4] = 64;                     /* mod 0 nameLen */
        memset(buf + 5, 'm', 64);        /* 5..68 */
        if (istRefused(buf, sizeof(buf), "a mod name length of 64")) return 1;
    }

    /* A description length of 201, with all 201 bytes present. */
    {
        uint8_t buf[1 + 1 + 201 + 2];
        memset(buf, 0, sizeof(buf));
        buf[0] = 0;
        buf[1] = 201;
        memset(buf + 2, 'd', 201);       /* 2..202 */
        if (istRefused(buf, sizeof(buf), "a description length of 201")) {
            return 1;
        }
    }

    /* Bytes after the last mod are a later field, not an error. */
    {
        static const uint8_t buf[] = {
            0x00, 0x00, 0x00,
            0x01, 0x02, 'O', 'k',       /* 3..6 one mod */
            0xEE, 0xFF                  /* 7..8 something appended later */
        };
        memset(&s, 0xAB, sizeof(s));
        UT_ASSERT_MSG(discoveryReadScriptTail(buf, sizeof(buf), &s),
                      "trailing bytes were refused");
        UT_ASSERT(s.hasScriptInfo);
        UT_ASSERT(s.modCount == 1);
        UT_ASSERT_MSG(strcmp(s.modNames[0], "Ok") == 0,
                      "the mod read as '%s'", s.modNames[0]);
    }
    return 0;
}
