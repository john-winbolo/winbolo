/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/*********************************************************
*Name:          http (NSURLSession)
*Filename:      http_nsurl.m
*Author:        John Morrison
*Creation Date: 16/10/26
*Last Modified: 16/10/26
*Purpose:
*  The http.h interface for Apple platforms that have no
*  libcurl (iOS). Every function behaves as its http.c
*  counterpart does for the caller: the same URLs, headers,
*  Ed25519 signature, bearer rules, return values (HTTP
*  status, -1 on a transport error, -2 on cancel), time
*  limits, response ceiling, progress reports and the
*  multipart log upload.
*
*  Callers expect a blocking call, so each request starts an
*  NSURLSession task and waits on a dispatch semaphore until
*  the task completes. Most callers are on worker threads;
*  the ones that are not (the sign-in calls a dialog makes
*  directly) block the thread they are on, as with libcurl.
*  On iOS the token check before a join runs on a thread of
*  its own behind the Connecting screen. The wait wakes
*  every 50 ms when the caller gave a cancel flag or a
*  connect limit, to check them.
*
*  Built with ARC (see CMakeLists.txt).
*
*  Differences from http.c, all forced by NSURLSession:
*  - httpSetAltIpAddress stores the address but cannot bind
*    to it: NSURLSession has no way to choose the local
*    interface.
*  - The connect limit is measured to the first request
*    body bytes sent, which shows the connection is up.
*    NSURLSession does not report the end of the connect
*    phase on its own, and a request with no body sends no
*    body bytes, so a GET has only its total limit: a
*    connect limit there would also cover the server's time
*    to answer.
*  - httpWorkerPoolBegin/End only record the worker thread
*    for the slow-call log line. NSURLSession keeps its own
*    pool of connections, shared by every thread.
*  - Every request path starts the module on first use, as
*    http.c's GET and prefs paths do, because the sign-in
*    calls run httpDestroy when they finish and the next
*    request may come from any thread. The host is read from
*    the preferences once, at the first httpCreate (on both
*    platforms the welcome screen's news fetch, on the main
*    thread), and kept: prefs.c has no lock, so a later
*    httpCreate on a worker thread must not read it again.
*    A Host changed in the preferences while the app runs
*    takes effect at the next launch.
*  - Paths and queries are percent-encoded before they are
*    parsed, since NSURL refuses a space or other character
*    a URL may not carry, which libcurl sends as it is. A
*    "%XX" already in the text, and the URL delimiters "#",
*    "[" and "]", are kept.
*  - A redirect to another scheme, host or port drops the
*    Authorization header, as libcurl does by default. Such
*    a redirect of a request with a body (a POST or PUT) is
*    refused, so the body is never sent to the new host.
*********************************************************/

#if !__has_feature(objc_arc)
#error "http_nsurl.m must be compiled with -fobjc-arc"
#endif

/* Same rule as http.c: this file embeds the WBN signing key and must never
 * reach the browser build. */
#ifdef __EMSCRIPTEN__
#error "http_nsurl.m (embeds WBN_SIGNING_KEY) must never be compiled for the WASM build"
#endif

#import <Foundation/Foundation.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include "tweetnacl.h"
#include "cJSON.h"
#include "wbn_signing_key.h"

/* TweetNaCl requires randombytes() for key generation functions.
   We only use crypto_sign() (deterministic), but the linker needs the symbol. */
void randombytes(unsigned char *buf, unsigned long long len) {
  (void)buf; (void)len;
}

#include "global.h"
#include "http.h"
#include "../common/wb_log.h"
#include "../common/prefs.h"
#include "winbolonet_core.h"
#include "wbn_bearer.h"

/* Atomic because httpDestroy clears it on the sign-in thread while workers
 * read it. A request path does not read it after its own httpCreate: it
 * goes by httpCreate's return value (see wbnEnsureStarted). */
static atomic_bool httpStarted = false;
/* Set, with release ordering, once wbnBaseUrl / wbnHostString have been
 * filled; they never change after that. See httpCreate. */
static atomic_bool s_baseUrlReady;
static char wbnHostString[FILENAME_MAX]; /* hostname only, no scheme */
static char wbnBaseUrl[FILENAME_MAX];    /* full base URL, e.g. https://wbn.winbolo.net */
static char altIpAddress[FILENAME_MAX];
static char wbnHostOverride[FILENAME_MAX]; /* command-line override for WBN host */

/* Per-call override for the log-upload total timeout, in seconds. 0 selects
 * the default. */
static long s_logUploadTimeoutOverride = 0;

/* httpSetDeadline: the SDL_GetTicks value every request has to be over by,
 * or 0 for none. Under the spinlock because the worker thread reads it while
 * another thread sets it. */
static SDL_SpinLock s_deadlineLock = 0;
static Uint64       s_deadlineMs   = 0;

static Uint64 wbnDeadlineGet(void) {
  Uint64 deadline;
  SDL_LockSpinlock(&s_deadlineLock);
  deadline = s_deadlineMs;
  SDL_UnlockSpinlock(&s_deadlineLock);
  return deadline;
}

/* The thread that called httpWorkerPoolBegin, for the slow-call log line. */
static SDL_ThreadID s_workerThread = 0;

/* Set once httpSetAltIpAddress has been told it cannot bind, so the warning
 * is logged once rather than on every request. */
static bool s_altIpWarned = false;

/* Time limits, matching http.c. */
#define WBN_POST_TIMEOUT_MS          30000L
#define WBN_POST_CONNECT_TIMEOUT_MS   5000L
#define WBN_API_GET_TIMEOUT_MS       30000L
#define WBN_PREFS_TIMEOUT_MS         30000L
#define WBN_DOWNLOAD_TIMEOUT_MS     120000L
#define WBN_UPLOAD_TIMEOUT_S            60L
#define WBN_UPLOAD_CONNECT_TIMEOUT_MS 5000L

/* How often a waiting request wakes to check its cancel flag and its
 * connect limit. */
#define WBN_WAIT_SLICE_MS 50L

/* See http.c: a call off the worker that runs longer than 100 ms is
 * reported, and one on the worker only past 1000 ms. */
#define WBN_SLOW_CALL_WARN_MS_OFF_WORKER 100u
#define WBN_SLOW_CALL_WARN_MS_ON_WORKER  1000u

