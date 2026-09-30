/*
 * The register and lobby_update bodies carry the scenario that plays, its
 * human cap and the mods that run (test_wbn_scenario_fields.c).
 *
 * winbolonetAddLobbyInfoFields writes "scenario" and "scenario_max_players"
 * only when the stashed lobby info says a scenario decides the round, and
 * "mods" always, [] when none run. The names are author-written text, so
 * cJSON escapes them as it does the map name.
 *
 * What this case pins, one post at a time so the arrival order is fixed:
 *   - the register body winbolonetCreateServer sends carries the three keys;
 *   - a lobby_update with no scenario and no mods carries neither scenario
 *     key and "mods":[];
 *   - one with a scenario and two mods carries all three, in list order;
 *   - one with a scenario and no mods carries the scenario keys and [];
 *   - a mod named with a quote, a backslash and non-ASCII UTF-8 arrives with
 *     the first two escaped and the UTF-8 bytes unchanged.
 *
 * Each body is checked against its arrived length first, so a body the
 * listener cut short fails rather than passing on the part it kept.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "wbn_bearer.h"
#include "winbolonet_core.h"
#include "winbolonet_server.h"
#include "winbolonetevents.h"
#include "winbolonetthread.h"

/* Module-global WBN state, as winbolonet_server.c declares it. */
extern bool winboloNetRunning;
extern char winboloNetServerKey[WINBOLONET_KEY_LEN];

/* No answer is held, so this only has to cover a worker sweep. */
#define SCNFIELDS_DRAIN_MS 15000

/* Pill "Tag" \ Déjà, the é and à as their UTF-8 bytes. */
#define SCNFIELDS_ODD_NAME "Pill \"Tag\" \\ D\xc3\xa9j\xc3\xa0"
#define SCNFIELDS_ODD_JSON \
  "\"mods\":[\"Pill \\\"Tag\\\" \\\\ D\xc3\xa9j\xc3\xa0\"]"

/*********************************************************
*NAME:          scnFieldsInfo
*PURPOSE:
* Fills a lobby info with the scenario and mods given and
* nothing else. The body fields this case does not read are
* left zero.
*********************************************************/
static void scnFieldsInfo(WbnLobbyInfo *info, const char *scenario,
                          BYTE maxPlayers, const char *mod0,
                          const char *mod1) {
  memset(info, 0, sizeof(*info));
  snprintf(info->map, sizeof(info->map), "%s", "wbn_test_map");
  if (scenario != NULL) {
    info->hasScenario = TRUE;
    snprintf(info->scenarioName, sizeof(info->scenarioName), "%s", scenario);
    info->scenarioMaxPlayers = maxPlayers;
  }
  if (mod0 != NULL) {
    snprintf(info->modNames[info->modCount], WBN_SCENARIO_NAME_LEN, "%s",
             mod0);
    info->modCount++;
  }
  if (mod1 != NULL) {
    snprintf(info->modNames[info->modCount], WBN_SCENARIO_NAME_LEN, "%s",
             mod1);
    info->modCount++;
  }
}

/*********************************************************
*NAME:          scnFieldsCheck
*PURPOSE:
* Checks request i arrived whole at the path given, and
* that its body holds every string in want and none in
* notWant. Reports each miss. Returns TRUE when all held.
*********************************************************/
static bool scnFieldsCheck(WbnTestListener *ln, int i, const char *label,
                           const char *path, const char *const *want,
                           int wantCount, const char *const *notWant,
                           int notWantCount) {
  bool ok = TRUE;
  int j;

  if (strcmp(ln->path[i], path) != 0) {
    fprintf(stderr, "scenarioFields: %s was %s, expected %s\n", label,
            ln->path[i], path);
    return FALSE;
  }
  if (ln->bodyLen[i] >= WBN_TEST_BODY_LEN) {
    fprintf(stderr,
            "scenarioFields: %s body was %d bytes, past the %d the listener "
            "keeps\n",
            label, ln->bodyLen[i], WBN_TEST_BODY_LEN);
    return FALSE;
  }
  for (j = 0; j < wantCount; j++) {
    if (strstr(ln->body[i], want[j]) == NULL) {
      fprintf(stderr, "scenarioFields: %s body lacks %s\n  body: %s\n", label,
              want[j], ln->body[i]);
      ok = FALSE;
    }
  }
  for (j = 0; j < notWantCount; j++) {
    if (strstr(ln->body[i], notWant[j]) != NULL) {
      fprintf(stderr, "scenarioFields: %s body holds %s\n  body: %s\n", label,
              notWant[j], ln->body[i]);
      ok = FALSE;
    }
  }
  return ok;
}

