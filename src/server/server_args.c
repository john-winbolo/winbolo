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

/* The dedicated server's -mod flags and -setting, read off the command
 * line — see server_args.h. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "control_event.h"      /* LOBBY_SCENARIO_FILE_LEN */
#include "scenario_settings.h"  /* ScnSetting and the blob it is read from */
#include "server_sim.h"
#include "server_args.h"

static const struct {
    const char       *flag;
    ServerModStrength strength;
} kServerModFlags[] = {
    { "mod",          SERVER_MOD_DEFAULT  },
    { "mod-required", SERVER_MOD_REQUIRED },
    { "mod-locked",   SERVER_MOD_LOCKED   },
};

/* Which of the three flags arg is, or -1: "-<flag>" with case ignored, the
   rule argExist uses for every other flag. */
static int serverModFlagOf(const char *arg) {
    int f;

    if (arg == NULL || arg[0] != '-') return -1;
    for (f = 0; f < (int)SDL_arraysize(kServerModFlags); f++) {
        if (SDL_strcasecmp(arg + 1, kServerModFlags[f].flag) == 0) return f;
    }
    return -1;
}

static void serverArgsSay(ServerArgsSay say, void *ctx,
                          SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
    SDL_PRINTF_VARARG_FUNC(3);

static void serverArgsSay(ServerArgsSay say, void *ctx, const char *fmt,
                          ...) {
    char    line[LOBBY_SCENARIO_FILE_LEN + 640];
    va_list ap;

    if (say == NULL) return;
    va_start(ap, fmt);
    SDL_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    say(ctx, line);
}

bool serverArgsModGiven(int argc, const char *const *argv) {
    int i;

    for (i = 1; i < argc; i++) {
        if (serverModFlagOf(argv[i]) >= 0) return true;
    }
    return false;
}

bool serverArgsModConflict(int argc, const char *const *argv) {
    bool locked = false;
    bool loose  = false;
    int  i;

    for (i = 1; i < argc; i++) {
        int f = serverModFlagOf(argv[i]);

        if (f < 0) continue;
        if (kServerModFlags[f].strength == SERVER_MOD_LOCKED) {
            locked = true;
        } else {
            loose = true;
        }
    }
    return locked && loose;
}

ServerModArgsResult serverArgsApplyMods(ServerSim *sim, int argc,
                                        const char *const *argv,
                                        ServerArgsSay say, void *ctx) {
    int i;

    if (serverArgsModConflict(argc, argv)) {
        /* Refused whole rather than merged: a locked list is a list nobody
           edits, and a -mod or -mod-required beside it asks for one that a
           host does edit. Which the operator meant is theirs to say. */
        serverArgsSay(say, ctx, "%s", SERVER_MOD_ARGS_CONFLICT_TEXT);
        return SERVER_MOD_ARGS_CONFLICT;
    }
    if (sim == NULL) return SERVER_MOD_ARGS_OK;

    for (i = 1; i < argc; i++) {
        int               f = serverModFlagOf(argv[i]);
        const char       *flag;
        ServerModStrength strength;
        char              tmp[1024];
        char             *tok;
        char             *next;

        if (f < 0) continue;
        flag     = kServerModFlags[f].flag;
        strength = kServerModFlags[f].strength;
        /* Even with nothing usable named, -mod-locked says the list is the
           operator's, and an operator who wrote it meant that much; the
           empty name records exactly that (serverSimAddOperatorMod). */
        if (i + 1 >= argc || argv[i + 1][0] == '-') {
            serverArgsSay(say, ctx, "Warning: -%s needs a mod name; ignored",
                          flag);
            if (strength == SERVER_MOD_LOCKED) {
                serverSimAddOperatorMod(sim, "", strength, NULL, 0);
            }
            continue;
        }
        /* Said and skipped rather than cut: a cut list ends in part of a
           name, which would resolve to the wrong file or to none. Ten names
           of a file name's length fit with room to spare. */
        if (strlen(argv[i + 1]) >= sizeof(tmp)) {
            serverArgsSay(say, ctx,
                          "Warning: -%s list is longer than %d characters; "
                          "ignored", flag, (int)sizeof(tmp) - 1);
            if (strength == SERVER_MOD_LOCKED) {
                serverSimAddOperatorMod(sim, "", strength, NULL, 0);
            }
            continue;
        }
        SDL_snprintf(tmp, sizeof(tmp), "%s", argv[i + 1]);
        /* Split by hand rather than with strtok, which holds its place in a
           static that the -lock parse in servermain.c also uses. */
        for (tok = tmp; tok != NULL; tok = next) {
            char  err[512];
            char *end;

            next = strchr(tok, ',');
            if (next != NULL) *next++ = '\0';
            while (*tok == ' ' || *tok == '\t') tok++;
            end = tok + strlen(tok);
            while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
                *--end = '\0';
            }
            if (tok[0] == '\0') {
                /* "a,,b" or a trailing comma: nothing named, nothing said —
                   except under -mod-locked, for the reason above. */
                if (strength == SERVER_MOD_LOCKED) {
                    serverSimAddOperatorMod(sim, "", strength, NULL, 0);
                }
                continue;
            }
            if (!serverSimAddOperatorMod(sim, tok, strength, err,
                                         sizeof(err))) {
                serverArgsSay(say, ctx, "Warning: -%s %s: %s; skipped", flag,
                              tok, err);
            }
        }
    }

    if (serverSimGetOperatorModsLocked(sim) &&
        serverSimGetOperatorModCount(sim) == 0) {
        serverArgsSay(say, ctx,
                      "Note: -mod-locked named nothing that loads; the "
                      "script list is locked empty.");
    }
    return SERVER_MOD_ARGS_OK;
}

