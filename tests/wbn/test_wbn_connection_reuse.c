/*
 * WinBolo.net posts share one connection.
 *
 * Every post the WinBolo.net worker thread fires used to be
 * curl_easy_init -> setopt -> curl_easy_perform -> curl_easy_cleanup, with no
 * curl_share and no CURLOPT_FORBID_REUSE, so nothing was pooled: a queued
 * post paid a fresh TCP connect and, against the real host, a full TLS
 * handshake. httpWorkerPoolBegin now gives the worker one handle it keeps for
 * its life and resets per request, which libcurl documents as leaving live
 * connections, the TLS session cache and the DNS cache alone.
 *
 * What this case pins: four posts queued through
 * winbolonetThreadAddServerRequest arrive as four requests on exactly one
 * accepted TCP connection. Before the change the listener accepted four.
 *
 * Why its own target rather than a case in WinBoloUnitTests: that binary
 * links tests/unit/test_stubs.c, which stubs WinBolo.net out and links no
 * libcurl, so nothing in it can show whether an HTTP call opened a connection
 * or reused one. This target links the real http.c and winbolonetthread.c and
 * puts a local TCP listener where the WBN host would be.
 *
 * The listener speaks plain HTTP, so the test needs no certificate:
 * httpSetHostOverride takes an http:// base URL and buildBaseUrl keeps the
 * scheme. It answers every request with a minimal 200 and a {} body and never
 * sends Connection: close, so whether the connection is reused is the
 * client's decision, which is what is being measured.
 *
 * What it deliberately does not pin: the order the four posts arrive in, or
 * how many worker sweeps they are spread over. The worker swaps the whole
 * waiting queue under its mutex and sleeps WBN_THREAD_SLEEP_TIME between
 * sweeps, so a post enqueued a moment late rides the next sweep; the kept
 * handle holds the connection open across that sleep either way.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ahead of SDL3 and global.h: on Windows the socket types must come from
 * WinSock2.h before anything can pull windows.h in. tests/unit's loopback
 * harness takes its socket types from the same header. */
#include "platform_net.h"

#include <SDL3/SDL.h>

#include "global.h"
#include "http.h"
#include "wbn_bearer.h"
#include "winbolonetthread.h"

/* winbolonet_server.c's only call back into the server. Nothing in this test
 * reaches it; it is here so the library links. */
void serverSimConsoleMessage(const char *msg) {
  (void)msg;
}

#define WBN_TEST_POSTS        4
#define WBN_TEST_DRAIN_MS     15000
#define WBN_TEST_POLL_MS      5
#define WBN_TEST_REQUEST_MAX  16384

static const char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "{}";

typedef struct {
  bolo_socket_t listenSock;
  SDL_Thread   *thread;
  SDL_AtomicInt connections; /* TCP connections accepted */
  SDL_AtomicInt requests;    /* complete requests answered */
  SDL_AtomicInt stop;        /* set by the main thread to end the loop */
} Listener;

/*********************************************************
*NAME:          socketWouldBlock
*PURPOSE:
* TRUE when the last socket call failed only because there
* was nothing to do yet on a non-blocking socket.
*********************************************************/
static bool socketWouldBlock(void) {
#ifdef _WIN32
  int err = WSAGetLastError();
  return err == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/*********************************************************
*NAME:          socketSetNonBlocking
*PURPOSE:
* Puts a socket in non-blocking mode so the listener loop
* can check its stop flag instead of parking in accept().
*********************************************************/
static void socketSetNonBlocking(bolo_socket_t s) {
  unsigned long on = 1;
  ioctlsocket(s, FIONBIO, &on);
}

/*********************************************************
*NAME:          headerContentLength
*PURPOSE:
* Reads the Content-Length of a request whose header runs
* from buf for hdrLen bytes. Returns 0 when there is none.
* Matches the field name without regard to case, so the
* parse does not rest on how libcurl spells it.
*********************************************************/
static size_t headerContentLength(const char *buf, size_t hdrLen) {
  static const char name[] = "content-length:";
  const size_t nameLen = sizeof(name) - 1;
  size_t i;

  for (i = 0; i + nameLen < hdrLen; i++) {
    size_t j;
    for (j = 0; j < nameLen; j++) {
      char c = buf[i + j];
      if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
      }
      if (c != name[j]) {
        break;
      }
    }
    if (j == nameLen) {
      return (size_t)strtoul(buf + i + nameLen, NULL, 10);
    }
  }
  return 0;
}

