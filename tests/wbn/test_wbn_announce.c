/*
 * server/announce (test_wbn_announce.c).
 *
 * WinBolo.net keeps one address per family for a server and only ever takes
 * it from the connection a request arrived on. After each successful register
 * the server posts server/announce, with its bearer and the body {}, once to
 * a name with only an A record and once to a name with only an AAAA record,
 * so each post can only arrive over its own family.
 *
 * Four cases:
 *   - announce_hosts_rule: which names the announce goes to, from the
 *     override flag, Host, Host4 and Host6 alone.
 *   - api_url_for_host: the URL a request with a host of its own is sent to,
 *     and that no host means the configured one.
 *   - hosted_job_host: a job queued with a host is posted to that host, and a
 *     job queued through the existing calls is posted to the configured one;
 *     a custom host with no Host4 or Host6 sends no announce at all.
 *   - announce_posts: a register followed by winbolonetServerAnnounce puts
 *     two server/announce posts on the wire, each with the bearer and the
 *     body {}, read from Host4 and Host6 in a preferences file.
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
#include "common/prefs.h"

#ifdef _WIN32
#include <process.h>
#define ANNOUNCE_PID() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define ANNOUNCE_PID() ((unsigned long)getpid())
#endif

/* Set by CMake to "${CMAKE_BINARY_DIR}/test-scratch"; see
 * test_wbn_log_upload.c. */
#ifndef WB_TEST_SCRATCH_DIR
#define WB_TEST_SCRATCH_DIR "test-scratch"
#endif

/* Module-global WBN state, as winbolonet_server.c declares it. */
extern bool winboloNetRunning;
extern char winboloNetServerKey[WINBOLONET_KEY_LEN];

#define ANNOUNCE_KEY "cccccccccccccccccccccccccccccccc"
#define ANNOUNCE_FAKE_BEARER \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* No answer is held, so this only covers a worker sweep. */
#define ANNOUNCE_WAIT_MS 15000

#define ANNOUNCE_PATH "/api/v1/server/announce"

/*********************************************************
*NAME:          hostsCase
*PURPOSE:
* Runs winbolonetAnnounceHosts on one set of inputs and
* compares both answers. Returns okSoFar when they match
* and FALSE when they do not.
*********************************************************/
static bool hostsCase(bool okSoFar, const char *what, bool overridden,
                      const char *host,
                      const char *host4, const char *host6,
                      const char *want4, const char *want6) {
  char out4[FILENAME_MAX];
  char out6[FILENAME_MAX];

  /* Filled first so a result that writes nothing is not read as "". */
  snprintf(out4, sizeof(out4), "unset");
  snprintf(out6, sizeof(out6), "unset");
  winbolonetAnnounceHosts(overridden, host, host4, host6,
                          out4, sizeof(out4), out6, sizeof(out6));
  if (strcmp(out4, want4) != 0 || strcmp(out6, want6) != 0) {
    fprintf(stderr,
            "announceHostsRule: %s: got '%s' / '%s', expected '%s' / '%s'\n",
            what, out4, out6, want4, want6);
    return FALSE;
  }
  return okSoFar;
}

bool wbnTestAnnounceHostsRule(void) {
  bool ok = TRUE;

  /* The built-in host with neither key set: the two per-family names. */
  ok = hostsCase(ok,"default host", FALSE, WBN_DEFAULT_HOST, "", "",
                  WBN_DEFAULT_HOST4, WBN_DEFAULT_HOST6);
  ok = hostsCase(ok,"no Host value", FALSE, NULL, NULL, NULL,
                  WBN_DEFAULT_HOST4, WBN_DEFAULT_HOST6);
  ok = hostsCase(ok,"default host written with scheme and slash", FALSE,
                  "https://wbn.winbolo.net/", "", "",
                  WBN_DEFAULT_HOST4, WBN_DEFAULT_HOST6);

  /* Host4 and Host6 set: used, whatever the host. */
  ok = hostsCase(ok,"both keys, default host", FALSE, WBN_DEFAULT_HOST,
                  "a4.example.test", "http://a6.example.test",
                  "a4.example.test", "http://a6.example.test");
  ok = hostsCase(ok,"both keys, custom host", FALSE, "wbn.example.test",
                  "a4.example.test", "a6.example.test",
                  "a4.example.test", "a6.example.test");

  /* A custom host with neither key set: no announce at all. */
  ok = hostsCase(ok,"custom host", FALSE, "http://127.0.0.1:8080", "", "",
                  "", "");

  /* A custom host with one key set: that one only. */
  ok = hostsCase(ok,"custom host, Host4 only", FALSE, "wbn.example.test",
                  "a4.example.test", "", "a4.example.test", "");
  ok = hostsCase(ok,"custom host, Host6 only", FALSE, "wbn.example.test",
                  "", "a6.example.test", "", "a6.example.test");

  /* The default host with one key set: the other keeps its default. */
  ok = hostsCase(ok,"default host, Host4 only", FALSE, WBN_DEFAULT_HOST,
                  "a4.example.test", "", "a4.example.test",
                  WBN_DEFAULT_HOST6);

  /* A command-line override counts as custom, even one naming the default. */
  ok = hostsCase(ok,"override naming the default", TRUE, WBN_DEFAULT_HOST,
                  "", "", "", "");
  ok = hostsCase(ok,"override, Host6 only", TRUE, "wbn.example.test",
                  "", "a6.example.test", "", "a6.example.test");

  return ok;
}

