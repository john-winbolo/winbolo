/*
 * A job's reply comes back to the thread that asked for it, and asking
 * costs nothing.
 *
 * The worker queue could only ever fire and forget: winbolonetThreadRun
 * threw every response away, so any WinBolo.net call whose answer mattered
 * had to be made synchronously on the tick thread and paid a full round trip
 * there. A job carries a kind, the worker keeps the reply for any kind but
 * WBN_JOB_NONE, and winbolonetThreadDrainResults hands the kept replies to
 * the caller's own thread. That is what lets a call whose result matters
 * move off the tick thread without the result arriving on the wrong one.
 *
 * What this case pins:
 *   - A kinded job's result comes back once, carrying the id the enqueue
 *     returned, the kind it was queued with, the status and the body, and
 *     the handler runs on the thread that drained, not the worker's.
 *   - Results arrive oldest first, and two jobs get two different non-zero
 *     ids.
 *   - Enqueuing returns in under 50ms against a host that takes 3s to
 *     answer. It is the property the mechanism exists for.
 *   - A WBN_JOB_NONE job still produces no result at all, so the
 *     fire-and-forget path every existing caller uses is unchanged.
 *   - An enqueue after the thread is gone is refused with id 0.
 *
 * The listener holds every answer for 3s, which is what makes the timing
 * assertion mean something and also what makes the three posts take about
 * nine seconds to drain. It answers server/register with a canned success
 * body and everything else with {}, which gives the two kinded jobs
 * distinguishable replies without either of them meaning anything to
 * WinBolo.net.
 *
 * Nothing outside this case calls winbolonetThreadAddJob yet.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "wbn_bearer.h"
#include "winbolonetthread.h"

#define JOBS_FAKE_BEARER \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* Two kinds, neither of which means anything yet. They only have to differ
 * from WBN_JOB_NONE and from each other. */
#define JOBS_KIND_A 7
#define JOBS_KIND_B 9

/* How long the listener sits on each answer, and the bound an enqueue has to
 * come in under. Sixty times apart, so a synchronous call could not slip
 * under the bound. */
#define JOBS_HOLD_MS       3000
#define JOBS_ENQUEUE_MAX_MS  50

/* Three posts held 3s each, drained one at a time, is about nine seconds. */
#define JOBS_DRAIN_MS 40000

/* Results kept by the handler. Two are expected; the room is for catching a
 * third that should not be there. */
#define JOBS_MAX_RESULTS 8

typedef struct {
  int          count;              /* handler calls */
  uint32_t     id[JOBS_MAX_RESULTS];
  uint8_t      kind[JOBS_MAX_RESULTS];
  int          status[JOBS_MAX_RESULTS];
  char         body[JOBS_MAX_RESULTS][256];
  SDL_ThreadID thread[JOBS_MAX_RESULTS];
} JobsCapture;

/*********************************************************
*NAME:          jobsCapture
*PURPOSE:
* Keeps what one result carried, and which thread it was
* handed over on.
*********************************************************/
static void jobsCapture(uint32_t id, uint8_t kind, int status,
                        const char *response, void *ctx) {
  JobsCapture *cap = (JobsCapture *)ctx;
  int i = cap->count;

  cap->count++;
  if (i < 0 || i >= JOBS_MAX_RESULTS) {
    return;
  }
  cap->id[i] = id;
  cap->kind[i] = kind;
  cap->status[i] = status;
  cap->thread[i] = SDL_GetCurrentThreadID();
  if (response != NULL) {
    snprintf(cap->body[i], sizeof(cap->body[i]), "%s", response);
  } else {
    cap->body[i][0] = '\0';
  }
}

/*********************************************************
*NAME:          jobsCheckResult
*PURPOSE:
* Compares one captured result against what the job that
* produced it should have brought back. Prints what it
* found when it does not match.
*********************************************************/
static bool jobsCheckResult(const JobsCapture *cap, int i, const char *label,
                            uint32_t wantId, uint8_t wantKind,
                            const char *wantBody, SDL_ThreadID wantThread) {
  bool ok = TRUE;

  if (cap->id[i] != wantId) {
    fprintf(stderr, "jobResultReturns: %s came back with id %u, expected %u\n",
            label, (unsigned int)cap->id[i], (unsigned int)wantId);
    ok = FALSE;
  }
  if (cap->kind[i] != wantKind) {
    fprintf(stderr, "jobResultReturns: %s came back with kind %u, "
            "expected %u\n", label, (unsigned int)cap->kind[i],
            (unsigned int)wantKind);
    ok = FALSE;
  }
  if (cap->status[i] != 200) {
    fprintf(stderr, "jobResultReturns: %s came back with status %d, "
            "expected 200\n", label, cap->status[i]);
    ok = FALSE;
  }
  if (strcmp(cap->body[i], wantBody) != 0) {
    fprintf(stderr, "jobResultReturns: %s came back with body '%s', "
            "expected '%s'\n", label, cap->body[i], wantBody);
    ok = FALSE;
  }
  if (cap->thread[i] != wantThread) {
    fprintf(stderr, "jobResultReturns: %s was handed over on thread %llu, "
            "expected the draining thread %llu\n", label,
            (unsigned long long)cap->thread[i],
            (unsigned long long)wantThread);
    ok = FALSE;
  }
  return ok;
}

