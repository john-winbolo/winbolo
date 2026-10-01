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

#include "lobby_script_rows.h"

#include <SDL3/SDL.h>

/* Whether two rows are the same script. The Workshop id decides when both
   rows have one, because a Workshop item keeps its id through a rename and
   two items may share a file name. A row without one is matched by name,
   ignoring case, the way the server's own listing folds two directories'
   copies of one name together. */
static bool lobbyScriptRowsSame(const ServerScenarioEntry *a,
                                const ServerScenarioEntry *b) {
    if (a->workshopId != 0 && b->workshopId != 0) {
        return a->workshopId == b->workshopId;
    }
    return SDL_strcasecmp(a->file, b->file) == 0;
}

int lobbyScriptRowsClassify(const ServerScenarioEntry *server, int serverCount,
                            const ServerScenarioEntry *localRows, int localCount,
                            bool inProcess, LobbyScriptRow *out, int max) {
    int n = 0;
    int i, j;

    if (out == NULL || max < 1) return 0;
    if (server == NULL || serverCount < 0) serverCount = 0;
    if (localRows == NULL || localCount < 0 || inProcess) localCount = 0;

    for (i = 0; i < serverCount && n < max; i++) {
        int match = -1;

        for (j = 0; j < localCount; j++) {
            if (lobbyScriptRowsSame(&server[i], &localRows[j])) {
                match = j;
                break;
            }
        }
        out[n].state     = (match >= 0) ? LOBBY_SCRIPT_ROW_BOTH
                                        : LOBBY_SCRIPT_ROW_SERVER_ONLY;
        out[n].serverIdx = i;
        out[n].localIdx  = match;
        n++;
    }

    /* A local row is the server's when any server row matches it, not only
       the one that took it above: two server rows can match one local row by
       Workshop id, and the second must not leave that file offered to send. */
    for (j = 0; j < localCount && n < max; j++) {
        bool held = false;

        for (i = 0; i < serverCount; i++) {
            if (lobbyScriptRowsSame(&server[i], &localRows[j])) {
                held = true;
                break;
            }
        }
        if (held) continue;
        out[n].state     = LOBBY_SCRIPT_ROW_LOCAL_ONLY;
        out[n].serverIdx = -1;
        out[n].localIdx  = j;
        n++;
    }
    return n;
}
