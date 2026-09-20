/*
 * WinBolo.net posts share one connection.
 *
 * Every post the WinBolo.net worker thread fires used to be
 * curl_easy_init -> setopt -> curl_easy_perform -> curl_easy_cleanup, with no
 * curl_share and no CURLOPT_FORBID_REUSE, so nothing was pooled: a queued
 * post paid a fresh TCP connect and, against the real host, a full TLS
 * handshake. httpWorkerPoolBegin gives the worker one handle it keeps for its
 * life and resets per request, which libcurl documents as leaving live
 * connections, the TLS session cache and the DNS cache alone.
 *
 * What this case pins: four posts queued through
 * winbolonetThreadAddServerRequest arrive as four requests on exactly one
 * accepted TCP connection. Before the pooled handle the listener accepted
 * four.
 *
 * Why its own target rather than a case in WinBoloUnitTests: that binary
 * links tests/unit/test_stubs.c, which stubs WinBolo.net out and links no
 * libcurl, so nothing in it can show whether an HTTP call opened a connection
 * or reused one. This target links the real http.c and winbolonetthread.c and
 * puts a local TCP listener where the WBN host would be.
 *
 * What it deliberately does not pin: the order the four posts arrive in, or
 * how many worker sweeps they are spread over. The worker swaps the whole
 * waiting queue under its mutex, so a post enqueued a moment late rides the
 * next sweep; the kept handle holds the connection open either way.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "wbn_bearer.h"
#include "winbolonetthread.h"

#define REUSE_POSTS    4
#define REUSE_DRAIN_MS 15000

bool wbnTestPostsShareConnection(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  int connections;
  int requests;
  int i;
  bool ok = TRUE;

  /* No hold: this case measures connections, not waiting. */
  if (!wbnTestListenerStart(&ln, &port, /*holdMs*/ 0)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "postsShareConnection: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* wbn_api_post_server refuses to send without a bearer. The listener
   * never reads it. */
  httpSetServerBearerToken(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "postsShareConnection: winbolonetThreadCreate failed\n");
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  for (i = 0; i < REUSE_POSTS; i++) {
    winbolonetThreadAddServerRequest("server/update", "{\"test\":1}");
  }

  wbnTestWaitForRequests(&ln, REUSE_POSTS, REUSE_DRAIN_MS);

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  wbnTestListenerStop(&ln);

  requests = SDL_GetAtomicInt(&ln.requests);
  connections = SDL_GetAtomicInt(&ln.connections);

  if (requests != REUSE_POSTS) {
    fprintf(stderr,
            "postsShareConnection: listener saw %d requests, expected %d\n",
            requests, REUSE_POSTS);
    ok = FALSE;
  }
  if (connections != 1) {
    fprintf(stderr,
            "postsShareConnection: listener accepted %d connections, "
            "expected 1\n", connections);
    ok = FALSE;
  }
  return ok;
}
