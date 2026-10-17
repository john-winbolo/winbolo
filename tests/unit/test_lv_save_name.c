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
 *   lv_save_name_bots   — every person is named and the bots are counted:
 *       people only, people and bots, bots only, one bot, and bots beside
 *       people who are in teams.
 *   lv_save_name_limits — characters a file name cannot hold, a dot or a
 *       space at the end, a name Windows keeps for a device, a map or a
 *       player name too long, and a UTF-8 character at a cut.
 *   lv_save_name_long   — a list of people too long for the name: whole
 *       names only, then "+ N more", then the bots.
 *   lv_save_name_roster — the list of a log's players as joins, new names,
 *       teams, bots and leavings come; the map a lobby chose; and the date
 *       at the start of a server's own file name.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "lv_save_name.h"
#include "test_harness.h"

/* Build a name and compare it. */
#define SN_EXPECT(want, y, mo, d, h, mi, map, names, teams, bots, n)           \
    do {                                                                       \
        char got_[300];                                                        \
        lvSaveNameBuild(got_, sizeof(got_), y, mo, d, h, mi, map, names,       \
                        teams, bots, n);                                       \
        UT_ASSERT_MSG(strcmp(got_, want) == 0, "got \"%s\", want \"%s\"",      \
                      got_, want);                                             \
    } while (0)

