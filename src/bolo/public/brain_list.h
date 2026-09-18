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

/*********************************************************
 * Name:          Brain list
 * Filename:      brain_list.h
 * Purpose:
 *   Discover the set of bot codebases available on the
 *   server by scanning the brains/ directory for any
 *   sub-directory that contains an init.lua. Each entry
 *   carries a display name and a coarse version string
 *   sourced from the most-recent modification time of the
 *   brain's .lua files. Disk paths are server-private and
 *   live alongside the catalogue under internal/.
 *
 *   Server-side discovery — the resulting list is shipped
 *   to clients via PACKET_LOBBY_BRAIN_LIST so the lobby
 *   AiConfig combo can list the available brains by name.
 *********************************************************/

#ifndef BRAIN_LIST_H
#define BRAIN_LIST_H

#include <stdint.h>   /* int64_t — brainListTextsMtimeForPath */

#include "global.h"

#define BRAIN_LIST_MAX        16
#define BRAIN_LIST_NAME_LEN   32
#define BRAIN_LIST_VER_LEN    24

typedef struct {
    char name[BRAIN_LIST_NAME_LEN];     /* "GoalHunter" — dir name */
    char version[BRAIN_LIST_VER_LEN];   /* "2026-05-11 12:30" or "" */
} BrainListEntry;

typedef struct {
    BrainListEntry entries[BRAIN_LIST_MAX];
    int            count;
} BrainList;

/* Buffer sizes for the optional human-readable metadata loaded by
 * brainListLoadMeta. These are display-only (read locally, never sent over
 * the wire), so they don't affect the BrainList struct or any packet. */
#define BRAIN_LIST_TAG_LEN   192   /* short one-line tagline (~20 words)     */
#define BRAIN_LIST_DESC_LEN  640   /* longer hover description (multi-line)  */

/* Load optional metadata for a brain by its catalogue name. Reads
 * "<brains-parent>/<name>/about.txt" from the first parent that has it
 * (working-dir brains/ and Brains/, then SDL_GetBasePath()). The file's first
 * non-empty line is the short tagline; the remainder is the long description.
 * Either out buffer may be NULL; both are NUL-terminated (and cleared) on
 * return. Returns true iff an about.txt was found. */
bool brainListLoadMeta(const char *name,
                       char *tagline, size_t taglineSz,
                       char *desc, size_t descSz);

/* A brain's own tag colour, from a "color: #RRGGBB" line anywhere in its
 * about.txt (the line is kept out of the tagline/description). The lobby
 * paints the bot's name tag with it; a brain that declares none gets a
 * colour derived from its name instead. Returns true iff a valid colour was
 * found; *rgb is 0xRRGGBB. */
bool brainListLoadColor(const char *name, uint32_t *rgb);

/* The init.lua of the named brain: "GoalHunter_1.7" becomes
 * "brains/GoalHunter_1.7/init.lua" in whichever brains parent holds it. The
 * parents are the ones brainListLoadMeta reads a brain's files from — the
 * working directory's brains/ and Brains/, then the same two beside the
 * executable — and the two searches share one list, so a brain whose about.txt
 * one of them finds is a brain the other resolves.
 *
 * False, with outPath emptied, when no parent holds the brain, when the name
 * carries a path separator or opens with a dot — a name is one directory under
 * a brains parent, not a path — and when the path would not fit outPath.
 *
 * This is the one disk path the catalogue hands out publicly. A scenario names
 * the brain its teams run rather than pathing to one, because a scenario
 * written on one machine knows nothing of another's layout; the scenario
 * runtime turns that name into the path the bot loader opens, and it sees
 * src/bolo/public/ alone. */
bool brainListResolve(const char *name, char *outPath, size_t outLen);


/* ── announce.txt / commands.txt: what a brain tells the lobby ────────
 *
 * Two more plain-text files a brain may ship beside about.txt:
 *
 *   announce.txt   one short message, blank lines allowed. The lobby drops
 *                  it into TEAM chat, as a line from the bot, the first time
 *                  a bot running this brain joins the reader's team.
 *   commands.txt   the long docs behind that line. Clicking the chat line
 *                  opens them in a dialog.
 *
 * Unlike about.txt these DO go over the wire (CTRL_LOBBY_BRAIN_DOCS_CHUNK),
 * because the server picks the brain and a client need not have it on disk.
 * The caps below are what the wire carries; a longer file is truncated at
 * the cap and the read says so, so the truncation is never silent. */
#define BRAIN_ANNOUNCE_MAX  512    /* announce.txt bytes, NUL not counted */
#define BRAIN_DOCS_MAX    16384    /* commands.txt bytes, NUL not counted */

/* Read a brain's announce.txt and commands.txt out of its DIRECTORY
 * ("brains/GoalHunter_1.7", or the server's own brainPaths[i]). Either out
 * buffer may be NULL; both are cleared and NUL-terminated on return. Pass
 * announceSz/docsSz as the full buffer size INCLUDING the NUL. Returns true
 * iff at least one of the two files was found and had content.
 * *truncated (optional) is set true when a file was longer than its buffer. */
bool brainListLoadTexts(const char *brainDir,
                        char *announce, size_t announceSz,
                        char *docs, size_t docsSz,
                        bool *truncated);

/* Same, keyed off the brain's init.lua path ("Brains/GoalHunter_1.7/init.lua")
 * — the shape brainListScan stores and the server holds in brainPaths[]. */
bool brainListLoadTextsForPath(const char *brainPath,
                               char *announce, size_t announceSz,
                               char *docs, size_t docsSz,
                               bool *truncated);

