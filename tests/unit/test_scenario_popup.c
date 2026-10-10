/*
 * game.popup's delivery, from the wire to the frontend's hand.
 *
 *   codec        — CTRL_SCN_POPUP is body-only, so the encode and decode
 *                  functions come from the body tables the way the live
 *                  control path takes them. Text with tokens survives, the
 *                  longest text survives, an empty body and an over-long
 *                  body are refused, and a NUL ends the text.
 *   client queue — the client keeps the popups addressed to it, in the
 *                  order they came, holds at most SCN_POPUP_QUEUE_MAX,
 *                  ignores one addressed to another seat, and drops them
 *                  all when the game goes back to the lobby.
 *   round over   — clientSimIsRoundOver, which closes a popup still on
 *                  screen, is false for a new client and in a running round
 *                  and true from the game-over hold on, through the lobby
 *                  and the next countdown.
 *   quick tokens — {QUICK_TREE} to {QUICK_MINE} are found in a text and
 *                  named by the five quick-build bindings, not by any other
 *                  key; the old tokens still resolve as before.
 *   gun tokens   — {GUN_UP} and {GUN_DOWN} are found and named by the
 *                  gunsight increase and decrease bindings.
 *   expand       — tutorialTokensExpand writes each token as the name a
 *                  callback gives, keeps a token the callback has no name
 *                  for, cuts a long text before a name it cannot fit whole,
 *                  always ends with a NUL and answers the length.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "client_sim.h"
#include "client_sim_control.h"     /* clientSimApplyControl */
#include "scenario_panel.h"         /* SCN_POPUP_TEXT_MAX */
#include "../../src/gui/sdl3/tutorial_tokens.h"
#include "test_harness.h"

/* ── codec ─────────────────────────────────────────────────────────── */

static bool popupRoundTrip(const char *text, char *out, size_t outCap) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SCN_POPUP);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SCN_POPUP);
    ControlEvent        evtIn, evtOut;
    uint8_t             buf[MAX_CONTROL_PACKET];
    size_t              outLen = 0;

    if (enc == NULL || dec == NULL) return false;
    memset(&evtIn, 0, sizeof(evtIn));
    evtIn.type = CTRL_SCN_POPUP;
    SDL_strlcpy(evtIn.u.scnPopup.text, text, sizeof(evtIn.u.scnPopup.text));
    evtIn.u.scnPopup.destTeam   = 3;      /* never travels */
    evtIn.u.scnPopup.destPlayer = 4;
    if (enc(&evtIn, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK) {
        return false;
    }
    if (outLen != strlen(text)) return false;   /* the text and nothing else */
    memset(&evtOut, 0, sizeof(evtOut));
    if (!dec(buf, outLen, &evtOut)) return false;
    if (evtOut.type != CTRL_SCN_POPUP) return false;
    /* The pair is the delivery path's, and a decoded event is addressed to
       whoever the server sent it to: everyone, as far as it knows. */
    if (evtOut.u.scnPopup.destTeam != 0 ||
        evtOut.u.scnPopup.destPlayer != 0xFF) {
        return false;
    }
    SDL_strlcpy(out, evtOut.u.scnPopup.text, outCap);
    return true;
}