int run_lv_save_name_parts(void) {
    static const char *const   four[]      = {"Canuck", "Maverick", "Eryan", "Bot1"};
    static const unsigned char fourTeams[] = {1, 2, 1, 2};
    static const unsigned char oneTeam[]   = {3, 3, 3, 3};
    static const unsigned char oneNone[]   = {1, 2, 1, 0};
    static const unsigned char three[]     = {5, 2, 9, 2};

    /* Everything: date, time, map, the players by team, lowest team first,
       each team in the order its players came. */
    SN_EXPECT("2026-10-13 14.05 Oil Rig - Canuck, Eryan vs Maverick, Bot1.wbv",
              2026, 10, 13, 14, 5, "Oil Rig", four, fourTeams, NULL, 4);
    /* A date with no time of day. */
    SN_EXPECT("2026-10-13 Oil Rig - Canuck, Eryan vs Maverick, Bot1.wbv",
              2026, 10, 13, -1, 0, "Oil Rig", four, fourTeams, NULL, 4);
    /* No date: the name starts with the map. */
    SN_EXPECT("Oil Rig - Canuck, Eryan vs Maverick, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, fourTeams, NULL, 4);
    /* No players: nothing after the map. */
    SN_EXPECT("2026-10-13 14.05 Oil Rig.wbv",
              2026, 10, 13, 14, 5, "Oil Rig", NULL, NULL, NULL, 0);
    SN_EXPECT("2026-10-13 14.05 Oil Rig.wbv",
              2026, 10, 13, 14, 5, "Oil Rig", four, fourTeams, NULL, 0);
    /* No map: the players follow the date. */
    SN_EXPECT("2026-10-13 14.05 - Canuck, Eryan vs Maverick, Bot1.wbv",
              2026, 10, 13, 14, 5, "", four, fourTeams, NULL, 4);
    SN_EXPECT("2026-10-13 14.05 - Canuck, Eryan vs Maverick, Bot1.wbv",
              2026, 10, 13, 14, 5, NULL, four, fourTeams, NULL, 4);
    /* Players alone, map alone, date alone. */
    SN_EXPECT("Canuck, Eryan vs Maverick, Bot1.wbv",
              0, 0, 0, -1, 0, NULL, four, fourTeams, NULL, 4);
    SN_EXPECT("Oil Rig.wbv", 0, 0, 0, -1, 0, "Oil Rig", NULL, NULL, NULL, 0);
    SN_EXPECT("2026-10-13.wbv", 2026, 10, 13, -1, 0, NULL, NULL, NULL, NULL, 0);
    /* One team only, no teams given, and a player with no team: one list,
       in the order the players came, and no "vs". */
    SN_EXPECT("Oil Rig - Canuck, Maverick, Eryan, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, oneTeam, NULL, 4);
    SN_EXPECT("Oil Rig - Canuck, Maverick, Eryan, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, NULL, NULL, 4);
    SN_EXPECT("Oil Rig - Canuck, Maverick, Eryan, Bot1.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, oneNone, NULL, 4);
    /* Three teams. */
    SN_EXPECT("Oil Rig - Maverick, Bot1 vs Canuck vs Eryan.wbv",
              0, 0, 0, -1, 0, "Oil Rig", four, three, NULL, 4);
    /* Nothing at all. */
    SN_EXPECT("log.wbv", 0, 0, 0, -1, 0, NULL, NULL, NULL, NULL, 0);
    SN_EXPECT("log.wbv", 0, 0, 0, -1, 0, "   ", NULL, NULL, NULL, 0);
    return 0;
}

int run_lv_save_name_bots(void) {
    static const char *const   six[]    = {"Sethos", "ErYan", "Canuck",
                                           "Chapu", "rider", "Elvis"};
    static const unsigned char noBots[] = {0, 0, 0, 0, 0, 0};
    static const char *const   mixed[]  = {"Canuck", "Hal", "Chapu", "Marvin",
                                           "", "Robby"};
    static const unsigned char mixedBots[] = {0, 1, 0, 1, 1, 1};
    static const unsigned char allBots[]   = {1, 1, 1, 1, 1, 1};
    static const char *const   eight[]     = {"a1", "a2", "a3", "a4", "a5",
                                              "b1", "b2", "b3"};
    static const unsigned char eightTeams[] = {1, 1, 1, 1, 1, 2, 2, 2};
    /* Two people on team 1, one on team 2, a bot on each team. */
    static const char *const   sides[]     = {"Canuck", "Eryan", "Maverick",
                                              "Hal", "Marvin"};
    static const unsigned char sidesTeams[] = {1, 1, 2, 2, 1};
    static const unsigned char sidesBots[]  = {0, 0, 0, 1, 1};
    /* The people are one team and the bots the other. */
    static const unsigned char vsBotsTeams[] = {1, 1, 1, 2, 2};
    static const unsigned char vsBotsBots[]  = {0, 0, 0, 1, 1};
    /* A bot with no team beside people who all have one. */
    static const unsigned char looseTeams[] = {1, 1, 2, 0, 0};

    /* People only: every one is named, and there is no word about bots. */
    SN_EXPECT("2026-10-13 Chew Toy 3 - Sethos, ErYan, Canuck, Chapu, rider, "
              "Elvis.wbv",
              2026, 10, 13, -1, 0, "Chew Toy 3", six, NULL, noBots, 6);
    SN_EXPECT("2026-10-13 Chew Toy 3 - Sethos, ErYan, Canuck, Chapu, rider, "
              "Elvis.wbv",
              2026, 10, 13, -1, 0, "Chew Toy 3", six, NULL, NULL, 6);
    /* More than six, which was the most the name once held: all are named,
       as one list and by team. */
    SN_EXPECT("M - a1, a2, a3, a4, a5, b1, b2, b3.wbv", 0, 0, 0, -1, 0, "M",
              eight, NULL, NULL, 8);
    SN_EXPECT("M - a1, a2, a3, a4, a5 vs b1, b2, b3.wbv", 0, 0, 0, -1, 0, "M",
              eight, eightTeams, NULL, 8);

    /* People and bots: the bots are not named. They are counted after the
       people, wherever in the list they came, and a bot with no name is
       still a bot. */
    SN_EXPECT("2026-10-13 Chew Toy 3 - Canuck, Chapu, 4 Bots.wbv",
              2026, 10, 13, -1, 0, "Chew Toy 3", mixed, NULL, mixedBots, 6);
    /* One bot is "1 Bot". */
    SN_EXPECT("2026-10-13 Chew Toy 3 - Canuck, 1 Bot.wbv",
              2026, 10, 13, -1, 0, "Chew Toy 3", mixed, NULL, mixedBots, 2);

    /* Bots only: the count alone. */
    SN_EXPECT("2026-10-13 Chew Toy 3 - 4 Bots.wbv",
              2026, 10, 13, -1, 0, "Chew Toy 3", mixed, NULL, allBots, 4);
    SN_EXPECT("2026-10-13 Chew Toy 3 - 1 Bot.wbv",
              2026, 10, 13, -1, 0, "Chew Toy 3", mixed, NULL, allBots, 1);
    SN_EXPECT("4 Bots.wbv", 0, 0, 0, -1, 0, NULL, mixed, NULL, allBots, 4);
    SN_EXPECT("2026-10-13 - 6 Bots.wbv", 2026, 10, 13, -1, 0, "", mixed, NULL,
              allBots, 6);

    /* Teams are the people's: the bots are one count at the end, whichever
       side each was on. */
    SN_EXPECT("Oil Rig - Canuck, Eryan vs Maverick, 2 Bots.wbv",
              0, 0, 0, -1, 0, "Oil Rig", sides, sidesTeams, sidesBots, 5);
    /* The people are all one side, so there is no "vs". */
    SN_EXPECT("Oil Rig - Canuck, Eryan, Maverick, 2 Bots.wbv",
              0, 0, 0, -1, 0, "Oil Rig", sides, vsBotsTeams, vsBotsBots, 5);
    /* A bot with no team does not take the "vs" from the people. */
    SN_EXPECT("Oil Rig - Canuck, Eryan vs Maverick, 2 Bots.wbv",
              0, 0, 0, -1, 0, "Oil Rig", sides, looseTeams, sidesBots, 5);
    return 0;
}

int run_lv_save_name_limits(void) {
    static const char *const bad[] = {"a/b\\c", "x:y*z?", "\"q\"<r>|s",
                                      "t\tu\x01v", "  two   spaces  ",
                                      "", "   "};
    static const unsigned char badBots[] = {0, 0, 0, 0, 0, 0, 0};
    static const char *const   dots[]    = {"end. ."};
    static const char *const   devices[] = {"CON", "prn", "Aux", "NUL", "COM1",
                                            "lpt9", "nul.old"};
    static const char *const   notDevices[] = {"COM10", "COM0", "Console",
                                               "CO", "LPT", "NULL"};
    char        got[400];
    char        want[400];
    char        text[200];
    const char *one[1];
    int         i;

    /* What a file name cannot hold becomes a dot; a tab and a control
       character too. Spaces at the ends go, a run of them is one, and a
       name with nothing in it is not a player. */
    SN_EXPECT("O.R - a.b.c, x.y.z., .q..r..s, t.u.v, two spaces.wbv", 0, 0, 0,
              -1, 0, "O/R", bad, NULL, badBots, 7);
    /* None of them is left anywhere in a name. */
    lvSaveNameBuild(got, sizeof(got), 2026, 10, 13, 14, 5, "a<b>c:d|e", bad,
                    NULL, NULL, 7);
    UT_ASSERT_MSG(strpbrk(got, "\\/:*?\"<>|\t\x01") == NULL,
                  "a character a file name cannot hold: \"%s\"", got);

    /* Windows refuses a name that ends with a dot or a space. */
    SN_EXPECT("Map - end.wbv", 0, 0, 0, -1, 0, "Map", dots, NULL, NULL, 1);
    SN_EXPECT("Map.wbv", 0, 0, 0, -1, 0, "Map. . ", NULL, NULL, NULL, 0);
    SN_EXPECT("log.wbv", 0, 0, 0, -1, 0, "...", NULL, NULL, NULL, 0);

    /* A name Windows keeps for a device, alone or before a dot, gets "log "
       in front. One that only starts like a device does not. */
    for (i = 0; i < (int)(sizeof(devices) / sizeof(devices[0])); i++) {
        snprintf(want, sizeof(want), "log %s.wbv", devices[i]);
        SN_EXPECT(want, 0, 0, 0, -1, 0, devices[i], NULL, NULL, NULL, 0);
        one[0] = devices[i];
        SN_EXPECT(want, 0, 0, 0, -1, 0, NULL, one, NULL, NULL, 1);
    }
    for (i = 0; i < (int)(sizeof(notDevices) / sizeof(notDevices[0])); i++) {
        snprintf(want, sizeof(want), "%s.wbv", notDevices[i]);
        SN_EXPECT(want, 0, 0, 0, -1, 0, notDevices[i], NULL, NULL, NULL, 0);
    }
    one[0] = "x";
    SN_EXPECT("CON - x.wbv", 0, 0, 0, -1, 0, "CON", one, NULL, NULL, 1);

    /* A map of more than 64 bytes is cut to 64. */
    memset(text, 'm', 150);
    text[150] = '\0';
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, text, NULL, NULL, NULL, 0);
    UT_ASSERT_MSG(strlen(got) == LV_SAVE_NAME_MAP_BYTES + 4 &&
                  strcmp(got + LV_SAVE_NAME_MAP_BYTES, ".wbv") == 0,
                  "a long map: length %d", (int)strlen(got));
    /* A player name of more than 64 bytes is cut to 64, as the game's own
       names are. */
    one[0] = text;
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, "M", one, NULL, NULL, 1);
    UT_ASSERT_MSG(strlen(got) == 4 + 64 + 4 && strncmp(got, "M - mmm", 7) == 0,
                  "a long player name: length %d", (int)strlen(got));

    /* UTF-8 is never split. 63 bytes and then a character of two, three or
       four bytes: the 64th byte is inside it, so all of it goes. */
    {
        static const char *const tails[] = {"\xC3\xA9", "\xE2\x82\xAC",
                                            "\xF0\x9F\x98\x80"};
        for (i = 0; i < 3; i++) {
            memset(text, 'm', 63);
            snprintf(text + 63, sizeof(text) - 63, "%szz", tails[i]);
            lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, text, NULL, NULL,
                            NULL, 0);
            UT_ASSERT_MSG(strlen(got) == 67 && strcmp(got + 63, ".wbv") == 0,
                          "a %d byte character in a map was split: length %d",
                          i + 2, (int)strlen(got));
            one[0] = text;
            lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, NULL, one, NULL,
                            NULL, 1);
            UT_ASSERT_MSG(strlen(got) == 67 && strcmp(got + 63, ".wbv") == 0,
                          "a %d byte character in a name was split: length %d",
                          i + 2, (int)strlen(got));
        }
        /* One that ends at the limit stays whole. */
        memset(text, 'm', 62);
        snprintf(text + 62, sizeof(text) - 62, "\xC3\xA9more");
        lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, text, NULL, NULL,
                        NULL, 0);
        UT_ASSERT_MSG(strlen(got) == 68 && strcmp(got + 62, "\xC3\xA9.wbv") == 0,
                      "a character that fits was cut");
        /* A name in UTF-8 that fits is not changed. */
        one[0] = "J\xC3\xBCrgen \xE2\x82\xAC";
        SN_EXPECT("M - J\xC3\xBCrgen \xE2\x82\xAC.wbv", 0, 0, 0, -1, 0, "M", one,
                  NULL, NULL, 1);
    }

    /* A small buffer is filled and ended, no more. */
    lvSaveNameBuild(got, 8, 2026, 10, 13, 14, 5, "Oil Rig", NULL, NULL, NULL, 0);
    UT_ASSERT_MSG(strcmp(got, "2026-10") == 0, "small buffer: \"%s\"", got);
    return 0;
}

