/*
 * Shared rig for the WinBolo.net HTTP cases.
 *
 * A local TCP listener that stands in for the WinBolo.net host: it speaks
 * plain HTTP so the cases need no certificate, answers every request with a
 * minimal 200, and never sends Connection: close, so whether a connection is
 * reused is the client's decision rather than the server's. A request for
 * server/register is answered with a body carrying a server_key and a
 * server_token, the two fields winbolonetApplyRegisterResponse reads; every
 * other endpoint gets {}.
 *
 * It serves several connections at once, which a case needs when the worker
 * thread is holding its pooled connection open while the caller's thread
 * makes a synchronous call of its own.
 *
 * It counts the connections it accepts and the requests it answers, and
 * records each request's path and the connection it arrived on, in arrival
 * order, which is what lets a case assert what was sent, how it got there,
 * and whether two posts shared a connection. holdMs delays each answer, so a
 * case can make a synchronous call visibly expensive.
 */

#ifndef WINBOLO_TEST_WBN_HARNESS_H
#define WINBOLO_TEST_WBN_HARNESS_H

/* Ahead of SDL3 and global.h: on Windows the socket types must come from
 * WinSock2.h before anything can pull windows.h in. tests/unit's loopback
 * harness takes its socket types from the same header. */
#include "platform_net.h"

#include <SDL3/SDL.h>

#include "global.h"

/* Requests whose paths are kept. Past this the count still climbs but the
 * path is dropped, which fails an ordering assertion rather than writing
 * off the end. */
#define WBN_TEST_MAX_PATHS 16
#define WBN_TEST_PATH_LEN  96

/* Body bytes kept per request. A case that needs to see what was sent reads
   these; bodyLen says how many bytes actually arrived, so a body cut off
   here is told apart from a short one. Kept NUL-terminated, which suits the
   JSON and multipart bodies these cases send and would not suit a body with
   a NUL in it. */
#define WBN_TEST_BODY_LEN 2048

/* Connections served at once. A case that needs more than this stalls on the
 * accept rather than failing outright, so the number is generous: the most
 * any case holds open together is the worker's pooled one plus a caller's
 * per-call handle, and a handle that has just been closed keeps its slot
 * until the loop next polls it. */
#define WBN_TEST_MAX_CONNS 8

/* Poll interval for every bounded wait in these cases. */
#define WBN_TEST_POLL_MS 5

/* What the listener answers a server/register with. The key is
 * WINBOLONET_KEY_LEN - 1 characters and the token WBN_SERVER_TOKEN_LEN - 1,
 * so both land whole in the buffers winbolonetApplyRegisterResponse copies
 * them into. A case that registers can assert the key arrived. */
#define WBN_TEST_REGISTER_KEY "0123456789abcdef0123456789abcdef"
#define WBN_TEST_REGISTER_TOKEN \
  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

typedef struct {
    bolo_socket_t listenSock;
    SDL_Thread   *thread;
    int           holdMs;       /* delay before answering each request */
    SDL_AtomicInt connections;  /* TCP connections accepted */
    SDL_AtomicInt requests;     /* complete requests answered */
    char          path[WBN_TEST_MAX_PATHS][WBN_TEST_PATH_LEN];
    char          body[WBN_TEST_MAX_PATHS][WBN_TEST_BODY_LEN];
    int           bodyLen[WBN_TEST_MAX_PATHS]; /* bytes sent, before truncation */
    /* Accept order number of the connection each request came in on, so a
     * case can tell a reused connection from a fresh one. Written by the
     * listener thread; read after wbnTestListenerStop has joined it. */
    int           conn[WBN_TEST_MAX_PATHS];
    SDL_AtomicInt stop;         /* set by the main thread to end the loop */
} WbnTestListener;

/* Bind a listener on an OS-chosen 127.0.0.1 port, read the port back into
 * *port, and run the accept loop on its own thread. holdMs is the delay
 * before each answer; 0 answers at once. The hold sits in the loop, so it
 * holds every connection, not just the one being answered. Returns success. */
bool wbnTestListenerStart(WbnTestListener *ln, unsigned short *port,
                          int holdMs);

/* End the accept loop, join its thread and close the listening socket. The
 * loop polls the stop flag between non-blocking calls, so nothing is left
 * parked in accept or recv. Safe on a listener that never started. */
void wbnTestListenerStop(WbnTestListener *ln);

/* Poll until the listener has answered `want` requests or timeoutMs has
 * passed. Returns the count it reached. */
int wbnTestWaitForRequests(WbnTestListener *ln, int want, int timeoutMs);

/* The cases. Each returns TRUE on success and reports its own failures. */
bool wbnTestPostsShareConnection(void);
bool wbnTestLeaveReturnsAtOnce(void);
bool wbnTestWorkerOutlivesSession(void);
bool wbnTestJobResultReturns(void);
bool wbnTestLogUploadRuns(void);

#endif /* WINBOLO_TEST_WBN_HARNESS_H */
