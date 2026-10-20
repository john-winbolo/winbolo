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
*Name:          http (JNI)
*Filename:      http_jni.c
*Author:        John Morrison
*Creation Date: 21/10/26
*Last Modified: 21/10/26
*Purpose:
*  The http.h interface for Android, which has no libcurl.
*  Every function behaves as its http.c counterpart does
*  for the caller: the same URLs, headers, Ed25519
*  signature, bearer rules, return values (HTTP status, -1
*  on a transport error, -2 on cancel), time limits,
*  response ceiling, progress reports and the multipart
*  log upload.
*
*  The transfer itself is Java's HttpURLConnection, called
*  through JNI in WinBoloHttp.java (the driver's package).
*  One Java call runs one request on a pooled Java thread
*  and blocks the calling thread until it is over; the
*  response comes back as it arrives through four native
*  methods registered here, so the buffer, the file, the
*  byte ceiling and the progress reports all stay in C.
*  The requesting thread is asked every 50 ms whether to
*  go on, which is where the cancel flag and the shutdown
*  deadline stop a request that is waiting on the network.
*  SDL attaches the calling thread to the Java VM
*  (SDL_GetAndroidJNIEnv), so the WinBolo.net worker and
*  the httpWorkerPool threads need nothing of their own.
*
*  Differences from http.c, all forced by HttpURLConnection:
*  - httpSetAltIpAddress stores the address but cannot bind
*    to it: HttpURLConnection has no way to choose the
*    local interface.
*  - httpWorkerPoolBegin/End only record the worker thread
*    for the slow-call log line. The Java side keeps its
*    own pool of connections, shared by every thread.
*  - Every request path starts the module on first use, as
*    http.c's GET and prefs paths do, because the sign-in
*    calls run httpDestroy when they finish and the next
*    request may come from any thread. The host is read
*    from the preferences once, at the first httpCreate,
*    and kept: prefs.c has no lock, so a later httpCreate
*    on a worker thread must not read it again. A Host
*    changed in the preferences while the app runs takes
*    effect at the next launch.
*  - Paths and queries are percent-encoded before they are
*    sent, as http_nsurl.m does, so a space or a byte above
*    127 in a path does not fail the request. A "%XX"
*    already in the text, and the URL delimiters "#", "["
*    and "]", are kept.
*  - A redirect to another scheme, host or port drops the
*    Authorization header, as libcurl does by default. Such
*    a redirect of a request with a body (a POST or PUT) is
*    refused, so the body is never sent to the new host.
*  - The connect limit covers name lookup, connect and the
*    TLS handshake together, as libcurl's does: the Java
*    side's own connect limit bounds the TCP connect alone,
*    so the requesting thread also ends a request whose
*    connection is not up when the limit runs out.
*  - A header value with a byte outside 0x20..0x7e is
*    refused before anything is sent, and the log names
*    the header, never its value: the Java side would
*    otherwise throw with the whole value in its message.
*  - Only https is sent. The app's manifest allows no
*    cleartext traffic, so a Host= of http:// fails at the
*    first request rather than going out in the clear.
*********************************************************/

/* Same rule as http.c: this file embeds the WBN signing key and must never
 * reach the browser build. */
#ifdef __EMSCRIPTEN__
#error "http_jni.c (embeds WBN_SIGNING_KEY) must never be compiled for the WASM build"
#endif

#include <jni.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <stdatomic.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_stdinc.h>
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
#include "http_jni.h"
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

/* See http.c: a call off the worker that runs longer than 100 ms is
 * reported, and one on the worker only past 1000 ms. */
#define WBN_SLOW_CALL_WARN_MS_OFF_WORKER 100u
#define WBN_SLOW_CALL_WARN_MS_ON_WORKER  1000u

/* The Java class and its request method, bound once (see wbnJniClass). */
#define WBN_JAVA_CLASS      "com/winbolo/android/WinBoloHttp"
#define WBN_JAVA_CLASS_DOT  "com.winbolo.android.WinBoloHttp"
#define WBN_JAVA_REQUEST_SIG "(JLjava/lang/String;Ljava/lang/String;[Ljava/lang/String;[BII)I"

/* The RESULT_ values WinBoloHttp.request returns in place of a status. */
#define WBN_JAVA_RESULT_TRANSPORT_ERROR (-1)
#define WBN_JAVA_RESULT_STOPPED         (-2)
#define WBN_JAVA_RESULT_TIMED_OUT       (-3)

/* What nativePoll answers. */
#define WBN_POLL_CONTINUE      0
#define WBN_POLL_CANCEL        1
#define WBN_POLL_PAST_DEADLINE 2

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

/*********************************************************
*NAME:          WbnHeaders
*PURPOSE:
* The request headers, as name and value pairs, in the
* order they are sent. Enough room for Content-Type, the
* two signature headers and Authorization.
*********************************************************/
#define WBN_HEADERS_MAX 4

typedef struct {
  const char *name[WBN_HEADERS_MAX];
  char        value[WBN_HEADERS_MAX][256 + WBN_SERVER_TOKEN_LEN];
  int         count;
  /* Set when a value was refused; wbnRunRequest then sends nothing. */
  bool        refused;
} WbnHeaders;

static void wbnHeadersInit(WbnHeaders *h) {
  h->count = 0;
  h->refused = false;
}

/* Adds a header. A value with a byte outside 0x20..0x7e (a control
 * character, a byte above 127, which a bearer token read from a damaged
 * preferences file could carry) is refused: the request is then not sent,
 * and the log names the header only, never the value. Returns false for a
 * refused value. */
static bool wbnHeaderAdd(WbnHeaders *h, const char *name, const char *value) {
  for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
    if (*p < 0x20 || *p > 0x7e) {
      WB_LOG_WARN(WB_LOG_CAT_NET,
                  "WinBolo.net: header %s refused, its value has a byte a header may not carry",
                  name);
      h->refused = true;
      return false;
    }
  }
  if (h->count >= WBN_HEADERS_MAX) {
    h->refused = true;
    return false;
  }
  h->name[h->count] = name;
  snprintf(h->value[h->count], sizeof(h->value[h->count]), "%s", value);
  h->count++;
  return true;
}

