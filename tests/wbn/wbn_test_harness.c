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

static const char kResponse[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 2\r\n"
    "\r\n"
    "{}";

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
*NAME:          recordPath
*PURPOSE:
* Stores the path out of a request line ("POST /a/b HTTP/1.1")
* at slot `index`, so a case can assert what arrived and in
* what order.
*********************************************************/
static void recordPath(WbnTestListener *ln, int index, const char *buf,
                       size_t hdrLen) {
  const char *start;
  const char *end;
  size_t len;

  if (index < 0 || index >= WBN_TEST_MAX_PATHS) {
    return;
  }
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
  if (len >= WBN_TEST_PATH_LEN) {
    len = WBN_TEST_PATH_LEN - 1;
  }
  memcpy(ln->path[index], start, len);
  ln->path[index][len] = '\0';
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
  WbnTestListener *ln = (WbnTestListener *)data;
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
        int    index;

        if (hdrEnd == NULL) {
          break;
        }
        hdrLen = (size_t)(hdrEnd - buf) + 4;
        total = hdrLen + headerContentLength(buf, hdrLen);
        if (used < total) {
          break;
        }
        index = SDL_GetAtomicInt(&ln->requests);
        recordPath(ln, index, buf, hdrLen);
        /* The hold goes before the answer, not before the read: the point
         * is to make the caller wait on the reply. */
        if (ln->holdMs > 0) {
          SDL_Delay((Uint32)ln->holdMs);
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
         * spin. The request count then falls short and the case fails. */
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

bool wbnTestListenerStart(WbnTestListener *ln, unsigned short *port,
                          int holdMs) {
  struct sockaddr_in addr;
  socklen_t addrLen = sizeof(addr);
  int on = 1;

  memset(ln, 0, sizeof(*ln));
  ln->holdMs = holdMs;
  SDL_SetAtomicInt(&ln->connections, 0);
  SDL_SetAtomicInt(&ln->requests, 0);
  SDL_SetAtomicInt(&ln->stop, 0);

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
  } else {
    fprintf(stderr, "usage: %s posts_share_connection|leave_returns_at_once\n",
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
