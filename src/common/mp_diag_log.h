/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * mp_diag_log.h — temporary diagnostic logger for the MP host/joiner
 * regression in the reliable-control-events seam.
 *
 * Output file (in the process's current working directory):
 *   $MP_DIAG_LOG_FILE if the env var is set and non-empty,
 *   otherwise "mp-logging-<pid>.txt".
 *
 * The pid-suffix default lets you run two clients on the same machine
 * without their log files trampling each other; set MP_DIAG_LOG_FILE
 * explicitly (e.g. "mp-host.txt" / "mp-joiner.txt") when you want
 * friendly names.
 *
 * Intentionally separate from wb_log so the output is easy to grep
 * and the whole helper can be torn out after the bug is found.
 *
 * Thread-safe (mutex around fwrite + fflush).  Lazy-opens the file on
 * first call.  Flushes after every line so a crash doesn't lose tail.
 */

#ifndef _MP_DIAG_LOG_H
#define _MP_DIAG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Enable/disable gate.  Default OFF so the welcome-screen bg game (which
 * runs a full ServerSim and publishes through the same bus) doesn't fill
 * the log with noise.  Flip ON when the actual MP host server starts,
 * back OFF when it tears down.
 *
 * Wire-side logs in transport_udp_server.c / transport_udp_client.c only
 * fire when the UDP transport is actually running, so they're naturally
 * MP-only and don't strictly need this gate — but they honour it anyway
 * so a stray bg-publish that somehow reaches the wire path stays silent. */
void mpDiagLogEnable(int enable);
int  mpDiagLogIsEnabled(void);

/* Write one diagnostic line.  No-op when disabled. */
void mpDiagLog(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* _MP_DIAG_LOG_H */