/* Adds X-WBN-Timestamp and X-WBN-Signature over body. */
static void wbnAddSignatureHeaders(WbnHeaders *h, const char *body) {
  char timestamp_str[32];
  snprintf(timestamp_str, sizeof(timestamp_str), "%ld", (long)time(NULL));

  char sig_hex[129];
  wbn_sign_request(timestamp_str, body, sig_hex);

  wbnHeaderAdd(h, "X-WBN-Signature", sig_hex);
  wbnHeaderAdd(h, "X-WBN-Timestamp", timestamp_str);
}

/* Adds Authorization: Bearer <token>. */
static void wbnAddBearerHeader(WbnHeaders *h, const char *token) {
  char value[32 + WBN_SERVER_TOKEN_LEN + 256];
  snprintf(value, sizeof(value), "Bearer %s", token);
  wbnHeaderAdd(h, "Authorization", value);
}

/*********************************************************
*NAME:          WbnJniTask
*PURPOSE:
* The per-request state the Java side reports into.
* Collects the response into a growing buffer or a file,
* applies the response ceiling, forwards progress, and
* records why a transfer was stopped. Its fields are
* written by the Java transfer thread through the native
* methods below, and read by the requesting thread only
* once WinBoloHttp.request has returned, after which no
* native method can run for it.
*********************************************************/
typedef struct {
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
  volatile int *cancelFlag;

  bool    overLimit;
  bool    writeFailed;
  bool    cancelled;
  bool    pastDeadline;
  int64_t received;
  int64_t expected;  /* 0 when the server sent no Content-Length */
  long    status;
  char    error[256];
} WbnJniTask;

static void wbnTaskInit(WbnJniTask *t) {
  memset(t, 0, sizeof(*t));
}

static bool wbnTaskInitBuffer(WbnJniTask *t) {
  t->useBuf = true;
  t->capacity = 1024;
  t->size = 0;
  t->buf = malloc(t->capacity);
  if (t->buf == NULL) return false;
  t->buf[0] = '\0';
  return true;
}

/* Hands the buffer to the caller, who frees it. */
static char *wbnTaskTakeBuffer(WbnJniTask *t) {
  char *out = t->buf;
  t->buf = NULL;
  t->size = 0;
  t->capacity = 0;
  return out;
}

static void wbnTaskFree(WbnJniTask *t) {
  free(t->buf);
  t->buf = NULL;
}

/* Appends n bytes read from data[offset..] to the task's buffer or file.
 * Returns false when the transfer must stop. */
static bool wbnTaskAppend(JNIEnv *env, WbnJniTask *t, jbyteArray data,
                          jint length) {
  if (t->overLimit || t->writeFailed) return false;

  size_t incoming = (size_t)length;
  /* Refused as it arrives rather than checked at the end, as http.c does:
   * a response meant to exhaust this client's memory never finishes. */
  if (t->useBuf && t->limit != 0 && t->size + incoming > t->limit) {
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "response exceeds its %zu byte ceiling — transfer stopped",
                t->limit);
    t->overLimit = true;
    return false;
  }

  size_t keep = incoming;
  if (t->useBuf && t->keepMax != 0) {
    /* Keep the first keepMax bytes and let the rest go by. */
    size_t room = (t->size < t->keepMax) ? t->keepMax - t->size : 0;
    if (keep > room) keep = room;
  }

  if (t->useBuf && keep > 0) {
    size_t needed = t->size + keep + 1;
    if (needed > t->capacity) {
      size_t newcap = t->capacity * 2;
      if (newcap < needed) newcap = needed;
      char *tmp = realloc(t->buf, newcap);
      if (tmp == NULL) {
        t->writeFailed = true;
        return false;
      }
      t->buf = tmp;
      t->capacity = newcap;
    }
    (*env)->GetByteArrayRegion(env, data, 0, (jsize)keep, (jbyte *)(t->buf + t->size));
    t->size += keep;
    t->buf[t->size] = '\0';
  } else if (t->fp != NULL) {
    jbyte slice[4096];
    jsize done = 0;
    while (done < length) {
      jsize n = length - done;
      if (n > (jsize)sizeof(slice)) n = (jsize)sizeof(slice);
      (*env)->GetByteArrayRegion(env, data, done, n, slice);
      if (fwrite(slice, 1, (size_t)n, t->fp) != (size_t)n) {
        t->writeFailed = true;
        return false;
      }
      done += n;
    }
  }

  t->received += (int64_t)incoming;
  if (t->progressFn) {
    t->progressFn(t->progressUser, t->received, t->expected);
  }
  return true;
}

