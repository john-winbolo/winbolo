/*
 * The round log uploads from the worker, and asking costs nothing.
 *
 * serverDedicatedLogFlushPendingUpload calls httpSendLogFile on the tick
 * thread, between winbolonetEndSession and winbolonetBeginSession. That is a
 * multipart POST of a whole round's log with a 60s timeout, made under the
 * tick lock, so a slow WinBolo.net stops the server for every player in the
 * game while the round changes over.
 *
 * WBN_JOB_UPLOAD is the one job kind the worker carries out itself rather
 * than handing a reply back for: winbolonetThreadAddUpload copies the file
 * name and the key, and the worker sends them with httpSendLogFile. Being a
 * job, it keeps its place in the queue, which is what lets it sit between
 * server/quit and server/register without any of the three being on the
 * tick thread.
 *
 * What this case pins:
 *   - The upload reaches the host: the request arrives on the log path
 *     carrying the key that was queued, and its body carries the file's
 *     contents under the form field the uploader has always used.
 *   - Enqueuing returns in under 50ms against a host that takes 3s to
 *     answer. It is the property the phase exists for.
 *   - An enqueue after the thread is gone is refused with id 0.
 *
 * The URL is not an /api/v1/ endpoint: httpSendLogFile builds
 * <baseUrl>/log.php?key=<key>, so the key rides in the query string and the
 * path the listener records carries it.
 *
 * Nothing outside this case calls winbolonetThreadAddUpload yet;
 * serverDedicatedLogFlushPendingUpload still posts on the tick thread.
 */

#include <stdio.h>
#include <string.h>

#include "wbn_test_harness.h"

#include "http.h"
#include "winbolonetthread.h"

#ifdef _WIN32
#include <process.h>
#define UPLOAD_PID() ((unsigned long)_getpid())
#else
#include <unistd.h>
#define UPLOAD_PID() ((unsigned long)getpid())
#endif

/* Set by CMake to "${CMAKE_BINARY_DIR}/test-scratch", the same root the unit
 * tests write under. The fallback keeps a hand-built translation unit
 * compiling; it resolves against the working directory, which for CTest is
 * the build directory anyway. */
#ifndef WB_TEST_SCRATCH_DIR
#define WB_TEST_SCRATCH_DIR "test-scratch"
#endif

/* A key is WINBOLONET_KEY_LEN - 1 characters. This one only has to come back
 * off the wire unchanged. */
#define UPLOAD_KEY "fedcba9876543210fedcba9876543210"

/* What goes in the file, and what the body has to carry. Small on purpose:
 * libcurl adds an Expect: 100-continue to a body over 1KB, and the listener
 * does not answer one. */
#define UPLOAD_CONTENTS "wbn log upload case\nround 1\nnothing else\n"

/* The form field and file name httpSendLogFile puts the log under. */
#define UPLOAD_FIELD    "name=\"logfile\""
#define UPLOAD_FILENAME "filename=\"log.dat\""

/* How long the listener sits on its answer, and the bound the enqueue has to
 * come in under. Sixty times apart, so a synchronous upload could not slip
 * under the bound. */
#define UPLOAD_HOLD_MS        3000
#define UPLOAD_ENQUEUE_MAX_MS   50

/* One post held 3s, with the connect and the multipart build around it. */
#define UPLOAD_WAIT_MS 40000

/*********************************************************
*NAME:          uploadWriteFile
*PURPOSE:
* Writes the log the case uploads, under the build's scratch
* root. The pid in the name is what keeps two runs sharing
* one build directory off each other's file, the same reason
* tests/unit/scratch_dir.c puts one in its path.
*********************************************************/
static bool uploadWriteFile(char *path, size_t pathSz) {
  FILE *f;
  size_t len = strlen(UPLOAD_CONTENTS);

  /* Succeeds when the root is already there, which it usually is. */
  SDL_CreateDirectory(WB_TEST_SCRATCH_DIR);

  snprintf(path, pathSz, "%s/wbn_log_upload_%lu.log", WB_TEST_SCRATCH_DIR,
           UPLOAD_PID());

  f = fopen(path, "wb");
  if (f == NULL) {
    fprintf(stderr, "logUploadRuns: cannot write '%s'\n", path);
    return FALSE;
  }
  if (fwrite(UPLOAD_CONTENTS, 1, len, f) != len) {
    fprintf(stderr, "logUploadRuns: short write to '%s'\n", path);
    fclose(f);
    return FALSE;
  }
  fclose(f);
  return TRUE;
}

