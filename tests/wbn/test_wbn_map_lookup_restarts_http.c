/*
 * A map lookup is sent when the HTTP layer has been stopped.
 *
 * The game finder draws a listed game's map by posting the md5 the host
 * advertises to /api/v1/map (wbnMapFetchByMd5). Nothing in the finder calls
 * httpCreate, and the one-shot login calls, leaving a game and closing the
 * WinBolo.net map browser all end in httpDestroy. wbn_api_post used to
 * return -1 at once in that state, with no request sent, and the finder
 * showed "preview unavailable" for the map for the rest of the session.
 * wbn_api_get has always started the layer on demand; the post now does the
 * same.
 *
 * What this case pins: after httpCreate and httpDestroy, one
 * wbnMapFetchByMd5 arrives at the listener as one POST to api/v1/map
 * carrying the md5, and the lookup reports that it was answered. Before the
 * change the listener saw no request.
 *
 * The listener answers every path with {}, which the lookup reads as "not
 * stored", so the call itself returns false; what was sent is the subject.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "wbn_map.h"

#define LOOKUP_MD5 "0123456789abcdef0123456789abcdef"

bool wbnTestMapLookupRestartsHttp(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  WbnMapResult r;
  int requests;
  bool found;
  bool answered;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, /*holdMs*/ 0)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  /* What a login check leaves behind: started, then stopped. */
  if (!httpCreate()) {
    fprintf(stderr, "mapLookupRestartsHttp: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  httpDestroy();

  found = wbnMapFetchByMd5(LOOKUP_MD5, &r);
  answered = r.answered;
  wbnMapResultFree(&r);

  httpDestroy();
  wbnTestListenerStop(&ln);

  requests = SDL_GetAtomicInt(&ln.requests);
  if (requests != 1) {
    fprintf(stderr,
            "mapLookupRestartsHttp: listener saw %d requests, expected 1\n",
            requests);
    return FALSE;
  }
  if (strstr(ln.path[0], "api/v1/map") == NULL) {
    fprintf(stderr, "mapLookupRestartsHttp: request went to '%s'\n",
            ln.path[0]);
    ok = FALSE;
  }
  if (strstr(ln.body[0], LOOKUP_MD5) == NULL) {
    fprintf(stderr, "mapLookupRestartsHttp: body '%s' has no md5\n",
            ln.body[0]);
    ok = FALSE;
  }
  if (found) {
    fprintf(stderr, "mapLookupRestartsHttp: {} was read as a stored map\n");
    ok = FALSE;
  }
  if (!answered) {
    fprintf(stderr,
            "mapLookupRestartsHttp: an answered lookup reported no answer\n");
    ok = FALSE;
  }
  return ok;
}