/*********************************************************
*NAME:          The native methods of WinBoloHttp
*PURPOSE:
* Called by the Java side with the task's address. Begin
* reports the status and the advertised length once the
* headers are in; chunk delivers body bytes and answers
* whether to go on; poll, on the requesting thread every
* 50 ms, answers the same for a request still waiting;
* error carries the reason a request failed.
*********************************************************/
static void JNICALL wbnNativeBegin(JNIEnv *env, jclass cls, jlong ctx,
                                   jint status, jlong expected) {
  (void)env; (void)cls;
  WbnJniTask *t = (WbnJniTask *)(intptr_t)ctx;
  t->status = (long)status;
  t->expected = expected > 0 ? (int64_t)expected : 0;
  if (t->progressFn) {
    t->progressFn(t->progressUser, t->received, t->expected);
  }
}

static jboolean JNICALL wbnNativeChunk(JNIEnv *env, jclass cls, jlong ctx,
                                       jbyteArray data, jint length) {
  (void)cls;
  WbnJniTask *t = (WbnJniTask *)(intptr_t)ctx;
  if (t->cancelFlag != NULL && *t->cancelFlag) {
    t->cancelled = true;
    return JNI_FALSE;
  }
  return wbnTaskAppend(env, t, data, length) ? JNI_TRUE : JNI_FALSE;
}

static jint JNICALL wbnNativePoll(JNIEnv *env, jclass cls, jlong ctx) {
  (void)env; (void)cls;
  WbnJniTask *t = (WbnJniTask *)(intptr_t)ctx;
  if (t->cancelFlag != NULL && *t->cancelFlag) {
    t->cancelled = true;
    return WBN_POLL_CANCEL;
  }
  /* A deadline set while this request waits stops it within one poll. */
  Uint64 deadline = wbnDeadlineGet();
  if (deadline != 0 && SDL_GetTicks() >= deadline) {
    t->pastDeadline = true;
    return WBN_POLL_PAST_DEADLINE;
  }
  return WBN_POLL_CONTINUE;
}

static void JNICALL wbnNativeError(JNIEnv *env, jclass cls, jlong ctx,
                                   jstring reason) {
  (void)cls;
  WbnJniTask *t = (WbnJniTask *)(intptr_t)ctx;
  if (reason == NULL) return;
  const char *text = (*env)->GetStringUTFChars(env, reason, NULL);
  if (text != NULL) {
    snprintf(t->error, sizeof(t->error), "%s", text);
    (*env)->ReleaseStringUTFChars(env, reason, text);
  }
}

static const JNINativeMethod s_nativeMethods[] = {
  { "nativeBegin", "(JIJ)V",   (void *)wbnNativeBegin },
  { "nativeChunk", "(J[BI)Z",  (void *)wbnNativeChunk },
  { "nativePoll",  "(J)I",     (void *)wbnNativePoll  },
  { "nativeError", "(JLjava/lang/String;)V", (void *)wbnNativeError },
};

/*********************************************************
*NAME:          wbnJniClass / httpAndroidInit
*PURPOSE:
* The WinBoloHttp class as a global reference, with its
* request method and its native methods bound, resolved
* once. FindClass finds app classes only on a thread the
* Java side created (the main thread is one), so the
* driver calls httpAndroidInit there; a request that
* comes first resolves the class through the activity's
* class loader, which works on any attached thread.
*********************************************************/
static SDL_SpinLock s_classLock = 0;
static jclass       s_httpClass = NULL;   /* global reference */
static jmethodID    s_midRequest = NULL;

/* Drops a pending Java exception, logging it, and says whether there was one. */
static bool wbnJniClearException(JNIEnv *env, const char *what) {
  if ((*env)->ExceptionCheck(env)) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "WinBolo.net JNI: Java exception in %s", what);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
  }
  return false;
}

/* The class through the activity's class loader. Local reference or NULL. */
static jclass wbnJniLoadClassViaActivity(JNIEnv *env) {
  jobject activity = (jobject)SDL_GetAndroidActivity();
  if (activity == NULL) return NULL;
  jclass activityClass = (*env)->GetObjectClass(env, activity);
  jmethodID midGetLoader = (*env)->GetMethodID(env, activityClass, "getClassLoader",
                                               "()Ljava/lang/ClassLoader;");
  if (midGetLoader == NULL) {
    wbnJniClearException(env, "getClassLoader lookup");
    return NULL;
  }
  jobject loader = (*env)->CallObjectMethod(env, activity, midGetLoader);
  if (wbnJniClearException(env, "getClassLoader") || loader == NULL) return NULL;
  jclass loaderClass = (*env)->GetObjectClass(env, loader);
  jmethodID midLoad = (*env)->GetMethodID(env, loaderClass, "loadClass",
                                          "(Ljava/lang/String;)Ljava/lang/Class;");
  if (midLoad == NULL) {
    wbnJniClearException(env, "loadClass lookup");
    return NULL;
  }
  jstring name = (*env)->NewStringUTF(env, WBN_JAVA_CLASS_DOT);
  jclass cls = (jclass)(*env)->CallObjectMethod(env, loader, midLoad, name);
  if (wbnJniClearException(env, "loadClass")) return NULL;
  return cls;
}

