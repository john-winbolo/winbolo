/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The file name the log viewer's Save Log File offers (lv_save_name.h).
 *
 *   lv_save_name_parts  — the whole name, and the name with each part left
 *       out: no date, no time, no map, no players, one team only, a player
 *       with no team, and nothing at all.
 *   lv_save_name_limits — more than six players, a name too long, a cut that
 *       would split a UTF-8 character, characters a file name cannot hold,
 *       and a dot or a space at the end.
 *   lv_save_name_roster — the list of a log's players as joins, new names,
 *       teams and leavings come; the map a lobby chose; and the date at the
 *       start of a server's own file name.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "lv_save_name.h"
#include "test_harness.h"

/* Build a name and compare it. */
#define SN_EXPECT(want, y, mo, d, h, mi, map, names, teams, n)                 \
    do {                                                                       \
        char got_[300];                                                        \
        lvSaveNameBuild(got_, sizeof(got_), y, mo, d, h, mi, map, names,       \
                        teams, n);                                             \
        UT_ASSERT_MSG(strcmp(got_, want) == 0, "got \"%s\", want \"%s\"",      \
                      got_, want);                                             \
    } while (0)

int run_lv_save_name_parts(void) {
    static const char *const   four[]      = {"Canuck", "rooklift", "Eryan", "Bot1"};
    static const unsigned char fourTeams[] = {1, 2, 1, 2};
    static const unsigned char oneTeam[]   = {3, 3, 3, 3};
    static const unsigned char oneNone[]   = {1, 2, 1, 0};
    static const unsigned char three[]     = {5, 2, 9, 2};

    /* Everything: date, time, map, the players by team, lowest team first,
       each team in the order its players came. */
    SN_EXPECT("2026-10-13 14.05 Oil Rig - Canuck, Eryan vs rooklift, Bot1.wbv",
              2026, 10, 13, 14, 5, "Oil Rig", four, fourTeams, 4);
    /* A date with no time of day. */
    SN_EXPECT("2026-10-13 Oil Rig - Canuck, Eryan vs rooklift, Bot1.wbv",
              2026, 10, 13, -1, 0, "Oil Rig", four, fourTeams, 4);
    /* No date: the name starts with the map. */
    SN_EXPECT("Oil Rig - Canuck, Eryan vs rooklift, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, fourTeams, 4);
    /* No players: nothing after the map. */
    SN_EXPECT("2026-10-13 14.05 Oil Rig.wbv",
              2026, 10, 13, 14, 5, "Oil Rig", NULL, NULL, 0);
    SN_EXPECT("2026-10-13 14.05 Oil Rig.wbv",
              2026, 10, 13, 14, 5, "Oil Rig", four, fourTeams, 0);
    /* No map: the players follow the date. */
    SN_EXPECT("2026-10-13 14.05 - Canuck, Eryan vs rooklift, Bot1.wbv",
              2026, 10, 13, 14, 5, "", four, fourTeams, 4);
    SN_EXPECT("2026-10-13 14.05 - Canuck, Eryan vs rooklift, Bot1.wbv",
              2026, 10, 13, 14, 5, NULL, four, fourTeams, 4);
    /* Players alone, map alone, date alone. */
    SN_EXPECT("Canuck, Eryan vs rooklift, Bot1.wbv",
              0, 0, 0, -1, 0, NULL, four, fourTeams, 4);
    SN_EXPECT("Oil Rig.wbv", 0, 0, 0, -1, 0, "Oil Rig", NULL, NULL, 0);
    SN_EXPECT("2026-10-13.wbv", 2026, 10, 13, -1, 0, NULL, NULL, NULL, 0);
    /* One team only, no teams given, and a player with no team: one list,
       in the order the players came, and no "vs". */
    SN_EXPECT("Oil Rig - Canuck, rooklift, Eryan, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, oneTeam, 4);
    SN_EXPECT("Oil Rig - Canuck, rooklift, Eryan, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, NULL, 4);
    SN_EXPECT("Oil Rig - Canuck, rooklift, Eryan, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, oneNone, 4);
    /* Three teams. */
    SN_EXPECT("Oil Rig - rooklift, Bot1 vs Canuck vs Eryan.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, three, 4);
    /* Nothing at all. */
    SN_EXPECT("log.wbv", 0, 0, 0, -1, 0, NULL, NULL, NULL, 0);
    SN_EXPECT("log.wbv", 0, 0, 0, -1, 0, "   ", NULL, NULL, 0);
    return 0;
}

int run_lv_save_name_limits(void) {
    static const char *const eight[] = {"a1", "a2", "a3", "a4", "a5",
                                        "b1", "b2", "b3"};
    static const unsigned char eightTeams[] = {1, 1, 1, 1, 1, 2, 2, 2};
    static const char *const   bad[]        = {"a/b\\c", "x:y*z?", "\"q\"<r>|s",
                                               "t\tu\x01v", "  two   spaces  ",
                                               "", "   "};
    static const char *const dots[] = {"end. ."};
    /* 20 bytes each. */
    static const char *const longNames[] = {
        "aaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbb", "cccccccccccccccccccc",
        "dddddddddddddddddddd", "eeeeeeeeeeeeeeeeeeee", "ffffffffffffffffffff",
        "gggggggggggggggggggg"};
    static const unsigned char longTeams[] = {1, 1, 1, 2, 2, 2, 2};
    char   got[400];
    char   map[200];
    const char *one[1];
    size_t n;
    int    i;

    /* More than six players: six are named and the rest counted. With no
       teams they are the first six. */
    SN_EXPECT("M - a1, a2, a3, a4, a5, b1 +2.wbv", 0, 0, 0, -1, 0, "M", eight,
              NULL, 8);
    /* By team, the six are taken a player from each team in turn. */
    SN_EXPECT("M - a1, a2, a3 vs b1, b2, b3 +2.wbv", 0, 0, 0, -1, 0, "M",
              eight, eightTeams, 8);

    /* What a file name cannot hold becomes a dot; a tab and a control
       character too. Spaces at the ends go, a run of them is one, and a
       name with nothing in it is not a player. */
    SN_EXPECT("O.R - a.b.c, x.y.z., .q..r..s, t.u.v, two spaces.wbv", 0, 0, 0,
              -1, 0, "O/R", bad, NULL, 7);

    /* Windows refuses a name that ends with a dot or a space. */
    SN_EXPECT("Map - end.wbv", 0, 0, 0, -1, 0, "Map", dots, NULL, 1);
    SN_EXPECT("Map.wbv", 0, 0, 0, -1, 0, "Map. . ", NULL, NULL, 0);
    SN_EXPECT("log.wbv", 0, 0, 0, -1, 0, "...", NULL, NULL, 0);

    /* Too long: cut at 120 bytes, and nothing left hanging. The whole name
       would be "2026-10-13 14.05 Map - " (23) then names of 20 with ", "
       and " vs " between: the cut falls inside the sixth name. */
    lvSaveNameBuild(got, sizeof(got), 2026, 10, 13, 14, 5, "Map", longNames,
                    longTeams, 7);
    n = strlen(got);
    UT_ASSERT_MSG(n == LV_SAVE_NAME_MAX_BYTES + 4, "length %d: \"%s\"", (int)n,
                  got);
    UT_ASSERT_MSG(strcmp(got + n - 4, ".wbv") == 0, "no extension: \"%s\"", got);
    UT_ASSERT_MSG(strncmp(got, "2026-10-13 14.05 Map - aaaaaaaaaaaaaaaaaaaa, ",
                          45) == 0, "the start: \"%s\"", got);
    UT_ASSERT_MSG(strstr(got, "+") == NULL, "a count after a cut: \"%s\"", got);

    /* A cut that lands on a separator takes the separator with it. A map of
       115 bytes and one player: 115 + " - " is 118, so two bytes of the
       name fit. With a map of 118 the " - " is what is cut, and it goes. */
    memset(map, 'm', sizeof(map));
    map[115] = '\0';
    one[0]   = "player";
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, map, one, NULL, 1);
    UT_ASSERT_MSG(strlen(got) == 124 && strcmp(got + 115, " - pl.wbv") == 0,
                  "a cut inside a name: \"%s\"", got);
    map[115] = 'm';
    map[118] = '\0';
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, map, one, NULL, 1);
    UT_ASSERT_MSG(strlen(got) == 122 && strcmp(got + 118, ".wbv") == 0,
                  "a cut inside \" - \": \"%s\"", got);
    /* " vs" left at the end by a cut goes too. */
    {
        static const char *const   two[]      = {"a", "bbbbbb"};
        static const unsigned char twoTeams[] = {1, 2};
        map[118] = 'm';
        map[112] = '\0'; /* 112 + " - a" = 116, " vs " ends at 120 */
        lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, map, two, twoTeams, 2);
        UT_ASSERT_MSG(strlen(got) == 120 && strcmp(got + 112, " - a.wbv") == 0,
                      "a cut after \" vs \": \"%s\"", got);
    }
    /* A count that does not all fit is not left as a smaller count. */
    {
        static const char *const many[] = {"a", "b", "c", "d", "e", "f", "g",
                                           "h", "i", "j", "k", "l", "m", "n",
                                           "o", "p", "q"};
        memset(map, 'm', sizeof(map));
        map[98] = '\0'; /* 98 + " - " + "a, b, c, d, e, f" (16) = 117, " +11" */
        lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, map, many, NULL, 17);
        UT_ASSERT_MSG(strcmp(got + 98, " - a, b, c, d, e, f.wbv") == 0,
                      "a count cut short: \"%s\"", got);
    }

    /* UTF-8 is never split. A map of 119 bytes and then a character of two,
       three or four bytes: the limit falls inside it, so all of it goes. */
    {
        static const char *const tails[] = {"\xC3\xA9", "\xE2\x82\xAC",
                                            "\xF0\x9F\x98\x80"};
        for (i = 0; i < 3; i++) {
            memset(map, 'm', 119);
            strcpy(map + 119, tails[i]);
            lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, map, NULL, NULL, 0);
            UT_ASSERT_MSG(strlen(got) == 123 && strcmp(got + 119, ".wbv") == 0,
                          "a %d byte character was split: length %d", i + 2,
                          (int)strlen(got));
        }
        /* One that ends at the limit stays whole. */
        memset(map, 'm', 118);
        strcpy(map + 118, "\xC3\xA9more");
        lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, map, NULL, NULL, 0);
        UT_ASSERT_MSG(strlen(got) == 124 &&
                      strcmp(got + 118, "\xC3\xA9.wbv") == 0,
                      "a character that fits was cut");
    }

    /* A small buffer is filled and ended, no more. */
    lvSaveNameBuild(got, 8, 2026, 10, 13, 14, 5, "Oil Rig", NULL, NULL, 0);
    UT_ASSERT_MSG(strcmp(got, "2026-10") == 0, "small buffer: \"%s\"", got);
    return 0;
}

