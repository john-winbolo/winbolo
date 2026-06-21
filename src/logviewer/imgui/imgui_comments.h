/*
 * imgui_comments.h - ImGui WinBolo.net comments window for Log Viewer
 *
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef LV_IMGUI_COMMENTS_H
#define LV_IMGUI_COMMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

void lv_imgui_comments_init(void);
void lv_imgui_comments_shutdown(void);

/* Push the WBN key from the loaded log. Pass an empty string to clear.
 * Triggers a fresh fetch the next time the panel is visible. */
void lv_imgui_comments_set_key(const char *wbnKey);

void lv_imgui_comments_window(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_IMGUI_COMMENTS_H */