static void wbnLogSlowCall(const char *what, Uint64 startMs) {
  Uint64 elapsedMs = SDL_GetTicks() - startMs;
  SDL_ThreadID self = SDL_GetCurrentThreadID();
  bool onWorker = (s_workerThread != 0 && self == s_workerThread);
  Uint64 warnMs = onWorker ? WBN_SLOW_CALL_WARN_MS_ON_WORKER
                           : WBN_SLOW_CALL_WARN_MS_OFF_WORKER;
  if (elapsedMs > warnMs) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "slow WBN call [%s]: %llu ms on thread %llu%s",
                what,
                (unsigned long long)elapsedMs,
                (unsigned long long)self,
                onWorker ? " (wbn worker)" : "");
  }
}

/*********************************************************
*NAME:          buildBaseUrl
*PURPOSE:
* Parses the INI host value into wbnHostString (bare hostname)
* and wbnBaseUrl (scheme + hostname, no trailing slash).
* Same rules as http.c.
*********************************************************/
static void buildBaseUrl(const char *iniValue) {
  const char *host;

  if (strncmp(iniValue, "https://", 8) == 0) {
    snprintf(wbnBaseUrl, sizeof(wbnBaseUrl), "%s", iniValue);
    host = iniValue + 8;
  } else if (strncmp(iniValue, "http://", 7) == 0) {
    snprintf(wbnBaseUrl, sizeof(wbnBaseUrl), "%s", iniValue);
    host = iniValue + 7;
  } else {
    /* No scheme — default to HTTPS */
    snprintf(wbnBaseUrl, sizeof(wbnBaseUrl), "https://%s", iniValue);
    host = iniValue;
  }

  /* Strip trailing slash from base URL */
  size_t len = strlen(wbnBaseUrl);
  if (len > 0 && wbnBaseUrl[len - 1] == '/') {
    wbnBaseUrl[len - 1] = '\0';
  }

  /* Store bare hostname (stop at any path separator) */
  snprintf(wbnHostString, sizeof(wbnHostString), "%s", host);
  char *slash = strchr(wbnHostString, '/');
  if (slash) *slash = '\0';
}

/*********************************************************
*NAME:          wbnLogKeyIsSensitive / wbnLogRedactNode /
*               wbnLogRedactJson / wbnLogRedactedJsonDebug
*PURPOSE:
* Credential-safe debug logging of JSON bodies, the same
* rules as http.c: any member whose key names a secret has
* its value replaced with "***", and a body that is not JSON
* is reported by length only.
*********************************************************/
static bool wbnLogKeyIsSensitive(const char *name) {
  char   low[64];
  size_t i;
  if (name == NULL) return false;
  for (i = 0; name[i] != '\0' && i < sizeof(low) - 1; i++) {
    low[i] = (char)tolower((unsigned char)name[i]);
  }
  low[i] = '\0';
  if (strstr(low, "token"))  return true;
  if (strstr(low, "key"))    return true;
  if (strstr(low, "pass"))   return true;
  if (strstr(low, "secret")) return true;
  if (strstr(low, "auth"))   return true;
  if (strstr(low, "bearer")) return true;
  /* WBN per-player credentials are carried as player_a / player_b */
  if (strcmp(low, "player_a") == 0 || strcmp(low, "player_b") == 0) return true;
  return false;
}

static void wbnLogRedactNode(cJSON *node) {
  cJSON *child = node->child;
  while (child != NULL) {
    cJSON *next = child->next;
    if (child->string != NULL && wbnLogKeyIsSensitive(child->string)) {
      cJSON *repl = cJSON_CreateString("***");
      if (repl != NULL) {
        cJSON_ReplaceItemViaPointer(node, child, repl);
      }
    } else if (cJSON_IsObject(child) || cJSON_IsArray(child)) {
      wbnLogRedactNode(child);
    }
    child = next;
  }
}

static char *wbnLogRedactJson(const char *json) {
  cJSON *root;
  char  *out;
  if (json == NULL) return NULL;
  root = cJSON_Parse(json);
  if (root == NULL) {
    out = malloc(48);
    if (out != NULL) {
      snprintf(out, 48, "(%lu bytes, non-JSON)", (unsigned long)strlen(json));
    }
    return out;
  }
  wbnLogRedactNode(root);
  out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

static void wbnLogRedactedJsonDebug(int cat, const char *prefix, const char *json) {
#if WB_LOG_LEVEL <= WB_LOG_LEVEL_DEBUG
  char *redacted = wbnLogRedactJson(json);
  WB_LOG_DEBUG(cat, "%s%s", prefix, redacted ? redacted : "(null)");
  free(redacted);
#else
  (void)cat; (void)prefix; (void)json;
#endif
}

/*********************************************************
*NAME:          wbn_sign_request
*PURPOSE:
* Produces a hex-encoded Ed25519 signature over the
* concatenation of timestamp_str and json_body.
* sig_hex_out must be at least 129 bytes.
*********************************************************/
static void wbn_sign_request(const char *timestamp_str, const char *json_body, char *sig_hex_out) {
  size_t ts_len   = strlen(timestamp_str);
  size_t body_len = strlen(json_body);
  size_t msg_len  = ts_len + body_len;

  unsigned char *message = malloc(msg_len);
  memcpy(message, timestamp_str, ts_len);
  memcpy(message + ts_len, json_body, body_len);

  /* crypto_sign outputs (signature ∥ message); we only need the first 64 bytes */
  unsigned long long smlen;
  unsigned char *sm = malloc(crypto_sign_BYTES + msg_len);
  crypto_sign(sm, &smlen, message, msg_len, WBN_SIGNING_KEY);

  for (int i = 0; i < crypto_sign_BYTES; i++) {
    snprintf(sig_hex_out + i * 2, 3, "%02x", sm[i]);
  }
  sig_hex_out[crypto_sign_BYTES * 2] = '\0';

  free(sm);
  free(message);
}

/* Adds X-WBN-Timestamp and X-WBN-Signature over body to req. */
static void wbnAddSignatureHeaders(NSMutableURLRequest *req, const char *body) {
  char timestamp_str[32];
  snprintf(timestamp_str, sizeof(timestamp_str), "%ld", (long)time(NULL));

  char sig_hex[129];
  wbn_sign_request(timestamp_str, body, sig_hex);

  [req setValue:@(sig_hex) forHTTPHeaderField:@"X-WBN-Signature"];
  [req setValue:@(timestamp_str) forHTTPHeaderField:@"X-WBN-Timestamp"];
}

/* Adds Authorization: Bearer <token> to req. */
static void wbnAddBearerHeader(NSMutableURLRequest *req, const char *token) {
  NSString *value = [NSString stringWithFormat:@"Bearer %s", token];
  [req setValue:value forHTTPHeaderField:@"Authorization"];
}

/*********************************************************
*NAME:          wbnSession
*PURPOSE:
* Returns the one NSURLSession every request runs on. It is
* made on first use and kept for the life of the process,
* so its connections are reused across httpCreate and
* httpDestroy. Ephemeral: no cookies, cache or credentials
* are kept, the same as a libcurl handle with no cookie
* file. The limits set here are only outer bounds; each
* request applies its own.
*********************************************************/
static NSURLSession *wbnSession(void) {
  static NSURLSession *session = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    NSURLSessionConfiguration *cfg =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    cfg.URLCache = nil;
    cfg.HTTPCookieStorage = nil;
    cfg.HTTPShouldSetCookies = NO;
    cfg.timeoutIntervalForRequest = 600.0;
    cfg.timeoutIntervalForResource = 600.0;
    cfg.waitsForConnectivity = NO;

    /* Delegate callbacks of all tasks run on this queue, one at a time. */
    NSOperationQueue *queue = [[NSOperationQueue alloc] init];
    queue.maxConcurrentOperationCount = 1;
    queue.name = @"WinBolo.net HTTP";

    session = [NSURLSession sessionWithConfiguration:cfg
                                            delegate:nil
                                       delegateQueue:queue];
  });
  return session;
}