/*********************************************************
*NAME:          scnFieldsSend
*PURPOSE:
* Stashes info, sends a lobby_update and waits for it to
* arrive as request number want - 1. Returns TRUE when it
* did.
*********************************************************/
static bool scnFieldsSend(WbnTestListener *ln, const WbnLobbyInfo *info,
                          int want) {
  winbolonetSetLobbyInfo(info);
  winbolonetSendLobbyUpdate();
  if (wbnTestWaitForRequests(ln, want, SCNFIELDS_DRAIN_MS) < want) {
    fprintf(stderr,
            "scenarioFields: lobby_update %d never arrived - listener saw %d "
            "requests\n",
            want - 1, SDL_GetAtomicInt(&ln->requests));
    return FALSE;
  }
  return TRUE;
}

bool wbnTestScenarioFields(void) {
  static const char *const kFull[] = {
    "\"scenario\":\"Survival\"",
    "\"scenario_max_players\":6",
    "\"mods\":[\"Infection\",\"Pillbox Tag\"]",
  };
  static const char *const kScenarioNoMods[] = {
    "\"scenario\":\"Survival\"",
    "\"scenario_max_players\":6",
    "\"mods\":[]",
  };
  static const char *const kPlainWant[] = {
    "\"mods\":[]",
  };
  static const char *const kPlainNot[] = {
    "\"scenario\"",
    "\"scenario_max_players\"",
  };
  static const char *const kOdd[] = {
    SCNFIELDS_ODD_JSON,
  };
  WbnTestListener ln;
  WbnLobbyInfo info;
  unsigned short port = 0;
  char baseUrl[64];
  char mapName[] = "wbn_test_map";
  bool registered;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, /*holdMs*/ 0)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  /* The register body, from the full info: httpCreate, server/register and
   * the one winbolonetThreadCreate the server makes. */
  scnFieldsInfo(&info, "Survival", 6, "Infection", "Pillbox Tag");
  winbolonetSetLobbyInfo(&info);
  registered = winbolonetCreateServer(mapName, /*port*/ 5187, /*gameType*/ 4,
                                      /*ai*/ 0, /*mines*/ FALSE,
                                      /*password*/ FALSE, /*numBases*/ 4,
                                      /*numPills*/ 8, /*freeBases*/ 4,
                                      /*freePills*/ 8, /*numPlayers*/ 0);
  if (!registered) {
    fprintf(stderr, "scenarioFields: winbolonetCreateServer failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  if (wbnTestWaitForRequests(&ln, 1, SCNFIELDS_DRAIN_MS) < 1) {
    fprintf(stderr, "scenarioFields: the register never arrived\n");
    ok = FALSE;
  }

  /* No scenario and no mods. */
  scnFieldsInfo(&info, NULL, 0, NULL, NULL);
  ok = scnFieldsSend(&ln, &info, 2) && ok;

  /* A scenario and two mods. */
  scnFieldsInfo(&info, "Survival", 6, "Infection", "Pillbox Tag");
  ok = scnFieldsSend(&ln, &info, 3) && ok;

  /* A scenario with Mods Enabled off, which the fill sends as no mods. */
  scnFieldsInfo(&info, "Survival", 6, NULL, NULL);
  ok = scnFieldsSend(&ln, &info, 4) && ok;

  /* A mod whose name needs escaping and carries UTF-8. */
  scnFieldsInfo(&info, NULL, 0, SCNFIELDS_ODD_NAME, NULL);
  ok = scnFieldsSend(&ln, &info, 5) && ok;

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  winboloNetRunning = FALSE;
  winboloNetServerKey[0] = '\0';
  winbolonetEventsDestroy();
  wbnTestListenerStop(&ln);

  if (!ok) {
    return FALSE;
  }
  ok = scnFieldsCheck(&ln, 0, "register", "/api/v1/server/register", kFull,
                      3, NULL, 0) && ok;
  ok = scnFieldsCheck(&ln, 1, "plain lobby_update",
                      "/api/v1/server/lobby_update", kPlainWant, 1, kPlainNot,
                      2) && ok;
  ok = scnFieldsCheck(&ln, 2, "scenario and mods lobby_update",
                      "/api/v1/server/lobby_update", kFull, 3, NULL, 0) && ok;
  ok = scnFieldsCheck(&ln, 3, "mods off lobby_update",
                      "/api/v1/server/lobby_update", kScenarioNoMods, 3, NULL,
                      0) && ok;
  ok = scnFieldsCheck(&ln, 4, "escaped mod lobby_update",
                      "/api/v1/server/lobby_update", kOdd, 1, kPlainNot, 2) &&
       ok;
  return ok;
}
