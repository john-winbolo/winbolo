/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CLIENT_FRONTEND_TUTORIAL_H
#define CLIENT_FRONTEND_TUTORIAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The tutorial step sequencer, one body in client_frontend_common.c for the
 * desktop, web and iOS drivers. The step data is tutorialSteps[] in
 * bolo/tutorial.c, the trigger is the sim calling frontEndTutorial(pos)
 * (frontend.h, as is frontEndTutorialReset), and the messages are shown by
 * imgui_tutorial_overlay.cpp. Main thread only. */

/* Count a presented frame. Call once a frame straight after the present: the
 * intro step waits for a few, so its message opens over the game rather than
 * an empty backbuffer. */
void frontEndTutorialNotePresentedFrame(void);

/* Show the once-per-run respawn message when the hosted server has flagged
 * the tutorial tank's death. Call once a frame with the client mutex free. */
void clientFrontTutorialPollRespawn(void);

/* True from the moment a step's messages are queued until the player has
 * dismissed the last of them. A driver stops catching up game ticks while it
 * is true, so the tick that raised the step is the last one to run. */
bool clientFrontTutorialStepShowing(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_FRONTEND_TUTORIAL_H */