/*********************************************************
*NAME:          WbnHttpTask
*PURPOSE:
* The per-request delegate (NSURLSessionTask.delegate,
* iOS 15). Collects the response into a growing buffer or
* a file, applies the response ceiling, forwards progress,
* and signals the waiting thread when the task completes.
* Its fields are read by the waiting thread only after
* that signal, except gotResponse, which is atomic.
*********************************************************/
@interface WbnHttpTask : NSObject <NSURLSessionDataDelegate> {
@public
  dispatch_semaphore_t done;

  /* Response body in memory (when useBuf). Always NUL terminated. */
  bool   useBuf;
  char  *buf;
  size_t size;
  size_t capacity;
  size_t limit;      /* 0 = no ceiling */
  size_t keepMax;    /* nonzero: keep only this many bytes, drop the rest
                      * without failing the transfer */

  /* Response body to a file (when fp is non-NULL). */
  FILE  *fp;

  WbnProgressFn progressFn;
  void         *progressUser;

  atomic_bool gotResponse;
  bool    overLimit;
  bool    writeFailed;
  int64_t received;
  int64_t expected;  /* 0 when the server sent no Content-Length */
  long    status;
  NSError *error;
}
@end

@implementation WbnHttpTask

- (instancetype)init {
  self = [super init];
  if (self) {
    done = dispatch_semaphore_create(0);
    atomic_init(&gotResponse, false);
  }
  return self;
}

- (bool)initBuffer {
  useBuf = true;
  capacity = 1024;
  size = 0;
  buf = malloc(capacity);
  if (buf == NULL) return false;
  buf[0] = '\0';
  return true;
}

/* Hands the buffer to the caller, who frees it. */
- (char *)takeBuffer {
  char *out = buf;
  buf = NULL;
  size = 0;
  capacity = 0;
  return out;
}

- (void)dealloc {
  free(buf);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
didReceiveResponse:(NSURLResponse *)response
 completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  (void)session; (void)dataTask;
  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
    status = (long)((NSHTTPURLResponse *)response).statusCode;
  }
  long long len = response.expectedContentLength;
  expected = (len > 0) ? (int64_t)len : 0;
  atomic_store(&gotResponse, true);
  if (progressFn) {
    progressFn(progressUser, received, expected);
  }
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
  (void)session;
  if (overLimit || writeFailed) return;

  size_t incoming = (size_t)data.length;
  /* Refused as it arrives rather than checked at the end, as http.c does:
   * a response meant to exhaust this client's memory never finishes. */
  if (useBuf && limit != 0 && size + incoming > limit) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "response exceeds its %zu byte ceiling — transfer stopped",
                limit);
    overLimit = true;
    [dataTask cancel];
    return;
  }

  if (useBuf && keepMax != 0) {
    /* Keep the first keepMax bytes and let the rest go by. */
    size_t room = (size < keepMax) ? keepMax - size : 0;
    if (incoming > room) incoming = room;
    if (incoming == 0) {
      received += (int64_t)data.length;
      return;
    }
  }

  if (useBuf) {
    size_t needed = size + incoming + 1;
    if (needed > capacity) {
      size_t newcap = capacity * 2;
      if (newcap < needed) newcap = needed;
      char *tmp = realloc(buf, newcap);
      if (tmp == NULL) {
        writeFailed = true;
        [dataTask cancel];
        return;
      }
      buf = tmp;
      capacity = newcap;
    }
    __block size_t left = incoming;
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
      size_t n = range.length < left ? range.length : left;
      memcpy(self->buf + self->size, bytes, n);
      self->size += n;
      left -= n;
      if (left == 0) *stop = YES;
    }];
    buf[size] = '\0';
  } else if (fp != NULL) {
    __block bool ok = true;
    FILE *out = fp;
    [data enumerateByteRangesUsingBlock:^(const void *bytes, NSRange range, BOOL *stop) {
      if (fwrite(bytes, 1, range.length, out) != range.length) {
        ok = false;
        *stop = YES;
      }
    }];
    if (!ok) {
      writeFailed = true;
      [dataTask cancel];
      return;
    }
  }

  received += (int64_t)data.length;
  if (progressFn) {
    progressFn(progressUser, received, expected);
  }
}

/* A redirect is followed, as libcurl follows one here; the Authorization
 * header goes with it only to the scheme, host and port the request was
 * first sent to, which is what libcurl does with a header it was given. The signature and
 * timestamp headers go either way on a request with no body, as libcurl's
 * other custom headers do; they prove nothing without the signing key.
 * A request with a body is not redirected to another scheme, host or port
 * at all: a 307 or 308 sends the body again, and bodies carry secrets (the
 * API token in auth/validate, the password in login). */
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
  (void)session; (void)response;
  /* Compared with where the request started, not with the previous hop,
   * so a chain that passes through another host never counts as the same
   * origin again. */
  NSURL *from = task.originalRequest.URL;
  NSURL *to = request.URL;
  bool sameOrigin =
      from != nil && to != nil &&
      [from.scheme caseInsensitiveCompare:to.scheme ?: @""] == NSOrderedSame &&
      [from.host caseInsensitiveCompare:to.host ?: @""] == NSOrderedSame &&
      ((from.port == nil && to.port == nil) ||
       (from.port != nil && to.port != nil && [from.port isEqualToNumber:to.port]));
  if (!sameOrigin) {
    NSURLRequest *orig = task.originalRequest;
    NSString *method = orig.HTTPMethod ?: @"GET";
    bool hasBody = orig.HTTPBody != nil || orig.HTTPBodyStream != nil ||
                   [method caseInsensitiveCompare:@"POST"] == NSOrderedSame ||
                   [method caseInsensitiveCompare:@"PUT"] == NSOrderedSame;
    if (hasBody) {
      /* Refused: the task completes with the 3xx response, which every
       * caller treats as a failure. */
      WB_LOG_WARN(WB_LOG_CAT_NET,
                  "redirect to another host refused: the request has a body");
      completionHandler(nil);
      return;
    }
  }
  if (sameOrigin || [request valueForHTTPHeaderField:@"Authorization"] == nil) {
    completionHandler(request);
    return;
  }
  NSMutableURLRequest *stripped = [request mutableCopy];
  [stripped setValue:nil forHTTPHeaderField:@"Authorization"];
  WB_LOG_DEBUG(WB_LOG_CAT_NET, "redirect to another host: Authorization dropped");
  completionHandler(stripped);
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)err {
  (void)session; (void)task;
  error = err;
  dispatch_semaphore_signal(done);
}