int run_scenario_popup_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SCN_POPUP);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SCN_POPUP);
    ControlEvent        evt;
    char                back[SCN_POPUP_TEXT_MAX + 1];
    char                longest[SCN_POPUP_TEXT_MAX + 1];
    uint8_t             body[SCN_POPUP_TEXT_MAX + 8];
    size_t              outLen = 0;
    uint8_t             buf[MAX_CONTROL_PACKET];

    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_SCN_POPUP");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_SCN_POPUP");
    UT_ASSERT_MSG(transportControlCodecEncoder(CTRL_SCN_POPUP) == NULL,
                  "CTRL_SCN_POPUP has a full-packet encoder; it is body-only");

    /* Tokens ride as text; the client expands them. */
    UT_ASSERT_MSG(popupRoundTrip("{ACCEL} forward, {QUICK_TREE} for trees.",
                                 back, sizeof(back)),
                  "a popup with tokens did not survive");
    UT_ASSERT_MSG(strcmp(back, "{ACCEL} forward, {QUICK_TREE} for trees.") == 0,
                  "a popup with tokens came back as '%s'", back);

    /* The longest text, which a chat-sized field could not hold. */
    memset(longest, 'p', SCN_POPUP_TEXT_MAX);
    longest[SCN_POPUP_TEXT_MAX] = '\0';
    UT_ASSERT_MSG(popupRoundTrip(longest, back, sizeof(back)),
                  "a %d-byte popup did not survive", SCN_POPUP_TEXT_MAX);
    UT_ASSERT(strcmp(back, longest) == 0);

    /* An empty popup is nothing to show: the encoder skips it and the
       decoder refuses an empty body. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_POPUP;
    UT_ASSERT_MSG(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_SKIP,
                  "an empty popup was encoded");
    UT_ASSERT_MSG(!dec(body, 0, &evt), "an empty body was decoded");

    /* A body one byte past the limit is refused, not cut. */
    memset(body, 'q', SCN_POPUP_TEXT_MAX + 1);
    UT_ASSERT_MSG(!dec(body, SCN_POPUP_TEXT_MAX + 1, &evt),
                  "a %d-byte body was decoded", SCN_POPUP_TEXT_MAX + 1);

    /* A NUL ends the text; what follows is room for later fields. */
    memcpy(body, "Hi\0xyz", 6);
    UT_ASSERT_MSG(dec(body, 6, &evt), "a body with a tail was refused");
    UT_ASSERT_MSG(strcmp(evt.u.scnPopup.text, "Hi") == 0,
                  "a body with a tail decoded as '%s'", evt.u.scnPopup.text);

    /* A body that starts with the NUL has no text at all. */
    memcpy(body, "\0abc", 4);
    UT_ASSERT_MSG(!dec(body, 4, &evt), "a body with no text was decoded");
    return 0;
}

/* ── client queue ──────────────────────────────────────────────────── */

static void popupApply(ClientSim *cs, const char *text, uint8_t destPlayer) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_POPUP;
    SDL_strlcpy(evt.u.scnPopup.text, text, sizeof(evt.u.scnPopup.text));
    evt.u.scnPopup.destTeam   = 0;
    evt.u.scnPopup.destPlayer = destPlayer;
    clientSimApplyControl(cs, &evt);
}

int run_scenario_popup_client_queue(void) {
    ClientSim   *cs = clientSimAlloc();
    ControlEvent evt;
    char         got[SCN_POPUP_TEXT_MAX + 1];
    char         want[16];
    int          i;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 2);

    UT_ASSERT_MSG(!clientSimTakeScnPopup(cs, got, sizeof(got)),
                  "a new client had a popup waiting");

    /* Addressed to another seat: not this client's. */
    popupApply(cs, "not mine", 3);
    UT_ASSERT_MSG(!clientSimTakeScnPopup(cs, got, sizeof(got)),
                  "a popup for seat 3 reached seat 2");

    /* Five arrive while nothing is taken: the first four wait, in order,
       and the fifth is dropped. One addressed to seat 2 counts the same as
       one addressed to everyone. */
    for (i = 0; i < SCN_POPUP_QUEUE_MAX + 1; i++) {
        snprintf(want, sizeof(want), "popup %d", i);
        popupApply(cs, want, (i == 1) ? 2 : 0xFF);
    }
    for (i = 0; i < SCN_POPUP_QUEUE_MAX; i++) {
        snprintf(want, sizeof(want), "popup %d", i);
        UT_ASSERT_MSG(clientSimTakeScnPopup(cs, got, sizeof(got)),
                      "popup %d was not waiting", i);
        UT_ASSERT_MSG(strcmp(got, want) == 0,
                      "the popup taken %d-th was '%s', expected '%s'", i, got,
                      want);
    }
    UT_ASSERT_MSG(!clientSimTakeScnPopup(cs, got, sizeof(got)),
                  "a popup past the queue's %d was kept", SCN_POPUP_QUEUE_MAX);

    /* The ring wraps: taking and adding past the end keeps the order. */
    for (i = 0; i < 6; i++) {
        snprintf(want, sizeof(want), "wrap %d", i);
        popupApply(cs, want, 0xFF);
        UT_ASSERT(clientSimTakeScnPopup(cs, got, sizeof(got)));
        UT_ASSERT_MSG(strcmp(got, want) == 0, "after wrapping took '%s'", got);
    }

    /* Back to the lobby: what was waiting belongs to the round it came in. */
    popupApply(cs, "stale", 0xFF);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(!clientSimTakeScnPopup(cs, got, sizeof(got)),
                  "a popup outlived the round");

    clientSimDestroy(cs);
    return 0;
}