static bool wbnJniBind(JNIEnv *env) {
  bool ok = false;
  if ((*env)->PushLocalFrame(env, 16) < 0) return false;

  jclass local = (*env)->FindClass(env, WBN_JAVA_CLASS);
  if (local == NULL) {
    (*env)->ExceptionClear(env);
    local = wbnJniLoadClassViaActivity(env);
  }
  if (local == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "WinBolo.net JNI: class %s not found", WBN_JAVA_CLASS_DOT);
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }

  jmethodID mid = (*env)->GetStaticMethodID(env, local, "request", WBN_JAVA_REQUEST_SIG);
  if (mid == NULL) {
    wbnJniClearException(env, "request lookup");
  } else if ((*env)->RegisterNatives(env, local, s_nativeMethods,
                                     (jint)SDL_arraysize(s_nativeMethods)) != JNI_OK) {
    wbnJniClearException(env, "RegisterNatives");
  } else {
    s_httpClass = (jclass)(*env)->NewGlobalRef(env, local);
    s_midRequest = mid;
    ok = (s_httpClass != NULL);
  }
  (*env)->PopLocalFrame(env, NULL);
  return ok;
}

static jclass wbnJniClass(JNIEnv *env) {
  jclass cls;
  SDL_LockSpinlock(&s_classLock);
  if (s_httpClass == NULL) {
    (void)wbnJniBind(env);
  }
  cls = s_httpClass;
  SDL_UnlockSpinlock(&s_classLock);
  return cls;
}

bool httpAndroidInit(void) {
  JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_NET, "WinBolo.net JNI: no JNI environment: %s", SDL_GetError());
    return false;
  }
  return wbnJniClass(env) != NULL;
}

/* Outcome of wbnRunRequest beyond the HTTP status. */
typedef enum {
  WBN_RUN_OK = 0,      /* the server answered; status holds its code */
  WBN_RUN_ERROR,       /* transport error, time limit, ceiling or write failure */
  WBN_RUN_CANCELLED    /* the caller's cancel flag stopped it */
} WbnRunResult;

