/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * lang_message.h — shared MessageArgs / langid definitions used by
 * both src/bolo/ (message-rendering call sites) and src/gui/lang.h
 * (the actual implementation). Kept here so non-GUI consumers can
 * name the type without reaching into src/gui/.
 */
#ifndef LANG_MESSAGE_H
#define LANG_MESSAGE_H

#include <stdint.h>
#include "global.h"      /* PLAYER_NAME_LEN */

typedef unsigned int langid;

/* Per-message arguments substituted into named placeholders by
 * langGetTextFmt(). The placeholders are:
 *   {player}                    -> playerName
 *   {other}                     -> otherName
 *   {number}/{number2..4}       -> rendered as %d
 *   {string1}/{string2}/{string3} -> arbitrary short strings (e.g. a
 *                                  pre-formatted "%.1f", a duration
 *                                  label, etc.)
 * Substitution is non-recursive — braces inside a substituted value
 * (e.g. a player name with "{ACCEL}" in it) are NOT rescanned. */
#define LANG_MSGARG_STRING_LEN 64

typedef struct {
    char    playerName[PLAYER_NAME_LEN];
    uint8_t playerFlags;        /* PLAYER_FLAG_* bits */
    char    playerCountry[3];   /* ISO 3166-1 alpha-2 + NUL; "" if unknown */
    char    otherName[PLAYER_NAME_LEN];
    uint8_t otherFlags;
    char    otherCountry[3];
    int  number;
    int  number2;
    int  number3;
    int  number4;
    char string1[LANG_MSGARG_STRING_LEN];
    char string2[LANG_MSGARG_STRING_LEN];
    char string3[LANG_MSGARG_STRING_LEN];
} MessageArgs;

#endif /* LANG_MESSAGE_H */