@end

/* Outcome of wbnRunRequest beyond the HTTP status. */
typedef enum {
  WBN_RUN_OK = 0,      /* the server answered; status holds its code */
  WBN_RUN_ERROR,       /* transport error, time limit, ceiling or write failure */
  WBN_RUN_CANCELLED    /* the caller's cancel flag stopped it */
} WbnRunResult;

/*********************************************************
*NAME:          wbnRunRequest
*PURPOSE:
* Starts req as a data task with t as its delegate and waits
* for it on the calling thread. Ends it early when the total
* limit runs out, when the connect limit runs out before the
* connection shows any sign of life, or when *cancel_flag
* becomes non-zero. Always waits for the task to complete
* before returning, so no callback can touch t afterwards.
*
*ARGUMENTS:
* req          - The request
* t            - Its delegate, buffer or file already set
* totalMs      - Total time limit in milliseconds
* connectMs    - Connect limit in milliseconds, or 0 for none
* cancel_flag  - Cancel flag, or NULL
* reasonOut    - Receives a description of an error
*********************************************************/
static WbnRunResult wbnRunRequest(NSMutableURLRequest *req, WbnHttpTask *t,
                                  long totalMs, long connectMs,
                                  volatile int *cancel_flag,
                                  NSString **reasonOut) {
  if (altIpAddress[0] != '\0' && !s_altIpWarned) {
    s_altIpWarned = true;
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "WinBolo.net: alternate local address '%s' is not supported "
                "on this platform; using the default route", altIpAddress);
  }

  /* The connect limit is judged by request body bytes going out, so a
   * request with no body has only its total limit; otherwise the limit
   * would also cover the server's time to answer (see the file header). */
  if (req.HTTPBody == nil || req.HTTPBody.length == 0) {
    connectMs = 0;
  }

  /* Past the deadline (httpSetDeadline) nothing is sent; before it, the
   * total is cut to the time left. */
  Uint64 deadline = wbnDeadlineGet();
  if (deadline != 0) {
    Uint64 now = SDL_GetTicks();
    if (now >= deadline) {
      if (reasonOut) *reasonOut = @"past the shutdown deadline";
      return WBN_RUN_ERROR;
    }
    if ((Uint64)totalMs > deadline - now) {
      totalMs = (long)(deadline - now);
    }
  }

  /* The idle limit of the request itself; the total is enforced below. */
  req.timeoutInterval = (NSTimeInterval)totalMs / 1000.0 + 1.0;
  /* Bodies arrive as the server sent them, as with libcurl here, so the
   * progress byte counts and the response ceiling see the real sizes. */
  [req setValue:@"identity" forHTTPHeaderField:@"Accept-Encoding"];

  NSURLSessionDataTask *task = [wbnSession() dataTaskWithRequest:req];
  task.delegate = t;

  bool cancelled = false;
  bool timedOut  = false;
  Uint64 startMs = SDL_GetTicks();
  [task resume];

  /* Always in slices, so a deadline set while this request waits stops it
   * within one slice. */
  for (;;) {
    Uint64 elapsed = SDL_GetTicks() - startMs;
    long remaining = totalMs - (long)elapsed;
    Uint64 deadlineNow = wbnDeadlineGet();
    if (remaining <= 0 ||
        (deadlineNow != 0 && SDL_GetTicks() >= deadlineNow)) {
      timedOut = true;
      break;
    }
    long waitMs = (remaining > WBN_WAIT_SLICE_MS) ? WBN_WAIT_SLICE_MS : remaining;
    if (dispatch_semaphore_wait(t->done,
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)waitMs * (int64_t)NSEC_PER_MSEC)) == 0) {
      /* Completed. */
      if (t->error == nil) {
        return WBN_RUN_OK;
      }
      if (t->overLimit) {
        if (reasonOut) *reasonOut = @"response exceeds its byte ceiling";
      } else if (t->writeFailed) {
        if (reasonOut) *reasonOut = @"failed writing received data";
      } else if (reasonOut) {
        *reasonOut = t->error.localizedDescription;
      }
      return WBN_RUN_ERROR;
    }
    if (cancel_flag != NULL && *cancel_flag) {
      cancelled = true;
      break;
    }
    if (connectMs > 0 &&
        (long)(SDL_GetTicks() - startMs) >= connectMs &&
        task.countOfBytesSent == 0 && !atomic_load(&t->gotResponse)) {
      timedOut = true;
      break;
    }
  }

  /* Stopped early: cancel and wait for the completion callback. */
  [task cancel];
  dispatch_semaphore_wait(t->done, DISPATCH_TIME_FOREVER);
  if (t->error == nil) {
    /* It finished before the cancel reached it. */
    return WBN_RUN_OK;
  }
  if (cancelled) {
    return WBN_RUN_CANCELLED;
  }
  if (reasonOut) {
    *reasonOut = timedOut ? @"timed out" : t->error.localizedDescription;
  }
  return WBN_RUN_ERROR;
}

/* Percent-encodes every byte of url a URL may not carry as it is: a space,
 * a quote, a byte above 127 and the like. Letters, digits, the characters
 * RFC 3986 allows in a path or a query (including "/", "?", "&", "=" and
 * ":"), the URL delimiters "#", "[" and "]", and a "%" that already starts
 * a "%XX" are kept, so a path a caller
 * has encoded itself is not encoded twice. Returns nil when the result does
 * not parse. */