/* ── round over ────────────────────────────────────────────────────── */

static void phaseApply(ClientSim *cs, ControlEventType type) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    clientSimApplyControl(cs, &evt);
}

int run_scenario_popup_round_over(void) {
    ClientSim *cs = clientSimAlloc();

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT_MSG(!clientSimIsRoundOver(cs), "a new client's round was over");

    phaseApply(cs, CTRL_GAME_PHASE_RUNNING);
    UT_ASSERT_MSG(!clientSimIsRoundOver(cs), "a running round was over");

    /* game.end_round: the hold before the lobby, with inLobby still false. */
    phaseApply(cs, CTRL_GAME_PHASE_GAME_OVER);
    UT_ASSERT_MSG(!clientSimIsInLobby(cs), "the hold put the client in the lobby");
    UT_ASSERT_MSG(clientSimIsRoundOver(cs), "the game-over hold was not over");

    phaseApply(cs, CTRL_GAME_PHASE_LOBBY);
    UT_ASSERT_MSG(clientSimIsRoundOver(cs), "the lobby was not over");

    phaseApply(cs, CTRL_GAME_PHASE_COUNTDOWN);
    UT_ASSERT_MSG(clientSimIsRoundOver(cs), "the countdown was not over");

    /* The next round runs again. */
    phaseApply(cs, CTRL_GAME_PHASE_RUNNING);
    UT_ASSERT_MSG(!clientSimIsRoundOver(cs), "the next round was over");

    clientSimDestroy(cs);
    return 0;
}

/* ── quick-build tokens ────────────────────────────────────────────── */

