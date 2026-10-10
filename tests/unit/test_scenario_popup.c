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
 *   quick tokens — {QUICK_TREE} to {QUICK_MINE} are found in a text and
 *                  named by the five quick-build bindings, not by any other
 *                  key; the old tokens still resolve as before.
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

    /* Not a token: a brace with an unknown word, a word with no brace. */
    UT_ASSERT(tutorialTokenMatch("{QUICK_BOAT}", &len) == TT_COUNT);
    UT_ASSERT(tutorialTokenMatch("QUICK_TREE}", &len) == TT_COUNT);
    UT_ASSERT(tutorialTokenText(TT_COUNT) == NULL);
    return 0;
}
