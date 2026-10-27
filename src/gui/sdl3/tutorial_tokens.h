/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 *Name:          Tutorial Tokens
 *Filename:      tutorial_tokens.h
 *Purpose:
 *  The key tokens a tutorial text or a scenario popup may
 *  carry ({ACCEL}, {QUICK_TREE} and so on): finding one in
 *  a text and naming the keyboard key bound to it. No
 *  renderer and no glyphs, so the unit tests can hold the
 *  table; tutorial_text.c turns a token into what is drawn.
 *********************************************************/

#ifndef TUTORIAL_TOKENS_H
#define TUTORIAL_TOKENS_H

#include <stdbool.h>
#include <stddef.h>

#include "../input.h"   /* keyItems */

#ifdef __cplusplus
extern "C" {
#endif

/* One per token. tutorial_text.c keeps a row per member, in this order. */
typedef enum {
  TT_ACCEL = 0,
  TT_BRAKE,
  TT_LEFT,
  TT_RIGHT,
  TT_FIRE,
  TT_MINE,
  TT_SCROLL_UP,
  TT_SCROLL_DOWN,
  TT_SCROLL_LEFT,
  TT_SCROLL_RIGHT,
  TT_DISMISS,
  TT_BUILD_MODE,
  TT_BUILD_PLACE,
  TT_BUILD_TOOL,
  TT_QUICK_TREE,
  TT_QUICK_ROAD,
  TT_QUICK_WALL,
  TT_QUICK_PILL,
  TT_QUICK_MINE,
  TT_GUN_UP,      /* the gunsight longer */
  TT_GUN_DOWN,    /* the gunsight shorter */
  /* The phrase tokens. Each is a whole sentence or two, taken from the
     language file in the wording for the player's input (keyboard and
     mouse, touch, controller), and filled in before the key tokens it
     carries. tutorialTokenIsPhrase answers true for these. */
  TT_DRIVE_HOW,         /* how to drive and turn */
  TT_BUILD_HOW,         /* how to pick a build tool and send the man */
  TT_HARVEST_TIP,       /* any build tool harvests a tree */
  TT_PILL_HOW,          /* how to build a carried pillbox */
  TT_STEP_TREE,         /* status line: harvest in the grove */
  TT_STEP_ROAD,         /* status line: build a road */
  TT_STEP_WALL,         /* status line: build a wall */
  TT_STEP_MINE,         /* status line: lay a mine */
  TT_STEP_PILL,         /* status line: build the carried pillbox */
  TT_STEP_PILL_GREEN,   /* status line: build it on the green square */
  TT_COUNT       /* how many there are, and no token */
} TutorialTokenId;

/* The token's text, braces included ("{ACCEL}"); NULL past the end. */
const char *tutorialTokenText(TutorialTokenId id);

/* The token that starts at s, with its length in *outLen; TT_COUNT when no
 * token starts there. */
TutorialTokenId tutorialTokenMatch(const char *s, size_t *outLen);

/* The keyboard scancode bound to the token, read from k; 0 for a token
 * with no keyboard key (the controller-only build tokens). */
int tutorialTokenScancode(TutorialTokenId id, const keyItems *k);

/* True for the five quick-build keys. A touch screen has no key for them,
 * so the resolver writes them as words there. */
bool tutorialTokenIsQuickBuild(TutorialTokenId id);

/* True for the phrase tokens ({DRIVE_HOW}, {STEP_TREE} and the rest): a
 * token whose words come from the language file as a whole sentence,
 * picked for the player's input. */
bool tutorialTokenIsPhrase(TutorialTokenId id);

/* What a token is written as in plain text, for a line drawn as text alone
 * (a scenario's status line or announcement). NULL leaves the token as it
 * is written. */
typedef const char *(*TutorialTokenNameFn)(TutorialTokenId id, void *ctx);

/* src into out with every token replaced by name(id, ctx). out is always
 * ended with a NUL; a text too long for it is cut, never a name half
 * written and never a UTF-8 character half written. Answers the length
 * written. */
size_t tutorialTokensExpand(const char *src, char *out, size_t cap,
                            TutorialTokenNameFn name, void *ctx);

/* One piece of a text: a run of plain text (id TT_COUNT) or one token.
 * start points into the text that was split, and the piece is len bytes
 * of it, with no NUL of its own. */
typedef struct {
  const char     *start;
  size_t          len;
  TutorialTokenId id;
} TutorialPiece;

/* src cut into pieces, in order, at most max of them: runs of plain text
 * and the tokens between them. A brace that starts no token ("{", "{FOO}",
 * "{accel}") is plain text. Answers the count. *rest, when rest is not
 * NULL, is set to the first byte no piece covers: src's NUL when all of
 * it fits, else the start of the first piece left out, so a caller can
 * tell the text was cut. A NULL src, a NULL out or a max below 1 answers
 * 0 with *rest at src. */
int tutorialTokensSplit(const char *src, TutorialPiece *out, int max,
                        const char **rest);

/* src into out with only the phrase tokens replaced, by phrase(id, ctx);
 * every other token is copied as it is written, for a second pass to fill
 * in. A phrase's own text is not searched again for phrases. Cut and
 * ended as tutorialTokensExpand. Answers the length written. */
size_t tutorialPhrasesExpand(const char *src, char *out, size_t cap,
                             TutorialTokenNameFn phrase, void *ctx);

/* A key's name as the tutorial writes it, from the name SDL gives it
 * (SDL_GetScancodeName). A keypad key's "Keypad " becomes "Num ", so
 * "Keypad ." is written "Num ." and "Keypad Enter" "Num Enter"; any other
 * name is answered as it is. The short name is written into buf (cap
 * bytes) when it differs; the answer is buf or name. NULL or an empty name
 * answers "?". */
const char *tutorialShortKeyName(const char *name, char *buf, size_t cap);

/* The width of a drawn key cap `capH` high whose label is `labelW` wide:
 * the label with a margin each side, and never narrower than the cap is
 * high (a one-letter cap stays square). */
float tutorialKeycapWidth(float capH, float labelW);

#ifdef __cplusplus
}
#endif

#endif /* TUTORIAL_TOKENS_H */
