/*
 * A forced WinBolo.net update does not post on the calling thread when the
 * worker queue is full (test_wbn_forced_update_full_queue.c).
 *
 * winbolonetServerUpdate with sendNow TRUE queues the flush and posts it
 * itself when the queue refuses it. That fallback is there for
 * winbolonetGoodbye, which flushes after the worker has been destroyed and
 * has no later moment to send. The backlog cap gave the same FALSE a second
 * meaning: with WBN_WAITING_NONE_MAX fire-and-forget posts already waiting,
 * a keyed enqueue is refused although the worker is running, so the flush
 * posted on the caller's thread. Both round transitions in
 * server_lifecycle.c make that call from the game tick, so a full queue
 * stopped the game for as long as the post took - up to 35s against a slow
 * WinBolo.net.
 *
 * The flush now queues through winbolonetThreadAddSessionRequest, which the
 * cap does not apply to, so a FALSE means only that the worker is not
 * running.
 *
 * What this case pins, against a listener that holds every answer long
 * enough that nothing drains:
 *   - with the queue full, winbolonetServerUpdate(..., TRUE) returns in
 *     FORCED_MAX_MS, far inside the answer the listener is holding;
 *   - the flush was queued rather than posted or dropped, and a server/quit
 *     queued after it sits ahead of it in the waiting list, which is newest
 *     first - so the update still goes out before the quit.
 *
 * The queue is read through the worker's own list head, and the two newest
 * entries are what the ordering rests on; the fillers behind them are not
 * read. The case waits for the listener to accept the worker's connection
 * before it reads anything, as test_wbn_key_at_fire_time.c does: the worker
 * is then inside a post the listener holds for FORCED_HOLD_MS, which is far
 * longer than the few milliseconds of work below, so nothing is moving the
 * list while it is read.
 *
 * The listener is stopped before the worker is, so the posts still queued
 * fail their connect at once rather than being held one at a time.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "wbn_bearer.h"
#include "winbolonet_core.h"
#include "winbolonet_server.h"
#include "winbolonetthread.h"

/* Module-global WBN state, as winbolonet_server.c declares it. */
extern bool winboloNetRunning;

/* The worker's waiting list, newest first, as winbolonetthread.c defines
 * it. Read to tell a queued flush from one posted on this thread, and to
 * put the flush and the quit behind it in order. */
extern wbnList wbnWaiting;

#define FORCED_KEY "cccccccccccccccccccccccccccccccc"
#define FORCED_FAKE_BEARER \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* Longer than everything this case does after the queue is filled, so no
 * post finishes while it runs and the queue stays full. */
#define FORCED_HOLD_MS 4000

/* What the forced update is allowed to take. A post that goes out on this
 * thread waits for the held answer, which is more than an order of
 * magnitude longer than this. */
#define FORCED_MAX_MS 250

/* How long to wait for the worker's first post to reach the listener. */
#define FORCED_CONNECT_MS 5000

/*********************************************************
*NAME:          forcedQueuedEndpoint
*PURPOSE:
* The endpoint of the nth entry of the waiting list,
* counting from the newest, or NULL when the list is
* shorter than that.
*********************************************************/
static const char *forcedQueuedEndpoint(int n) {
  wbnList q = wbnWaiting;
  int i;

  for (i = 0; i < n && q != NULL; i++) {
    q = q->next;
  }
  return (q == NULL) ? NULL : q->endpoint;
}