static NSURL *wbnUrlFromText(const char *url) {
  /* "#" (a fragment), "[" and "]" (an IPv6 host) are kept too, as libcurl
   * reads them. */
  static const char kKeep[] = "-._~!$&'()*+,;=:@/?#[]";
  NSMutableString *enc = [NSMutableString stringWithCapacity:strlen(url) + 16];
  for (const unsigned char *p = (const unsigned char *)url; *p != '\0'; p++) {
    unsigned char c = *p;
    bool keep = isalnum(c) || (c != '\0' && strchr(kKeep, c) != NULL);
    if (c == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
      keep = true;
    }
    if (keep) {
      [enc appendFormat:@"%c", (char)c];
    } else {
      [enc appendFormat:@"%%%02X", (unsigned)c];
    }
  }
  return [NSURL URLWithString:enc];
}

/* Builds <baseUrl>/api/v1/<path> as an NSURL, or nil when it does not
 * parse. urlOut keeps the text as given, for the log. */
static NSURL *wbnApiUrl(const char *path, char *urlOut, size_t urlOutSize) {
  snprintf(urlOut, urlOutSize, "%s/api/v1/%s", wbnBaseUrl, path);
  return wbnUrlFromText(urlOut);
}

/* Starts the module when nothing has yet. See the file header. The flag is
 * read once; when httpCreate runs, its return value decides, because an
 * httpDestroy on another thread may clear the flag again before a second
 * read. */
static bool wbnEnsureStarted(void) {
  bool started = atomic_load(&httpStarted);
  if (started && atomic_load_explicit(&s_baseUrlReady, memory_order_acquire)) {
    return true;
  }
  return httpCreate();
}

/*********************************************************
*NAME:          httpSetHostOverride
*********************************************************/
void httpSetHostOverride(const char *host) {
  strncpy(wbnHostOverride, host, FILENAME_MAX - 1);
  wbnHostOverride[FILENAME_MAX - 1] = '\0';
}

/*********************************************************
*NAME:          httpCreate
*PURPOSE:
* Reads the WBN host as http.c does and marks the module
* started. There is no global state to set up: the session
* is made on first request.
*********************************************************/
bool httpCreate(void) {
  char iniValue[FILENAME_MAX];

  @synchronized ([WbnHttpTask class]) {
    /* The host is read once and kept (see the file header): the base URL
     * is then never written again, so it can be read without the lock. */
    if (!atomic_load_explicit(&s_baseUrlReady, memory_order_acquire)) {
      if (wbnHostOverride[0] != '\0') {
        strncpy(iniValue, wbnHostOverride, sizeof(iniValue) - 1);
        iniValue[sizeof(iniValue) - 1] = '\0';
      } else {
        iniValue[0] = '\0';
        prefsGetString("WINBOLO.NET", "Host", "wbn.winbolo.net",
                       iniValue, (unsigned int)sizeof(iniValue));
        prefsSetString("WINBOLO.NET", "Host", iniValue);
      }
      buildBaseUrl(iniValue);
      atomic_store_explicit(&s_baseUrlReady, true, memory_order_release);
    }
    atomic_store(&httpStarted, true);
  }
  /* TRUE on success, as http.h says; the caller goes by this rather than
   * reading httpStarted again. */
  return true;
}

/*********************************************************
*NAME:          httpDestroy
*PURPOSE:
* Marks the module stopped. The session and its pooled
* connections stay for the next httpCreate.
*********************************************************/
void httpDestroy(void) {
  atomic_store(&httpStarted, false);
}

/*********************************************************
*NAME:          httpWorkerPoolBegin / httpWorkerPoolEnd
*PURPOSE:
* Record and clear the WinBolo.net worker thread, which the
* slow-call log line names. Connection reuse needs nothing
* more here: the session pools connections for every thread.
*********************************************************/
void httpWorkerPoolBegin(void) {
  s_workerThread = SDL_GetCurrentThreadID();
}

void httpWorkerPoolEnd(void) {
  s_workerThread = 0;
}

/*********************************************************
*NAME:          wbn_api_post_impl
*PURPOSE:
* Shared signed JSON POST. bearer is attached as
* Authorization: Bearer when non-NULL.
*********************************************************/
static int wbn_api_post_impl(const char *endpoint, const char *json_body,
                             const char *bearer, char **response_out) {
  if (response_out) *response_out = NULL;
  if (!wbnEnsureStarted()) return -1;

  @autoreleasepool {
    char url[FILENAME_MAX + 64];
    NSURL *nsurl = wbnApiUrl(endpoint, url, sizeof(url));
    if (nsurl == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_post [%s]: malformed URL", endpoint);
      return -1;
    }

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
    req.HTTPMethod = @"POST";
    req.HTTPBody = [NSData dataWithBytes:json_body length:strlen(json_body)];
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    wbnAddSignatureHeaders(req, json_body);
    if (bearer != NULL) {
      wbnAddBearerHeader(req, bearer);
    }

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    if (![t initBuffer]) return -1;

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_post: POST %s", url);
    wbnLogRedactedJsonDebug(WB_LOG_CAT_NET, "wbn_api_post: body=", json_body);

    NSString *reason = nil;
    Uint64 performStartMs = SDL_GetTicks();
    WbnRunResult res = wbnRunRequest(req, t, WBN_POST_TIMEOUT_MS,
                                     WBN_POST_CONNECT_TIMEOUT_MS, NULL, &reason);
    wbnLogSlowCall(endpoint, performStartMs);

    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_post [%s]: request error: %s",
                  endpoint, reason ? reason.UTF8String : "unknown");
      return -1;
    }

    {
      char respPrefix[FILENAME_MAX + 64];
      snprintf(respPrefix, sizeof(respPrefix), "wbn_api_post [%s]: HTTP %ld, response=",
               endpoint, t->status);
      wbnLogRedactedJsonDebug(WB_LOG_CAT_NET, respPrefix, t->buf);
    }

    if (response_out) {
      *response_out = [t takeBuffer];
    }
    return (int)t->status;
  }
}

int wbn_api_post(const char *endpoint, const char *json_body, char **response_out) {
  return wbn_api_post_impl(endpoint, json_body, NULL, response_out);
}

/*********************************************************
*NAME:          wbn_api_post_server
*PURPOSE:
* Server-scoped POST. Attaches the stored server bearer and
* refuses to send (returns -1) when none is set.
*********************************************************/
int wbn_api_post_server(const char *endpoint, const char *json_body, char **response_out) {
  if (response_out) *response_out = NULL;

  char token[WBN_SERVER_TOKEN_LEN];
  winboloNetGetServerToken(token, sizeof(token));
  if (token[0] == '\0') {
    fprintf(stderr, "WBN bearer token missing for endpoint %s\n", endpoint);
    return -1;
  }
  return wbn_api_post_impl(endpoint, json_body, token, response_out);
}

/* Parses resp_str into *response and frees it, as the three call variants
 * in http.c do. */
