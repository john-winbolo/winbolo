/*
 * Shared rig for the WinBolo.net HTTP cases.
 *
 * A local TCP listener that stands in for the WinBolo.net host: it speaks
 * plain HTTP so the cases need no certificate, answers every request with a
 * minimal 200 and a {} body, and never sends Connection: close, so whether a
 * connection is reused is the client's decision rather than the server's.
 *
 * It counts the connections it accepts and the requests it answers, and
 * records each request's path in arrival order, which is what lets a case
 * assert both what was sent and how it got there. holdMs delays each answer,
 * so a case can make a synchronous call visibly expensive.
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

/* Poll interval for every bounded wait in these cases. */
#define WBN_TEST_POLL_MS 5

typedef struct {
    bolo_socket_t listenSock;
    SDL_Thread   *thread;
    int           holdMs;       /* delay before answering each request */
    SDL_AtomicInt connections;  /* TCP connections accepted */
    SDL_AtomicInt requests;     /* complete requests answered */
    char          path[WBN_TEST_MAX_PATHS][WBN_TEST_PATH_LEN];
    SDL_AtomicInt stop;         /* set by the main thread to end the loop */
} WbnTestListener;

/* Bind a listener on an OS-chosen 127.0.0.1 port, read the port back into
 * *port, and run the accept loop on its own thread. holdMs is the delay
 * before each answer; 0 answers at once. Returns success. */
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

#endif /* WINBOLO_TEST_WBN_HARNESS_H */
