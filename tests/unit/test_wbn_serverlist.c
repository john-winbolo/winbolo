/*
 * Tests for the pure cJSON parser in wbn_serverlist.c:
 * wbnServerListParse (GET /api/v1/games 200 body).
 *
 * Parser-only: no network. The module references
 * wbn_api_get_public (in http.c) only from wbnFetchServerList,
 * which is stubbed in test_stubs.c so the binary links without
 * curl. wbnFetchServerList itself is validated live, not here.
 */

#include <string.h>
#include <stdlib.h>

#include "test_harness.h"
#include "wbn_serverlist.h"

/* -------------------------------------------------------------------- */

static int parse_full_entry(void) {
    const char *body =
        "{"
        "\"tversion\":2,"
        "\"motd\":[\"line one\",\"line two\"],"
        "\"servers\":[{"
        "\"address\":\"play.example.net\","
        "\"port\":50000,"
        "\"server_key\":\"abc123\","
        "\"map\":\"Everard Island\","
        "\"map_md5\":\"0123456789abcdef0123456789abcdef\","
        "\"version\":\"1.0\","
        "\"country\":\"US\","
        "\"game_type\":2,"
        "\"ai\":1,"
        "\"mines\":true,"
        "\"password\":false,"
        "\"random_map\":true,"
        "\"ranked\":true,"
        "\"in_lobby\":true,"
        "\"has_lobby\":true,"
        "\"allow_new_players\":true,"
        "\"auto_lock\":false,"
        "\"time_limit\":true,"
        "\"time_minutes\":30,"
        "\"free_bases\":4,"
        "\"free_pills\":8,"
        "\"num_bases\":16,"
        "\"num_pills\":12,"
        "\"num_players\":5,"
        "\"num_humans\":3,"
        "\"num_bots\":2,"
        "\"pillview\":1,"
        "\"baseview\":2,"
        "\"allyview\":3,"
        "\"alliesintrees\":true,"
        "\"overviewwindow\":1,"
        "\"lineofsight\":1,"
        "\"smartpingsoff\":true,"
        "\"pillviewdecay\":45,"
        "\"baseviewdecay\":90,"
        "\"allyviewdecay\":15,"
        "\"players\":[\"alice\",\"bob\",\"carol\"]"
        "}]"
        "}";
    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    UT_ASSERT_MSG(list.tversion == 2, "tversion=%d", list.tversion);
    UT_ASSERT_MSG(list.numMotd == 2, "numMotd=%d", list.numMotd);
    UT_ASSERT_MSG(strcmp(list.motd[0], "line one") == 0, "motd0=\"%s\"", list.motd[0]);
    UT_ASSERT_MSG(list.count == 1, "count=%d", list.count);

    const WbnServerListEntry *s = &list.servers[0];
    UT_ASSERT_MSG(strcmp(s->address, "play.example.net") == 0, "address=\"%s\"", s->address);
    UT_ASSERT_MSG(s->port == 50000, "port=%d", s->port);
    UT_ASSERT_MSG(strcmp(s->serverKey, "abc123") == 0, "serverKey=\"%s\"", s->serverKey);
    UT_ASSERT_MSG(strcmp(s->map, "Everard Island") == 0, "map=\"%s\"", s->map);
    UT_ASSERT_MSG(strcmp(s->country, "US") == 0, "country=\"%s\"", s->country);
    UT_ASSERT_MSG(s->gameType == 2, "gameType=%d", s->gameType);
    UT_ASSERT_MSG(s->ai == 1, "ai=%d", s->ai);
    UT_ASSERT_MSG(s->mines, "mines should be true");
    UT_ASSERT_MSG(!s->password, "password should be false");
    UT_ASSERT_MSG(s->randomMap && s->ranked && s->inLobby && s->hasLobby,
                  "flags should be true");
    UT_ASSERT_MSG(s->allowNewPlayers && !s->autoLock, "new-players/auto-lock");
    UT_ASSERT_MSG(s->timeLimit && s->timeMinutes == 30, "time limit/minutes");
    UT_ASSERT_MSG(s->freeBases == 4 && s->freePills == 8, "free bases/pills");
    UT_ASSERT_MSG(s->numBases == 16 && s->numPills == 12, "total bases/pills");
    UT_ASSERT_MSG(s->numPlayers == 5 && s->numHumans == 3 && s->numBots == 2,
                  "player counts");
    UT_ASSERT_MSG(s->numPlayerNames == 3, "numPlayerNames=%d", s->numPlayerNames);
    UT_ASSERT_MSG(strcmp(s->players[1], "bob") == 0, "players1=\"%s\"", s->players[1]);
    UT_ASSERT_MSG(s->pillView == 1 && s->baseView == 2 && s->allyView == 3,
                  "view policies=%d/%d/%d", s->pillView, s->baseView, s->allyView);
    UT_ASSERT_MSG(s->alliesInTrees, "alliesInTrees should be true");
    /* 1 is the narrow overview window, and 1 is sight blocked by
     * buildings and trees. Both are 0 by default. */
    UT_ASSERT_MSG(s->overviewWindow == 1 && s->lineOfSight == 1,
                  "overview window/line of sight=%d/%d, want 1/1",
                  s->overviewWindow, s->lineOfSight);
    UT_ASSERT_MSG(s->pillViewDecay == 45 && s->baseViewDecay == 90 &&
                      s->allyViewDecay == 15,
                  "decay secs=%d/%d/%d", s->pillViewDecay, s->baseViewDecay,
                  s->allyViewDecay);
    /* Negative sense: true means the host banned smart pings. */
    UT_ASSERT_MSG(s->smartPingsOff, "smartpingsoff should be true");

    wbnServerListFree(&list);
    return 0;
}

