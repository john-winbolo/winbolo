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

/*********************************************************
 *Name:          Lobby Script Rows
 *Filename:      lobby_script_rows.h
 *Purpose:
 *  The left column of the lobby's Mods chooser lists two
 *  places at once: what the server offers, and the scripts
 *  in the player's own Mods directory. This says, for each
 *  row, which of the two holds it — the server alone, both,
 *  or this computer alone — so the chooser can offer to send
 *  a file the server does not have.
 *
 *  Plain C and apart from the chooser so the unit tests,
 *  which link no lobby UI, can hold the matching rules down.
 *********************************************************/

#ifndef LOBBY_SCRIPT_ROWS_H
#define LOBBY_SCRIPT_ROWS_H

#include <stdbool.h>

#include "server_sim.h" /* ServerScenarioEntry */

typedef enum {
    LOBBY_SCRIPT_ROW_SERVER_ONLY,  /* the server offers it; this computer
                                      does not hold it */
    LOBBY_SCRIPT_ROW_BOTH,         /* the server offers it and this
                                      computer holds it too */
    LOBBY_SCRIPT_ROW_LOCAL_ONLY    /* only this computer holds it */
} LobbyScriptRowState;

/* One row of the merged column. serverIdx indexes the server list and is -1
 * for a LOCAL_ONLY row; localIdx indexes the local list and is -1 for a
 * SERVER_ONLY row. */
typedef struct {
    LobbyScriptRowState state;
    int                 serverIdx;
    int                 localIdx;
} LobbyScriptRow;

/* Merges the server's list and this computer's into out, at most max rows,
 * and answers how many were written.
 *
 * A server row and a local row are the same script when both carry a
 * non-zero workshopId and the two ids are equal. When either id is 0 they
 * are the same script when their file names match ignoring case. Two rows
 * with different non-zero ids are different scripts whatever they are
 * called.
 *
 * The server rows come first, in the server's order, each paired with the
 * first local row it matches. The local rows no server row matches follow,
 * in the local order.
 *
 * inProcess true is a server in this process, whose list already holds
 * every file this computer has, so the local list is not read at all and
 * every row is SERVER_ONLY.
 *
 * Either list may be NULL with a count of 0. Answers 0 for a NULL out or a
 * max under 1. */
int lobbyScriptRowsClassify(const ServerScenarioEntry *server, int serverCount,
                            const ServerScenarioEntry *localRows, int localCount,
                            bool inProcess, LobbyScriptRow *out, int max);

#endif /* LOBBY_SCRIPT_ROWS_H */
