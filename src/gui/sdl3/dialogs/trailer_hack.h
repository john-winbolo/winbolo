/* Trailer/screenshot hack — fake internet game + lobby for marketing capture.
 * Toggle off (or git revert) before shipping. */

#ifndef TRAILER_HACK_H
#define TRAILER_HACK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ClientSim;

extern bool g_trailerHack;

/* Build a fake 16-player lobby into a freshly-created ClientSim. */
void trailerHackBuildFakeLobby(struct ClientSim *cs);

/* Compressed map bytes for the current trailer fixture (lobby preview). */
bool trailerHackGetCurrentMapData(const unsigned char **outData, int *outLen);

/* Rotate to the next preloaded map; updates cs->mapName + counts. */
void trailerHackAdvanceMap(struct ClientSim *cs);

/* User clicked Ready; toggles ready and starts/stops the local countdown. */
void trailerHackOnReady(struct ClientSim *cs, bool ready);

/* Decrement countdownSeconds based on wall clock. Returns true when the
 * countdown has reached zero and the lobby should exit to main menu. */
bool trailerHackTickCountdown(struct ClientSim *cs);

#ifdef __cplusplus
}
#endif

#endif