static int parse_forward_compat_version(void) {
    /* A higher tversion still parses the known fields. */
    const char *body =
        "{\"tversion\":99,\"unknown_top\":42,"
        "\"servers\":[{\"address\":\"h\",\"port\":1,\"extra_field\":7}]}";
    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    UT_ASSERT_MSG(list.tversion == 99, "tversion=%d", list.tversion);
    UT_ASSERT_MSG(list.count == 1, "count=%d", list.count);
    UT_ASSERT_MSG(strcmp(list.servers[0].address, "h") == 0,
                  "address=\"%s\"", list.servers[0].address);
    UT_ASSERT_MSG(list.servers[0].port == 1, "port=%d", list.servers[0].port);
    wbnServerListFree(&list);
    return 0;
}

static int parse_defaults(void) {
    /* A server object omitting most fields -> false/0/"". */
    const char *body = "{\"servers\":[{\"address\":\"only\"}]}";
    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    UT_ASSERT_MSG(list.tversion == 0, "tversion default=%d", list.tversion);
    UT_ASSERT_MSG(list.numMotd == 0, "numMotd default=%d", list.numMotd);
    UT_ASSERT_MSG(list.count == 1, "count=%d", list.count);

    const WbnServerListEntry *s = &list.servers[0];
    UT_ASSERT_MSG(!s->hasLobby && !s->ranked && !s->allowSpectators,
                  "missing bools default false");
    UT_ASSERT_MSG(s->country[0] == '\0', "country default empty, got \"%s\"", s->country);
    UT_ASSERT_MSG(s->map[0] == '\0', "map default empty");
    UT_ASSERT_MSG(s->port == 0 && s->numPlayers == 0 && s->spectatorCount == 0,
                  "missing ints default 0");
    UT_ASSERT_MSG(s->numPlayerNames == 0, "numPlayerNames default 0");
    /* The view policies do NOT all default to 0 — an absent "baseview"
     * means bases are off (3), not always-visible. */
    UT_ASSERT_MSG(s->pillView == 0 && s->baseView == 3 && s->allyView == 0,
                  "absent view policies=%d/%d/%d, want 0/3/0",
                  s->pillView, s->baseView, s->allyView);
    UT_ASSERT_MSG(!s->alliesInTrees, "absent alliesintrees should be false");
    /* Absent "smartpingsoff" is false, which means smart pings are ALLOWED —
     * what every server and tracker that predates the field reports. */
    UT_ASSERT_MSG(!s->smartPingsOff,
                  "absent smartpingsoff should be false, meaning pings "
                  "are allowed");
    /* Absent overview window and line of sight are the expanded window
     * with nothing blocking sight, which is what the server ran before
     * the fields existed. */
    UT_ASSERT_MSG(s->overviewWindow == 0 && s->lineOfSight == 0,
                  "absent overview window/line of sight=%d/%d, want 0/0",
                  s->overviewWindow, s->lineOfSight);
    /* Absent decay seconds are VIEW_DECAY_DEFAULT_SECS, not 0 — a
     * tracker that has not learned the fields must not report 0s. */
    UT_ASSERT_MSG(s->pillViewDecay == 30 && s->baseViewDecay == 30 &&
                      s->allyViewDecay == 30,
                  "absent decay secs=%d/%d/%d, want 30/30/30",
                  s->pillViewDecay, s->baseViewDecay, s->allyViewDecay);
    wbnServerListFree(&list);

    /* Empty servers array -> success, count 0, nothing to free. */
    UT_ASSERT(wbnServerListParse("{\"servers\":[]}", &list));
    UT_ASSERT_MSG(list.count == 0, "empty count=%d", list.count);
    UT_ASSERT_MSG(list.servers == NULL, "empty servers should be NULL");
    wbnServerListFree(&list);
    return 0;
}