int run_tutorial_tokens_quick_build(void) {
    static const struct {
        const char     *text;
        TutorialTokenId id;
    } kQuick[] = {
        { "{QUICK_TREE}", TT_QUICK_TREE },
        { "{QUICK_ROAD}", TT_QUICK_ROAD },
        { "{QUICK_WALL}", TT_QUICK_WALL },
        { "{QUICK_PILL}", TT_QUICK_PILL },
        { "{QUICK_MINE}", TT_QUICK_MINE },
    };
    keyItems keys;
    size_t   len = 0;
    size_t   i;

    memset(&keys, 0, sizeof(keys));
    /* Every binding a different number, so a token named by the wrong one
       shows up as the wrong number. The quick-build defaults are 1 to 5. */
    keys.kiForward      = 101;
    keys.kiLayMine      = 102;
    keys.kiQuickTree    = 30;
    keys.kiQuickRoad    = 31;
    keys.kiQuickWall    = 32;
    keys.kiQuickPillbox = 33;
    keys.kiQuickMine    = 34;
    keys.kiGunIncrease  = 40;
    keys.kiGunDecrease  = 41;

    for (i = 0; i < sizeof(kQuick) / sizeof(kQuick[0]); i++) {
        char text[64];
        snprintf(text, sizeof(text), "%s and more", kQuick[i].text);
        UT_ASSERT_MSG(tutorialTokenMatch(text, &len) == kQuick[i].id,
                      "%s was not found", kQuick[i].text);
        UT_ASSERT_MSG(len == strlen(kQuick[i].text),
                      "%s matched %u bytes", kQuick[i].text, (unsigned)len);
        UT_ASSERT_MSG(strcmp(tutorialTokenText(kQuick[i].id),
                             kQuick[i].text) == 0,
                      "the token's text is '%s'",
                      tutorialTokenText(kQuick[i].id));
        UT_ASSERT_MSG(tutorialTokenScancode(kQuick[i].id, &keys) ==
                          30 + (int)i,
                      "%s named key %d, expected %d", kQuick[i].text,
                      tutorialTokenScancode(kQuick[i].id, &keys), 30 + (int)i);
        UT_ASSERT_MSG(tutorialTokenIsQuickBuild(kQuick[i].id),
                      "%s is not a quick-build token", kQuick[i].text);
    }

    /* The old tokens resolve as they did, and are not quick-build ones. */
    UT_ASSERT(tutorialTokenMatch("{ACCEL}", &len) == TT_ACCEL && len == 7);
    UT_ASSERT(tutorialTokenScancode(TT_ACCEL, &keys) == 101);
    UT_ASSERT(tutorialTokenScancode(TT_MINE, &keys) == 102);
    UT_ASSERT(!tutorialTokenIsQuickBuild(TT_MINE));
    UT_ASSERT(tutorialTokenScancode(TT_BUILD_TOOL, &keys) == 0);
    UT_ASSERT(tutorialTokenMatch("{DISMISS}", &len) == TT_DISMISS);

    /* The gunsight range tokens: named by the gunsight bindings, and not
       quick-build ones. */
    UT_ASSERT(tutorialTokenMatch("{GUN_UP} more", &len) == TT_GUN_UP &&
              len == strlen("{GUN_UP}"));
    UT_ASSERT(tutorialTokenMatch("{GUN_DOWN} more", &len) == TT_GUN_DOWN &&
              len == strlen("{GUN_DOWN}"));
    UT_ASSERT(strcmp(tutorialTokenText(TT_GUN_UP), "{GUN_UP}") == 0);
    UT_ASSERT(strcmp(tutorialTokenText(TT_GUN_DOWN), "{GUN_DOWN}") == 0);
    UT_ASSERT_MSG(tutorialTokenScancode(TT_GUN_UP, &keys) == 40,
                  "{GUN_UP} named key %d, expected 40",
                  tutorialTokenScancode(TT_GUN_UP, &keys));
    UT_ASSERT_MSG(tutorialTokenScancode(TT_GUN_DOWN, &keys) == 41,
                  "{GUN_DOWN} named key %d, expected 41",
                  tutorialTokenScancode(TT_GUN_DOWN, &keys));
    UT_ASSERT(!tutorialTokenIsQuickBuild(TT_GUN_UP));
    UT_ASSERT(!tutorialTokenIsQuickBuild(TT_GUN_DOWN));
    UT_ASSERT(tutorialTokenMatch("{GUN}", &len) == TT_COUNT);

    /* Every token has its text, and that text is found as that token. */
    for (i = 0; i < (size_t)TT_COUNT; i++) {
        const char *t = tutorialTokenText((TutorialTokenId)i);
        UT_ASSERT_MSG(t != NULL, "token %u has no text", (unsigned)i);
        UT_ASSERT_MSG(tutorialTokenMatch(t, &len) == (TutorialTokenId)i &&
                          len == strlen(t),
                      "%s is not found as itself", t);
    }

    /* Not a token: a brace with an unknown word, a word with no brace. */
    UT_ASSERT(tutorialTokenMatch("{QUICK_BOAT}", &len) == TT_COUNT);
    UT_ASSERT(tutorialTokenMatch("QUICK_TREE}", &len) == TT_COUNT);
    UT_ASSERT(tutorialTokenText(TT_COUNT) == NULL);
    return 0;
}

