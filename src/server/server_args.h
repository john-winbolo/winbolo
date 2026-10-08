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

/* The dedicated server's -mod, -mod-required, -mod-locked and -setting,
 * read off the command line.
 *
 * Split out of servermain.c so the unit tests reach it without linking
 * main(): the comma split, the blank and missing values, the -mod-locked
 * exclusivity and the -setting value read are all decided here, and
 * servermain.c only prints what it says. What the flags record goes in
 * through the sim's own setters on server_sim.h (serverSimAddOperatorMod,
 * serverSimSetScriptSetting, serverSimRecordOperatorSetting), so no
 * command-line text reaches the sim's API. Public headers only. Compiled
 * into WinBoloDS and WinBoloUnitTests. */

#ifndef WINBOLO_SERVER_ARGS_H
#define WINBOLO_SERVER_ARGS_H

#include <stdbool.h>

#include "server_sim.h"

/* What serverArgsApplyMods answers. */
typedef enum {
    SERVER_MOD_ARGS_OK       = 0,
    SERVER_MOD_ARGS_CONFLICT = 1  /* -mod-locked beside -mod or
                                     -mod-required; nothing recorded */
} ServerModArgsResult;

/* The line the dedicated server prints, and exits on, for
 * SERVER_MOD_ARGS_CONFLICT. */
#define SERVER_MOD_ARGS_CONFLICT_TEXT \
    "Error: -mod-locked cannot be combined with -mod or -mod-required"

/* Where the functions below send each warning and note: one whole line, no
 * newline. */
typedef void (*ServerArgsSay)(void *ctx, const char *line);

/*********************************************************
 *NAME:          serverArgsModGiven /
 *               serverArgsModConflict
 *PURPOSE:
 *  Given: whether argv holds -mod, -mod-required or
 *  -mod-locked at all, value or no. Conflict: whether it
 *  holds -mod-locked and also -mod or -mod-required, which
 *  the dedicated server refuses to start with. Flags match
 *  as argExist matches them: a leading '-', case ignored.
 *********************************************************/
bool serverArgsModGiven(int argc, const char *const *argv);
bool serverArgsModConflict(int argc, const char *const *argv);

/*********************************************************
 *NAME:          serverArgsApplyMods
 *PURPOSE:
 *  The dedicated server's -mod, -mod-required and
 *  -mod-locked, read off argv into serverSimAddOperatorMod
 *  in the order given. Each flag takes the next argument as
 *  a comma-separated list of names; spaces round a name and
 *  blank names ("a,,b", a trailing comma) are skipped. A
 *  flag with no value (the last argument, or followed by
 *  another flag) and a value longer than the parse buffer
 *  are warned about and skipped. A name that does not
 *  resolve is warned about and skipped.
 *
 *  Answers SERVER_MOD_ARGS_CONFLICT, recording nothing and
 *  saying SERVER_MOD_ARGS_CONFLICT_TEXT, when -mod-locked
 *  is given beside -mod or -mod-required. say may be NULL.
 *********************************************************/
ServerModArgsResult serverArgsApplyMods(ServerSim *sim, int argc,
                                        const char *const *argv,
                                        ServerArgsSay say, void *ctx);

/*********************************************************
 *NAME:          serverArgsApplySetting
 *PURPOSE:
 *  The dedicated server's -setting "[file:]id=value": a
 *  value for one of a script's own settings, chosen as the
 *  lobby host's CMD_SET_SCRIPT_SETTING would choose it.
 *  defaultFile is the map's own script, used when arg names
 *  no file. The file may be named the way -mod names one:
 *  as given or without .scenario.lua, .lua or .scenario,
 *  case ignored (serverSimResolveScriptName).
 *
 *  The value is read against the setting's declaration: a
 *  number for any type, true or false (or on or off) for a
 *  bool, one of the words for a choice.
 *
 *  When the file is one of the operator's rows (-mod,
 *  -mod-required, -mod-locked), the value is also recorded
 *  (serverSimRecordOperatorSetting), so it is put back at
 *  every new lobby, and on a fixed row the host cannot
 *  change it.
 *
 *  Says on say what it did, or why it did nothing, one line;
 *  say may be NULL. Answers whether the value was set.
 *********************************************************/
bool serverArgsApplySetting(ServerSim *sim, const char *arg,
                            const char *defaultFile, ServerArgsSay say,
                            void *ctx);

#endif /* WINBOLO_SERVER_ARGS_H */
