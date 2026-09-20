/*
 * The WinBolo.net worker outlives a session boundary.
 *
 * winbolonetEndSession used to call winbolonetThreadDestroy and
 * winbolonetBeginSession used to call winbolonetThreadCreate, so every round
 * boundary stopped the worker and started a new one. Two costs came with
 * that. The destroy spins until both queues are empty and only then stops the
 * thread, so a boundary that followed a few departures waited out that whole
 * queue on the tick thread before its own server/quit even began. And the
 * pooled libcurl handle belongs to the worker — httpWorkerPoolBegin makes it
 * when the thread starts, httpWorkerPoolEnd destroys it when the thread exits
 * — so the connection died with the thread and the next queued post paid a
 * fresh TCP connect, and against the real host a fresh TLS handshake.
 *
 * What this case pins: the queued posts either side of a session boundary
 * arrive on the same TCP connection. That is the observable that says the
 * worker is the same thread: the handle, and so the connection, cannot
 * survive a thread that does not. Before the change the two posts arrived on
 * two connections.
 *
 * It also pins the order the five requests reach the host in, since the
 * boundary's own server/quit and server/register are posted synchronously by
 * this thread while the worker holds its connection open. The listener
 * therefore has to serve more than one connection at a time; see the harness.
 *
 * What it deliberately does not pin: how many connections the listener
 * accepted in total. The synchronous calls take their own per-call handle by
 * design — only the worker's handle is pooled — so that count is three or
 * four and says nothing about the worker.
 *
 * winboloNetRunning and winboloNetServerKey are module-global state defined
 * in winbolonet_core.c; winbolonet_server.c reaches them by extern
 * declaration and so does this case, which is how the teardown below puts
 * the module back without a synchronous goodbye post of its own.
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

/* register, the first queued post, quit, register, the second queued post. */
#define OUTLIVES_REQUESTS 5

/* The two queued posts, by their place in the arrival order above. */
#define OUTLIVES_FIRST_POST  1
#define OUTLIVES_SECOND_POST 4

/* No answer is held, so this only has to cover a worker sweep. */
#define OUTLIVES_DRAIN_MS 15000

static const char *kExpectedPath[OUTLIVES_REQUESTS] = {
  "/api/v1/server/register",
  "/api/v1/server/lock",
  "/api/v1/server/quit",
  "/api/v1/server/register",
  "/api/v1/server/lock",
};

/* The register arguments. Neither session reads them back, so one set does
 * for both: what the case is about is which thread posts afterwards. */
static bool outlivesRegister(bool begin) {
  char mapName[] = "wbn_test_map";

  if (begin) {
    return winbolonetBeginSession(mapName, /*port*/ 5187, /*gameType*/ 0,
                                  /*ai*/ 0, /*mines*/ FALSE,
                                  /*password*/ FALSE, /*numBases*/ 4,
                                  /*numPills*/ 8, /*freeBases*/ 4,
                                  /*freePills*/ 8, /*numPlayers*/ 0);
  }
  return winbolonetCreateServer(mapName, /*port*/ 5187, /*gameType*/ 0,
                                /*ai*/ 0, /*mines*/ FALSE,
                                /*password*/ FALSE, /*numBases*/ 4,
                                /*numPills*/ 8, /*freeBases*/ 4,
                                /*freePills*/ 8, /*numPlayers*/ 0);
}

bool wbnTestWorkerOutlivesSession(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  int requests;
  int firstConn;
  int secondConn;
  int i;
  bool ok = TRUE;

  /* No hold: this case measures which connection carried a post, not
   * waiting. */
  if (!wbnTestListenerStart(&ln, &port, /*holdMs*/ 0)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  /* The real startup path: httpCreate, server/register, and the one
   * winbolonetThreadCreate the server makes. */
  if (!outlivesRegister(/*begin*/ FALSE)) {
    fprintf(stderr, "workerOutlivesSession: winbolonetCreateServer failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  if (strcmp(winboloNetServerKey, WBN_TEST_REGISTER_KEY) != 0) {
    fprintf(stderr,
            "workerOutlivesSession: server key after register was '%s', "
            "expected '%s'\n",
            winboloNetServerKey, WBN_TEST_REGISTER_KEY);
    ok = FALSE;
  }

  /* One queued post on this session, drained before the boundary so its
   * place in the arrival order is fixed. */
  winboloNetSendLock(TRUE);
  if (wbnTestWaitForRequests(&ln, 2, OUTLIVES_DRAIN_MS) < 2) {
    fprintf(stderr,
            "workerOutlivesSession: the first queued post never arrived - "
            "listener saw %d requests, expected 2\n",
            SDL_GetAtomicInt(&ln.requests));
    ok = FALSE;
  }

  /* The session boundary, as the lifecycle runs it: quit the old session,
   * then register the next one. The round-log upload the lifecycle puts
   * between the two is not this case's subject. */
  winbolonetEndSession();
  if (!outlivesRegister(/*begin*/ TRUE)) {
    fprintf(stderr, "workerOutlivesSession: winbolonetBeginSession failed\n");
    ok = FALSE;
  }

  /* And one queued post on the new session. */
  winboloNetSendLock(FALSE);
  requests = wbnTestWaitForRequests(&ln, OUTLIVES_REQUESTS, OUTLIVES_DRAIN_MS);

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  winboloNetRunning = FALSE;
  winboloNetServerKey[0] = '\0';
  winbolonetEventsDestroy();
  wbnTestListenerStop(&ln);

  if (requests != OUTLIVES_REQUESTS) {
    fprintf(stderr,
            "workerOutlivesSession: listener saw %d requests, expected %d\n",
            requests, OUTLIVES_REQUESTS);
    ok = FALSE;
  } else {
    for (i = 0; i < OUTLIVES_REQUESTS; i++) {
      if (strcmp(ln.path[i], kExpectedPath[i]) != 0) {
        fprintf(stderr,
                "workerOutlivesSession: request %d was %s, expected %s\n",
                i, ln.path[i], kExpectedPath[i]);
        ok = FALSE;
      }
    }
  }

  firstConn = ln.conn[OUTLIVES_FIRST_POST];
  secondConn = ln.conn[OUTLIVES_SECOND_POST];
  if (firstConn < 0 || secondConn < 0 || firstConn != secondConn) {
    fprintf(stderr,
            "workerOutlivesSession: the queued posts arrived on connections "
            "%d and %d, expected one connection for both - the worker did "
            "not survive the session boundary (listener accepted %d "
            "connections in all)\n",
            firstConn, secondConn, SDL_GetAtomicInt(&ln.connections));
    ok = FALSE;
  }
  return ok;
}