/*********************************************************
*NAME:          urlCase
*PURPOSE:
* Compares httpApiUrlForHost's answer for one host with the
* URL expected. Returns okSoFar when they match and FALSE
* when they do not.
*********************************************************/
static bool urlCase(bool okSoFar, const char *host, const char *want) {
  char url[FILENAME_MAX + 64];

  httpApiUrlForHost(host, "server/announce", url, sizeof(url));
  if (strcmp(url, want) != 0) {
    fprintf(stderr, "apiUrlForHost: host '%s' gave '%s', expected '%s'\n",
            host != NULL ? host : "(null)", url, want);
    return FALSE;
  }
  return okSoFar;
}

bool wbnTestApiUrlForHost(void) {
  WbnHostSettings settings;
  bool ok = TRUE;

  httpSetHostOverride("http://127.0.0.1:9");
  if (!httpCreate()) {
    fprintf(stderr, "apiUrlForHost: httpCreate failed\n");
    return FALSE;
  }

  ok = urlCase(ok,"wbn4.winbolo.net",
                "https://wbn4.winbolo.net/api/v1/server/announce");
  ok = urlCase(ok,"https://wbn6.winbolo.net/",
                "https://wbn6.winbolo.net/api/v1/server/announce");
  ok = urlCase(ok,"http://wbn6.example.test",
                "http://wbn6.example.test/api/v1/server/announce");
  /* No host: the configured one, as every other request uses. */
  ok = urlCase(ok,NULL, "http://127.0.0.1:9/api/v1/server/announce");
  ok = urlCase(ok,"", "http://127.0.0.1:9/api/v1/server/announce");

  /* What httpCreate read: the override, and no Host4 or Host6, since this
   * binary loads no preferences file. */
  httpGetHostSettings(&settings);
  if (settings.hostOverridden != TRUE ||
      strcmp(settings.host, "http://127.0.0.1:9") != 0 ||
      settings.host4[0] != '\0' || settings.host6[0] != '\0') {
    fprintf(stderr,
            "apiUrlForHost: host settings were overridden=%d host='%s' "
            "host4='%s' host6='%s'\n",
            (int)settings.hostOverridden, settings.host, settings.host4,
            settings.host6);
    ok = FALSE;
  }

  httpDestroy();
  return ok;
}

bool wbnTestHostedJobHost(void) {
  WbnTestListener configured;
  WbnTestListener hosted;
  unsigned short configuredPort = 0;
  unsigned short hostedPort = 0;
  char configuredUrl[64];
  char hostedUrl[64];
  int configuredRequests;
  int hostedRequests;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&configured, &configuredPort, /*holdMs*/ 0)) {
    return FALSE;
  }
  if (!wbnTestListenerStart(&hosted, &hostedPort, /*holdMs*/ 0)) {
    wbnTestListenerStop(&configured);
    return FALSE;
  }
  snprintf(configuredUrl, sizeof(configuredUrl), "http://127.0.0.1:%u",
           (unsigned int)configuredPort);
  snprintf(hostedUrl, sizeof(hostedUrl), "http://127.0.0.1:%u",
           (unsigned int)hostedPort);

  /* A custom host by override, and no preferences file, so no Host4 or
   * Host6: the announce below must send nothing. */
  httpSetHostOverride(configuredUrl);
  if (!httpCreate()) {
    fprintf(stderr, "hostedJobHost: httpCreate failed\n");
    wbnTestListenerStop(&hosted);
    wbnTestListenerStop(&configured);
    return FALSE;
  }
  httpSetServerBearerToken(ANNOUNCE_FAKE_BEARER);
  winboloNetRunning = TRUE;
  winbolonetThreadSetServerKey(ANNOUNCE_KEY);

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "hostedJobHost: winbolonetThreadCreate failed\n");
    winboloNetRunning = FALSE;
    winbolonetThreadSetServerKey(NULL);
    httpClearServerBearerToken();
    httpDestroy();
    wbnTestListenerStop(&hosted);
    wbnTestListenerStop(&configured);
    return FALSE;
  }

  if (winbolonetThreadAddHostedServerRequest("", "server/announce", "{}")) {
    fprintf(stderr, "hostedJobHost: a hosted request with no host was "
                    "queued\n");
    ok = FALSE;
  }
  if (!winbolonetThreadAddHostedServerRequest(hostedUrl, "server/announce",
                                              "{}")) {
    fprintf(stderr, "hostedJobHost: the hosted request was refused\n");
    ok = FALSE;
  }
  /* An existing path: a keyed post with no host of its own. */
  winboloNetSendLock(TRUE);

  wbnTestWaitForRequests(&hosted, 1, ANNOUNCE_WAIT_MS);
  wbnTestWaitForRequests(&configured, 1, ANNOUNCE_WAIT_MS);

  /* Custom host, no Host4 or Host6: nothing is queued. The drain returns
   * once anything that was queued has been sent. */
  winbolonetServerAnnounce();
  winbolonetThreadDrain();

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  winboloNetRunning = FALSE;
  winbolonetThreadSetServerKey(NULL);
  wbnTestListenerStop(&hosted);
  wbnTestListenerStop(&configured);

  hostedRequests = SDL_GetAtomicInt(&hosted.requests);
  configuredRequests = SDL_GetAtomicInt(&configured.requests);
  if (hostedRequests != 1 || configuredRequests != 1) {
    fprintf(stderr,
            "hostedJobHost: the hosted listener saw %d requests and the "
            "configured one %d, expected 1 each - a custom host with no "
            "Host4 or Host6 must send no announce\n",
            hostedRequests, configuredRequests);
    return FALSE;
  }
  if (strcmp(hosted.path[0], ANNOUNCE_PATH) != 0 || !hosted.bearer[0] ||
      strcmp(hosted.body[0], "{}") != 0) {
    fprintf(stderr,
            "hostedJobHost: the hosted listener got %s (bearer %d, body %s), "
            "expected %s with the bearer and {}\n",
            hosted.path[0], (int)hosted.bearer[0], hosted.body[0],
            ANNOUNCE_PATH);
    ok = FALSE;
  }
  if (strcmp(configured.path[0], "/api/v1/server/lock") != 0 ||
      !configured.bearer[0]) {
    fprintf(stderr,
            "hostedJobHost: the configured listener got %s (bearer %d), "
            "expected /api/v1/server/lock with the bearer - a job queued "
            "through an existing call went elsewhere\n",
            configured.path[0], (int)configured.bearer[0]);
    ok = FALSE;
  }
  return ok;
}