static void wbnParseCallResponse(const char *endpoint, char *resp_str, cJSON **response) {
  if (resp_str && response) {
    *response = cJSON_Parse(resp_str);
    if (!*response) {
      fprintf(stderr, "WinBolo.net: failed to parse JSON response from %s\n", endpoint);
    }
  }
  free(resp_str);
}

int wbn_api_call(const char *endpoint, cJSON *body, cJSON **response) {
  if (response) *response = NULL;

  char *json_str = cJSON_PrintUnformatted(body);
  if (!json_str) return -1;

  char *resp_str = NULL;
  int status = wbn_api_post(endpoint, json_str, &resp_str);
  free(json_str);

  wbnParseCallResponse(endpoint, resp_str, response);
  return status;
}

int wbn_api_call_server(const char *endpoint, cJSON *body, cJSON **response) {
  if (response) *response = NULL;

  char *json_str = cJSON_PrintUnformatted(body);
  if (!json_str) return -1;

  char *resp_str = NULL;
  int status = wbn_api_post_server(endpoint, json_str, &resp_str);
  free(json_str);

  wbnParseCallResponse(endpoint, resp_str, response);
  return status;
}

int wbn_api_call_bearer(const char *endpoint, cJSON *body, const char *bearerToken, cJSON **response) {
  if (response) *response = NULL;

  char *json_str = cJSON_PrintUnformatted(body);
  if (!json_str) return -1;

  const char *bearer = NULL;
  if (bearerToken != NULL && bearerToken[0] != '\0') {
    bearer = bearerToken;
  }

  char *resp_str = NULL;
  int status = wbn_api_post_impl(endpoint, json_str, bearer, &resp_str);
  free(json_str);

  wbnParseCallResponse(endpoint, resp_str, response);
  return status;
}

/*********************************************************
*NAME:          httpSetLogUploadTimeout
*********************************************************/
void httpSetLogUploadTimeout(long seconds) {
  s_logUploadTimeoutOverride = seconds;
}

/*********************************************************
*NAME:          httpSetDeadline / httpClearDeadline
*PURPOSE:
* See http.h. wbnRunRequest checks the deadline before it
* sends and on every slice while it waits.
*********************************************************/
void httpSetDeadline(long msFromNow) {
  Uint64 now = SDL_GetTicks();
  SDL_LockSpinlock(&s_deadlineLock);
  /* At least 1 so a deadline already passed is not read as none. */
  s_deadlineMs = msFromNow > 0 ? now + (Uint64)msFromNow : (now > 0 ? now : 1);
  SDL_UnlockSpinlock(&s_deadlineLock);
}

void httpClearDeadline(void) {
  SDL_LockSpinlock(&s_deadlineLock);
  s_deadlineMs = 0;
  SDL_UnlockSpinlock(&s_deadlineLock);
}

/*********************************************************
*NAME:          httpSendLogFile
*PURPOSE:
* Uploads a log file to <baseUrl>/log.php?key=<key> as a
* multipart/form-data POST with one part: name "logfile",
* filename "log.dat", type application/octet-stream, the
* same form libcurl's mime API sends in http.c. Attaches the
* server bearer when one is set and sends without it when
* not. Returns TRUE when the server answered, whatever the
* status, as http.c does.
*********************************************************/
bool httpSendLogFile(const char *fileName, char *key, bool wantFeedback) {
  (void)wantFeedback;

  if (fileName == NULL || key == NULL || !wbnEnsureStarted()) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: skipped (httpStarted=%d, fileName=%s, key=%s)",
            (int)atomic_load(&httpStarted), fileName ? fileName : "NULL", key ? "(set)" : "NULL");
    return false;
  }

  @autoreleasepool {
    char url[FILENAME_MAX + 64];
    snprintf(url, sizeof(url), "%s/log.php?key=%s", wbnBaseUrl, key);

    /* Don't log the URL verbatim — the ?key= query is an upload secret. */
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: POST %s/log.php?key=***", wbnBaseUrl);
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: file=%s", fileName);

    NSURL *nsurl = wbnUrlFromText(url);
    if (nsurl == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "httpSendLogFile: malformed URL");
      return false;
    }

    /* libcurl reads the file during the transfer and fails it when the file
     * cannot be read; here it is read first, with the same outcome. */
    NSString *path = [NSString stringWithUTF8String:fileName];
    NSError *readError = nil;
    NSData *fileData = path ? [NSData dataWithContentsOfFile:path options:0
                                                       error:&readError]
                            : nil;
    if (fileData == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "httpSendLogFile: cannot open file '%s': %s",
                  fileName,
                  readError ? readError.localizedDescription.UTF8String
                            : "file name is not valid UTF-8");
      return false;
    }
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: file size=%ld bytes",
                 (long)fileData.length);

    NSString *boundary = [NSString stringWithFormat:@"------------------------%@",
                          [[NSUUID UUID].UUIDString stringByReplacingOccurrencesOfString:@"-"
                                                                              withString:@""]];
    NSMutableData *body = [NSMutableData data];
    NSString *head = [NSString stringWithFormat:
        @"--%@\r\n"
        @"Content-Disposition: form-data; name=\"logfile\"; filename=\"log.dat\"\r\n"
        @"Content-Type: application/octet-stream\r\n"
        @"\r\n", boundary];
    [body appendData:[head dataUsingEncoding:NSUTF8StringEncoding]];
    [body appendData:fileData];
    NSString *tail = [NSString stringWithFormat:@"\r\n--%@--\r\n", boundary];
    [body appendData:[tail dataUsingEncoding:NSUTF8StringEncoding]];

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
    req.HTTPMethod = @"POST";
    req.HTTPBody = body;
    [req setValue:[NSString stringWithFormat:@"multipart/form-data; boundary=%@", boundary]
        forHTTPHeaderField:@"Content-Type"];

    /* Log upload is permissive: with no server bearer it goes
     * unauthenticated, rather than refusing as wbn_api_post_server does. */
    char token[WBN_SERVER_TOKEN_LEN];
    winboloNetGetServerToken(token, sizeof(token));
    if (token[0] != '\0') {
      wbnAddBearerHeader(req, token);
    }

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    if (![t initBuffer]) return false;
    /* The reply is only logged; keep its first 1023 bytes, as http.c's
     * fixed 1024-byte buffer does, and let the rest go by. */
    t->keepMax = 1023;

    long timeoutS = s_logUploadTimeoutOverride > 0 ? s_logUploadTimeoutOverride
                                                   : WBN_UPLOAD_TIMEOUT_S;
    NSString *reason = nil;
    Uint64 performStartMs = SDL_GetTicks();
    WbnRunResult res = wbnRunRequest(req, t, timeoutS * 1000L,
                                     WBN_UPLOAD_CONNECT_TIMEOUT_MS, NULL, &reason);
    wbnLogSlowCall("log.php upload", performStartMs);

    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "httpSendLogFile: request error: %s",
                  reason ? reason.UTF8String : "unknown");
      return false;
    }

    {
      char respPrefix[64];
      snprintf(respPrefix, sizeof(respPrefix), "httpSendLogFile: HTTP %ld, response=", t->status);
      wbnLogRedactedJsonDebug(WB_LOG_CAT_NET, respPrefix, t->buf);
    }
    return true;
  }
}

