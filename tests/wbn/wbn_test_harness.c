/*
 * The local listener the WinBolo.net cases post at, and the runner that
 * selects one case by name. See wbn_test_harness.h for what the listener
 * records and why.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wbn_test_harness.h"

/* winbolonet_server.c's only call back into the server. Nothing in these
 * cases reaches it; it is here so the library links. */
void serverSimConsoleMessage(const char *msg) {
  (void)msg;
}

#define WBN_TEST_REQUEST_MAX 16384

/* One accepted connection: its socket, the accept order number a case reads
 * back to tell a reused connection from a fresh one, and the bytes read on
 * it that do not yet make a whole request. */
typedef struct {
  bolo_socket_t sock;
  int           index;
  size_t        used;
  char          buf[WBN_TEST_REQUEST_MAX];
} WbnTestConn;

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
*NAME:          extractPath
*PURPOSE:
* Copies the path out of a request line
* ("POST /a/b HTTP/1.1") into out. Leaves out empty when
* there is no path to read.
*********************************************************/
static void extractPath(const char *buf, size_t hdrLen, char *out,
                        size_t outLen) {
  const char *start;
  const char *end;
  size_t len;

  out[0] = '\0';
  start = memchr(buf, ' ', hdrLen);
  if (start == NULL) {
    return;
  }
  start++;
  end = memchr(start, ' ', hdrLen - (size_t)(start - buf));
  if (end == NULL) {
    return;
  }
  len = (size_t)(end - start);
  if (len >= outLen) {
    len = outLen - 1;
  }
  memcpy(out, start, len);
  out[len] = '\0';
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
*NAME:          sendAnswer
*PURPOSE:
* Answers one request with a 200 and a JSON body. A
* server/register gets the key and token pair the register
* apply path reads; everything else gets {}. No
* Connection: close, so the client decides whether the
* connection is kept.
*********************************************************/
static bool sendAnswer(bolo_socket_t s, const char *path) {
  char body[256];
  char msg[512];
  int len;

  if (strstr(path, "server/register") != NULL) {
    snprintf(body, sizeof(body),
             "{\"server_key\":\"%s\",\"server_token\":\"%s\"}",
             WBN_TEST_REGISTER_KEY, WBN_TEST_REGISTER_TOKEN);
  } else {
    snprintf(body, sizeof(body), "{}");
  }
  len = snprintf(msg, sizeof(msg),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %u\r\n"
                 "\r\n"
                 "%s",
                 (unsigned int)strlen(body), body);
  if (len <= 0) {
    return FALSE;
  }
  return sendAll(s, msg, (size_t)len);
}

/*********************************************************
*NAME:          answerRequests
*PURPOSE:
* Answers every complete request sitting in the
* connection's buffer, recording each one's path and the
* connection it came in on. Returns FALSE when the peer is
* gone and the connection should be dropped.
*********************************************************/
static bool answerRequests(WbnTestListener *ln, WbnTestConn *c) {
  for (;;) {
    char *hdrEnd = strstr(c->buf, "\r\n\r\n");
    char path[WBN_TEST_PATH_LEN];
    size_t hdrLen;
    size_t total;
    int    index;

    if (hdrEnd == NULL) {
      return TRUE;
    }
    hdrLen = (size_t)(hdrEnd - c->buf) + 4;
    total = hdrLen + headerContentLength(c->buf, hdrLen);
    if (c->used < total) {
      return TRUE;
    }

    extractPath(c->buf, hdrLen, path, sizeof(path));
    index = SDL_GetAtomicInt(&ln->requests);
    if (index >= 0 && index < WBN_TEST_MAX_PATHS) {
      memcpy(ln->path[index], path, strlen(path) + 1);
      ln->conn[index] = c->index;
    }
    /* The hold goes before the answer, not before the read: the point
     * is to make the caller wait on the reply. */
    if (ln->holdMs > 0) {
      SDL_Delay((Uint32)ln->holdMs);
    }
    if (!sendAnswer(c->sock, path)) {
      return FALSE;
    }
    SDL_AddAtomicInt(&ln->requests, 1);
    memmove(c->buf, c->buf + total, c->used - total);
    c->used -= total;
    c->buf[c->used] = '\0';
  }
}

/*********************************************************
*NAME:          listenerRun
*PURPOSE:
* Accepts connections up to WBN_TEST_MAX_CONNS, answers
* every complete request on each, and counts both. Holds a
* connection open until the client closes it, so a reused
* connection and a fresh one are told apart by the accept
* order number recorded against each request.
*********************************************************/
static int listenerRun(void *data) {
  WbnTestListener *ln = (WbnTestListener *)data;
  WbnTestConn *conns;
  int nconns = 0;
  int i;

  conns = (WbnTestConn *)malloc(sizeof(WbnTestConn) * WBN_TEST_MAX_CONNS);
  if (conns == NULL) {
    fprintf(stderr, "listenerRun: out of memory\n");
    return 1;
  }

  while (SDL_GetAtomicInt(&ln->stop) == 0) {
    bool worked = FALSE;

    if (nconns < WBN_TEST_MAX_CONNS) {
      bolo_socket_t accepted = accept(ln->listenSock, NULL, NULL);
      if (accepted != BOLO_INVALID_SOCKET) {
        socketSetNonBlocking(accepted);
        conns[nconns].sock = accepted;
        conns[nconns].used = 0;
        conns[nconns].buf[0] = '\0';
        /* SDL_AddAtomicInt answers with the value before the add, which is
         * this connection's 0-based place in the accept order. */
        conns[nconns].index = SDL_AddAtomicInt(&ln->connections, 1);
        nconns++;
        worked = TRUE;
      }
    }

    i = 0;
    while (i < nconns) {
      WbnTestConn *c = &conns[i];
      bool drop = FALSE;
      int n;

      n = (int)recv(c->sock, c->buf + c->used,
                    (int)(WBN_TEST_REQUEST_MAX - c->used - 1), 0);
      if (n > 0) {
        worked = TRUE;
        c->used += (size_t)n;
        c->buf[c->used] = '\0';
        if (!answerRequests(ln, c)) {
          drop = TRUE;
        } else if (c->used >= WBN_TEST_REQUEST_MAX - 1) {
          /* A request larger than the buffer: drop the connection rather
           * than spin. The request count then falls short and the case
           * fails. */
          drop = TRUE;
        }
      } else if (n < 0 && socketWouldBlock()) {
        /* Nothing on this one yet. */
      } else {
        drop = TRUE;
      }

      if (drop) {
        closesocket(c->sock);
        nconns--;
        if (i != nconns) {
          memcpy(c, &conns[nconns], sizeof(WbnTestConn));
        }
        continue; /* the slot holds a different connection now */
      }
      i++;
    }

    if (!worked) {
      SDL_Delay(WBN_TEST_POLL_MS);
    }
  }

  for (i = 0; i < nconns; i++) {
    closesocket(conns[i].sock);
  }
  free(conns);
  return 0;
}

bool wbnTestListenerStart(WbnTestListener *ln, unsigned short *port,
                          int holdMs) {
  struct sockaddr_in addr;
  socklen_t addrLen = sizeof(addr);
  int on = 1;
  int i;

  memset(ln, 0, sizeof(*ln));
  ln->holdMs = holdMs;
  SDL_SetAtomicInt(&ln->connections, 0);
  SDL_SetAtomicInt(&ln->requests, 0);
  SDL_SetAtomicInt(&ln->stop, 0);
  for (i = 0; i < WBN_TEST_MAX_PATHS; i++) {
    ln->conn[i] = -1;
  }

  ln->listenSock = socket(AF_INET, SOCK_STREAM, 0);
  if (ln->listenSock == BOLO_INVALID_SOCKET) {
    fprintf(stderr, "wbnTestListenerStart: socket failed\n");
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
    fprintf(stderr, "wbnTestListenerStart: bind failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  if (listen(ln->listenSock, 8) == BOLO_SOCKET_ERROR) {
    fprintf(stderr, "wbnTestListenerStart: listen failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  if (getsockname(ln->listenSock, (struct sockaddr *)&addr, &addrLen) ==
      BOLO_SOCKET_ERROR) {
    fprintf(stderr, "wbnTestListenerStart: getsockname failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  *port = ntohs(addr.sin_port);

  socketSetNonBlocking(ln->listenSock);

  ln->thread = SDL_CreateThread(listenerRun, "WbnTestListener", ln);
  if (ln->thread == NULL) {
    fprintf(stderr, "wbnTestListenerStart: SDL_CreateThread failed\n");
    closesocket(ln->listenSock);
    ln->listenSock = BOLO_INVALID_SOCKET;
    return FALSE;
  }
  return TRUE;
}

void wbnTestListenerStop(WbnTestListener *ln) {
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

int wbnTestWaitForRequests(WbnTestListener *ln, int want, int timeoutMs) {
  int waited = 0;

  while (waited < timeoutMs && SDL_GetAtomicInt(&ln->requests) < want) {
    SDL_Delay(WBN_TEST_POLL_MS);
    waited += WBN_TEST_POLL_MS;
  }
  return SDL_GetAtomicInt(&ln->requests);
}

/*********************************************************
*NAME:          main
*PURPOSE:
* Runs the case named on the command line. Each case owns
* its own listener and HTTP layer, so they are never run in
* one process together.
*********************************************************/
int main(int argc, char **argv) {
  const char *name = (argc > 1) ? argv[1] : "";
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

  if (strcmp(name, "posts_share_connection") == 0) {
    ok = wbnTestPostsShareConnection();
  } else if (strcmp(name, "leave_returns_at_once") == 0) {
    ok = wbnTestLeaveReturnsAtOnce();
  } else if (strcmp(name, "worker_outlives_session") == 0) {
    ok = wbnTestWorkerOutlivesSession();
  } else if (strcmp(name, "job_result_returns") == 0) {
    ok = wbnTestJobResultReturns();
  } else {
    fprintf(stderr,
            "usage: %s posts_share_connection|leave_returns_at_once|"
            "worker_outlives_session|job_result_returns\n",
            argv[0]);
    SDL_Quit();
    bolo_net_cleanup();
    return 2;
  }

  printf("wbn_%s ... %s\n", name, ok ? "OK" : "FAILED");

  SDL_Quit();
  bolo_net_cleanup();
  return ok ? 0 : 1;
}