/*********************************************************
*NAME:          sendAll
*PURPOSE:
* Writes the whole buffer to a non-blocking socket.
* Returns FALSE when the peer is gone.
*********************************************************/
static bool sendAll(bolo_socket_t s, const char *data, size_t len) {
  size_t sent = 0;

  while (sent < len) {
    int n = (int)send(s, data + sent, (int)(len - sent), 0);
    if (n > 0) {
      sent += (size_t)n;
    } else if (n < 0 && socketWouldBlock()) {
      SDL_Delay(WBN_TEST_POLL_MS);
    } else {
      return FALSE;
    }
  }
  return TRUE;
}

/*********************************************************
*NAME:          listenerRun
*PURPOSE:
* Accepts one connection at a time, answers every complete
* request on it, and counts both. Holds the connection open
* until the client closes it, so a reused connection and a
* fresh one are told apart by the accept count.
*********************************************************/
static int listenerRun(void *data) {
  Listener *ln = (Listener *)data;
  bolo_socket_t conn = BOLO_INVALID_SOCKET;
  char buf[WBN_TEST_REQUEST_MAX];
  size_t used = 0;

  while (SDL_GetAtomicInt(&ln->stop) == 0) {
    int n;

    if (conn == BOLO_INVALID_SOCKET) {
      bolo_socket_t accepted = accept(ln->listenSock, NULL, NULL);
      if (accepted == BOLO_INVALID_SOCKET) {
        SDL_Delay(WBN_TEST_POLL_MS);
        continue;
      }
      socketSetNonBlocking(accepted);
      conn = accepted;
      used = 0;
      SDL_AddAtomicInt(&ln->connections, 1);
    }

    n = (int)recv(conn, buf + used, (int)(sizeof(buf) - used - 1), 0);
    if (n > 0) {
      used += (size_t)n;
      buf[used] = '\0';

      /* Answer every complete request sitting in the buffer. */
      for (;;) {
        char *hdrEnd = strstr(buf, "\r\n\r\n");
        size_t hdrLen;
        size_t total;

        if (hdrEnd == NULL) {
          break;
        }
        hdrLen = (size_t)(hdrEnd - buf) + 4;
        total = hdrLen + headerContentLength(buf, hdrLen);
        if (used < total) {
          break;
        }
        if (!sendAll(conn, kResponse, sizeof(kResponse) - 1)) {
          break;
        }
        SDL_AddAtomicInt(&ln->requests, 1);
        memmove(buf, buf + total, used - total);
        used -= total;
        buf[used] = '\0';
      }

      if (used >= sizeof(buf) - 1) {
        /* A request larger than the buffer: drop the connection rather than
         * spin. The request count then falls short and the test fails. */
        closesocket(conn);
        conn = BOLO_INVALID_SOCKET;
      }
    } else if (n < 0 && socketWouldBlock()) {
      SDL_Delay(WBN_TEST_POLL_MS);
    } else {
      closesocket(conn);
      conn = BOLO_INVALID_SOCKET;
    }
  }

  if (conn != BOLO_INVALID_SOCKET) {
    closesocket(conn);
  }
  return 0;
}