static int parse_view_out_of_range(void) {
    /* A number neither enum names — a newer tracker, or a bad row — must
     * not reach the browser as a mode with no name. Each clamps back to
     * its default. */
    const char *body =
        "{\"servers\":[{\"address\":\"h\",\"overviewwindow\":7,"
        "\"lineofsight\":-3}]}";
    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    UT_ASSERT_MSG(list.count == 1, "count=%d", list.count);

    const WbnServerListEntry *s = &list.servers[0];
    UT_ASSERT_MSG(s->overviewWindow == 0,
                  "out-of-range overview window=%d, want 0", s->overviewWindow);
    UT_ASSERT_MSG(s->lineOfSight == 0,
                  "out-of-range line of sight=%d, want 0", s->lineOfSight);
    wbnServerListFree(&list);
    return 0;
}

int run_wbn_serverlist_parse(void) {
    int rc;
    rc = parse_full_entry();            if (rc) return rc;
    rc = parse_forward_compat_version(); if (rc) return rc;
    rc = parse_defaults();              if (rc) return rc;
    rc = parse_view_out_of_range();     if (rc) return rc;
    return 0;
}

/* -------------------------------------------------------------------- */

static int players_skip_blanks(void) {
    /* Empty and all-whitespace usernames are skipped; count is real names. */
    const char *body =
        "{\"servers\":[{\"players\":[\"alice\",\"\",\"   \",\"bob\"]}]}";
    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    const WbnServerListEntry *s = &list.servers[0];
    UT_ASSERT_MSG(s->numPlayerNames == 2, "numPlayerNames=%d", s->numPlayerNames);
    UT_ASSERT_MSG(strcmp(s->players[0], "alice") == 0, "players0=\"%s\"", s->players[0]);
    UT_ASSERT_MSG(strcmp(s->players[1], "bob") == 0, "players1=\"%s\"", s->players[1]);
    wbnServerListFree(&list);
    return 0;
}

