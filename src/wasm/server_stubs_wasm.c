/*
 * server_stubs_wasm.c — wasm-only stubs for server-frontend symbols
 *
 * server_sim.c and the shared lobby dialog are linked into the wasm
 * client but reference symbols that only exist in the real server
 * frontend (servermain.c) or in libplum-backed code paths that the
 * wasm build never exercises:
 *
 *   - isLogging / dontSendLog / makeLogFileName: server-side game log
 *     writing. The wasm client never hosts a server log, so the
 *     globals stay FALSE and logStart() is fed an empty filename so
 *     it fails cleanly.
 *
 *   - serverInstanceGetPortmapInfo / TriggerManualProbe /
 *     GetManualProbeState: NAT/UPnP probe state owned by the
 *     server-instance lifecycle. The wasm client never hosts a server,
 *     so report idle/empty state.
 */

#include <stddef.h>
#include <string.h>
#include "../bolo/global.h"
#include "../server/server_lifecycle.h"

bool isLogging = FALSE;
bool dontSendLog = TRUE;

void makeLogFileName(char *outFileName, const char *mapName) {
  (void)mapName;
  if (outFileName) outFileName[0] = '\0';
}

void serverInstanceGetPortmapInfo(ServerPortmapInfo *out) {
  if (out) memset(out, 0, sizeof(*out));
}

void serverInstanceTriggerManualProbe(void) {
}

ManualProbeState serverInstanceGetManualProbeState(void) {
  return MANUAL_PROBE_IDLE;
}