/*********************************************************
*NAME:          wbnRunRequest
*PURPOSE:
* Runs one request through WinBoloHttp.request on the
* calling thread and waits for it. Ends it early when the
* total limit runs out, when the connect limit runs out,
* when *cancelFlag becomes non-zero, or at the shutdown
* deadline. Always returns after the Java side has
* finished with t, so no native method can touch it
* afterwards.
*
*ARGUMENTS:
* method    - GET, POST or PUT
* url       - The full URL, as text
* headers   - The request headers
* body      - The request body, or NULL for none
* bodyLen   - Its length in bytes
* t         - The task, buffer or file already set
* totalMs   - Total time limit in milliseconds
* connectMs - Connect limit in milliseconds (name lookup,
*             connect and handshake together), or 0 for none
* reasonOut - Receives a description of an error
*********************************************************/
static WbnRunResult wbnRunRequest(const char *method, const char *url,
                                  const WbnHeaders *headers,
                                  const void *body, size_t bodyLen,
                                  WbnJniTask *t, long totalMs, long connectMs,
                                  const char **reasonOut) {
  if (altIpAddress[0] != '\0' && !s_altIpWarned) {
    s_altIpWarned = true;
    WB_LOG_WARN(WB_LOG_CAT_NET,
                "WinBolo.net: alternate local address '%s' is not supported "
                "on this platform; using the default route", altIpAddress);
  }

  /* A refused header value (see wbnHeaderAdd): nothing goes out. */
  if (headers->refused) {
    if (reasonOut) *reasonOut = "a header value was refused";
    return WBN_RUN_ERROR;
  }

  /* Past the deadline (httpSetDeadline) nothing is sent; before it, the
   * total is cut to the time left. */
  Uint64 deadline = wbnDeadlineGet();
  if (deadline != 0) {
    Uint64 now = SDL_GetTicks();
    if (now >= deadline) {
      if (reasonOut) *reasonOut = "past the shutdown deadline";
      return WBN_RUN_ERROR;
    }
    if ((Uint64)totalMs > deadline - now) {
      totalMs = (long)(deadline - now);
    }
  }
  if (totalMs < 1) totalMs = 1;
  if (totalMs > INT32_MAX) totalMs = INT32_MAX;
  if (connectMs > totalMs) connectMs = totalMs;

  JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) {
    if (reasonOut) *reasonOut = "no JNI environment for this thread";
    return WBN_RUN_ERROR;
  }
  jclass cls = wbnJniClass(env);
  if (cls == NULL) {
    if (reasonOut) *reasonOut = "WinBoloHttp class not bound";
    return WBN_RUN_ERROR;
  }
  if ((*env)->PushLocalFrame(env, 16 + 2 * WBN_HEADERS_MAX) < 0) {
    (*env)->ExceptionClear(env);
    if (reasonOut) *reasonOut = "out of JNI local references";
    return WBN_RUN_ERROR;
  }

  jstring jMethod = (*env)->NewStringUTF(env, method);
  jstring jUrl = (*env)->NewStringUTF(env, url);
  jclass stringClass = (*env)->FindClass(env, "java/lang/String");
  jobjectArray jHeaders = (*env)->NewObjectArray(env, (jsize)(headers->count * 2),
                                                 stringClass, NULL);
  jbyteArray jBody = NULL;
  bool built = (jMethod != NULL && jUrl != NULL && jHeaders != NULL);
  for (int i = 0; built && i < headers->count; i++) {
    jstring name = (*env)->NewStringUTF(env, headers->name[i]);
    jstring value = (*env)->NewStringUTF(env, headers->value[i]);
    if (name == NULL || value == NULL) {
      built = false;
      break;
    }
    (*env)->SetObjectArrayElement(env, jHeaders, (jsize)(2 * i), name);
    (*env)->SetObjectArrayElement(env, jHeaders, (jsize)(2 * i + 1), value);
    (*env)->DeleteLocalRef(env, name);
    (*env)->DeleteLocalRef(env, value);
  }
  if (built && body != NULL) {
    if (bodyLen > (size_t)INT32_MAX) {
      built = false;
    } else {
      jBody = (*env)->NewByteArray(env, (jsize)bodyLen);
      if (jBody == NULL) {
        built = false;
      } else {
        (*env)->SetByteArrayRegion(env, jBody, 0, (jsize)bodyLen, (const jbyte *)body);
      }
    }
  }
  if (!built) {
    (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
    if (reasonOut) *reasonOut = "could not build the Java request";
    return WBN_RUN_ERROR;
  }

  jint result = (*env)->CallStaticIntMethod(env, cls, s_midRequest,
                                            (jlong)(intptr_t)t, jMethod, jUrl,
                                            jHeaders, jBody,
                                            (jint)connectMs, (jint)totalMs);
  bool threw = wbnJniClearException(env, "WinBoloHttp.request");
  (*env)->PopLocalFrame(env, NULL);

  if (threw) {
    if (reasonOut) *reasonOut = "Java exception in WinBoloHttp.request";
    return WBN_RUN_ERROR;
  }
  if (result >= 0) {
    t->status = (long)result;
    return WBN_RUN_OK;
  }
  if (result == WBN_JAVA_RESULT_STOPPED) {
    if (t->cancelled) {
      return WBN_RUN_CANCELLED;
    }
    if (reasonOut) {
      *reasonOut = t->overLimit    ? "response exceeds its byte ceiling"
                 : t->writeFailed  ? "failed writing received data"
                 : t->pastDeadline ? "past the shutdown deadline"
                                   : "stopped";
    }
    return WBN_RUN_ERROR;
  }
  if (result == WBN_JAVA_RESULT_TIMED_OUT) {
    if (reasonOut) *reasonOut = "timed out";
    return WBN_RUN_ERROR;
  }
  if (reasonOut) *reasonOut = t->error[0] != '\0' ? t->error : "transport error";
  return WBN_RUN_ERROR;
}

/* Percent-encodes every byte of url a URL may not carry as it is: a space,
 * a quote, a byte above 127 and the like. Letters, digits, the characters
 * RFC 3986 allows in a path or a query (including "/", "?", "&", "=" and
 * ":"), the URL delimiters "#", "[" and "]", and a "%" that already starts
 * a "%XX" are kept, so a path a caller has encoded itself is not encoded
 * twice. Returns false when the result does not fit. */
static bool wbnUrlEncode(const char *url, char *out, size_t outSize) {
  static const char kKeep[] = "-._~!$&'()*+,;=:@/?#[]";
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)url; *p != '\0'; p++) {
    unsigned char c = *p;
    bool keep = isalnum(c) || strchr(kKeep, c) != NULL;
    if (c == '%' && isxdigit(p[1]) && isxdigit(p[2])) {
      keep = true;
    }
    if (keep) {
      if (n + 1 >= outSize) return false;
      out[n++] = (char)c;
    } else {
      if (n + 3 >= outSize) return false;
      snprintf(out + n, outSize - n, "%%%02X", (unsigned)c);
      n += 3;
    }
  }
  out[n] = '\0';
  return true;
}

/* Builds <baseUrl>/api/v1/<path>, encoded, into urlOut. logOut keeps the
 * text as given, for the log. */