/*********************************************************
*NAME:          listenerStart
*PURPOSE:
* Binds a TCP listener on an OS-chosen 127.0.0.1 port,
* reads the port back into *port, and runs the accept loop
* on its own thread. Returns success.
*********************************************************/
static bool listenerStart(Listener *ln, unsigned short *port) {
  struct sockaddr_in addr;
  socklen_t addrLen = sizeof(addr);
  int on = 1;

  memset(ln, 0, sizeof(*ln));
  SDL_SetAtomicInt(&ln->connections, 0);
  SDL_SetAtomicInt(&ln->requests, 0);
  SDL_SetAtomicInt(&ln->stop, 0);

  ln->listenSock = socket(AF_INET, SOCK_STREAM, 0);
  if (ln->listenSock == BOLO_INVALID_SOCKET) {
    fprintf(stderr, "listenerStart: socket failed\n");
    return FALSE;
  }
  setsockopt(ln->listenSock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on,
             sizeof(on));

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(ln->listenSock, (struct sockaddr *)&addr, sizeof(addr)) ==
      BOLO_SOCKET_ERROR) {
    fprintf(stderr, "listenerStart: bind failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  if (listen(ln->listenSock, WBN_TEST_POSTS + 1) == BOLO_SOCKET_ERROR) {
    fprintf(stderr, "listenerStart: listen failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  if (getsockname(ln->listenSock, (struct sockaddr *)&addr, &addrLen) ==
      BOLO_SOCKET_ERROR) {
    fprintf(stderr, "listenerStart: getsockname failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  *port = ntohs(addr.sin_port);

  socketSetNonBlocking(ln->listenSock);

  ln->thread = SDL_CreateThread(listenerRun, "WbnTestListener", ln);
  if (ln->thread == NULL) {
    fprintf(stderr, "listenerStart: SDL_CreateThread failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  return TRUE;
}

/*********************************************************
*NAME:          listenerStop
*PURPOSE:
* Ends the accept loop, joins its thread, and closes the
* listening socket. The loop polls the stop flag between
* non-blocking calls, so nothing is left parked in accept().
*********************************************************/
static void listenerStop(Listener *ln) {
  SDL_SetAtomicInt(&ln->stop, 1);
  if (ln->thread != NULL) {
    SDL_WaitThread(ln->thread, NULL);
    ln->thread = NULL;
  }
  if (ln->listenSock != BOLO_INVALID_SOCKET) {
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
  }
}

/*********************************************************
*NAME:          testPostsShareConnection
*PURPOSE:
* Queues four server posts through the worker thread and
* asserts the listener saw four requests on one connection.
* Returns success.
*********************************************************/
static bool testPostsShareConnection(void) {
  Listener ln;
  unsigned short port = 0;
  char baseUrl[64];
  int drained = 0;
  int connections;
  int requests;
  int i;
  bool ok = TRUE;

  if (!listenerStart(&ln, &port)) {
    return FALSE;
  }

  snprintf(baseUrl, sizeof(baseUrl), "http://127.0.0.1:%u",
           (unsigned int)port);
  httpSetHostOverride(baseUrl);

  if (!httpCreate()) {
    fprintf(stderr, "testPostsShareConnection: httpCreate failed\n");
    listenerStop(&ln);
    return FALSE;
  }

  /* wbn_api_post_server refuses to send without a bearer. The listener
   * never reads it. */
  httpSetServerBearerToken(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

  if (!winbolonetThreadCreate()) {
    fprintf(stderr, "testPostsShareConnection: winbolonetThreadCreate failed\n");
    httpDestroy();
    listenerStop(&ln);
    return FALSE;
  }

  for (i = 0; i < WBN_TEST_POSTS; i++) {
    winbolonetThreadAddServerRequest("server/update", "{\"test\":1}");
  }

  while (drained < WBN_TEST_DRAIN_MS &&
         SDL_GetAtomicInt(&ln.requests) < WBN_TEST_POSTS) {
    SDL_Delay(WBN_TEST_POLL_MS);
    drained += WBN_TEST_POLL_MS;
  }

  winbolonetThreadDestroy();
  httpDestroy();
  httpClearServerBearerToken();
  listenerStop(&ln);

  requests = SDL_GetAtomicInt(&ln.requests);
  connections = SDL_GetAtomicInt(&ln.connections);

  if (requests != WBN_TEST_POSTS) {
    fprintf(stderr,
            "testPostsShareConnection: listener saw %d requests, expected %d\n",
            requests, WBN_TEST_POSTS);
    ok = FALSE;
  }
  if (connections != 1) {
    fprintf(stderr,
            "testPostsShareConnection: listener accepted %d connections, "
            "expected 1\n",
            connections);
    ok = FALSE;
  }
  return ok;
}

int main(void) {
  bool ok;

  if (bolo_net_init() != 0) {
    fprintf(stderr, "bolo_net_init failed\n");
    return 1;
  }
  if (!SDL_Init(0)) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    bolo_net_cleanup();
    return 1;
  }

  ok = testPostsShareConnection();
  printf("wbn_posts_share_connection ... %s\n", ok ? "OK" : "FAILED");

  SDL_Quit();
  bolo_net_cleanup();
  return ok ? 0 : 1;
}
