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

/* A text that names one string of the language file, written as
 * "{LANG:STR_TUTORIAL01}". The whole text must be the reference, with
 * nothing before or after it, and the name is capital letters, digits and
 * '_' only. A scenario sends such a text so that each client shows the
 * string in its own language and in the wording for its own input.
 * On a match the name (without "{LANG:" and "}") is written into name
 * (cap bytes, ended with a NUL) and the answer is true. A text that does
 * not match, or a name too long for cap, answers false. */
bool tutorialLangRefParse(const char *s, char *name, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TUTORIAL_TOKENS_H */