/*********************************************************
*NAME:          httpGetBaseUrl
*********************************************************/
const char *httpGetBaseUrl(void) {
  /* Empty until the first httpCreate has filled it, as http.c's is; read
   * after the ready flag so a half-written URL is never returned. */
  if (!atomic_load_explicit(&s_baseUrlReady, memory_order_acquire)) {
    return "";
  }
  return wbnBaseUrl;
}

/*********************************************************
*NAME:          wbn_api_get_impl
*PURPOSE:
* Shared GET. sign adds the signature headers over an empty
* body; bearerToken, when non-empty, adds Authorization.
*********************************************************/
static int wbn_api_get_impl(const char *path, char **response_out,
                            volatile int *cancel_flag, bool sign,
                            const char *bearerToken, long timeout_ms,
                            long connect_timeout_ms) {
  if (response_out) *response_out = NULL;
  if (!wbnEnsureStarted()) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
        "wbn_api_get [%s]: SKIP — httpStarted=false after lazy httpCreate()",
        path ? path : "(null)");
    return -1;
  }
  if (cancel_flag && *cancel_flag) return -2;

  @autoreleasepool {
    char url[FILENAME_MAX + 256];
    NSURL *nsurl = wbnApiUrl(path, url, sizeof(url));
    if (nsurl == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_get [%s]: malformed URL", path);
      return -1;
    }

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
    req.HTTPMethod = @"GET";
    if (sign) {
      wbnAddSignatureHeaders(req, "");
    }
    if (bearerToken != NULL && bearerToken[0] != '\0') {
      wbnAddBearerHeader(req, bearerToken);
    }

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    if (![t initBuffer]) return -1;

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_get: GET %s", url);

    NSString *reason = nil;
    WbnRunResult res = wbnRunRequest(req, t,
                                     timeout_ms > 0 ? timeout_ms : WBN_API_GET_TIMEOUT_MS,
                                     connect_timeout_ms > 0 ? connect_timeout_ms : 0,
                                     cancel_flag, &reason);
    if (res == WBN_RUN_CANCELLED) {
      return -2;
    }
    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_get [%s]: request error: %s",
                  path, reason ? reason.UTF8String : "unknown");
      return -1;
    }

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_get [%s]: HTTP %ld", path, t->status);

    if (response_out) {
      *response_out = [t takeBuffer];
    }
    return (int)t->status;
  }
}

int wbn_api_get(const char *path, char **response_out) {
  return wbn_api_get_impl(path, response_out, NULL, true, NULL, 0L, 0L);
}

int wbn_api_get_timeout(const char *path, char **response_out,
                        long timeout_ms, long connect_timeout_ms) {
  return wbn_api_get_impl(path, response_out, NULL, true, NULL, timeout_ms,
                          connect_timeout_ms);
}

int wbn_api_get_cancellable(const char *path, char **response_out,
                            volatile int *cancel_flag) {
  return wbn_api_get_impl(path, response_out, cancel_flag, true, NULL, 0L, 0L);
}

int wbn_api_get_public(const char *path, char **response_out) {
  return wbn_api_get_impl(path, response_out, NULL, false, NULL, 0L, 0L);
}

int wbn_api_get_bearer_cancellable(const char *path, const char *bearerToken,
                                   char **response_out, volatile int *cancel_flag) {
  if (response_out) *response_out = NULL;
  if (bearerToken == NULL || bearerToken[0] == '\0') return -1;
  return wbn_api_get_impl(path, response_out, cancel_flag, true, bearerToken,
                          0L, 0L);
}

/*********************************************************
*NAME:          wbn_prefs_request_impl
*PURPOSE:
* Cloud-prefs GET (is_put false, signs an empty body) or
* PUT of json_body (signs json_body). Always sends the
* signature, Content-Type: application/json and the user's
* bearer; refuses (-1) when bearerToken is NULL/empty.
*********************************************************/
static int wbn_prefs_request_impl(const char *bearerToken, bool is_put,
                                  const char *json_body, char **response_out) {
  if (response_out) *response_out = NULL;
  if (bearerToken == NULL || bearerToken[0] == '\0') {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_prefs %s: aborted — empty bearer token",
                is_put ? "PUT" : "GET");
    return -1;
  }
  if (!wbnEnsureStarted()) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_prefs %s: aborted — http subsystem not started",
                is_put ? "PUT" : "GET");
    return -1;
  }

  const char *sign_body = is_put ? json_body : "";

  @autoreleasepool {
    char url[FILENAME_MAX + 64];
    NSURL *nsurl = wbnApiUrl("prefs", url, sizeof(url));
    if (nsurl == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_prefs: malformed URL");
      return -1;
    }

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
    if (is_put) {
      req.HTTPMethod = @"PUT";
      req.HTTPBody = [NSData dataWithBytes:json_body length:strlen(json_body)];
    } else {
      req.HTTPMethod = @"GET";
    }
    [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    wbnAddSignatureHeaders(req, sign_body);
    wbnAddBearerHeader(req, bearerToken);

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    if (![t initBuffer]) return -1;

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_prefs: %s %s", is_put ? "PUT" : "GET", url);

    NSString *reason = nil;
    Uint64 performStartMs = SDL_GetTicks();
    WbnRunResult res = wbnRunRequest(req, t, WBN_PREFS_TIMEOUT_MS, 0, NULL, &reason);
    if (is_put) {
      wbnLogSlowCall("prefs PUT", performStartMs);
    }

    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_prefs %s: request error: %s",
                  is_put ? "PUT" : "GET", reason ? reason.UTF8String : "unknown");
      return -1;
    }

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_prefs %s: HTTP %ld",
                 is_put ? "PUT" : "GET", t->status);

    if (response_out) {
      *response_out = [t takeBuffer];
    }
    return (int)t->status;
  }
}

int wbn_prefs_get(const char *bearerToken, char **response_out) {
  return wbn_prefs_request_impl(bearerToken, false, "", response_out);
}

int wbn_prefs_put(const char *bearerToken, const char *json_body,
                  char **response_out) {
  return wbn_prefs_request_impl(bearerToken, true, json_body, response_out);
}