bool wbnTestLogUploadRuns(void) {
  WbnTestListener ln;
  unsigned short port = 0;
  char baseUrl[64];
  char filePath[1024];
  char wantPath[256];
  Uint64 started;
  Uint64 elapsed;
  uint32_t id;
  uint32_t refused;
  int requests;
  bool ok = TRUE;

  snprintf(wantPath, sizeof(wantPath), "/log.php?key=%s", UPLOAD_KEY);

  if (!wbnTestListenerStart(&ln, &port, UPLOAD_HOLD_MS)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "logUploadRuns: httpCreate failed\n");
    wbnTestListenerStop(&ln);
    return FALSE;
  }
  /* No bearer is set: httpSendLogFile is permissive about a missing one and
   * sends unauthenticated, which is what the round-end upload does after
   * winbolonetEndSession has cleared it. */

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "logUploadRuns: winbolonetThreadCreate failed\n");
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  if (!uploadWriteFile(filePath, sizeof(filePath))) {
    winbolonetThreadDestroy();
    httpDestroy();
    wbnTestListenerStop(&ln);
    return FALSE;
  }

  /* The call under test. The post behind it takes 3s; this must not. */
  started = SDL_GetTicks();
  id = winbolonetThreadAddUpload(filePath, UPLOAD_KEY);
  elapsed = SDL_GetTicks() - started;

  if (id == 0) {
    fprintf(stderr, "logUploadRuns: the upload was refused with the thread "
                    "running - id 0\n");
    ok = FALSE;
  }
  if (elapsed >= UPLOAD_ENQUEUE_MAX_MS) {
    fprintf(stderr,
            "logUploadRuns: winbolonetThreadAddUpload took %llums against a "
            "host answering in %dms - the upload is on the caller's thread\n",
            (unsigned long long)elapsed, UPLOAD_HOLD_MS);
    ok = FALSE;
  }

  wbnTestWaitForRequests(&ln, 1, UPLOAD_WAIT_MS);

  /* The destroy waits for the worker to finish the upload, so the file is
   * not taken away from under it. */
  winbolonetThreadDestroy();

  refused = winbolonetThreadAddUpload(filePath, UPLOAD_KEY);
  if (refused != 0) {
    fprintf(stderr,
            "logUploadRuns: an upload queued after the thread was destroyed "
            "returned id %u, expected 0\n", (unsigned int)refused);
    ok = FALSE;
  }

  httpDestroy();
  wbnTestListenerStop(&ln);
  SDL_RemovePath(filePath);

  requests = SDL_GetAtomicInt(&ln.requests);
  if (requests != 1) {
    fprintf(stderr, "logUploadRuns: listener saw %d requests, expected 1\n",
            requests);
    return FALSE;
  }

  if (strcmp(ln.path[0], wantPath) != 0) {
    fprintf(stderr, "logUploadRuns: the upload went to %s, expected %s\n",
            ln.path[0], wantPath);
    ok = FALSE;
  }
  if (strstr(ln.body[0], UPLOAD_CONTENTS) == NULL) {
    fprintf(stderr,
            "logUploadRuns: the file's contents are not in the %d byte body "
            "the listener read\n", ln.bodyLen[0]);
    ok = FALSE;
  }
  if (strstr(ln.body[0], UPLOAD_FIELD) == NULL ||
      strstr(ln.body[0], UPLOAD_FILENAME) == NULL) {
    fprintf(stderr,
            "logUploadRuns: the %d byte body carries no %s / %s part\n",
            ln.bodyLen[0], UPLOAD_FIELD, UPLOAD_FILENAME);
    ok = FALSE;
  }
  return ok;
}