/* ── tokens written out in plain text ──────────────────────────────── */

/* Names three tokens, has none for {MINE}, and counts its calls. */
static const char *expandName(TutorialTokenId id, void *ctx) {
    int *calls = (int *)ctx;
    if (calls != NULL) (*calls)++;
    switch (id) {
        case TT_ACCEL:  return "W";
        case TT_FIRE:   return "Space";
        case TT_GUN_UP: return "range up";
        default:        return NULL;
    }
}

/* out filled with 'X' first, so a byte written past the NUL shows up. */
static size_t expandInto(const char *src, char *out, size_t cap,
                         size_t fill) {
    memset(out, 'X', fill);
    return tutorialTokensExpand(src, out, cap, expandName, NULL);
}

int run_scenario_status_tokens_expand(void) {
    char   out[64];
    size_t n;
    int    calls = 0;

    /* Tokens become the callback's names; the rest is copied as it is. */
    n = tutorialTokensExpand("Press {ACCEL}, then {FIRE}.", out, sizeof(out),
                             expandName, &calls);
    UT_ASSERT_MSG(strcmp(out, "Press W, then Space.") == 0, "got '%s'", out);
    UT_ASSERT(n == strlen("Press W, then Space."));
    UT_ASSERT_MSG(calls == 2, "the callback was called %d times", calls);

    /* No name for {MINE}: it stays as written. An unknown word in braces
       is plain text. */
    n = expandInto("{MINE} and {BOAT} {GUN_UP}", out, sizeof(out),
                   sizeof(out));
    UT_ASSERT_MSG(strcmp(out, "{MINE} and {BOAT} range up") == 0,
                  "got '%s'", out);
    UT_ASSERT(n == strlen(out));

    /* No callback at all: every token stays as written. */
    n = tutorialTokensExpand("{ACCEL}{FIRE}", out, sizeof(out), NULL, NULL);
    UT_ASSERT_MSG(strcmp(out, "{ACCEL}{FIRE}") == 0, "got '%s'", out);
    UT_ASSERT(n == 13);

    /* Out of room before a name: the name is left out whole, never cut.
       "ab" fits in 6 bytes, "ab" + "range up" does not. */
    n = expandInto("ab{GUN_UP}cd", out, 6, sizeof(out));
    UT_ASSERT_MSG(n == 2 && strcmp(out, "ab") == 0,
                  "got '%s' (%u)", out, (unsigned)n);
    UT_ASSERT_MSG(out[3] == 'X', "a byte was written past the NUL");

    /* Plain text is cut at the last byte that leaves room for the NUL. */
    n = expandInto("abcdef", out, 4, sizeof(out));
    UT_ASSERT_MSG(n == 3 && strcmp(out, "abc") == 0,
                  "got '%s' (%u)", out, (unsigned)n);
    UT_ASSERT(out[4] == 'X');
    n = expandInto("abc", out, 4, sizeof(out));
    UT_ASSERT(n == 3 && strcmp(out, "abc") == 0);

    /* A name that fits exactly with its NUL is written. */
    n = expandInto("{FIRE}", out, 6, sizeof(out));
    UT_ASSERT_MSG(n == 5 && strcmp(out, "Space") == 0,
                  "got '%s' (%u)", out, (unsigned)n);

    /* One byte holds only the NUL; a NULL text is an empty one. */
    n = expandInto("{ACCEL}", out, 1, sizeof(out));
    UT_ASSERT(n == 0 && out[0] == 0 && out[1] == 'X');
    n = expandInto(NULL, out, sizeof(out), sizeof(out));
    UT_ASSERT(n == 0 && out[0] == 0);

    /* No room at all: nothing is written. */
    memset(out, 'X', sizeof(out));
    n = tutorialTokensExpand("abc", out, 0, expandName, NULL);
    UT_ASSERT(n == 0 && out[0] == 'X');
    UT_ASSERT(tutorialTokensExpand("abc", NULL, 8, expandName, NULL) == 0);
    return 0;
}
