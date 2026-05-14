/*
 * Copyright (c) 1998-2008 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*********************************************************
 *Name:          Command-stream parser
 *Filename:      cmd_stdin.h
 *Purpose:
 *  JSON-per-line command stream for scripted scenario
 *  drivers in WinBoloHeadless (--cmd-stdin) and
 *  WinBoloDS (-cmd-stdin). Lines have shape
 *
 *    {"tick": 100, "op": "add_bot"}
 *    {"tick": 110, "op": "set_team", "slot": 0, "team": 1}
 *    {"tick": 130, "op": "alliance_request", "from": 0, "to": 1}
 *    {"tick": 150, "op": "name_change", "slot": 0, "name": "Alpha"}
 *
 *  The parser is intentionally hand-rolled (flat objects,
 *  no nested objects/arrays) so neither binary picks up a
 *  JSON-library dependency on the test path. Comment lines
 *  starting with '#' and blank lines are skipped. Malformed
 *  lines abort the binary with exit code 2.
 *
 *  Tick-based dispatch is the caller's job: cmdStdinPeek
 *  exposes the next pending command, cmdStdinConsume
 *  advances when the caller's tick has reached the queued
 *  command's tick.
 *********************************************************/

#ifndef CMD_STDIN_H
#define CMD_STDIN_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "global.h"
#include "wire_limits.h"

typedef enum {
  CMD_OP_ADD_BOT,
  CMD_OP_SET_TEAM,
  CMD_OP_SET_READY,
  CMD_OP_NAME_CHANGE,
  CMD_OP_ALLIANCE_REQUEST,
  CMD_OP_ALLIANCE_ACCEPT,
  CMD_OP_ALLIANCE_LEAVE,
  CMD_OP_MAP_SKIP_VOTE,
  CMD_OP_START_GAME,
  CMD_OP_REAPPLY_ALLIANCES,
  CMD_OP_SHUTDOWN,
  CMD_OP_EXIT,
  CMD_OP__COUNT
} CmdOp;

typedef struct CmdLine {
  uint32_t tick;
  CmdOp    op;
  BYTE     slot;                          /* set_team / set_ready / name_change */
  BYTE     team;                          /* set_team */
  bool     ready;                         /* set_ready */
  BYTE     from;                          /* alliance_request / alliance_accept */
  BYTE     to;                            /* alliance_request / alliance_accept */
  char     name[PACKET_MAX_PLAYER_NAME];  /* name_change */
  int      lineNumber;                    /* 1-based, for error messages */
} CmdLine;

typedef struct CmdStdin CmdStdin;

/* Open a command stream from the given path. "-" means stdin.
 * Returns NULL on failure (no file, no memory). The harness
 * binary should treat NULL as an immediate exit-with-error. */
CmdStdin *cmdStdinOpen(const char *path);

/* Close and free. Safe to pass NULL. */
void cmdStdinClose(CmdStdin *cs);

/* Look at the next pending command without removing it. Returns
 * true with *out filled if a command is queued; false if EOF.
 * A malformed line is fatal: prints to stderr and exits 2
 * (determinism matters more than recovery). */
bool cmdStdinPeek(CmdStdin *cs, CmdLine *out);

/* Drop the next pending command. Caller invokes after the peek-
 * returned command has been dispatched. */
void cmdStdinConsume(CmdStdin *cs);

/* Convert a CmdOp to a stable string. Used for error messages
 * and (sparingly) for stderr diagnostics in scenarios that
 * exercise bad-op-for-mode error paths. */
const char *cmdOpName(CmdOp op);

#endif /* CMD_STDIN_H */
