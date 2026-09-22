/*
 * A queued server post carries the session key that is current when it
 * fires (test_wbn_key_at_fire_time.c).
 *
 * The round transition is three queued jobs: server/quit, the round-log
 * upload and server/register, and the key swaps on the tick when the
 * register's result is applied. That is one round trip after the quit was
 * queued, or up to 35s against a slow tracker. A leave, a lock, a teams
 * change or an update flush queued in that window used to serialise
 * winboloNetServerKey into its body as it was queued, so it fired against a
 * session the quit ahead of it had already ended and was refused.
 *
 * The builders now leave server_key out and queue through
 * winbolonetThreadAddServerKeyedRequest; the worker adds the key it reads
 * at fire time, through the same lock the tick writes it under
 * (winbolonetThreadSetServerKey).
 *
 * What this case pins, against a listener that holds every answer 3s:
 *   - a keyed post queued under key A and already on the wire carries A;
 *   - a keyed post queued after the key changed to B, while the first is
 *     still being held, carries B when it fires;
 *   - each body carries server_key exactly once.
 *
 * The first post is known to be on the wire when the listener has accepted
 * a connection: the worker builds the body before curl connects. Only then
 * does the key change, so the second post's key is B from before it was
 * queued, and the two cannot be told apart by luck of scheduling.
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

/* Two keys, each WINBOLONET_KEY_LEN - 1 characters, told apart by their
 * first character. */
#define KEY_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define KEY_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define KEY_FAKE_BEARER \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* How long the listener sits on each answer: long enough that the second
 * post is queued while the first is still held. */
#define KEY_HOLD_MS 3000

/* Room for the first connection to be accepted, and for both posts to be
 * answered one after the other. */
#define KEY_CONNECT_MS 5000
#define KEY_DRAIN_MS   20000

#define KEY_POSTS 2

/* How many times needle occurs in hay. */
static int keyCount(const char *hay, const char *needle) {
  int n = 0;
  const char *p = hay;
  size_t len = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) {
    n++;
    p += len;
  }
  return n;
}

bool wbnTestKeyStampedAtFireTime(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  Uint64 deadline;
  int requests;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, KEY_HOLD_MS)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "keyStampedAtFireTime: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  httpSetServerBearerToken(KEY_FAKE_BEARER);
  winboloNetRunning = TRUE;
  winbolonetThreadSetServerKey(KEY_A);

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "keyStampedAtFireTime: winbolonetThreadCreate failed\n");
    winboloNetRunning = FALSE;
    winbolonetThreadSetServerKey(NULL);
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* Queued under A. server/lock is the simplest keyed post: one field. */
  winboloNetSendLock(TRUE);

  /* On the wire: the listener has accepted the worker's connection. */
  deadline = SDL_GetTicks() + KEY_CONNECT_MS;
  while (SDL_GetAtomicInt(&ln.connections) < 1 && SDL_GetTicks() < deadline) {
    SDL_Delay(WBN_TEST_POLL_MS);
  }
  if (SDL_GetAtomicInt(&ln.connections) < 1) {
    fprintf(stderr,
            "keyStampedAtFireTime: the first post never connected within "
            "%dms\n", KEY_CONNECT_MS);
    ok = FALSE;
  }

  /* The key swaps, as the register's result does on the tick, with the
   * first post still held by the listener. The second post is queued
   * behind it. */
  winbolonetThreadSetServerKey(KEY_B);
  winboloNetSendLock(FALSE);

  wbnTestWaitForRequests(&ln, KEY_POSTS, KEY_DRAIN_MS);

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  winboloNetRunning = FALSE;
  winbolonetThreadSetServerKey(NULL);
  wbnTestListenerStop(&ln);

  requests = SDL_GetAtomicInt(&ln.requests);
  if (requests != KEY_POSTS) {
    fprintf(stderr,
            "keyStampedAtFireTime: listener saw %d requests, expected %d\n",
            requests, KEY_POSTS);
    return FALSE;
  }

  if (strcmp(ln.path[0], "/api/v1/server/lock") != 0 ||
      strcmp(ln.path[1], "/api/v1/server/lock") != 0) {
    fprintf(stderr,
            "keyStampedAtFireTime: requests were %s and %s, expected two "
            "server/lock posts\n", ln.path[0], ln.path[1]);
    ok = FALSE;
  }
  if (strstr(ln.body[0], "\"server_key\":\"" KEY_A "\"") == NULL) {
    fprintf(stderr,
            "keyStampedAtFireTime: the post queued under key A carried: %s\n",
            ln.body[0]);
    ok = FALSE;
  }
  if (strstr(ln.body[1], "\"server_key\":\"" KEY_B "\"") == NULL) {
    fprintf(stderr,
            "keyStampedAtFireTime: the post queued after the swap to key B "
            "carried: %s - it was keyed when it was queued, not when it "
            "fired\n", ln.body[1]);
    ok = FALSE;
  }
  if (keyCount(ln.body[0], "\"server_key\"") != 1 ||
      keyCount(ln.body[1], "\"server_key\"") != 1) {
    fprintf(stderr,
            "keyStampedAtFireTime: server_key must appear once per body: "
            "%s / %s\n", ln.body[0], ln.body[1]);
    ok = FALSE;
  }
  if (strstr(ln.body[0], "\"locked\":true") == NULL ||
      strstr(ln.body[1], "\"locked\":false") == NULL) {
    fprintf(stderr,
            "keyStampedAtFireTime: stamping the key lost the body's own "
            "fields: %s / %s\n", ln.body[0], ln.body[1]);
    ok = FALSE;
  }
  return ok;
}