/* A signed GET of <baseUrl>/api/v1/<path> for the download paths. */
static NSMutableURLRequest *wbnDownloadRequest(const char *path, char *urlOut,
                                               size_t urlOutSize) {
  NSURL *nsurl = wbnApiUrl(path, urlOut, urlOutSize);
  if (nsurl == nil) return nil;
  NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
  req.HTTPMethod = @"GET";
  wbnAddSignatureHeaders(req, "");
  return req;
}

/*********************************************************
*NAME:          wbn_api_download / wbn_api_download_progress
*PURPOSE:
* Downloads to dest_path. The file is removed on a transport
* error, on cancel (-2) and on any status other than 200.
*********************************************************/
int wbn_api_download(const char *path, const char *dest_path) {
  return wbn_api_download_progress(path, dest_path, NULL, NULL, NULL);
}

int wbn_api_download_progress(const char *path, const char *dest_path,
                              WbnProgressFn progressFn, void *progressUserData,
                              volatile int *cancel_flag) {
  if (!wbnEnsureStarted()) return -1;
  if (cancel_flag && *cancel_flag) return -2;

  FILE *fp = fopen(dest_path, "wb");
  if (!fp) return -1;

  @autoreleasepool {
    char url[FILENAME_MAX + 256];
    NSMutableURLRequest *req = wbnDownloadRequest(path, url, sizeof(url));
    if (req == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download: malformed URL");
      fclose(fp);
      remove(dest_path);
      return -1;
    }

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    t->fp = fp;
    t->progressFn = progressFn;
    t->progressUser = progressUserData;

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_download: GET %s -> %s", url, dest_path);

    NSString *reason = nil;
    WbnRunResult res = wbnRunRequest(req, t, WBN_DOWNLOAD_TIMEOUT_MS, 0,
                                     cancel_flag, &reason);
    t->fp = NULL;
    fclose(fp);

    if (res == WBN_RUN_CANCELLED) {
      remove(dest_path);
      return -2;
    }
    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download: request error: %s",
                  reason ? reason.UTF8String : "unknown");
      remove(dest_path);
      return -1;
    }

    if (t->status != 200) {
      remove(dest_path);
    }
    return (int)t->status;
  }
}

/*********************************************************
*NAME:          wbn_api_download_to_memory*
*PURPOSE:
* In-memory downloads. *data_out / *size_out are set only
* for a 200 answer; anything else leaves them NULL/0.
*********************************************************/
int wbn_api_download_to_memory_cancellable(const char *path,
                                           uint8_t **data_out,
                                           size_t *size_out,
                                           volatile int *cancel_flag) {
  return wbn_api_download_to_memory_progress(path, data_out, size_out, 0,
                                             NULL, NULL, cancel_flag);
}

int wbn_api_download_to_memory(const char *path, uint8_t **data_out, size_t *size_out) {
  return wbn_api_download_to_memory_progress(path, data_out, size_out, 0,
                                             NULL, NULL, NULL);
}

int wbn_api_download_to_memory_progress(const char *path,
                                        uint8_t **data_out, size_t *size_out,
                                        size_t maxBytes,
                                        WbnProgressFn progressFn,
                                        void *progressUserData,
                                        volatile int *cancel_flag) {
  if (data_out) *data_out = NULL;
  if (size_out) *size_out = 0;
  if (!wbnEnsureStarted()) return -1;
  if (cancel_flag && *cancel_flag) return -2;

  @autoreleasepool {
    char url[FILENAME_MAX + 256];
    NSMutableURLRequest *req = wbnDownloadRequest(path, url, sizeof(url));
    if (req == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download_to_memory: malformed URL");
      return -1;
    }

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    if (![t initBuffer]) return -1;
    t->limit = maxBytes;
    t->progressFn = progressFn;
    t->progressUser = progressUserData;

    WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_download_to_memory: GET %s", url);

    NSString *reason = nil;
    WbnRunResult res = wbnRunRequest(req, t, WBN_DOWNLOAD_TIMEOUT_MS, 0,
                                     cancel_flag, &reason);
    if (res == WBN_RUN_CANCELLED) {
      return -2;
    }
    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download_to_memory: request error: %s",
                  reason ? reason.UTF8String : "unknown");
      return -1;
    }

    if (t->status == 200 && data_out && size_out) {
      size_t got = t->size;
      *data_out = (uint8_t *)[t takeBuffer];
      *size_out = got;
    }
    return (int)t->status;
  }
}

/*********************************************************
*NAME:          httpGetUrlToMemory
*PURPOSE:
* Unsigned GET of a full URL into memory. See http.h.
*********************************************************/
int httpGetUrlToMemory(const char *url, uint8_t **data_out, size_t *size_out,
                       size_t maxBytes, long timeout_ms) {
  if (data_out) *data_out = NULL;
  if (size_out) *size_out = 0;
  if (url == NULL) return -1;

  @autoreleasepool {
    NSURL *nsurl = wbnUrlFromText(url);
    if (nsurl == nil) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "httpGetUrlToMemory: malformed URL");
      return -1;
    }
    /* The URL comes from the news JSON, so only https is fetched. ATS would
     * refuse http anyway; this also keeps file: and data: URLs out. */
    if (nsurl.scheme == nil ||
        [nsurl.scheme caseInsensitiveCompare:@"https"] != NSOrderedSame) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "httpGetUrlToMemory: refused, scheme is not https");
      return -1;
    }

    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsurl];
    req.HTTPMethod = @"GET";

    WbnHttpTask *t = [[WbnHttpTask alloc] init];
    if (![t initBuffer]) return -1;
    t->limit = maxBytes;

    NSString *reason = nil;
    WbnRunResult res = wbnRunRequest(req, t,
                                     timeout_ms > 0 ? timeout_ms : WBN_API_GET_TIMEOUT_MS,
                                     0, NULL, &reason);
    if (res != WBN_RUN_OK) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "httpGetUrlToMemory: request error: %s",
                  reason ? reason.UTF8String : "unknown");
      return -1;
    }

    if (t->status == 200 && data_out && size_out) {
      size_t got = t->size;
      *data_out = (uint8_t *)[t takeBuffer];
      *size_out = got;
    }
    return (int)t->status;
  }
}

/*********************************************************
*NAME:          httpSetAltIpAddress
*PURPOSE:
* Stores the alternate local address. NSURLSession cannot
* bind to one, so the first request after it is set logs a
* warning and goes out on the default route.
*********************************************************/
void httpSetAltIpAddress(char *iptoset) {
  strncpy(altIpAddress, iptoset, sizeof(altIpAddress) - 1);
  altIpAddress[sizeof(altIpAddress) - 1] = '\0';
  s_altIpWarned = false;
}
