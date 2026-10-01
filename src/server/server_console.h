/*
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

/* Operator console command set for the dedicated server.
 *
 * The line parsing lives here, split out of servermain.c so it can be
 * unit-tested without linking main() and the whole server runtime. The
 * things a command actually does — take the sim mutex, kick a player,
 * broadcast a message — are reached through ServerConsoleOps, which
 * servermain.c fills in with the real server calls and a test fills in
 * with recorders. */

#ifndef WINBOLO_SERVER_CONSOLE_H
#define WINBOLO_SERVER_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Size of the line buffers serverConsoleDispatch is handed. Both buffers
 * must be this big: savemap appends ".map" to its argument in place. */
#define SERVER_CONSOLE_LINE 256

/* What each command does. Every entry is called with the sim mutex NOT
 * held — taking it is the implementation's job, so that the whole of a
 * command's server work happens under one lock the way it did when these
 * calls were inline in servermain.c. */
typedef struct {
  /* lock / unlock — stop or allow new players joining. */
  void (*setLock)(bool locked);
  /* info — print the current game's details to the console. */
  void (*info)(void);
  /* savemap <file> — write the live map to `path` (already suffixed
   * ".map"). Returns false if the save failed. */
  bool (*saveMap)(const char *path);
  /* say <text> — send `text` to every player. */
  void (*say)(const char *text);
  /* say <text> — record the message in the replay log. `pstr` is a Bolo
   * pascal string: a length byte followed by that many characters. */
  void (*logSay)(const char *pstr);
  /* status — list the players who aren't locked. */
  void (*status)(void);
  /* kick <name> — disconnect the named player. */
  void (*kick)(const char *name);
  /* host <name> — hand the host role to the named player. Returns false
   * if there is no such player. */
  bool (*setHost)(const char *name);
  /* reload — read the scenario script beside the map again. Writes one
   * line into msg either way: what was re-read, or why nothing was.
   * Returns false when nothing changed.
   *
   * Last on purpose. Both the server's list and the test's are positional,
   * so a field inserted above this one would quietly point every entry
   * after it at its neighbour's implementation. */
  bool (*reloadScenario)(char *msg, size_t msgLen);
} ServerConsoleOps;

/* Lower-case `s` in place. */
void strlower(char *s);

/* Print the console command list to stderr. */
void serverConsolePrintHelp(void);

/* Run one console command line.
 *
 * keyBuff is the lower-cased line the command word is matched against;
 * saveBuff is the same line with the operator's original capitalisation,
 * which savemap needs for the file path and say for the message text.
 * Both are SERVER_CONSOLE_LINE-byte buffers and may be modified. */
void serverConsoleDispatch(const ServerConsoleOps *ops, char *keyBuff,
                           char *saveBuff);

#ifndef _WIN32
/* What serverConsoleReadLine found on the console. */
typedef enum {
  SERVER_CONSOLE_READ_LINE,     /* a line was read into buf */
  SERVER_CONSOLE_READ_TIMEOUT,  /* nothing arrived in time; buf is empty */
  SERVER_CONSOLE_READ_EOF       /* the stream ended; buf is empty and the
                                 * caller must stop reading it */
} ServerConsoleRead;

/* Wait up to timeoutSecs for a line on `stream` and read it into buf.
 *
 * buf is always left with a valid string — empty on anything but a line —
 * so a caller that dispatches whatever is in it cannot re-run the command
 * it read last time round. The EOF answer is the one that matters: on a
 * stream that has ended, select() reports the descriptor readable for
 * ever and the read fails immediately, so a caller that ignores it spins.
 *
 * Windows reads the console on a background thread instead (see
 * servermain.c) because select() does not work on console handles there.
 */
ServerConsoleRead serverConsoleReadLine(FILE *stream, char *buf,
                                        size_t bufSize, int timeoutSecs);
#endif /* !_WIN32 */

#endif /* WINBOLO_SERVER_CONSOLE_H */
