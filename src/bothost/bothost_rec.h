/*
 * WinBoloBotHost - brain recordings of the bots this host runs.
 *
 * A server run with -brain-debug records its own bots into one folder per
 * session block (debug_sessions/<block>/brainrec.btr). A bot whose brain
 * runs here has no brain on the server, so the server writes only a marker
 * for it and tells this host each block it opens and closes
 * (CTRL_BOT_HOST_REC). The host records each such bot into
 * <cache>/recordings/<server>/<block>/: one .hbr file per seat (the same
 * per-bot entries the server would write, keyed by the bot's input
 * sequence) and the bot's print2 log. When the server asks (back in its
 * lobby, or slowly during a game) the host sends a copy of each finished
 * block over the control connection's bulk channel. When the server
 * answers MERGED the block gets an "uploaded" marker and is not sent
 * again; when it answers FAILED (with the reason) the block is sent again
 * later. The files stay here: only the server's cleardebug deletes them,
 * and the free-disk floor only stops new recording. Blocks not sent before
 * a drop or a stop go on the next connect to the same server.
 */
#ifndef BOTHOST_REC_H
#define BOTHOST_REC_H

#include <stdbool.h>
#include <stdint.h>

#include "control_event.h"
#include "bulk_transfer.h"
#include "brain_overlay.h"

struct lua_State;

/* How the recorder talks to a server: the control connection of server
 * srv. */
typedef struct {
    void (*sendEvent)(int srv, const ControlEvent *evt);
    /* True when the bulk sender of srv has no transfer in flight. */
    bool (*bulkIdle)(int srv);
    /* Queue one bulk transfer on srv; false when it cannot. */
    bool (*bulkSend)(int srv, const BulkStreamHeader *h, const uint8_t *blob,
                     uint32_t len);
} HostRecIo;

/* cacheDir: the brain cache ('/' separators). minFreeGb: record only while
 * the cache's disk has this much free. */
void hostRecInit(const char *cacheDir, const char *hostName,
                 double minFreeGb, const HostRecIo *io);

/* The control connection to server srv (label "host:port") is up, or went
 * down. */
void hostRecServerUp(int srv, const char *label);
void hostRecServerDown(int srv);

/* A CTRL_BOT_HOST_REC from server srv. */
void hostRecOnEvent(int srv, const ControlEvent *evt);

/* Seats (idx: the host's pool index). SeatBuilt after a brain was made for
 * the seat; SeatGone when the seat or its brain goes. */
void hostRecSeatBuilt(int idx, int srv, struct lua_State *L);
void hostRecSeatGone(int idx);

/* Before the think, on the main thread: the Lua expression of the pool
 * capture srv's recording takes this tick, or NULL when it takes none.
 * *poolMode: 1 = round robin, 2 = every think (0 with NULL). Pass the
 * pointer back to hostRecSeatThink unchanged. */
const char *hostRecSeatPoolPlan(int srv, bool debug, uint8_t *poolMode);

/* The capture rule: does a seat take its pool capture this think. Pure,
 * so the seat's worker and hostRecSeatThink both call it. */
bool hostRecPoolWanted(uint8_t poolMode, uint32_t seq, int rrIndex,
                       int rrCount);

/* After a seat's think, on the main thread. seq: the input sequence the
 * think sent (the server records the last one it took). estTick: the
 * server tick it is thought to be now. debug: the server runs its brains
 * in debug mode (its brain manifest says so). rrIndex / rrCount: the
 * seat's place among the seats of srv that thought this tick, for the
 * round-robin pool capture. poolPrefetchExpr: the expression the seat's
 * worker evaluated after its think (NULL when it did not), and
 * poolPrefetched its result (NULL when the eval gave nothing). This call
 * always takes poolPrefetched and frees it. */
void hostRecSeatThink(int idx, int srv, uint8_t slot, uint16_t placeGen,
                      uint32_t seq, uint32_t estTick, float thinkMs,
                      struct lua_State *L, const OverlayCmdBuffer *ovl,
                      bool debug, int rrIndex, int rrCount,
                      char *poolPrefetched, const char *poolPrefetchExpr);

/* Once per host tick: the disk check, the uploads, the state reports. */
void hostRecService(uint32_t nowMs);

/* Close every file (the host stops). */
void hostRecShutdown(void);

/* Free bytes on the disk that holds path; false when unknown. */
bool hostRecFreeBytes(const char *path, uint64_t *out);

#endif /* BOTHOST_REC_H */