static bool wbnApiUrl(const char *path, char *logOut, size_t logOutSize,
                      char *urlOut, size_t urlOutSize) {
  snprintf(logOut, logOutSize, "%s/api/v1/%s", wbnBaseUrl, path);
  return wbnUrlEncode(logOut, urlOut, urlOutSize);
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
* started. There is no global state to set up: the Java
* side keeps its connections for the life of the process.
*********************************************************/
static SDL_SpinLock s_createLock = 0;

bool httpCreate(void) {
  char iniValue[FILENAME_MAX];

  SDL_LockSpinlock(&s_createLock);
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
  SDL_UnlockSpinlock(&s_createLock);
  /* TRUE on success, as http.h says; the caller goes by this rather than
   * reading httpStarted again. */
  return true;
}

/*********************************************************
*NAME:          httpDestroy
*PURPOSE:
* Marks the module stopped. The Java side's pooled
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
* more here: the Java side pools connections for every
* thread.
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

  char logUrl[FILENAME_MAX + 64];
  char url[(FILENAME_MAX + 64) * 3];
  if (!wbnApiUrl(endpoint, logUrl, sizeof(logUrl), url, sizeof(url))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_post [%s]: malformed URL", endpoint);
    return -1;
  }

  WbnHeaders headers;
  wbnHeadersInit(&headers);
  wbnHeaderAdd(&headers, "Content-Type", "application/json");
  wbnAddSignatureHeaders(&headers, json_body);
  if (bearer != NULL) {
    wbnAddBearerHeader(&headers, bearer);
  }

  WbnJniTask t;
  wbnTaskInit(&t);
  if (!wbnTaskInitBuffer(&t)) return -1;

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_post: POST %s", logUrl);
  wbnLogRedactedJsonDebug(WB_LOG_CAT_NET, "wbn_api_post: body=", json_body);

  const char *reason = NULL;
  Uint64 performStartMs = SDL_GetTicks();
  WbnRunResult res = wbnRunRequest("POST", url, &headers, json_body, strlen(json_body),
                                   &t, WBN_POST_TIMEOUT_MS,
                                   WBN_POST_CONNECT_TIMEOUT_MS, &reason);
  wbnLogSlowCall(endpoint, performStartMs);

  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_post [%s]: request error: %s",
                endpoint, reason ? reason : "unknown");
    wbnTaskFree(&t);
    return -1;
  }

  {
    char respPrefix[FILENAME_MAX + 64];
    snprintf(respPrefix, sizeof(respPrefix), "wbn_api_post [%s]: HTTP %ld, response=",
             endpoint, t.status);
    wbnLogRedactedJsonDebug(WB_LOG_CAT_NET, respPrefix, t.buf);
  }

  if (response_out) {
    *response_out = wbnTaskTakeBuffer(&t);
  }
  wbnTaskFree(&t);
  return (int)t.status;
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
* sends, and the poll checks it every 50 ms while a
* request waits.
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

/* Reads a whole file into memory. Returns NULL when it cannot be read. */
static unsigned char *wbnReadFile(const char *fileName, size_t *lenOut) {
  FILE *f = fopen(fileName, "rb");
  if (f == NULL) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long size = ftell(f);
  if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  unsigned char *data = malloc((size_t)size + 1);
  if (data == NULL) { fclose(f); return NULL; }
  size_t got = fread(data, 1, (size_t)size, f);
  fclose(f);
  if (got != (size_t)size) { free(data); return NULL; }
  *lenOut = got;
  return data;
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

  char logUrl[FILENAME_MAX + 64];
  char url[(FILENAME_MAX + 64) * 3];
  snprintf(logUrl, sizeof(logUrl), "%s/log.php?key=%s", wbnBaseUrl, key);

  /* Don't log the URL verbatim — the ?key= query is an upload secret. */
  WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: POST %s/log.php?key=***", wbnBaseUrl);
  WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: file=%s", fileName);

  if (!wbnUrlEncode(logUrl, url, sizeof(url))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "httpSendLogFile: malformed URL");
    return false;
  }

  /* libcurl reads the file during the transfer and fails it when the file
   * cannot be read; here it is read first, with the same outcome. */
  size_t fileLen = 0;
  unsigned char *fileData = wbnReadFile(fileName, &fileLen);
  if (fileData == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "httpSendLogFile: cannot open file '%s'", fileName);
    return false;
  }
  WB_LOG_DEBUG(WB_LOG_CAT_NET, "httpSendLogFile: file size=%ld bytes", (long)fileLen);

  /* The boundary: 24 dashes and 32 hex digits, the shape libcurl uses. */
  char boundary[64];
  {
    Uint64 a = SDL_GetPerformanceCounter();
    Uint64 b = (Uint64)time(NULL) ^ ((Uint64)(uintptr_t)fileData << 7);
    snprintf(boundary, sizeof(boundary), "------------------------%016llx%016llx",
             (unsigned long long)a, (unsigned long long)b);
  }
  char head[256];
  int headLen = snprintf(head, sizeof(head),
      "--%s\r\n"
      "Content-Disposition: form-data; name=\"logfile\"; filename=\"log.dat\"\r\n"
      "Content-Type: application/octet-stream\r\n"
      "\r\n", boundary);
  char tail[128];
  int tailLen = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
  size_t bodyLen = (size_t)headLen + fileLen + (size_t)tailLen;
  unsigned char *body = malloc(bodyLen);
  if (body == NULL) {
    free(fileData);
    return false;
  }
  memcpy(body, head, (size_t)headLen);
  memcpy(body + headLen, fileData, fileLen);
  memcpy(body + headLen + fileLen, tail, (size_t)tailLen);
  free(fileData);

  WbnHeaders headers;
  wbnHeadersInit(&headers);
  {
    char contentType[128];
    snprintf(contentType, sizeof(contentType), "multipart/form-data; boundary=%s", boundary);
    wbnHeaderAdd(&headers, "Content-Type", contentType);
  }

  /* Log upload is permissive: with no server bearer it goes
   * unauthenticated, rather than refusing as wbn_api_post_server does. */
  char token[WBN_SERVER_TOKEN_LEN];
  winboloNetGetServerToken(token, sizeof(token));
  if (token[0] != '\0') {
    wbnAddBearerHeader(&headers, token);
  }

  WbnJniTask t;
  wbnTaskInit(&t);
  if (!wbnTaskInitBuffer(&t)) {
    free(body);
    return false;
  }
  /* The reply is only logged; keep its first 1023 bytes, as http.c's
   * fixed 1024-byte buffer does, and let the rest go by. */
  t.keepMax = 1023;

  long timeoutS = s_logUploadTimeoutOverride > 0 ? s_logUploadTimeoutOverride
                                                 : WBN_UPLOAD_TIMEOUT_S;
  const char *reason = NULL;
  Uint64 performStartMs = SDL_GetTicks();
  WbnRunResult res = wbnRunRequest("POST", url, &headers, body, bodyLen, &t,
                                   timeoutS * 1000L, WBN_UPLOAD_CONNECT_TIMEOUT_MS,
                                   &reason);
  wbnLogSlowCall("log.php upload", performStartMs);
  free(body);

  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "httpSendLogFile: request error: %s",
                reason ? reason : "unknown");
    wbnTaskFree(&t);
    return false;
  }

  {
    char respPrefix[64];
    snprintf(respPrefix, sizeof(respPrefix), "httpSendLogFile: HTTP %ld, response=", t.status);
    wbnLogRedactedJsonDebug(WB_LOG_CAT_NET, respPrefix, t.buf);
  }
  wbnTaskFree(&t);
  return true;
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

  char logUrl[FILENAME_MAX + 256];
  char url[(FILENAME_MAX + 256) * 3];
  if (!wbnApiUrl(path, logUrl, sizeof(logUrl), url, sizeof(url))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_get [%s]: malformed URL", path);
    return -1;
  }

  WbnHeaders headers;
  wbnHeadersInit(&headers);
  if (sign) {
    wbnAddSignatureHeaders(&headers, "");
  }
  if (bearerToken != NULL && bearerToken[0] != '\0') {
    wbnAddBearerHeader(&headers, bearerToken);
  }

  WbnJniTask t;
  wbnTaskInit(&t);
  if (!wbnTaskInitBuffer(&t)) return -1;
  t.cancelFlag = cancel_flag;

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_get: GET %s", logUrl);

  const char *reason = NULL;
  WbnRunResult res = wbnRunRequest("GET", url, &headers, NULL, 0, &t,
                                   timeout_ms > 0 ? timeout_ms : WBN_API_GET_TIMEOUT_MS,
                                   connect_timeout_ms > 0 ? connect_timeout_ms : 0,
                                   &reason);
  if (res == WBN_RUN_CANCELLED) {
    wbnTaskFree(&t);
    return -2;
  }
  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_get [%s]: request error: %s",
                path, reason ? reason : "unknown");
    wbnTaskFree(&t);
    return -1;
  }

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_get [%s]: HTTP %ld", path, t.status);

  if (response_out) {
    *response_out = wbnTaskTakeBuffer(&t);
  }
  wbnTaskFree(&t);
  return (int)t.status;
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

  char logUrl[FILENAME_MAX + 64];
  char url[(FILENAME_MAX + 64) * 3];
  if (!wbnApiUrl("prefs", logUrl, sizeof(logUrl), url, sizeof(url))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_prefs: malformed URL");
    return -1;
  }

  WbnHeaders headers;
  wbnHeadersInit(&headers);
  wbnHeaderAdd(&headers, "Content-Type", "application/json");
  wbnAddSignatureHeaders(&headers, sign_body);
  wbnAddBearerHeader(&headers, bearerToken);

  WbnJniTask t;
  wbnTaskInit(&t);
  if (!wbnTaskInitBuffer(&t)) return -1;

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_prefs: %s %s", is_put ? "PUT" : "GET", logUrl);

  const char *reason = NULL;
  Uint64 performStartMs = SDL_GetTicks();
  WbnRunResult res = wbnRunRequest(is_put ? "PUT" : "GET", url, &headers,
                                   is_put ? json_body : NULL,
                                   is_put ? strlen(json_body) : 0,
                                   &t, WBN_PREFS_TIMEOUT_MS, 0, &reason);
  if (is_put) {
    wbnLogSlowCall("prefs PUT", performStartMs);
  }

  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_prefs %s: request error: %s",
                is_put ? "PUT" : "GET", reason ? reason : "unknown");
    wbnTaskFree(&t);
    return -1;
  }

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_prefs %s: HTTP %ld",
               is_put ? "PUT" : "GET", t.status);

  if (response_out) {
    *response_out = wbnTaskTakeBuffer(&t);
  }
  wbnTaskFree(&t);
  return (int)t.status;
}