/* The newer of the two texts' modification times, as a plain number to
 * compare against a number kept from an earlier read; 0 when the brain ships
 * neither file. Same init.lua-path key as the read above.
 *
 * What a CACHE of these texts is kept honest with. The server reads each
 * brain's files once and holds the wire blob, because re-reading them on
 * every lobby keyframe opened up to two files per brain per tick; this is
 * how a file the operator edited between rounds is still picked up without
 * a restart. */
int64_t brainListTextsMtimeForPath(const char *brainPath);


/* ── Bot modes and their difficulty levels ────────────────────────────
 *
 * A brain says for itself which MODES it can be run in, and which
 * difficulty LEVELS each of those modes offers. The lobby needs this on
 * every client, in C, before a game starts, so it comes from a plain text
 * manifest that ships with the brain and is read locally the same way
 * about.txt is:
 *
 *   brains/<brain>/modes.txt
 *   ------------------------
 *   # comments run to end of line
 *   [default]
 *   label   = Default
 *   levels  = easy:Easy:1, medium:Medium:2, hard:Hard:3
 *   default = hard
 *
 *   [survival]
 *   label   = Survival
 *   levels  = easy:Easy:1, medium:Medium:2, hard:Hard:3
 *   default = hard
 *
 * The section header is the mode KEY, one `levels` entry is
 * `key:Label:chips`, and `default` names the level key a freshly added bot
 * starts at. The FIRST section is mode 0 — the mode every ordinary game
 * uses.
 *
 * `chips` is 1, 2 or 3: how many of the lobby's three difficulty chips the
 * level lights. It is OPTIONAL. A level that omits it takes its POSITION on
 * the scale instead — first kept level 1, second 2, third and later 3 — so a
 * manifest written before chips existed still means what it always meant, and
 * a third-party brain does not have to be edited to keep working. A level that
 * DOES declare it and gives anything other than 1, 2 or 3 is dropped.
 *
 * `levels` itself is OPTIONAL too. A mode may declare none, which says the
 * brain has one way of playing and no difficulty to pick. The lobby then shows
 * no difficulty tag on the row and no difficulty dropdown in the gear form for
 * that mode. Note the consequence: a `levels` line whose entries are all
 * malformed is indistinguishable from an absent one, so it reads as "no
 * difficulty" rather than falling back.
 *
 * Keys are what reach the brain (the "mode=" / "difficulty=" init tokens);
 * labels are what the lobby shows. A brain with no modes.txt is given the
 * synthesized single mode below, which is exactly the behaviour that
 * existed before manifests: Default with easy / medium / hard, hard by
 * default. */
#define BRAIN_MODES_MAX       8    /* modes one brain may declare  */
#define BRAIN_LEVELS_MAX      8    /* levels one mode may declare  */
#define BRAIN_MODE_KEY_LEN    16   /* key, incl. NUL (15 chars)    */
#define BRAIN_MODE_LABEL_LEN  32   /* label, incl. NUL (31 chars)  */

typedef struct {
    char key[BRAIN_MODE_KEY_LEN];      /* "hard" — reaches the brain  */
    char label[BRAIN_MODE_LABEL_LEN];  /* "Hard" — shown in the lobby */
    /* How many of the lobby's three difficulty chips this level lights, 1..3.
     * DECLARED by the brain, never inferred from the level's position in the
     * list: a brain that offers four levels, or names them something other
     * than easy/medium/hard, still has to say where each one sits on the
     * three-chip scale. A level that declares anything else is DROPPED at
     * parse time rather than defaulted, because the lobby has nothing
     * meaningful to draw for it. */
    int  chips;
} BrainLevel;

typedef struct {
    char       key[BRAIN_MODE_KEY_LEN];
    char       label[BRAIN_MODE_LABEL_LEN];
    int        levelCount;                     /* 1..BRAIN_LEVELS_MAX */
    BrainLevel levels[BRAIN_LEVELS_MAX];
    int        defaultLevel;                   /* index into levels[] */
} BrainMode;

typedef struct {
    int       modeCount;                       /* 1..BRAIN_MODES_MAX  */
    BrainMode modes[BRAIN_MODES_MAX];
} BrainModes;

/* Load a brain's mode manifest by its catalogue name ("GoalHunter_1.7"),
 * searching the same parents brainListLoadMeta does. *out is ALWAYS filled
 * with something usable: a brain with no (or an unreadable, or an empty)
 * modes.txt gets the synthesized "default" mode described above. Returns
 * true only when a manifest was read and yielded at least one mode, so a
 * caller that cares can tell "the brain said so" from "we made it up". */
bool brainListLoadModes(const char *name, BrainModes *out);

/* Same, keyed off a brain's init.lua path ("Brains/GoalHunter_1.7/init.lua")
 * rather than its catalogue name — the shape the dedicated server, the
 * single-player seed and the bot manager all hold. */
bool brainListLoadModesForPath(const char *brainPath, BrainModes *out);

/* Key -> index lookups, case-insensitive. Return -1 when the key is
 * absent (or any argument is NULL), so a caller can warn about an unknown
 * key instead of silently running a different mode. */
int brainModesFindMode(const BrainModes *modes, const char *key);
int brainModeFindLevel(const BrainMode *mode, const char *key);

/* Split "Name_<ver>" into base ("Name") + numeric version (1.7). No trailing
 * _<digit> suffix → version 0 and the whole name as base. Used to sort the
 * catalogue newest-first, and by the lobby to label a bot row "GoalHunter"
 * rather than "GoalHunter_1.7". `base` is always NUL-terminated. */
double brainListSplitVersion(const char *name, char *base, size_t baseSz);

#endif /* BRAIN_LIST_H */
