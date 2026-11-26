/*
 * The HTTP layer is stopped while a map lookup is in flight.
 *
 * The game finder's map lookups run on worker threads nothing ever joins.
 * The GUI thread stops the HTTP layer (httpDestroy) when a login check
 * ends, when the player leaves a game and when the WinBolo.net map browser
 * closes, and any of those can happen while a lookup is still waiting for
 * its answer: the player picks a game in the finder and joins it before the
 * preview has arrived. httpDestroy used to call curl_global_cleanup at
 * once, which libcurl does not allow while another thread is inside a
 * transfer.
 *
 * What this case pins: with a lookup held open by the listener, httpDestroy
 * leaves libcurl's global state up; so does a whole start and stop made on
 * top of it, which is what a login check during the lookup is; the lookup
 * then completes with its answer; and once it has, libcurl's global state
 * is down, released by the lookup on its way out. A second lookup after
 * all of that is sent as well.
 *
 * The listener answers every path with {}, which the lookup reads as "not
 * stored", so wbnMapFetchByMd5 returns false with answered set.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "wbn_map.h"

#define STOP_LOOKUP_MD5 "0123456789abcdef0123456789abcdef"

/* Long enough that the stops below land inside the lookup on a loaded
 * machine; the case checks that they did. */
#define STOP_HOLD_MS 1500

typedef struct {
  bool          found;
  bool          answered;
  SDL_AtomicInt done;
} StopLookup;

static int stopLookupThread(void *data) {
  StopLookup *lk = (StopLookup *)data;
  WbnMapResult r;
  lk->found = wbnMapFetchByMd5(STOP_LOOKUP_MD5, &r);
  lk->answered = r.answered;
  wbnMapResultFree(&r);
  SDL_SetAtomicInt(&lk->done, 1);
  return 0;
}

bool wbnTestStopDuringLookup(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  StopLookup lk;
  SDL_Thread *thread;
  WbnMapResult r;
  int waited = 0;
  int requests;
  bool upAfterStop;
  bool upAfterLoginCheck;
  bool stoppedInside;
  bool upAtEnd;
  bool secondAnswered;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, STOP_HOLD_MS)) {
    return FALSE;
  }
  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  /* The lookup starts the layer itself, as it does in the finder. */
  memset(&lk, 0, sizeof(lk));
  SDL_SetAtomicInt(&lk.done, 0);
  thread = SDL_CreateThread(stopLookupThread, "StopLookup", &lk);
  if (thread == NULL) {
    fprintf(stderr, "stopDuringLookup: SDL_CreateThread failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* Wait for the lookup's connection: from here it is inside libcurl, held
   * by the listener. */
  while (waited < 5000 && SDL_GetAtomicInt(&ln.connections) < 1) {
    SDL_Delay(WBN_TEST_POLL_MS);
    waited += WBN_TEST_POLL_MS;
  }

  /* Leaving a game, or a login check ending. */
  httpDestroy();
  upAfterStop = httpCurlIsUp();

  /* A whole login check on top of it: started, then stopped. */
  (void)httpCreate();
  httpDestroy();
  upAfterLoginCheck = httpCurlIsUp();
  stoppedInside = (SDL_GetAtomicInt(&lk.done) == 0);

  SDL_WaitThread(thread, NULL);
  upAtEnd = httpCurlIsUp();

  /* And the layer still starts for the next lookup. */
  ln.holdMs = 0;
  (void)wbnMapFetchByMd5(STOP_LOOKUP_MD5, &r);
  secondAnswered = r.answered;
  wbnMapResultFree(&r);
  httpDestroy();

  wbnTestListenerStop(&ln);
  requests = SDL_GetAtomicInt(&ln.requests);

  if (SDL_GetAtomicInt(&ln.connections) < 1) {
    fprintf(stderr, "stopDuringLookup: the lookup never connected\n");
    return FALSE;
  }
  if (!stoppedInside) {
    fprintf(stderr,
            "stopDuringLookup: the lookup finished before the stops, so "
            "nothing was tested\n");
    ok = FALSE;
  }
  if (!upAfterStop) {
    fprintf(stderr,
            "stopDuringLookup: httpDestroy tore libcurl down under a lookup "
            "in flight\n");
    ok = FALSE;
  }
  if (!upAfterLoginCheck) {
    fprintf(stderr,
            "stopDuringLookup: a start and stop during the lookup tore "
            "libcurl down under it\n");
    ok = FALSE;
  }
  if (lk.found || !lk.answered) {
    fprintf(stderr,
            "stopDuringLookup: the lookup came back found=%d answered=%d, "
            "expected 0 and 1\n", (int)lk.found, (int)lk.answered);
    ok = FALSE;
  }
  if (upAtEnd) {
    fprintf(stderr,
            "stopDuringLookup: libcurl was still up after the last lookup "
            "finished with the layer stopped\n");
    ok = FALSE;
  }
  if (!secondAnswered) {
    fprintf(stderr, "stopDuringLookup: the second lookup got no answer\n");
    ok = FALSE;
  }
  if (requests != 2) {
    fprintf(stderr,
            "stopDuringLookup: listener saw %d requests, expected 2\n",
            requests);
    ok = FALSE;
  }
  if (httpCurlIsUp()) {
    fprintf(stderr, "stopDuringLookup: libcurl left up at the end\n");
    ok = FALSE;
  }
  return ok;
}