bool wbnTestAnnouncePosts(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  char prefsPath[1024];
  char mapName[] = "wbn_test_map";
  int requests;
  int i;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, /*holdMs*/ 0)) {
    return FALSE;
  }
  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);

  /* Host4 and Host6 both name the listener: one TCP listener stands in for
   * both per-family names, so the case shows that each one is posted to and
   * what each post carries, not which family carried it. */
  SDL_CreateDirectory(WB_TEST_SCRATCH_DIR);
  snprintf(prefsPath, sizeof(prefsPath), "%s/wbn_announce_%lu.json",
           WB_TEST_SCRATCH_DIR, ANNOUNCE_PID());
  remove(prefsPath);
  prefsInit(prefsPath);
  prefsSetString("WINBOLO.NET", "Host4", baseUrl);
  prefsSetString("WINBOLO.NET", "Host6", baseUrl);

  httpSetHostOverride(baseUrl);
  if (!winbolonetCreateServer(mapName, /*port*/ 5187, /*gameType*/ 0,
                              /*ai*/ 0, /*mines*/ FALSE, /*password*/ FALSE,
                              /*numBases*/ 4, /*numPills*/ 8,
                              /*freeBases*/ 4, /*freePills*/ 8,
                              /*numPlayers*/ 0)) {
    fprintf(stderr, "announcePosts: winbolonetCreateServer failed\n");
    ok = FALSE;
  } else {
    winbolonetServerAnnounce();
  }
  requests = wbnTestWaitForRequests(&ln, 3, ANNOUNCE_WAIT_MS);

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  winboloNetRunning = FALSE;
  winboloNetServerKey[0] = '\0';
  winbolonetEventsDestroy();
  wbnTestListenerStop(&ln);
  prefsShutdown();
  remove(prefsPath);

  if (!ok) {
    return FALSE;
  }
  if (requests != 3) {
    fprintf(stderr,
            "announcePosts: listener saw %d requests, expected 3 (register "
            "and two announces)\n", requests);
    return FALSE;
  }
  if (strcmp(ln.path[0], "/api/v1/server/register") != 0) {
    fprintf(stderr, "announcePosts: request 0 was %s, expected the "
                    "register\n", ln.path[0]);
    ok = FALSE;
  }
  for (i = 1; i < 3; i++) {
    if (strcmp(ln.path[i], ANNOUNCE_PATH) != 0) {
      fprintf(stderr, "announcePosts: request %d was %s, expected %s\n",
              i, ln.path[i], ANNOUNCE_PATH);
      ok = FALSE;
    }
    if (!ln.bearer[i]) {
      fprintf(stderr,
              "announcePosts: request %d carried no Authorization: Bearer\n",
              i);
      ok = FALSE;
    }
    if (strcmp(ln.body[i], "{}") != 0 || ln.bodyLen[i] != 2) {
      fprintf(stderr,
              "announcePosts: request %d body was '%s' (%d bytes), expected "
              "{}\n", i, ln.body[i], ln.bodyLen[i]);
      ok = FALSE;
    }
  }
  return ok;
}