bool serverArgsApplySetting(ServerSim *sim, const char *arg,
                            const char *defaultFile, ServerArgsSay say,
                            void *ctx) {
    char              name[LOBBY_SCENARIO_FILE_LEN];
    char              file[LOBBY_SCENARIO_FILE_LEN];
    char              id[SCN_SETTING_ID_LEN];
    char              shown[128];
    const char       *whole = arg;
    const char       *eq;
    const char       *colon;
    const char       *word;
    const char       *how = "";
    uint8_t           blob[SCN_SETTINGS_BLOB_MAX];
    ScnSetting        rows[SCN_SETTINGS_MAX];
    const ScnSetting *decl = NULL;
    int               len;
    int               n = 0;
    int32_t           value = 0;
    int32_t           got = 0;
    char             *end = NULL;
    long              num;
    size_t            idLen;

    if (sim == NULL || arg == NULL) return false;
    eq = strchr(arg, '=');
    if (eq == NULL) {
        serverArgsSay(say, ctx, "-setting %s: wanted [file:]id=value", whole);
        return false;
    }
    colon = memchr(arg, ':', (size_t)(eq - arg));
    if (colon != NULL) {
        size_t fl = (size_t)(colon - arg);
        if (fl == 0 || fl >= sizeof(name)) {
            serverArgsSay(say, ctx, "-setting %s: bad file name", whole);
            return false;
        }
        memcpy(name, arg, fl);
        name[fl] = '\0';
        serverSimResolveScriptName(sim, name, defaultFile, file,
                                   sizeof(file));
        arg = colon + 1;
    } else {
        if (defaultFile == NULL || defaultFile[0] == '\0') {
            serverArgsSay(say, ctx,
                          "-setting %s: the map has no script, so name the "
                          "file", whole);
            return false;
        }
        SDL_strlcpy(file, defaultFile, sizeof(file));
    }
    idLen = (size_t)(eq - arg);
    if (idLen == 0 || idLen >= sizeof(id)) {
        serverArgsSay(say, ctx, "-setting %s: bad setting id", whole);
        return false;
    }
    memcpy(id, arg, idLen);
    id[idLen] = '\0';
    word = eq + 1;

    len = serverSimScenarioSettingsDecl(sim, file, blob, sizeof(blob));
    if (len > 0) {
        n = scnSettingsBlobRead(blob, (size_t)len, rows, SCN_SETTINGS_MAX);
    }
    if (n > 0) {
        decl = scnSettingFind(rows, n, id);
    }
    if (decl == NULL) {
        serverArgsSay(say, ctx, "-setting: %s declares no setting '%s'",
                      file, id);
        return false;
    }
    /* A choice's own words come first, so a choice whose words are numerals
       is set by word, not read as an index. */
    num = strtol(word, &end, 10);
    if (decl->type == SCN_SETTING_TYPE_CHOICE &&
        scnSettingChoiceIndex(decl, word) >= 0) {
        value = scnSettingChoiceIndex(decl, word);
    } else if (word[0] != '\0' && end != NULL && *end == '\0') {
        value = (int32_t)num;
    } else if (decl->type == SCN_SETTING_TYPE_BOOL &&
               (strcmp(word, "true") == 0 || strcmp(word, "on") == 0)) {
        value = 1;
    } else if (decl->type == SCN_SETTING_TYPE_BOOL &&
               (strcmp(word, "false") == 0 || strcmp(word, "off") == 0)) {
        value = 0;
    } else {
        serverArgsSay(say, ctx, "-setting: '%s' is not a value of %s:%s",
                      word, file, id);
        return false;
    }
    if (!serverSimSetScriptSetting(sim, file, id, value, &got)) {
        serverArgsSay(say, ctx, "-setting: %s:%s refused %d", file, id,
                      (int)value);
        return false;
    }

    /* On one of the operator's rows the value is the operator's too, kept
       for every lobby after this one. */
    switch (serverSimRecordOperatorSetting(sim, file, id, got)) {
    case SERVER_OPERATOR_SETTING_FULL:
        how = " (this game only, and the host may change it: too many "
              "-setting values on mods)";
        break;
    case SERVER_OPERATOR_SETTING_HELD:
        how = " (every game; the host cannot change it)";
        break;
    case SERVER_OPERATOR_SETTING_DEFAULT:
        how = " (each new lobby; the host may change it)";
        break;
    case SERVER_OPERATOR_SETTING_NOT_OPERATOR:
    default:
        break;
    }
    if (decl->type == SCN_SETTING_TYPE_CHOICE &&
        scnSettingChoiceText(decl, got) != NULL) {
        SDL_strlcpy(shown, scnSettingChoiceText(decl, got), sizeof(shown));
    } else {
        SDL_snprintf(shown, sizeof(shown), "%d", (int)got);
    }
    serverArgsSay(say, ctx, "Setting %s:%s = %s%s", file, id, shown, how);
    return true;
}