int wbn_prefs_get(const char *bearerToken, char **response_out) {
  return wbn_prefs_request_impl(bearerToken, false, "", response_out);
}

int wbn_prefs_put(const char *bearerToken, const char *json_body,
                  char **response_out) {
  return wbn_prefs_request_impl(bearerToken, true, json_body, response_out);
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

  char logUrl[FILENAME_MAX + 256];
  char url[(FILENAME_MAX + 256) * 3];
  if (!wbnApiUrl(path, logUrl, sizeof(logUrl), url, sizeof(url))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download: malformed URL");
    return -1;
  }

  FILE *fp = fopen(dest_path, "wb");
  if (!fp) return -1;

  WbnHeaders headers;
  wbnHeadersInit(&headers);
  wbnAddSignatureHeaders(&headers, "");

  WbnJniTask t;
  wbnTaskInit(&t);
  t.fp = fp;
  t.progressFn = progressFn;
  t.progressUser = progressUserData;
  t.cancelFlag = cancel_flag;

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_download: GET %s -> %s", logUrl, dest_path);

  const char *reason = NULL;
  WbnRunResult res = wbnRunRequest("GET", url, &headers, NULL, 0, &t,
                                   WBN_DOWNLOAD_TIMEOUT_MS, 0, &reason);
  t.fp = NULL;
  fclose(fp);

  if (res == WBN_RUN_CANCELLED) {
    remove(dest_path);
    return -2;
  }
  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download: request error: %s",
                reason ? reason : "unknown");
    remove(dest_path);
    return -1;
  }

  if (t.status != 200) {
    remove(dest_path);
  }
  return (int)t.status;
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

  char logUrl[FILENAME_MAX + 256];
  char url[(FILENAME_MAX + 256) * 3];
  if (!wbnApiUrl(path, logUrl, sizeof(logUrl), url, sizeof(url))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download_to_memory: malformed URL");
    return -1;
  }

  WbnHeaders headers;
  wbnHeadersInit(&headers);
  wbnAddSignatureHeaders(&headers, "");

  WbnJniTask t;
  wbnTaskInit(&t);
  if (!wbnTaskInitBuffer(&t)) return -1;
  t.limit = maxBytes;
  t.progressFn = progressFn;
  t.progressUser = progressUserData;
  t.cancelFlag = cancel_flag;

  WB_LOG_DEBUG(WB_LOG_CAT_NET, "wbn_api_download_to_memory: GET %s", logUrl);

  const char *reason = NULL;
  WbnRunResult res = wbnRunRequest("GET", url, &headers, NULL, 0, &t,
                                   WBN_DOWNLOAD_TIMEOUT_MS, 0, &reason);
  if (res == WBN_RUN_CANCELLED) {
    wbnTaskFree(&t);
    return -2;
  }
  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "wbn_api_download_to_memory: request error: %s",
                reason ? reason : "unknown");
    wbnTaskFree(&t);
    return -1;
  }

  if (t.status == 200 && data_out && size_out) {
    size_t got = t.size;
    *data_out = (uint8_t *)wbnTaskTakeBuffer(&t);
    *size_out = got;
  }
  wbnTaskFree(&t);
  return (int)t.status;
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

  /* The URL comes from the news JSON, so only https is fetched. This also
   * keeps file: and data: URLs out. */
  if (SDL_strncasecmp(url, "https://", 8) != 0) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "httpGetUrlToMemory: refused, scheme is not https");
    return -1;
  }

  char encoded[(FILENAME_MAX + 256) * 3];
  if (!wbnUrlEncode(url, encoded, sizeof(encoded))) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "httpGetUrlToMemory: malformed URL");
    return -1;
  }

  WbnHeaders headers;
  wbnHeadersInit(&headers);

  WbnJniTask t;
  wbnTaskInit(&t);
  if (!wbnTaskInitBuffer(&t)) return -1;
  t.limit = maxBytes;

  const char *reason = NULL;
  WbnRunResult res = wbnRunRequest("GET", encoded, &headers, NULL, 0, &t,
                                   timeout_ms > 0 ? timeout_ms : WBN_API_GET_TIMEOUT_MS,
                                   0, &reason);
  if (res != WBN_RUN_OK) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "httpGetUrlToMemory: request error: %s",
                reason ? reason : "unknown");
    wbnTaskFree(&t);
    return -1;
  }

  if (t.status == 200 && data_out && size_out) {
    size_t got = t.size;
    *data_out = (uint8_t *)wbnTaskTakeBuffer(&t);
    *size_out = got;
  }
  wbnTaskFree(&t);
  return (int)t.status;
}

/*********************************************************
*NAME:          httpSetAltIpAddress
*PURPOSE:
* Stores the alternate local address. HttpURLConnection
* cannot bind to one, so the first request after it is set
* logs a warning and goes out on the default route.
*********************************************************/
void httpSetAltIpAddress(char *iptoset) {
  strncpy(altIpAddress, iptoset, sizeof(altIpAddress) - 1);
  altIpAddress[sizeof(altIpAddress) - 1] = '\0';
  s_altIpWarned = false;
}
