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
*Name:          gui_message
*Filename:      gui_message.c
*Purpose:
*  Platform-independent message box abstraction.
*********************************************************/

#include <stdio.h>
#include "gui_message.h"

static gui_message_fn g_messageHandler = NULL;

void guiMessageSetHandler(gui_message_fn handler) {
  g_messageHandler = handler;
}

void guiMessageShow(const char *message, const char *title) {
  if (g_messageHandler != NULL) {
    g_messageHandler(message, title);
  } else {
    fprintf(stderr, "[%s] %s\n", title ? title : "Message", message ? message : "");
  }
}
