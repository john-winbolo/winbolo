/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sentry_integration.h"

#include <string.h>

static int hasArg(int argc, char *argv[], const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return 1;
    }
    return 0;
}

#ifdef HAVE_SENTRY
#include <stdio.h>
#include <stdlib.h>
#include <sentry.h>
#include <SDL3/SDL.h>

int sentryInit(const char *executable_name, int argc, char *argv[]) {
#ifndef SENTRY_DSN
    (void)executable_name;
    (void)argc;
    (void)argv;
    return 0;
#else
    if (hasArg(argc, argv, "-nocrashreporting")) {
        return 0;
    }

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options, SENTRY_DSN);

    char release[128];
    snprintf(release, sizeof(release), "%s@%s", executable_name, WINBOLO_VERSION);
    sentry_options_set_release(options, release);

    /* Build an absolute path for the sentry database in the per-user
     * writable pref directory, NOT next to the executable. On macOS
     * SDL_GetBasePath() resolves inside the signed .app bundle
     * (Contents/Resources/), so writing the sentry DB there adds files
     * under the sealed bundle and invalidates the code signature at
     * runtime ("a sealed resource is missing or invalid"), making the
     * app fail Gatekeeper on subsequent launches. SDL_GetPrefPath is the
     * convention used everywhere else in the project for writable data. */
    char *base = SDL_GetPrefPath("WinBolo", "WinBolo");
    if (base) {
        size_t len = strlen(base) + sizeof(".sentry-native");
        char *db = (char *)malloc(len);
        if (db) {
            snprintf(db, len, "%s.sentry-native", base);
            sentry_options_set_database_path(options, db);
            free(db);
        }
        SDL_free(base);
    }

    int rv = sentry_init(options);
    if (rv == 0) {
        sentry_set_tag("executable", executable_name);
    }
    return rv;
#endif
}

void sentryClose(void) {
    sentry_close();
}

#else

int sentryInit(const char *executable_name, int argc, char *argv[]) {
    (void)executable_name;
    (void)argc;
    (void)argv;
    return 0;
}
void sentryClose(void) {}

#endif
