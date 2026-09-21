/*
 * A player leaving does not make the caller wait on WinBolo.net.
 *
 * winboloNetClientLeaveGame runs on the server's tick thread, under the tick
 * lock, and used to make two HTTPS round trips there: a flush server/update
 * and then a client/leave. A few hundred milliseconds of that is enough to
 * lock out every player above roughly 100ms round trip, and a WinBolo.net
 * that is unreachable costs the connect timeout while one that accepts and
 * stalls costs the whole transfer timeout. Clients give up after 10s without
 * a server packet, so the game stopped for everyone because one player left.
 *
 * What this case pins:
 *   - winboloNetClientLeaveGame returns in under 50ms against a host that
 *     takes 3s to answer. It is the assertion the change exists for: with
 *     the posts on the caller's thread the call could not return before the
 *     first answer arrived.
 *   - The four posts still go out, in the order they were made:
 *     server/update (the flush inside the leave), client/leave, server/lock,
 *     server/teams. The queue is drained oldest first by a single worker, so
 *     the order is the enqueue order with no ordering machinery.
 *   - All four arrive on one accepted TCP connection: the worker's pooled
 *     handle still holds under a queue that is being used.
 *
 * The listener holds every answer for 3s, which is what makes the timing
 * assertion mean something and also what makes the four posts take about
 * twelve seconds to drain. It speaks plain HTTP, so no certificate is needed.
 *
 * winboloNetRunning, winboloNetServerKey and winboloNetPlayerKey are
 * module-global state defined in winbolonet_core.c; winbolonet_server.c
 * reaches them by extern declaration and so does this case, which is why no
 * test-only setter was added for the player key.
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
extern char winboloNetPlayerKey[MAX_TANKS][WINBOLONET_KEY_LEN];

/* A key is WINBOLONET_KEY_LEN - 1 characters. */
#define LEAVE_FAKE_KEY "0123456789abcdef0123456789abcdef"
#define LEAVE_FAKE_BEARER \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* The slot that leaves. */
#define LEAVE_SLOT 1

/* How long the listener sits on each answer. Sixty times the bound below,
 * so a synchronous call could not slip under it. */
#define LEAVE_HOLD_MS 3000

/* The bound. A queued call does no I/O at all, so this is orders of
 * magnitude over what it costs and still far under one held answer. */
#define LEAVE_RETURN_MAX_MS 50

/* Posts expected, and the room they are given. Four answers held 3s each,
 * drained one at a time, is about twelve seconds. */
#define LEAVE_POSTS    4
#define LEAVE_DRAIN_MS 40000

static const char *kExpectedPath[LEAVE_POSTS] = {
  "/api/v1/server/update",
  "/api/v1/client/leave",
  "/api/v1/server/lock",
  "/api/v1/server/teams",
};

bool wbnTestLeaveReturnsAtOnce(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  BYTE teams[3];
  Uint64 started;
  Uint64 elapsed;
  int connections;
  int requests;
  int i;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, LEAVE_HOLD_MS)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "leaveReturnsAtOnce: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  httpSetServerBearerToken(LEAVE_FAKE_BEARER);

  /* A live session with one keyed player. winbolonetAddEvent needs the event
   * queue, and the flush inside the leave only sends because that event is
   * sitting in it. */
  winbolonetEventsCreate();
  winboloNetRunning = TRUE;
  snprintf(winboloNetServerKey, sizeof(winboloNetServerKey), "%s",
           LEAVE_FAKE_KEY);
  snprintf(winboloNetPlayerKey[LEAVE_SLOT],
           sizeof(winboloNetPlayerKey[LEAVE_SLOT]), "%s", LEAVE_FAKE_KEY);

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "leaveReturnsAtOnce: winbolonetThreadCreate failed\n");
    winboloNetRunning = FALSE;
    winbolonetEventsDestroy();
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* The call under test. It flushes the buffered events (server/update) and
   * then posts the leave, both of which used to go out on this thread. */
  started = SDL_GetTicks();
  winboloNetClientLeaveGame(LEAVE_SLOT, /*numPlayers*/ 2, /*freeBases*/ 3,
                            /*freePills*/ 4);
  elapsed = SDL_GetTicks() - started;

  if (elapsed >= LEAVE_RETURN_MAX_MS) {
    fprintf(stderr,
            "leaveReturnsAtOnce: winboloNetClientLeaveGame took %llums "
            "against a host answering in %dms - the posts are still on the "
            "caller's thread\n",
            (unsigned long long)elapsed, LEAVE_HOLD_MS);
    ok = FALSE;
  }

  /* Two more queued posts behind it, to pin the order the worker fires in. */
  winboloNetSendLock(TRUE);

  /* array[1..length] holds slot numbers, WINBOLO_NET_TEAM_MARKER separating
   * teams; one team holding the slot that just left is enough to build a
   * body. */
  memset(teams, 0, sizeof(teams));
  teams[1] = LEAVE_SLOT;
  winbolonetServerSendTeams(teams, /*length*/ 1, /*numTeams*/ 1);

  wbnTestWaitForRequests(&ln, LEAVE_POSTS, LEAVE_DRAIN_MS);

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  winboloNetRunning = FALSE;
  winboloNetServerKey[0] = '\0';
  winboloNetPlayerKey[LEAVE_SLOT][0] = '\0';
  winbolonetEventsDestroy();
  wbnTestListenerStop(&ln);

  requests = SDL_GetAtomicInt(&ln.requests);
  connections = SDL_GetAtomicInt(&ln.connections);

  if (requests != LEAVE_POSTS) {
    fprintf(stderr,
            "leaveReturnsAtOnce: listener saw %d requests, expected %d\n",
            requests, LEAVE_POSTS);
    ok = FALSE;
  } else {
    for (i = 0; i < LEAVE_POSTS; i++) {
      if (strcmp(ln.path[i], kExpectedPath[i]) != 0) {
        fprintf(stderr,
                "leaveReturnsAtOnce: request %d was %s, expected %s\n",
                i, ln.path[i], kExpectedPath[i]);
        ok = FALSE;
      }
    }
  }
  if (connections != 1) {
    fprintf(stderr,
            "leaveReturnsAtOnce: listener accepted %d connections, "
            "expected 1\n", connections);
    ok = FALSE;
  }
  return ok;
}