static int players_clamp_to_max(void) {
    /* MAX_TANKS+4 real names -> clamped to MAX_TANKS, no overflow. */
    char body[2048];
    int n = snprintf(body, sizeof(body), "{\"servers\":[{\"players\":[");
    for (int i = 0; i < MAX_TANKS + 4; i++) {
        n += snprintf(body + n, sizeof(body) - n, "%s\"p%d\"",
                      i == 0 ? "" : ",", i);
    }
    snprintf(body + n, sizeof(body) - n, "]}]}");

    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    UT_ASSERT_MSG(list.servers[0].numPlayerNames == MAX_TANKS,
                  "numPlayerNames=%d expected %d",
                  list.servers[0].numPlayerNames, MAX_TANKS);
    wbnServerListFree(&list);
    return 0;
}

int run_wbn_serverlist_players(void) {
    int rc;
    rc = players_skip_blanks();   if (rc) return rc;
    rc = players_clamp_to_max();  if (rc) return rc;
    return 0;
}

/* -------------------------------------------------------------------- */

static int motd_cap_and_truncate(void) {
    /* More than WBN_MOTD_LINES lines -> dropped to the cap. */
    char body[1024];
    int n = snprintf(body, sizeof(body), "{\"servers\":[],\"motd\":[");
    for (int i = 0; i < WBN_MOTD_LINES + 3; i++) {
        n += snprintf(body + n, sizeof(body) - n, "%s\"m%d\"",
                      i == 0 ? "" : ",", i);
    }
    snprintf(body + n, sizeof(body) - n, "]}");

    WbnServerList list;
    UT_ASSERT(wbnServerListParse(body, &list));
    UT_ASSERT_MSG(list.numMotd == WBN_MOTD_LINES, "numMotd=%d expected %d",
                  list.numMotd, WBN_MOTD_LINES);
    wbnServerListFree(&list);

    /* An over-long line is truncated to WBN_MOTD_LINE_LEN-1, no overflow. */
    char longbody[512];
    char longline[WBN_MOTD_LINE_LEN + 64];
    memset(longline, 'x', sizeof(longline) - 1);
    longline[sizeof(longline) - 1] = '\0';
    snprintf(longbody, sizeof(longbody),
             "{\"servers\":[],\"motd\":[\"%s\"]}", longline);
    UT_ASSERT(wbnServerListParse(longbody, &list));
    UT_ASSERT_MSG(list.numMotd == 1, "numMotd=%d", list.numMotd);
    UT_ASSERT_MSG(strlen(list.motd[0]) == WBN_MOTD_LINE_LEN - 1,
                  "motd len=%zu expected %d", strlen(list.motd[0]),
                  WBN_MOTD_LINE_LEN - 1);
    wbnServerListFree(&list);
    return 0;
}

int run_wbn_serverlist_motd(void) {
    return motd_cap_and_truncate();
}

/* -------------------------------------------------------------------- */

static int malformed_rejected(void) {
    WbnServerList list;

    /* Not JSON at all. */
    UT_ASSERT(!wbnServerListParse("Not Found", &list));
    UT_ASSERT_MSG(list.servers == NULL, "servers should be NULL on failure");
    UT_ASSERT_MSG(list.count == 0, "count should be 0 on failure");
    wbnServerListFree(&list);  /* no-op, must not crash */

    /* Valid JSON object but no servers array. */
    UT_ASSERT(!wbnServerListParse("{\"tversion\":2}", &list));
    UT_ASSERT_MSG(list.servers == NULL, "servers should be NULL on failure");
    wbnServerListFree(&list);

    /* servers present but not an array. */
    UT_ASSERT(!wbnServerListParse("{\"servers\":42}", &list));
    UT_ASSERT_MSG(list.servers == NULL, "servers should be NULL on failure");
    wbnServerListFree(&list);

    /* NULL input. */
    UT_ASSERT(!wbnServerListParse(NULL, &list));
    wbnServerListFree(&list);
    return 0;
}

int run_wbn_serverlist_malformed(void) {
    return malformed_rejected();
}
