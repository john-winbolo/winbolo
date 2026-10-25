/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          imgui_connecting.h
 * Purpose:       The "Connecting to <host>..." screen the
 *                iOS client shows while a join waits on the
 *                network: the server's info reply, the
 *                WinBolo.net token check and the join
 *                itself. The waiting is done by the
 *                caller's step function, called once a
 *                frame; this screen only draws, pumps
 *                events and offers Cancel. Built for iOS
 *                only.
 *********************************************************/

#ifndef IMGUI_CONNECTING_H
#define IMGUI_CONNECTING_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  CONNECTING_STEP_WAITING = 0,  /* not finished: call again next frame */
  CONNECTING_STEP_DONE          /* finished, whatever the outcome */
} ConnectingStep;

/* Called once a frame. *stageOut names what is being waited on (shown under
 * the title); *progressOut is a 0..1 fraction to draw as a bar, or below 0
 * for none. */
typedef ConnectingStep (*ConnectingStepFn)(void *ctx, const char **stageOut,
                                          float *progressOut);

/* Runs the screen until step reports CONNECTING_STEP_DONE (returns true) or
 * the player presses Cancel or quits the application (returns false; ask
 * windowIsQuitting() to tell the two apart). */
bool imguiConnectingRun(const char *host, ConnectingStepFn step, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_CONNECTING_H */
