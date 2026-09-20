/*
 * The worker's waiting queue is bounded (test_wbn_queue_cap.c).
 *
 * Every fire-and-forget post - a lobby update, a leave, a lock, a teams
 * change - used to be taken however many were already waiting. Each one can
 * take up to 35s on the wire, so against a slow or unreachable WinBolo.net
 * the queue grew for as long as the server ran and nothing at the back of it
 * was ever going to be sent. The queue now holds at most
 * WBN_WAITING_NONE_MAX of them and answers FALSE past that, which is the
 * answer a caller already has a path for: it is what a worker that is not
 * running has always said.
 *
 * What the cap must not do is refuse the posts the round transition and the
 * join path depend on. A register, a verify and an upload carry a kind and
 * are never counted against it, and neither is the session quit, which goes
 * through winbolonetThreadAddSessionRequest for exactly that reason.
 *
 * What this case pins, against a listener that holds every answer long
 * enough that nothing drains while the queue is being filled:
 *   - the first WBN_WAITING_NONE_MAX fire-and-forget posts are taken;
 *   - every one past that is refused, and refused without being queued;
 *   - a register is still taken with the queue full;
 *   - the session quit is still taken with the queue full.
 *
 * The listener is stopped before the worker is, so the posts still queued
 * fail their connect at once rather than being held one at a time.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "winbolonet_core.h"
#include "winbolonetthread.h"

/* Longer than this case takes to fill the queue, so the worker finishes
 * nothing while it is being filled and the count it is measured against
 * cannot move underneath it. */
#define CAP_HOLD_MS 4000

/* Posts attempted: the cap plus a few that must be refused. */
#define CAP_ATTEMPTS (WBN_WAITING_NONE_MAX + 6)

bool wbnTestQueueCapRefusesPosts(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  int taken = 0;
  int refused = 0;
  int firstRefused = -1;
  int i;
  uint32_t registerJob;
  bool quitTaken;
  bool ok = TRUE;

  if (!wbnTestListenerStart(&ln, &port, CAP_HOLD_MS)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "queueCapRefusesPosts: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "queueCapRefusesPosts: winbolonetThreadCreate failed\n");
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  for (i = 0; i < CAP_ATTEMPTS; i++) {
    if (winbolonetThreadAddRequest("server/update", "{}") == TRUE) {
      taken++;
    } else {
      refused++;
      if (firstRefused < 0) {
        firstRefused = i;
      }
    }
  }

  /* The two the round transition cannot do without, with the queue full. */
  registerJob = winbolonetThreadAddJob("server/register", "{}",
                                       /*needs_bearer*/ FALSE,
                                       WBN_JOB_REGISTER);
  quitTaken = winbolonetThreadAddSessionRequest("server/quit", "{}");

  /* The listener first: what is still queued then fails its connect rather
   * than being held for CAP_HOLD_MS each. */
  wbnTestListenerStop(&ln);
  winbolonetThreadDestroy();
  httpDestroy();

  if (taken != WBN_WAITING_NONE_MAX) {
    fprintf(stderr,
            "queueCapRefusesPosts: the queue took %d posts, expected %d\n",
            taken, WBN_WAITING_NONE_MAX);
    ok = FALSE;
  }
  if (refused != CAP_ATTEMPTS - WBN_WAITING_NONE_MAX) {
    fprintf(stderr,
            "queueCapRefusesPosts: %d posts were refused, expected %d\n",
            refused, CAP_ATTEMPTS - WBN_WAITING_NONE_MAX);
    ok = FALSE;
  }
  if (firstRefused != WBN_WAITING_NONE_MAX) {
    fprintf(stderr,
            "queueCapRefusesPosts: the first refusal was post %d, expected "
            "post %d\n", firstRefused, WBN_WAITING_NONE_MAX);
    ok = FALSE;
  }
  if (registerJob == 0) {
    fprintf(stderr,
            "queueCapRefusesPosts: the register was refused with the queue "
            "full - the round transition has no other way through\n");
    ok = FALSE;
  }
  if (quitTaken != TRUE) {
    fprintf(stderr,
            "queueCapRefusesPosts: the session quit was refused with the "
            "queue full\n");
    ok = FALSE;
  }
  return ok;
}