/*********************************************************
*NAME:          jobsElapsedOk
*PURPOSE:
* TRUE when an enqueue came back inside the bound. Prints
* what it took when it did not.
*********************************************************/
static bool jobsElapsedOk(const char *label, Uint64 elapsed) {
  if (elapsed >= JOBS_ENQUEUE_MAX_MS) {
    fprintf(stderr,
            "jobResultReturns: enqueuing %s took %llums against a host "
            "answering in %dms - the post is on the caller's thread\n",
            label, (unsigned long long)elapsed, JOBS_HOLD_MS);
    return FALSE;
  }
  return TRUE;
}

bool wbnTestJobResultReturns(void) {
  WbnTestListener ln;
  JobsCapture cap;
  unsigned short port = 0;
  char baseUrl[64];
  char registerBody[256];
  SDL_ThreadID mainThread;
  Uint64 started;
  uint32_t idA;
  uint32_t idB;
  uint32_t refused;
  int waited;
  bool queuedNone;
  bool ok = TRUE;

  memset(&cap, 0, sizeof(cap));
  mainThread = SDL_GetCurrentThreadID();
  snprintf(registerBody, sizeof(registerBody),
           "{\"server_key\":\"%s\",\"server_token\":\"%s\"}",
           WBN_TEST_REGISTER_KEY, WBN_TEST_REGISTER_TOKEN);

  if (!wbnTestListenerStart(&ln, &port, JOBS_HOLD_MS)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "jobResultReturns: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  /* wbn_api_post_server refuses to send without a bearer. The listener
   * never reads it. */
  httpSetServerBearerToken(JOBS_FAKE_BEARER);

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "jobResultReturns: winbolonetThreadCreate failed\n");
    httpDestroy();
    httpClearServerBearerToken();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* A fire-and-forget post first. It is the oldest, so the worker sends it
   * before either job below, and a result from it would arrive ahead of
   * theirs if the kind were ever ignored. */
  started = SDL_GetTicks();
  queuedNone = winbolonetThreadAddServerRequest("server/lock",
                                                "{\"locked\":true}");
  if (!jobsElapsedOk("the fire-and-forget post", SDL_GetTicks() - started)) {
    ok = FALSE;
  }
  if (queuedNone != TRUE) {
    fprintf(stderr, "jobResultReturns: the fire-and-forget post was "
                    "refused with the thread running\n");
    ok = FALSE;
  }

  /* The two jobs whose replies are kept. Every post here goes to a host
   * that holds its answer 3s, so an enqueue that did the post itself could
   * not come in under the bound. */
  started = SDL_GetTicks();
  idA = winbolonetThreadAddJob("server/register", "{}", /*needs_bearer*/ FALSE,
                               JOBS_KIND_A);
  if (!jobsElapsedOk("job A", SDL_GetTicks() - started)) {
    ok = FALSE;
  }

  started = SDL_GetTicks();
  idB = winbolonetThreadAddJob("server/lock", "{\"locked\":false}",
                               /*needs_bearer*/ TRUE, JOBS_KIND_B);
  if (!jobsElapsedOk("job B", SDL_GetTicks() - started)) {
    ok = FALSE;
  }

  if (idA == 0 || idB == 0 || idA == idB) {
    fprintf(stderr,
            "jobResultReturns: job ids were %u and %u, expected two "
            "different non-zero ids\n",
            (unsigned int)idA, (unsigned int)idB);
    ok = FALSE;
  }

  /* Poll the drain as a tick would. Nothing arrives until the worker has
   * worked through the fire-and-forget post ahead of them. */
  waited = 0;
  while (waited < JOBS_DRAIN_MS && cap.count < 2) {
    winbolonetThreadDrainResults(jobsCapture, &cap);
    if (cap.count >= 2) {
      break;
    }
    SDL_Delay(WBN_TEST_POLL_MS);
    waited += WBN_TEST_POLL_MS;
  }

  /* One more pass once everything has been answered: it is what would pick
   * up a result from the WBN_JOB_NONE post if the worker ever kept one. */
  wbnTestWaitForRequests(&ln, 3, JOBS_DRAIN_MS);
  winbolonetThreadDrainResults(jobsCapture, &cap);

  if (cap.count != 2) {
    fprintf(stderr,
            "jobResultReturns: %d results came back, expected 2 - the "
            "listener answered %d requests\n",
            cap.count, SDL_GetAtomicInt(&ln.requests));
    ok = FALSE;
  } else {
    /* Oldest first: job A was queued before job B. */
    if (!jobsCheckResult(&cap, 0, "job A", idA, JOBS_KIND_A, registerBody,
                         mainThread)) {
      ok = FALSE;
    }
    if (!jobsCheckResult(&cap, 1, "job B", idB, JOBS_KIND_B, "{}",
                         mainThread)) {
      ok = FALSE;
    }
  }

  winbolonetThreadDestroy();

  /* With the worker gone the enqueue takes nothing on and says so, which is
   * the answer winbolonetGoodbye's fallback reads. */
  refused = winbolonetThreadAddJob("server/lock", "{}", /*needs_bearer*/ TRUE,
                                   JOBS_KIND_A);
  if (refused != 0) {
    fprintf(stderr,
            "jobResultReturns: an enqueue after the thread was destroyed "
            "returned id %u, expected 0\n", (unsigned int)refused);
    ok = FALSE;
  }

  /* And a drain with no thread hands nothing over. */
  winbolonetThreadDrainResults(jobsCapture, &cap);
  if (cap.count != 2) {
    fprintf(stderr,
            "jobResultReturns: draining after the thread was destroyed "
            "delivered %d results in all, expected the same 2\n", cap.count);
    ok = FALSE;
  }

  httpDestroy();
  httpClearServerBearerToken();
  wbnTestListenerStop(&ln);

  return ok;
}