bool wbnTestForcedUpdateFullQueue(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  char newest[128];
  char behind[128];
  Uint64 start;
  Uint64 elapsed;
  Uint64 deadline;
  const char *endpoint;
  int taken = 0;
  int i;
  bool connected;
  bool stillFull;
  bool quitTaken;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, FORCED_HOLD_MS)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "forcedUpdateFullQueue: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  httpSetServerBearerToken(FORCED_FAKE_BEARER);
  winboloNetRunning = TRUE;
  winbolonetThreadSetServerKey(FORCED_KEY);

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "forcedUpdateFullQueue: winbolonetThreadCreate failed\n");
    winboloNetRunning = FALSE;
    winbolonetThreadSetServerKey(NULL);
    httpClearServerBearerToken();
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* Fill the queue. server/lobby rather than server/update, so a filler is
   * never mistaken for the flush below. */
  for (i = 0; i < WBN_WAITING_NONE_MAX; i++) {
    if (winbolonetThreadAddServerRequest("server/lobby", "{}") == TRUE) {
      taken++;
    }
  }

  /* The worker's first post is on the wire: the listener has accepted its
   * connection and holds the answer for FORCED_HOLD_MS. Everything below
   * happens inside that window. */
  deadline = SDL_GetTicks() + FORCED_CONNECT_MS;
  while (SDL_GetAtomicInt(&ln.connections) < 1 && SDL_GetTicks() < deadline) {
    SDL_Delay(WBN_TEST_POLL_MS);
  }
  connected = (SDL_GetAtomicInt(&ln.connections) >= 1);

  /* The queue is still full: a fire-and-forget post is refused. */
  stillFull = (winbolonetThreadAddServerRequest("server/lobby", "{}") == FALSE);

  start = SDL_GetTicks();
  winbolonetServerUpdate(/*numPlayers*/ 1, /*numFreeBases*/ 1,
                         /*numFreePills*/ 1, /*sendNow*/ TRUE);
  elapsed = SDL_GetTicks() - start;

  /* The round transition queues the rotation straight after the flush. */
  quitTaken = winbolonetThreadAddSessionRequest("server/quit", "{}");

  newest[0] = '\0';
  behind[0] = '\0';
  endpoint = forcedQueuedEndpoint(0);
  if (endpoint != NULL) {
    snprintf(newest, sizeof(newest), "%s", endpoint);
  }
  endpoint = forcedQueuedEndpoint(1);
  if (endpoint != NULL) {
    snprintf(behind, sizeof(behind), "%s", endpoint);
  }

  /* The listener first: what is still queued then fails its connect at once
   * rather than being held for FORCED_HOLD_MS each. */
  wbnTestListenerStop(&ln);
  winbolonetThreadDestroy();
  httpClearServerBearerToken();
  httpDestroy();
  winboloNetRunning = FALSE;
  winbolonetThreadSetServerKey(NULL);

  if (taken != WBN_WAITING_NONE_MAX) {
    fprintf(stderr,
            "forcedUpdateFullQueue: the queue took %d posts, expected %d\n",
            taken, WBN_WAITING_NONE_MAX);
    ok = FALSE;
  }
  if (connected != TRUE) {
    fprintf(stderr,
            "forcedUpdateFullQueue: the worker's first post never connected "
            "within %d ms\n", FORCED_CONNECT_MS);
    ok = FALSE;
  }
  if (stillFull != TRUE) {
    fprintf(stderr,
            "forcedUpdateFullQueue: the queue was not full when the forced "
            "update was made, so this case measured nothing\n");
    ok = FALSE;
  }
  if (elapsed > FORCED_MAX_MS) {
    fprintf(stderr,
            "forcedUpdateFullQueue: the forced update took %u ms on the "
            "calling thread, expected under %d ms - it posted here instead "
            "of queueing\n",
            (unsigned int)elapsed, FORCED_MAX_MS);
    ok = FALSE;
  } else {
    printf("forcedUpdateFullQueue: the forced update returned in %u ms\n",
           (unsigned int)elapsed);
  }
  if (quitTaken != TRUE) {
    fprintf(stderr,
            "forcedUpdateFullQueue: the session quit was refused with the "
            "queue full\n");
    ok = FALSE;
  }
  if (strcmp(newest, "server/quit") != 0 ||
      strcmp(behind, "server/update") != 0) {
    fprintf(stderr,
            "forcedUpdateFullQueue: the waiting list held '%s' then '%s', "
            "expected 'server/quit' then 'server/update' - the flush was "
            "not queued ahead of the quit\n",
            newest, behind);
    ok = FALSE;
  }
  return ok;
}
