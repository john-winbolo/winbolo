/*
 * WinBolo - The dedicated server's host bot capacity.
 *
 * How many bots this server can run in its own process at full strength.
 * The answer comes from the 2 s CPU speed test (src/cpu_speed) and the
 * capacity answer of the server's bot brain (brain_capacity.h), or from
 * -botcapacity N, which skips the test. It goes into the sim
 * (serverSimSetHostBotCapacity), which refuses lobby Add Bot requests past
 * it and tells clients the number in the lobby settings, and into
 * BotPoolStats.hostBotCapacity.
 *
 * The test runs once for each version: the first time a lobby starts. A
 * finished test is stored (cpu_speed_store.h; WINBOLO_CPU_SPEED_FILE puts
 * the file somewhere else), and later starts read the stored copy. The test
 * runs on low-priority worker threads and never holds the tick or the
 * network. When the game starts before the test finishes, the test stops
 * and stores nothing; the next lobby tries again.
 *
 * WinBoloDS only: it links the CPU speed test and the Lua brain handler.
 */

#ifndef WINBOLO_SERVER_BOT_CAPACITY_H
#define WINBOLO_SERVER_BOT_CAPACITY_H

#include "server_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once, after the sim and its bot brain path are set and before the
 * tick starts. override is the -botcapacity value (0..16), or -1 for none.
 * With an override the sim gets it at once and no test runs. */
void serverBotCapacityInit(ServerSim *sim, int override);

/* Call on the tick thread after each tick, outside the sim mutex. Cheap
 * when there is nothing to do. */
void serverBotCapacityPoll(ServerSim *sim);

/* Stops a test that runs and waits for the worker threads. Call after the
 * tick has stopped, before the sim is freed. */
void serverBotCapacityShutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_SERVER_BOT_CAPACITY_H */
