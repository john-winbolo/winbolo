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

/* Replay-log path composition, split into its own translation unit so it
 * can be unit-tested without dragging in server_dedicated_log.c's
 * winbolonet / upload / log-writer dependencies. Pure: no globals, no
 * time/RNG, only SDL_GetPathInfo for the cross-platform directory probe. */

#include <stdbool.h>
#include <string.h>

#include <SDL3/SDL.h>   /* SDL_GetPathInfo for -log <dir> detection */

#include "server_dedicated_log.h"

/* Compose the final replay path from the -log argument value.
 *
 *   - logArg == ""             -> use autoBase verbatim (auto-name in cwd)
 *   - logArg is an existing dir -> autoBase placed *inside* the dir
 *                                  (e.g. -log /tmp -> /tmp/<autoBase>)
 *   - logArg is anything else   -> use logArg verbatim (explicit file)
 *
 * A .wbv extension is appended if not already present. autoBase is the
 * caller-generated <timestamp>_<map> base name. The directory probe uses
 * SDL_GetPathInfo to stay cross-platform, matching serverSimScanMapDir's
 * use of SDL_GlobDirectory. */
void serverDedicatedLogComposePath(const char *logArg, const char *autoBase,
                                   char *out, size_t outSize) {
    bool argIsDir = false;

    if (logArg != NULL && logArg[0] != '\0') {
        SDL_PathInfo info;
        if (SDL_GetPathInfo(logArg, &info) &&
            info.type == SDL_PATHTYPE_DIRECTORY) {
            argIsDir = true;
        }
    }

    if (argIsDir) {
        /* Auto-name the file inside the supplied directory. */
        size_t dlen;
        strncpy(out, logArg, outSize - 1);
        out[outSize - 1] = '\0';
        dlen = strlen(out);
        if (dlen > 0 && out[dlen - 1] != '/' && out[dlen - 1] != '\\') {
            strncat(out, "/", outSize - dlen - 1);
            dlen++;
        }
        strncat(out, autoBase, outSize - dlen - 1);
    } else if (logArg != NULL && logArg[0] != '\0') {
        /* Explicit file path — use as given. */
        strncpy(out, logArg, outSize - 1);
        out[outSize - 1] = '\0';
    } else {
        /* No name supplied — auto-name in the cwd. */
        strncpy(out, autoBase, outSize - 1);
        out[outSize - 1] = '\0';
    }

    {
        size_t flen = strlen(out);
        if (flen <= 4 || strcmp(out + flen - 4, ".wbv") != 0) {
            strncat(out, ".wbv", outSize - flen - 1);
        }
    }
}
