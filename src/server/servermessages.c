/*
 * $Id$
 *
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


/*********************************************************
*Name:          ServerMessages
*Filename:      ServerMessages.c
*Author:        John Morrison
*Creation Date:  10/8/99
*Last Modified:  10/8/99
*Purpose:
*  Responsable for messages.
*********************************************************/

#include <stdio.h>
#include <string.h>
#include "server_sim.h"
#include "messages.h"

void serverMessageSetQuietMode(ServerSim *sim, bool modeOn) {
  serverSimSetQuiet(sim, modeOn);
}

void serverMessagesSetLogFile(ServerSim *sim, char *logFile) {
  serverSimSetMessageLogFile(sim, logFile);
}

void serverMessageAdd(ServerSim *sim, messageType msgType, char *top, char *bottom) {
  FILE *fp;
  if (msgType != assistantMessage && !serverSimIsQuiet(sim)) {
    if (!serverSimGetServerMessageUseLogFile(sim)) {
      fprintf(stdout, "%s\n%s\n", top, bottom);
     } else {
      fp = fopen(serverSimGetServerMessageLogFile(sim), "a");
      if (fp) {
        fprintf(fp, "%s\n%s\n", top, bottom);
        fclose(fp);
      }
    }
  }
}

void serverMessageConsoleMessage(ServerSim *sim, char *msg) {
  FILE *fp;
  if (!serverSimIsQuiet(sim)) {
    if (!serverSimGetServerMessageUseLogFile(sim)) {
      fprintf(stderr, "%s\n", msg);
    } else {
      fp = fopen(serverSimGetServerMessageLogFile(sim), "a");
      if (fp) {
        fprintf(fp, "%s\n", msg);
        fclose(fp);
      }
    }
  }
}
