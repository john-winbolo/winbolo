/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 * Name:          imgui_onboarding.h
 * Purpose:       First-run online onboarding wizard shown
 *                once on the first Internet / LAN entry.
 *********************************************************/

#ifndef IMGUI_ONBOARDING_H
#define IMGUI_ONBOARDING_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show the first-run online onboarding wizard as a blocking modal
 * loop (own ImGui context + SDL event loop, modeled on
 * imguiKeySetupShow). Returns 1 if the user finished or skipped the
 * wizard (caller proceeds into the online flow), 0 if they closed or
 * quit the dialog (caller returns to the welcome screen). Marks
 * onboarding complete on the finish and skip paths only. */
int imguiOnboardingShow(void);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_ONBOARDING_H */
