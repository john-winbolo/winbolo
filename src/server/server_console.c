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

/* Operator console command parsing for the dedicated server — see
 * server_console.h. Deliberately dependency-free: only the C library and
 * the ServerConsoleOps the caller supplies, so the unit tests can drive
 * every command without a server. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "server_console.h"

void strlower(char *s) {
  while (*s) {
    *s = (char)tolower((unsigned char)*s);
    s++;
  }
}

void serverConsolePrintHelp(void) {
  fprintf(stderr, "Help:\n Lock - Locks the server and stops new players from joining.\n Unlock - Unlocks the server and allows new players to join.\n savemap <map file> - Save the map file to path and file <map file>\n Say <text> - Sends this message to all players in the game unless they have turned off server messages.\n Quit - Exits the server.\n Info - Provide information about the current game\n Kick - Kicks a player. Case insensitive, prefix a * for WBN players.\n Host - Transfers the host role to a player. Case insensitive.\n Status - Returns list of players who aren't locked.\n");
}

/* savemap <file>: step over the command word and any whitespace, append
 * ".map" when the name doesn't already end in it, then hand the path to
 * the ops. `line` is the original-case copy of the operator's line and is
 * edited in place, so it must be a SERVER_CONSOLE_LINE-byte buffer. */
static void serverConsoleSaveMap(const ServerConsoleOps *ops, char *line) {
  char *ptr;
  int len;
  ptr = line;
  ptr += 7;

  /* Strip newline */
  len = (int) strlen(line);
  if (line[len-1] == '\n') {
    line[len-1] = '\0';
  }

  while (*ptr != '\0'  && (*ptr == '\t' || *ptr == ' ')) {
    ptr++;
  }
  if (*ptr == '\0') {
    fprintf(stderr, "Sorry, you must enter a filename for this command\n");
  } else {
    len = (int) strlen(ptr);
    {
      size_t remaining = SERVER_CONSOLE_LINE - (size_t)(ptr - line) - (size_t)len - 1;
      if (len < 4) {
        strncat(ptr, ".map", remaining);
      } else if (strcmp(ptr+len-4, ".map") != 0) {
        strncat(ptr, ".map", remaining);
      }
    }
    if (ops->saveMap(ptr) == false) {
      fprintf(stderr, "Sorry, an error occured saving the map. Is the path correct?\n");
    }
  }
}

/* Copy the player-name argument of a kick / host line into dst: at most
 * dstSize-1 characters, with the trailing end-of-line removed.
 *
 * The trim has to be guarded. The argument is empty when the operator
 * typed nothing after the command word and the line carries no newline —
 * a command piped in without a final newline, or stdin at EOF — and the
 * unguarded "strip the last character" this replaces then wrote a NUL
 * one byte in front of the buffer. Trimming only an actual end-of-line
 * also keeps the last character of a name that arrives without one. */
static void serverConsoleCopyName(char *dst, size_t dstSize, const char *arg) {
  size_t len;

  snprintf(dst, dstSize, "%s", arg);
  len = strlen(dst);
  while (len > 0 && (dst[len-1] == '\n' || dst[len-1] == '\r')) {
    len--;
    dst[len] = '\0';
  }
}

void serverConsoleDispatch(const ServerConsoleOps *ops, char *keyBuff,
                           char *saveBuff) {
  char playerKick[33] = "\0";
  char playerHost[33] = "\0";

  if (strncmp(keyBuff, "help", 4) == 0) {
    serverConsolePrintHelp();
  } else if (strncmp(keyBuff, "unlock", 6) == 0) {
    ops->setLock(false);
  } else if (strncmp(keyBuff, "lock", 4) == 0) {
    ops->setLock(true);
  } else if (strncmp(keyBuff, "info", 4) == 0) {
    ops->info();
  } else if (strncmp(keyBuff, "savemap", 7) == 0) {
    serverConsoleSaveMap(ops, saveBuff);
  } else if (strncmp(keyBuff, "say ", 4) == 0) {
    ops->say(keyBuff+4);
    {
        char pstr[256];
        int len = (int)strlen(keyBuff + 4);
        if (len > 0 && keyBuff[4 + len - 1] == '\n') len--;
        if (len > 255) len = 255;
        pstr[0] = (char)len;
        memcpy(pstr + 1, keyBuff + 4, len);
        ops->logSay(pstr);
    }
  } else if(strncmp(keyBuff, "status", 6) == 0){
    ops->status();
  } else if (strncmp(keyBuff, "kick ", 5) == 0) {
    serverConsoleCopyName(playerKick, sizeof(playerKick), keyBuff+5);
    ops->kick(playerKick);
  } else if (strncmp(keyBuff, "host ", 5) == 0) {
    bool hostSet;
    serverConsoleCopyName(playerHost, sizeof(playerHost), keyBuff+5);
    hostSet = ops->setHost(playerHost);
    if (hostSet) {
      printf("Host set to %s\n", playerHost);
    } else {
      printf("No such player\n");
    }
  } else if (strncmp(keyBuff, "quit", 4) == 0) {
    /* Caller's while-condition will exit on next check */
  } else if (strncmp(keyBuff, "\n", 1) != 0 && strncmp(keyBuff, "\0", 1) != 0) {
    fprintf(stderr, "Unknown command - Type \"help\" for help\n");
  }
}