int run_lv_save_name_roster(void) {
    static LvRoster r;
    LvLobbyMap      m;
    int             y = 0, mo = 0, d = 0, h = 0, mi = 0;
    int             i;
    char            name[16];

    /* Joins in order; a team for a slot is that player's. */
    lvRosterClear(&r);
    lvRosterJoin(&r, 0, "Canuck");
    lvRosterJoin(&r, 1, "rooklift");
    lvRosterTeam(&r, 0, 1);
    lvRosterTeam(&r, 1, 2);
    UT_ASSERT(r.count == 2);
    UT_ASSERT(strcmp(r.row[0].name, "Canuck") == 0 && r.row[0].team == 1);
    UT_ASSERT(strcmp(r.row[1].name, "rooklift") == 0 && r.row[1].team == 2);
    /* A new name is the same player; leaving a team keeps the side. */
    lvRosterRename(&r, 1, "rook");
    lvRosterTeam(&r, 1, 0);
    UT_ASSERT(r.count == 2 && strcmp(r.row[1].name, "rook") == 0 &&
              r.row[1].team == 2);
    /* A player leaves and another takes the slot: both are in the list, and
       the slot's team is now the new player's. */
    lvRosterQuit(&r, 1);
    lvRosterTeam(&r, 1, 5); /* nobody is there */
    lvRosterJoin(&r, 1, "Eryan");
    lvRosterTeam(&r, 1, 1);
    UT_ASSERT(r.count == 3 && r.row[1].team == 2 &&
              strcmp(r.row[2].name, "Eryan") == 0 && r.row[2].team == 1);
    /* The first comes back in another slot: no second row. */
    lvRosterJoin(&r, 4, "rook");
    lvRosterTeam(&r, 4, 3);
    UT_ASSERT(r.count == 3 && r.row[1].team == 3);
    /* What is not a player or a slot changes nothing. */
    lvRosterJoin(&r, 5, "");
    lvRosterJoin(&r, 5, NULL);
    lvRosterJoin(&r, -1, "x");
    lvRosterJoin(&r, LV_ROSTER_SLOTS, "x");
    lvRosterRename(&r, 5, "y");
    lvRosterTeam(&r, 99, 1);
    lvRosterQuit(&r, 99);
    UT_ASSERT(r.count == 3);
    /* A full list takes no more and is not written past. */
    lvRosterClear(&r);
    for (i = 0; i < LV_ROSTER_MAX + 5; i++) {
        snprintf(name, sizeof(name), "p%d", i);
        lvRosterJoin(&r, i % LV_ROSTER_SLOTS, name);
    }
    UT_ASSERT_MSG(r.count == LV_ROSTER_MAX, "a full list holds %d", r.count);

    /* The server's line about a lobby's map. */
    UT_ASSERT(lvMapChangeName("Map changed to Oil Rig") != NULL &&
              strcmp(lvMapChangeName("Map changed to Oil Rig"), "Oil Rig") == 0);
    UT_ASSERT(lvMapChangeName("Map changed to ") == NULL);
    UT_ASSERT(lvMapChangeName("map changed to x") == NULL);
    UT_ASSERT(lvMapChangeName("The Map changed to x") == NULL);
    UT_ASSERT(lvMapChangeName("") == NULL && lvMapChangeName(NULL) == NULL);

    /* Only a map named in the lobby counts, and the last one named there. */
    lvLobbyMapClear(&m);
    lvLobbyMapNamed(&m, "Before");
    UT_ASSERT_MSG(!m.known, "a map named with no lobby open was taken");
    lvLobbyMapEnter(&m);
    lvLobbyMapNamed(&m, NULL); /* a server line about something else */
    lvLobbyMapNamed(&m, "");
    UT_ASSERT(!m.known);
    lvLobbyMapNamed(&m, "First Choice");
    lvLobbyMapNamed(&m, "Oil Rig");
    lvLobbyMapExit(&m);
    lvLobbyMapNamed(&m, "Next Round");
    UT_ASSERT_MSG(m.known && strcmp(m.name, "Oil Rig") == 0,
                  "the lobby's map is \"%s\"", m.name);

    /* The date at the start of a server's own log name. */
    UT_ASSERT(lvSaveNameStamp("20261013t140559_Oil_Rig.wbv", &y, &mo, &d, &h,
                              &mi));
    UT_ASSERT_MSG(y == 2026 && mo == 10 && d == 13 && h == 14 && mi == 5,
                  "%d-%d-%d %d.%d", y, mo, d, h, mi);
    UT_ASSERT(!lvSaveNameStamp("rooklift 08b799de0a0868f4a4e685c8d421e78f.wbv",
                               &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("20261013t140559.wbv", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("20261313t140559_x.wbv", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("20261013t250559_x.wbv", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("2026", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp(NULL, &y, &mo, &d, &h, &mi));
    UT_ASSERT_MSG(y == 2026 && mi == 5, "a refused name changed the parts");
    return 0;
}
