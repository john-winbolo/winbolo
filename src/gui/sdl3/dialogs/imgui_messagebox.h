/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          imgui_messagebox.h
 * Purpose:       ImGui message box dialog with icon and
 *                button configuration support.
 *********************************************************/

#ifndef IMGUI_MESSAGEBOX_H
#define IMGUI_MESSAGEBOX_H

#include "../tutorial_text.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message type — determines which icon is shown. */
typedef enum {
    IMGUI_MSG_INFO,
    IMGUI_MSG_WARNING,
    IMGUI_MSG_ERROR,
    IMGUI_MSG_NONE
} ImguiMsgType;

/* Button configuration. */
typedef enum {
    IMGUI_MSG_OK,
    IMGUI_MSG_YES_NO,
    IMGUI_MSG_YES_NO_CANCEL
} ImguiMsgButtons;

/* Return values. */
#define IMGUI_MSG_RESULT_OK      0
#define IMGUI_MSG_RESULT_YES     0
#define IMGUI_MSG_RESULT_NO      1
#define IMGUI_MSG_RESULT_CANCEL  2

/* Show a blocking ImGui message box.
 * Returns IMGUI_MSG_RESULT_OK/YES/NO/CANCEL. */
int imguiMessageBoxEx(const char *title, const char *message,
                      ImguiMsgType type, ImguiMsgButtons buttons);

/* Same as imguiMessageBoxEx but the message body is a sequence of
 * TutorialSeg entries — text runs interleaved with PNG glyphs and
 * procedural keycaps for inline button hints.  Word wrap respects
 * each glyph as a single square element of 2 * GetTextLineHeight(). */
int imguiMessageBoxRich(const char *title,
                        const TutorialSeg *segments, int segmentCount,
                        ImguiMsgType type, ImguiMsgButtons buttons);

/* Convenience: blocking OK-only info message box (legacy API). */
void imguiMessageBox(const char *message, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* IMGUI_MESSAGEBOX_H */