/* Names of 20 bytes: "aaaaaaaaaaaaaaaaaaaa", "bbbb...", and so on. */
#define SN_LONG_COUNT 12
static char        s_longText[SN_LONG_COUNT][21];
static const char *s_longNames[SN_LONG_COUNT];

static void snMakeLongNames(void) {
    int i;
    for (i = 0; i < SN_LONG_COUNT; i++) {
        memset(s_longText[i], 'a' + i, 20);
        s_longText[i][20] = '\0';
        s_longNames[i]    = s_longText[i];
    }
}

/* The names at the given indexes with ", " between them; index -1 puts
 * " vs " in place of the ", " there. */
static void snJoin(char *out, size_t size, const int *idx, int n) {
    size_t len = 0;
    int    i;
    bool   vs = false, first = true;
    out[0] = '\0';
    for (i = 0; i < n; i++) {
        if (idx[i] < 0) {
            vs = true;
            continue;
        }
        len += (size_t)snprintf(out + len, size - len, "%s%s",
                                first ? "" : (vs ? " vs " : ", "),
                                s_longNames[idx[i]]);
        vs    = false;
        first = false;
    }
}

int run_lv_save_name_long(void) {
    static const unsigned char tenTeams[] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 2};
    static const unsigned char twoBots[]  = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1};
    static const int first7[]  = {0, 1, 2, 3, 4, 5, 6};
    static const int first8[]  = {0, 1, 2, 3, 4, 5, 6, 7};
    static const int byTeam7[] = {0, 1, 2, 3, -1, 6, 7, 8};
    char        got[400], want[400], list[300];
    char        shortName[24];
    const char *names[SN_LONG_COUNT];
    size_t      n;
    int         i;

    snMakeLongNames();

    /* Ten people of 20 bytes after "M - " would be 4 + 200 + 18 = 222 bytes,
       past the 176 that go before ".wbv". Eight with " + 2 more" would be
       187. Seven with " + 3 more" are 165: seven whole names, and the count
       of the three that are not there. */
    snJoin(list, sizeof(list), first7, 7);
    snprintf(want, sizeof(want), "M - %s + 3 more.wbv", list);
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, "M", s_longNames, NULL,
                    NULL, 10);
    UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got, want);
    UT_ASSERT_MSG(strlen(got) <= LV_SAVE_NAME_MAX_BYTES, "length %d",
                  (int)strlen(got));

    /* With two bots too: the people that fit, the count of the people that
       do not, and then the bots. */
    snprintf(want, sizeof(want), "M - %s + 3 more, 2 Bots.wbv", list);
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, "M", s_longNames, NULL,
                    twoBots, 12);
    UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got, want);

    /* By team the names kept are taken a player from each team in turn: of
       six against four, four and three. */
    snJoin(list, sizeof(list), byTeam7, 8);
    snprintf(want, sizeof(want), "M - %s + 3 more.wbv", list);
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, "M", s_longNames,
                    tenTeams, NULL, 10);
    UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got, want);

    /* A list that is the limit to the byte is all there: "M - " and eight
       names, one of them 18 bytes, are 4 + 158 + 14 = 176, and the name is
       180 with ".wbv". */
    memset(shortName, 'z', 18);
    shortName[18] = '\0';
    for (i = 0; i < 8; i++) {
        names[i] = s_longNames[i];
    }
    names[7] = shortName;
    snJoin(list, sizeof(list), first7, 7);
    snprintf(want, sizeof(want), "M - %s, %s.wbv", list, shortName);
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, "M", names, NULL, NULL, 8);
    UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got, want);
    UT_ASSERT_MSG(strlen(got) == LV_SAVE_NAME_MAX_BYTES, "length %d",
                  (int)strlen(got));
    /* One byte more and the last name goes, whole. */
    shortName[18] = 'z';
    shortName[19] = '\0';
    snprintf(want, sizeof(want), "M - %s + 1 more.wbv", list);
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, "M", names, NULL, NULL, 8);
    UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got, want);

    /* Eight of 20 bytes fit with no map and no date: 160 + 14 = 174. */
    snJoin(list, sizeof(list), first8, 8);
    snprintf(want, sizeof(want), "%s.wbv", list);
    lvSaveNameBuild(got, sizeof(got), 0, 0, 0, -1, 0, NULL, s_longNames, NULL,
                    NULL, 8);
    UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got, want);

    /* The worst the viewer can give: a date, a map of 64 bytes, 32 people
       with names of 64 bytes and 16 bots. The name is in the limit, starts
       with a whole name, and counts the 31 that are not there. */
    {
        static char          big[LV_ROSTER_MAX + 16][LV_ROSTER_NAME_LEN];
        static const char   *bigNames[LV_ROSTER_MAX + 16];
        static unsigned char bigBots[LV_ROSTER_MAX + 16];
        char                 map[LV_SAVE_NAME_MAP_BYTES + 1];

        memset(map, 'M', LV_SAVE_NAME_MAP_BYTES);
        map[LV_SAVE_NAME_MAP_BYTES] = '\0';
        for (i = 0; i < LV_ROSTER_MAX + 16; i++) {
            memset(big[i], 'A' + (i % 26), LV_ROSTER_NAME_LEN - 1);
            big[i][0]                      = (char)('a' + (i / 26));
            big[i][LV_ROSTER_NAME_LEN - 1] = '\0';
            bigNames[i]                    = big[i];
            bigBots[i]                     = (i >= LV_ROSTER_MAX) ? 1 : 0;
        }
        lvSaveNameBuild(got, sizeof(got), 2026, 10, 13, 14, 5, map, bigNames,
                        NULL, bigBots, LV_ROSTER_MAX + 16);
        n = strlen(got);
        snprintf(want, sizeof(want), "2026-10-13 14.05 %s - %s + 31 more, "
                 "16 Bots.wbv", map, big[0]);
        UT_ASSERT_MSG(strcmp(got, want) == 0, "got \"%s\", want \"%s\"", got,
                      want);
        UT_ASSERT_MSG(n <= LV_SAVE_NAME_MAX_BYTES, "length %d", (int)n);
    }
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
    lvRosterJoin(&r, 1, "Maverick");
    lvRosterTeam(&r, 0, 1);
    lvRosterTeam(&r, 1, 2);
    UT_ASSERT(r.count == 2);
    UT_ASSERT(strcmp(r.row[0].name, "Canuck") == 0 && r.row[0].team == 1);
    UT_ASSERT(strcmp(r.row[1].name, "Maverick") == 0 && r.row[1].team == 2);
    /* Nobody is a bot until the log says so. */
    UT_ASSERT(r.row[0].bot == 0 && r.row[1].bot == 0);
    /* A new name is the same player; leaving a team keeps the side. */
    lvRosterRename(&r, 1, "rook");
    lvRosterTeam(&r, 1, 0);
    UT_ASSERT(r.count == 2 && strcmp(r.row[1].name, "rook") == 0 &&
              r.row[1].team == 2);
    /* A player leaves and another takes the slot: both are in the list, and
       the slot's team is now the new player's. */
    lvRosterQuit(&r, 1);
    lvRosterTeam(&r, 1, 5); /* nobody is there */
    lvRosterBot(&r, 1, true); /* nobody is there */
    lvRosterJoin(&r, 1, "Eryan");
    lvRosterTeam(&r, 1, 1);
    UT_ASSERT(r.count == 3 && r.row[1].team == 2 &&
              strcmp(r.row[2].name, "Eryan") == 0 && r.row[2].team == 1);
    UT_ASSERT_MSG(r.row[1].bot == 0, "a player who left was made a bot");
    /* The first comes back in another slot: no second row. */
    lvRosterJoin(&r, 4, "rook");
    lvRosterTeam(&r, 4, 3);
    UT_ASSERT(r.count == 3 && r.row[1].team == 3);
    /* A bot takes a slot: its row has the mark, and a row of a person in a
       slot a bot was in does not. */
    lvRosterJoin(&r, 6, "Hal");
    lvRosterBot(&r, 6, true);
    UT_ASSERT(r.count == 4 && r.row[3].bot == 1);
    lvRosterQuit(&r, 6);
    lvRosterJoin(&r, 6, "Chapu");
    lvRosterBot(&r, 6, false);
    UT_ASSERT(r.count == 5 && r.row[3].bot == 1 && r.row[4].bot == 0);
    /* What is not a player or a slot changes nothing. */
    lvRosterJoin(&r, 5, "");
    lvRosterJoin(&r, 5, NULL);
    lvRosterJoin(&r, -1, "x");
    lvRosterJoin(&r, LV_ROSTER_SLOTS, "x");
    lvRosterRename(&r, 5, "y");
    lvRosterTeam(&r, 99, 1);
    lvRosterBot(&r, 99, true);
    lvRosterBot(&r, -1, true);
    lvRosterBot(&r, 5, true);
    lvRosterQuit(&r, 99);
    UT_ASSERT(r.count == 5);
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
    UT_ASSERT(!lvSaveNameStamp("Maverick 08b799de0a0868f4a4e685c8d421e78f.wbv",
                               &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("20261013t140559.wbv", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("20261313t140559_x.wbv", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("20261013t250559_x.wbv", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp("2026", &y, &mo, &d, &h, &mi));
    UT_ASSERT(!lvSaveNameStamp(NULL, &y, &mo, &d, &h, &mi));
    UT_ASSERT_MSG(y == 2026 && mi == 5, "a refused name changed the parts");
    return 0;
}
