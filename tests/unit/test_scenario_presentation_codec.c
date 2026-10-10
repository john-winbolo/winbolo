/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The four control events a scenario presents with: their body codecs
 * and what a client keeps when one arrives.
 *
 * All four are delivered body-only (no full-packet wrapper, no PACKET_*
 * type), so the functions are resolved through the body tables the live
 * control path uses, the way CTRL_SIM_RULES is.
 *
 *   scn_presentation_codec_bodies — each body encoded from a filled
 *       event and compared against bytes written out by hand, then
 *       decoded from those same hand-written bytes and checked field by
 *       field. The bytes are composed here rather than taken from the
 *       encoder: a fixture that reads its own output back through the
 *       codec under test passes whenever the two drift together.
 *       The same case also encodes a score label that fills its field and
 *       decodes the encoder's own body back, so the encoder cannot emit a
 *       label length its decoder refuses.
 *   scn_presentation_codec_refuses_short — a body one byte short of each
 *       fixed part, a panel whose declared length overruns, an announce
 *       longer than the field, and a score whose label length overruns.
 *   scn_presentation_decoder_sets_broadcast — after each decode the
 *       recipient pair reads as everyone, so a wire client is never
 *       accidentally unicast to slot 0.
 *   scn_presentation_client_filters — the in-process path: an event for
 *       another team, and one for another slot, are not stored; the same
 *       event addressed to everyone is.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "scenario_panel.h"
#include "scenario_defs.h"   /* SCN_ANNOUNCE_POSITIONED_TEXT_MAX */
#include "transport_control_codec.h"
#include "test_harness.h"

/* ── Hand-written bodies ──────────────────────────────────────────── */

/* CTRL_SCN_PANEL: [panel 1][len 2 BE][bytes × len].
 * The list is one sprite primitive (opcode 5, x, y, tile) — four bytes,
 * so the whole body is seven. */
static const uint8_t kPanelBody[] = {
    0x02,              /* panel 2 */
    0x00, 0x04,        /* len 4, big-endian */
    0x05, 0x0A, 0x0B, 0x20   /* sprite x=10 y=11 tile=32 */
};

/* CTRL_SCN_SCORE: [kind 1][target 1][score 4 BE signed][labelLen 1]
 * [label × labelLen]. score is -2 as two's complement: 0xFFFFFFFE. */
static const uint8_t kScoreBody[] = {
    0x01,                          /* kind: team */
    0x03,                          /* target: team 3 */
    0xFF, 0xFF, 0xFF, 0xFE,        /* score -2 */
    0x05,                          /* labelLen */
    'W', 'a', 'v', 'e', 's'
};

/* CTRL_SCN_ANNOUNCE: [ticks 2 BE][text, the rest of the body].
 * No terminator travels; the decoder terminates what it stores. */
static const uint8_t kAnnounceBody[] = {
    0x01, 0x2C,                    /* ticks 300 */
    'G', 'o', '!'
};

/* CTRL_SCN_MARKER: [id 1][kind 1][x 1][y 1][slot 1][colour 1]. */
static const uint8_t kMarkerBody[] = {
    0x07,   /* id */
    0x01,   /* kind: follow */
    0x14,   /* x */
    0x15,   /* y */
    0x09,   /* slot */
    0x05    /* colour: red */
};

/* Encode one event's body through the live body table and compare it
 * against `want`. 0 on success, 1 on failure — callers return it. */
static int encodesTo(const char *what, const ControlEvent *evt,
                     const uint8_t *want, size_t wantLen) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(evt->type);
    uint8_t buf[SCN_PANEL_MAX + 16];
    size_t outLen = 0;

    UT_ASSERT_MSG(enc != NULL, "%s: no body encoder registered", what);
    memset(buf, 0xCD, sizeof(buf));
    UT_ASSERT_MSG(enc(evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "%s: encode refused", what);
    UT_ASSERT_MSG(outLen == wantLen, "%s: wrote %u bytes, wanted %u", what,
                  (unsigned)outLen, (unsigned)wantLen);
    UT_ASSERT_MSG(memcmp(buf, want, wantLen) == 0, "%s: bytes differ", what);
    return 0;
}

/* Decode a body through the live body table. */
static bool decodeBody(ControlEventType type, const uint8_t *body, size_t len,
                       ControlEvent *out) {
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(type);
    if (dec == NULL) return false;
    return dec(body, len, out);
}

int run_scn_presentation_codec_bodies(void) {
    ControlEvent evt;
    ControlEvent back;

    /* ── panel ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_PANEL;
    /* A panel id no arm produces, on purpose: the codec carries the byte it
       is given rather than assuming the only id there is, so an encoder that
       wrote a constant 0 would fail here. The arm is where an id is bounded
       against SCN_PANEL_IDS; the wire just carries a byte. */
    evt.u.scnPanel.panel = 2;
    evt.u.scnPanel.len = 4;
    evt.u.scnPanel.bytes[0] = SCN_PANEL_OP_SPRITE;
    evt.u.scnPanel.bytes[1] = 10;
    evt.u.scnPanel.bytes[2] = 11;
    evt.u.scnPanel.bytes[3] = 32;
    /* Set on the event and deliberately absent from the bytes below:
     * the recipient pair is a server-side filter and does not travel. */
    evt.u.scnPanel.destTeam = 3;
    evt.u.scnPanel.destPlayer = 5;
    if (encodesTo("panel", &evt, kPanelBody, sizeof(kPanelBody))) return 1;

    UT_ASSERT(decodeBody(CTRL_SCN_PANEL, kPanelBody, sizeof(kPanelBody), &back));
    UT_ASSERT(back.type == CTRL_SCN_PANEL);
    UT_ASSERT(back.u.scnPanel.panel == 2);
    UT_ASSERT(back.u.scnPanel.len == 4);
    UT_ASSERT(back.u.scnPanel.bytes[0] == SCN_PANEL_OP_SPRITE);
    UT_ASSERT(back.u.scnPanel.bytes[1] == 10);
    UT_ASSERT(back.u.scnPanel.bytes[2] == 11);
    UT_ASSERT(back.u.scnPanel.bytes[3] == 32);

    /* ── score ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_SCORE;
    evt.u.scnScore.kind = SCN_SCORE_KIND_TEAM;
    evt.u.scnScore.target = 3;
    evt.u.scnScore.score = -2;
    memcpy(evt.u.scnScore.label, "Waves", 6);
    if (encodesTo("score", &evt, kScoreBody, sizeof(kScoreBody))) return 1;

    UT_ASSERT(decodeBody(CTRL_SCN_SCORE, kScoreBody, sizeof(kScoreBody), &back));
    UT_ASSERT(back.type == CTRL_SCN_SCORE);
    UT_ASSERT(back.u.scnScore.kind == SCN_SCORE_KIND_TEAM);
    UT_ASSERT(back.u.scnScore.target == 3);
    UT_ASSERT(back.u.scnScore.score == -2);
    UT_ASSERT(strcmp(back.u.scnScore.label, "Waves") == 0);

    /* ── announce ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.ticks = 300;
    memcpy(evt.u.scnAnnounce.text, "Go!", 4);
    evt.u.scnAnnounce.destTeam = 1;
    evt.u.scnAnnounce.destPlayer = 2;
    if (encodesTo("announce", &evt, kAnnounceBody, sizeof(kAnnounceBody))) return 1;

    UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kAnnounceBody,
                         sizeof(kAnnounceBody), &back));
    UT_ASSERT(back.type == CTRL_SCN_ANNOUNCE);
    UT_ASSERT(back.u.scnAnnounce.ticks == 300);
    UT_ASSERT(strcmp(back.u.scnAnnounce.text, "Go!") == 0);

    /* ── marker ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_MARKER;
    evt.u.scnMarker.id = 7;
    evt.u.scnMarker.kind = SCN_MARKER_KIND_FOLLOW;
    evt.u.scnMarker.x = 20;
    evt.u.scnMarker.y = 21;
    evt.u.scnMarker.slot = 9;
    evt.u.scnMarker.colour = SCN_PANEL_COLOUR_RED;
    evt.u.scnMarker.destTeam = 4;
    evt.u.scnMarker.destPlayer = 6;
    if (encodesTo("marker", &evt, kMarkerBody, sizeof(kMarkerBody))) return 1;

    UT_ASSERT(decodeBody(CTRL_SCN_MARKER, kMarkerBody, sizeof(kMarkerBody),
                         &back));
    UT_ASSERT(back.type == CTRL_SCN_MARKER);
    UT_ASSERT(back.u.scnMarker.id == 7);
    UT_ASSERT(back.u.scnMarker.kind == SCN_MARKER_KIND_FOLLOW);
    UT_ASSERT(back.u.scnMarker.x == 20);
    UT_ASSERT(back.u.scnMarker.y == 21);
    UT_ASSERT(back.u.scnMarker.slot == 9);
    UT_ASSERT(back.u.scnMarker.colour == SCN_PANEL_COLOUR_RED);

    /* ── a label that fills its field ──
     * label[] is sixteen bytes with no room for a terminator past the last
     * one, so the decoder refuses a declared length of sixteen. An encoder
     * that measured a sixteen-byte unterminated label at its full width
     * would put a body on the wire that its own decoder throws away, so the
     * encoder caps at fifteen and what comes back is those fifteen bytes. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_SCORE;
    evt.u.scnScore.kind = SCN_SCORE_KIND_PLAYER;
    evt.u.scnScore.target = 2;
    evt.u.scnScore.score = 7;
    memset(evt.u.scnScore.label, 'L', sizeof(evt.u.scnScore.label));
    {
        ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SCN_SCORE);
        uint8_t body[32];
        size_t  bodyLen = 0;

        UT_ASSERT(enc != NULL);
        UT_ASSERT(enc(&evt, NULL, body, sizeof(body), &bodyLen) == ENCODE_OK);
        UT_ASSERT_MSG(bodyLen == 7 + sizeof(evt.u.scnScore.label) - 1,
                      "a full-width label encoded to %u body bytes",
                      (unsigned)bodyLen);
        UT_ASSERT_MSG(body[6] == (uint8_t)(sizeof(evt.u.scnScore.label) - 1),
                      "the encoder declared a label length of %u",
                      (unsigned)body[6]);
        UT_ASSERT_MSG(decodeBody(CTRL_SCN_SCORE, body, bodyLen, &back),
                      "the decoder refused the encoder's own body");
        UT_ASSERT(back.u.scnScore.kind == SCN_SCORE_KIND_PLAYER);
        UT_ASSERT(back.u.scnScore.target == 2);
        UT_ASSERT(back.u.scnScore.score == 7);
        UT_ASSERT(strcmp(back.u.scnScore.label, "LLLLLLLLLLLLLLL") == 0);
    }

    /* None of the four has a full-packet encoder: they ride
     * CHANNEL_CONTROL body-only, as CTRL_SIM_RULES does. */
    UT_ASSERT(transportControlCodecEncoder(CTRL_SCN_PANEL) == NULL);
    UT_ASSERT(transportControlCodecEncoder(CTRL_SCN_SCORE) == NULL);
    UT_ASSERT(transportControlCodecEncoder(CTRL_SCN_ANNOUNCE) == NULL);
    UT_ASSERT(transportControlCodecEncoder(CTRL_SCN_MARKER) == NULL);

    return 0;
}

/* A body the decoder must refuse. The event is pre-filled with a
 * sentinel and checked afterwards: a refusal writes nothing. */
static int refusesBody(const char *what, ControlEventType type,
                       const uint8_t *body, size_t len) {
    ControlEvent evt;
    memset(&evt, 0xAB, sizeof(evt));
    UT_ASSERT_MSG(!decodeBody(type, body, len, &evt), "%s: was accepted", what);
    UT_ASSERT_MSG(evt.u.scnPanel.panel == 0xAB,
                  "%s: refusal wrote into the event", what);
    return 0;
}

int run_scn_presentation_codec_refuses_short(void) {
    uint8_t buf[SCN_PANEL_MAX + 16];
    size_t i;

    /* One byte short of each fixed part. */
    if (refusesBody("panel short header", CTRL_SCN_PANEL, kPanelBody, 2)) return 1;
    if (refusesBody("score short header", CTRL_SCN_SCORE, kScoreBody, 6)) return 1;
    if (refusesBody("announce short header", CTRL_SCN_ANNOUNCE,
                    kAnnounceBody, 1)) return 1;
    if (refusesBody("marker short body", CTRL_SCN_MARKER, kMarkerBody,
                    sizeof(kMarkerBody) - 1)) return 1;
    /* And a marker one byte long, which is fixed-length both ways. */
    if (refusesBody("marker long body", CTRL_SCN_MARKER, kMarkerBody,
                    sizeof(kMarkerBody) + 1)) return 1;

    /* A panel declaring more bytes than follow it. */
    memcpy(buf, kPanelBody, sizeof(kPanelBody));
    buf[1] = 0x00;
    buf[2] = 0x40;   /* says 64 bytes; four follow */
    if (refusesBody("panel len overruns", CTRL_SCN_PANEL, buf,
                    sizeof(kPanelBody))) return 1;

    /* And one declaring more than a list may ever hold. */
    buf[1] = 0xFF;
    buf[2] = 0xFF;
    if (refusesBody("panel len past the cap", CTRL_SCN_PANEL, buf,
                    sizeof(kPanelBody))) return 1;

    /* An announce whose text fills the field exactly has no room left
     * for the terminator the decoder adds, so it is one too many. */
    buf[0] = 0x00;
    buf[1] = 0x01;
    for (i = 0; i < (size_t)PACKET_MAX_CHAT_MESSAGE + 1; i++) {
        buf[2 + i] = 'x';
    }
    if (refusesBody("announce text past the field", CTRL_SCN_ANNOUNCE, buf,
                    2 + (size_t)PACKET_MAX_CHAT_MESSAGE + 1)) return 1;

    /* A score label longer than label[] holds. */
    memcpy(buf, kScoreBody, 7);
    buf[6] = 16;   /* label[] is 16 bytes, so 16 leaves no terminator */
    for (i = 0; i < 16; i++) {
        buf[7 + i] = 'y';
    }
    if (refusesBody("score label past the field", CTRL_SCN_SCORE, buf, 7 + 16))
        return 1;

    /* A score whose declared label length disagrees with the body. */
    buf[6] = 5;
    if (refusesBody("score label disagrees", CTRL_SCN_SCORE, buf, 7 + 16))
        return 1;

    return 0;
}

int run_scn_presentation_decoder_sets_broadcast(void) {
    ControlEvent evt;

    /* 0 is a real slot and 0 is a real team, so a decoder that left the
     * pair alone would hand every wire client an event addressed to
     * slot 0. Each decoder sets 0xFF for the same reason
     * decodeServerTextBody does. */
    memset(&evt, 0xAB, sizeof(evt));
    UT_ASSERT(decodeBody(CTRL_SCN_PANEL, kPanelBody, sizeof(kPanelBody), &evt));
    UT_ASSERT(evt.u.scnPanel.destPlayer == 0xFF);
    UT_ASSERT(evt.u.scnPanel.destTeam == 0);

    memset(&evt, 0xAB, sizeof(evt));
    UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kAnnounceBody,
                         sizeof(kAnnounceBody), &evt));
    UT_ASSERT(evt.u.scnAnnounce.destPlayer == 0xFF);
    UT_ASSERT(evt.u.scnAnnounce.destTeam == 0);

    memset(&evt, 0xAB, sizeof(evt));
    UT_ASSERT(decodeBody(CTRL_SCN_MARKER, kMarkerBody, sizeof(kMarkerBody),
                         &evt));
    UT_ASSERT(evt.u.scnMarker.destPlayer == 0xFF);
    UT_ASSERT(evt.u.scnMarker.destTeam == 0);

    /* CTRL_SCN_SCORE carries no pair at all — it is broadcast, and its
     * target says whose score it is rather than who receives it. */
    memset(&evt, 0xAB, sizeof(evt));
    UT_ASSERT(decodeBody(CTRL_SCN_SCORE, kScoreBody, sizeof(kScoreBody), &evt));
    UT_ASSERT(evt.u.scnScore.target == 3);

    return 0;
}

/* ── The in-process filter ────────────────────────────────────────── */

/* A ClientSim holding slot `me` on team `team`. The filter reads the
 * team off the lobby slot mirror, so the slot is set the way the server
 * would set it. */
static ClientSim *scnClientOnTeam(BYTE me, uint8_t team) {
    ControlEvent evt;
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, me);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SLOT;
    evt.u.lobbySlot.playerNum = me;
    evt.u.lobbySlot.slot.connected = true;
    evt.u.lobbySlot.slot.teamNumber = team;
    clientSimApplyControl(cs, &evt);
    return cs;
}

/* A panel event for panel 0 holding one sprite, addressed as asked. */
static void scnPanelEventFor(ControlEvent *evt, uint8_t destTeam,
                             uint8_t destPlayer) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_SCN_PANEL;
    evt->u.scnPanel.panel = 0;
    evt->u.scnPanel.len = 4;
    evt->u.scnPanel.bytes[0] = SCN_PANEL_OP_SPRITE;
    evt->u.scnPanel.bytes[1] = 1;
    evt->u.scnPanel.bytes[2] = 2;
    evt->u.scnPanel.bytes[3] = 3;
    evt->u.scnPanel.destTeam = destTeam;
    evt->u.scnPanel.destPlayer = destPlayer;
}

int run_scn_presentation_client_filters(void) {
    ControlEvent evt;
    ClientSim *cs;

    /* This client is slot 2 on team 1. */
    cs = scnClientOnTeam(2, 1);
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimGetScnPanel(cs, 0) == NULL);

    /* Addressed to another team: not stored. The stored list is what is
     * read, not a queue depth — nothing drains it. */
    scnPanelEventFor(&evt, 2, 0xFF);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScnPanel(cs, 0) == NULL,
                  "a panel for another team was stored");

    /* Addressed to another slot: not stored. */
    scnPanelEventFor(&evt, 0, 5);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScnPanel(cs, 0) == NULL,
                  "a panel for another slot was stored");

    /* Addressed to everyone: stored. */
    scnPanelEventFor(&evt, 0, 0xFF);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScnPanel(cs, 0) != NULL,
                  "a panel for everyone was dropped");
    UT_ASSERT(clientSimGetScnPanel(cs, 0)->count == 1);
    UT_ASSERT(clientSimGetScnPanel(cs, 0)->items[0].op == SCN_PANEL_OP_SPRITE);
    UT_ASSERT(clientSimGetScnPanel(cs, 0)->items[0].u.sprite.tile == 3);

    /* Addressed to this client's own team and own slot: also stored. */
    scnPanelEventFor(&evt, 1, 2);
    evt.u.scnPanel.bytes[3] = 4;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanel(cs, 0) != NULL);
    UT_ASSERT(clientSimGetScnPanel(cs, 0)->items[0].u.sprite.tile == 4);

    /* A list the parser refuses is dropped and counted; the panel keeps
     * what it was showing. Opcode 0 is not a primitive. */
    UT_ASSERT(clientSimGetScnPanelRejectCount(cs) == 0);
    scnPanelEventFor(&evt, 0, 0xFF);
    evt.u.scnPanel.bytes[0] = 0;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanelRejectCount(cs) == 1);
    UT_ASSERT(clientSimGetScnPanel(cs, 0) != NULL);
    UT_ASSERT(clientSimGetScnPanel(cs, 0)->items[0].u.sprite.tile == 4);

    /* An announcement and a marker take the same test. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.ticks = 100;
    memcpy(evt.u.scnAnnounce.text, "hidden", 7);
    evt.u.scnAnnounce.destTeam = 2;      /* not this client's team */
    evt.u.scnAnnounce.destPlayer = 0xFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnounce(cs, NULL, NULL) == NULL);

    evt.u.scnAnnounce.destTeam = 0;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnounce(cs, NULL, NULL) != NULL);
    UT_ASSERT(strcmp(clientSimGetScnAnnounce(cs, NULL, NULL), "hidden") == 0);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_MARKER;
    evt.u.scnMarker.id = 3;
    evt.u.scnMarker.kind = SCN_MARKER_KIND_SQUARE;
    evt.u.scnMarker.x = 8;
    evt.u.scnMarker.y = 9;
    evt.u.scnMarker.destTeam = 0;
    evt.u.scnMarker.destPlayer = 7;      /* not this client's slot */
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnMarker(cs, 3) != NULL);
    UT_ASSERT(!clientSimGetScnMarker(cs, 3)->active);

    evt.u.scnMarker.destPlayer = 0xFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnMarker(cs, 3)->active);
    UT_ASSERT(clientSimGetScnMarker(cs, 3)->x == 8);
    UT_ASSERT(clientSimGetScnMarker(cs, 3)->y == 9);

    /* CLEAR removes the id rather than storing a third kind. */
    evt.u.scnMarker.kind = SCN_MARKER_KIND_CLEAR;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(!clientSimGetScnMarker(cs, 3)->active);

    /* A score is broadcast: it is kept whatever team this client is on. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_SCORE;
    evt.u.scnScore.kind = SCN_SCORE_KIND_TEAM;
    evt.u.scnScore.target = 2;           /* not this client's team */
    evt.u.scnScore.score = 42;
    memcpy(evt.u.scnScore.label, "Kills", 6);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnTeamScore(cs, 2) != NULL);
    UT_ASSERT(clientSimGetScnTeamScore(cs, 2)->valid);
    UT_ASSERT(clientSimGetScnTeamScore(cs, 2)->score == 42);
    UT_ASSERT(strcmp(clientSimGetScnTeamScore(cs, 2)->label, "Kills") == 0);
    UT_ASSERT(!clientSimGetScnPlayerScore(cs, 2)->valid);

    /* The return to lobby drops all of it. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanel(cs, 0) == NULL);
    UT_ASSERT(clientSimGetScnAnnounce(cs, NULL, NULL) == NULL);
    UT_ASSERT(!clientSimGetScnMarker(cs, 3)->active);
    UT_ASSERT(!clientSimGetScnTeamScore(cs, 2)->valid);
    UT_ASSERT(clientSimGetScnPanelRejectCount(cs) == 0);

    clientSimDestroy(cs);
    return 0;
}

/* ── The status line and the announcement's position ──────────────── */

/* CTRL_SCN_STATUS: [endsAt 4 BE][text, the rest of the body]. */
static const uint8_t kStatusBody[] = {
    0x00, 0x01, 0x86, 0xA0,        /* endsAt 100000 */
    'W', 'a', 'v', 'e', ' ', '3', '/', '1', '0'
};

/* A CTRL_SCN_ANNOUNCE with a position: [ticks 2 BE][text][0x00][x 1][y 1]. */
static const uint8_t kAnnouncePosBody[] = {
    0x01, 0xF4,                    /* ticks 500 */
    'H', 'i',
    0x00, 0x7F, 0x00               /* x 127 (the middle), y 0 (the top) */
};

/*
 * scn_status_codec_and_client — the status body both ways against
 * hand-written bytes, and what a client keeps: addressed like the other
 * presentation events, replaced by the next line, cleared by an empty one
 * and by the return to the lobby.
 */
int run_scn_status_codec_and_client(void) {
    ControlEvent evt;
    ControlEvent back;
    ClientSim   *cs;
    uint32_t     endsAt = 0;
    size_t       i;

    /* ── status, both ways ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_STATUS;
    memcpy(evt.u.scnStatus.text, "Wave 3/10", 10);
    evt.u.scnStatus.endsAt = 100000;
    evt.u.scnStatus.destTeam = 1;      /* never travels */
    evt.u.scnStatus.destPlayer = 2;
    if (encodesTo("status", &evt, kStatusBody, sizeof(kStatusBody))) return 1;
    memset(&back, 0xAB, sizeof(back));
    UT_ASSERT(decodeBody(CTRL_SCN_STATUS, kStatusBody, sizeof(kStatusBody),
                         &back));
    UT_ASSERT(back.type == CTRL_SCN_STATUS);
    UT_ASSERT(back.u.scnStatus.endsAt == 100000);
    UT_ASSERT(strcmp(back.u.scnStatus.text, "Wave 3/10") == 0);
    UT_ASSERT(back.u.scnStatus.destTeam == 0);
    UT_ASSERT(back.u.scnStatus.destPlayer == 0xFF);
    UT_ASSERT(transportControlCodecEncoder(CTRL_SCN_STATUS) == NULL);

    /* The clear is the four-byte header alone. */
    {
        static const uint8_t kClear[] = { 0xFF, 0xFF, 0xFF, 0xFF };
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_SCN_STATUS;
        evt.u.scnStatus.endsAt = SCN_STATUS_NO_COUNTDOWN;
        if (encodesTo("status clear", &evt, kClear, sizeof(kClear))) return 1;
        UT_ASSERT(decodeBody(CTRL_SCN_STATUS, kClear, sizeof(kClear), &back));
        UT_ASSERT(back.u.scnStatus.text[0] == '\0');
        UT_ASSERT(back.u.scnStatus.endsAt == SCN_STATUS_NO_COUNTDOWN);
    }

    /* A short header, and a text past the field, are refused. */
    if (refusesBody("status short header", CTRL_SCN_STATUS, kStatusBody, 3))
        return 1;
    {
        uint8_t buf[4 + PACKET_MAX_CHAT_MESSAGE + 1];
        memset(buf, 0, 4);
        for (i = 0; i < (size_t)PACKET_MAX_CHAT_MESSAGE + 1; i++) {
            buf[4 + i] = 'x';
        }
        if (refusesBody("status text past the field", CTRL_SCN_STATUS, buf,
                        sizeof(buf))) return 1;
    }

    /* Bytes after a 0x00 are room for a later build and are skipped. */
    {
        static const uint8_t kExt[] = { 0, 0, 0, 5, 'A', 0x00, 0x42, 0x43 };
        UT_ASSERT(decodeBody(CTRL_SCN_STATUS, kExt, sizeof(kExt), &back));
        UT_ASSERT(strcmp(back.u.scnStatus.text, "A") == 0);
        UT_ASSERT(back.u.scnStatus.endsAt == 5);
    }

    /* ── the client ── */
    cs = scnClientOnTeam(2, 1);
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimGetScnStatus(cs, &endsAt) == NULL);
    UT_ASSERT(endsAt == SCN_STATUS_NO_COUNTDOWN);

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_STATUS;
    memcpy(evt.u.scnStatus.text, "hidden", 7);
    evt.u.scnStatus.endsAt = 777;
    evt.u.scnStatus.destTeam = 2;      /* not this client's team */
    evt.u.scnStatus.destPlayer = 0xFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimGetScnStatus(cs, NULL) == NULL,
                  "a status line for another team was kept");
    evt.u.scnStatus.destTeam = 0;
    evt.u.scnStatus.destPlayer = 5;    /* not this client's slot */
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnStatus(cs, NULL) == NULL);

    evt.u.scnStatus.destPlayer = 0xFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnStatus(cs, &endsAt) != NULL);
    UT_ASSERT(strcmp(clientSimGetScnStatus(cs, NULL), "hidden") == 0);
    UT_ASSERT(endsAt == 777);

    /* The next line replaces it, and a line with no countdown has none. */
    memcpy(evt.u.scnStatus.text, "Wave 1/5 over", 14);
    evt.u.scnStatus.endsAt = SCN_STATUS_NO_COUNTDOWN;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(strcmp(clientSimGetScnStatus(cs, &endsAt), "Wave 1/5 over") == 0);
    UT_ASSERT(endsAt == SCN_STATUS_NO_COUNTDOWN);

    /* An empty line clears it. */
    evt.u.scnStatus.text[0] = '\0';
    evt.u.scnStatus.endsAt = 5;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnStatus(cs, &endsAt) == NULL);
    UT_ASSERT(endsAt == SCN_STATUS_NO_COUNTDOWN);

    /* The return to the lobby drops the status line. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_STATUS;
    memcpy(evt.u.scnStatus.text, "Wave 2/5", 9);
    evt.u.scnStatus.endsAt = 900;
    evt.u.scnStatus.destPlayer = 0xFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnStatus(cs, NULL) != NULL);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnStatus(cs, NULL) == NULL);

    clientSimDestroy(cs);
    return 0;
}

/* What a client from before the position does with an announcement body:
 * main's decodeScnAnnounceBody, copied. It refuses a text part of the field
 * or longer, then stores the whole rest of the body and terminates it, so a
 * 0x00 in the body ends the string it draws. False when it drops the line. */
static bool oldClientAnnounceText(const uint8_t *buf, size_t len,
                                  char out[PACKET_MAX_CHAT_MESSAGE + 1]) {
    size_t textLen;
    if (len < 2) return false;
    textLen = len - 2;
    if (textLen >= PACKET_MAX_CHAT_MESSAGE + 1) return false;
    memset(out, 0, PACKET_MAX_CHAT_MESSAGE + 1);
    if (textLen > 0) memcpy(out, buf + 2, textLen);
    out[textLen] = '\0';
    return true;
}

/*
 * scn_announce_position_codec — the announcement's position both ways
 * against hand-written bytes, what a client from before the position does
 * with each body, the text limits on both sides of them, and what a client
 * keeps.
 */
int run_scn_announce_position_codec(void) {
    ControlEvent evt;
    ControlEvent back;
    ClientSim   *cs;
    char         oldText[PACKET_MAX_CHAT_MESSAGE + 1];
    uint8_t      x = 0;
    uint8_t      y = 0;
    size_t       i;

    /* ── both ways ── */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.ticks  = 500;
    memcpy(evt.u.scnAnnounce.text, "Hi", 3);
    evt.u.scnAnnounce.hasPos = 1;
    evt.u.scnAnnounce.posX   = (uint8_t)SCN_ANNOUNCE_POS_MID;
    evt.u.scnAnnounce.posY   = 0;
    if (encodesTo("announce with a position", &evt, kAnnouncePosBody,
                  sizeof(kAnnouncePosBody))) return 1;
    memset(&back, 0xAB, sizeof(back));
    UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kAnnouncePosBody,
                         sizeof(kAnnouncePosBody), &back));
    UT_ASSERT(back.type == CTRL_SCN_ANNOUNCE);
    UT_ASSERT(strcmp(back.u.scnAnnounce.text, "Hi") == 0);
    UT_ASSERT(back.u.scnAnnounce.ticks == 500);
    UT_ASSERT(back.u.scnAnnounce.hasPos == 1);
    UT_ASSERT(back.u.scnAnnounce.posX == SCN_ANNOUNCE_POS_MID);
    UT_ASSERT(back.u.scnAnnounce.posY == 0);

    /* Every byte pair round-trips, up to the far edge. */
    for (i = 0; i <= SCN_ANNOUNCE_POS_MAX; i += 127) {
        uint8_t body[sizeof(kAnnouncePosBody)];
        memcpy(body, kAnnouncePosBody, sizeof(body));
        body[5] = (uint8_t)i;
        body[6] = (uint8_t)(SCN_ANNOUNCE_POS_MAX - i);
        evt.u.scnAnnounce.posX = body[5];
        evt.u.scnAnnounce.posY = body[6];
        if (encodesTo("announce position edge", &evt, body, sizeof(body)))
            return 1;
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, body, sizeof(body), &back));
        UT_ASSERT(back.u.scnAnnounce.posX == body[5]);
        UT_ASSERT(back.u.scnAnnounce.posY == body[6]);
    }

    /* A line with no position is the body from before the position, byte
       for byte, and decodes with none. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.ticks = 300;
    memcpy(evt.u.scnAnnounce.text, "Go!", 4);
    evt.u.scnAnnounce.posX = 9;          /* ignored with no hasPos */
    evt.u.scnAnnounce.posY = 9;
    if (encodesTo("announce with no position", &evt, kAnnounceBody,
                  sizeof(kAnnounceBody))) return 1;
    UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kAnnounceBody,
                         sizeof(kAnnounceBody), &back));
    UT_ASSERT(back.u.scnAnnounce.hasPos == 0);

    /* A clear carries no position, whatever the event says. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.hasPos = 1;
    evt.u.scnAnnounce.posX   = 5;
    evt.u.scnAnnounce.posY   = 5;
    {
        static const uint8_t kAnnClear[] = { 0x00, 0x00 };
        static const uint8_t kAnnClearPos[] = { 0x00, 0x00, 0x00, 0x05, 0x05 };
        if (encodesTo("announce clear", &evt, kAnnClear, sizeof(kAnnClear)))
            return 1;
        /* And a clear that arrives with position bytes decodes with none. */
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kAnnClearPos,
                             sizeof(kAnnClearPos), &back));
        UT_ASSERT(back.u.scnAnnounce.text[0] == '\0');
        UT_ASSERT(back.u.scnAnnounce.hasPos == 0);
    }

    /* A byte of 255 reads as the far edge. */
    {
        static const uint8_t kFar[] = { 0, 1, 'A', 0x00, 0xFF, 0xFF };
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kFar, sizeof(kFar), &back));
        UT_ASSERT(back.u.scnAnnounce.hasPos == 1);
        UT_ASSERT(back.u.scnAnnounce.posX == SCN_ANNOUNCE_POS_MAX);
        UT_ASSERT(back.u.scnAnnounce.posY == SCN_ANNOUNCE_POS_MAX);
    }

    /* Bytes after the two position bytes are a later build's and are
       skipped; one byte after the 0x00 is no position at all. */
    {
        static const uint8_t kMore[] = { 0, 1, 'A', 0x00, 0x10, 0x20, 0x30 };
        static const uint8_t kOne[]  = { 0, 1, 'A', 0x00, 0x10 };
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kMore, sizeof(kMore), &back));
        UT_ASSERT(strcmp(back.u.scnAnnounce.text, "A") == 0);
        UT_ASSERT(back.u.scnAnnounce.hasPos == 1);
        UT_ASSERT(back.u.scnAnnounce.posX == 0x10);
        UT_ASSERT(back.u.scnAnnounce.posY == 0x20);
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, kOne, sizeof(kOne), &back));
        UT_ASSERT(strcmp(back.u.scnAnnounce.text, "A") == 0);
        UT_ASSERT(back.u.scnAnnounce.hasPos == 0);
    }

    /* ── an older client ── it draws the script's text, in its usual place,
       for a positioned line; and a line with no position is unchanged. */
    UT_ASSERT(oldClientAnnounceText(kAnnouncePosBody, sizeof(kAnnouncePosBody),
                                    oldText));
    UT_ASSERT_MSG(strcmp(oldText, "Hi") == 0,
                  "an older client read the positioned line as \"%s\"",
                  oldText);
    UT_ASSERT(oldClientAnnounceText(kAnnounceBody, sizeof(kAnnounceBody),
                                    oldText));
    UT_ASSERT(strcmp(oldText, "Go!") == 0);

    /* ── the limits ── a line with no position of the full 128 bytes, and
       a positioned line of 125, which both kinds of client take. A
       positioned line of 126 would be dropped by an older client, which is
       why the senders refuse it; this build still decodes it. */
    UT_ASSERT(SCN_ANNOUNCE_POSITIONED_TEXT_MAX == PACKET_MAX_CHAT_MESSAGE - 3);
    {
        uint8_t      buf[SCN_PANEL_MAX + 16];
        const size_t full = (size_t)PACKET_MAX_CHAT_MESSAGE;
        const size_t posMax = (size_t)SCN_ANNOUNCE_POSITIONED_TEXT_MAX;
        size_t       outLen = 0;
        ControlEncodeBodyFn enc =
            transportControlCodecBodyEncoder(CTRL_SCN_ANNOUNCE);
        UT_ASSERT(enc != NULL);

        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_SCN_ANNOUNCE;
        evt.u.scnAnnounce.ticks = 100;
        memset(evt.u.scnAnnounce.text, 'x', full);
        evt.u.scnAnnounce.text[full] = '\0';
        UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT(outLen == 2 + full);
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, buf, outLen, &back));
        UT_ASSERT(strlen(back.u.scnAnnounce.text) == full);
        UT_ASSERT(back.u.scnAnnounce.hasPos == 0);
        UT_ASSERT(oldClientAnnounceText(buf, outLen, oldText));
        UT_ASSERT(strlen(oldText) == full);

        memset(evt.u.scnAnnounce.text, 0, sizeof(evt.u.scnAnnounce.text));
        memset(evt.u.scnAnnounce.text, 'x', posMax);
        evt.u.scnAnnounce.hasPos = 1;
        evt.u.scnAnnounce.posX   = 200;
        evt.u.scnAnnounce.posY   = 50;
        UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT(outLen == 2 + posMax + 3);
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, buf, outLen, &back));
        UT_ASSERT(strlen(back.u.scnAnnounce.text) == posMax);
        UT_ASSERT(back.u.scnAnnounce.posX == 200);
        UT_ASSERT(back.u.scnAnnounce.posY == 50);
        UT_ASSERT_MSG(oldClientAnnounceText(buf, outLen, oldText),
                      "an older client drops a %u-byte positioned line",
                      (unsigned)posMax);
        UT_ASSERT(strlen(oldText) == posMax);

        /* One byte more is past what an older client takes. */
        evt.u.scnAnnounce.text[posMax] = 'x';
        UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        UT_ASSERT(outLen == 2 + posMax + 1 + 3);
        UT_ASSERT(!oldClientAnnounceText(buf, outLen, oldText));
        UT_ASSERT(decodeBody(CTRL_SCN_ANNOUNCE, buf, outLen, &back));
        UT_ASSERT(strlen(back.u.scnAnnounce.text) == posMax + 1);
    }

    /* ── the client ── it keeps the position with the line, drops it for
       a line with none and on the return to the lobby. */
    cs = scnClientOnTeam(2, 1);
    UT_ASSERT(cs != NULL);
    UT_ASSERT(!clientSimGetScnAnnouncePos(cs, &x, &y));

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.ticks = 100;
    memcpy(evt.u.scnAnnounce.text, "News", 5);
    evt.u.scnAnnounce.hasPos = 1;
    evt.u.scnAnnounce.posX = 30;
    evt.u.scnAnnounce.posY = 240;
    evt.u.scnAnnounce.destPlayer = 0xFF;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnouncePos(cs, &x, &y));
    UT_ASSERT(x == 30 && y == 240);
    UT_ASSERT(clientSimGetScnAnnouncePos(cs, NULL, NULL));

    evt.u.scnAnnounce.hasPos = 0;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(!clientSimGetScnAnnouncePos(cs, &x, &y),
                  "a line with no position kept the last line's position");

    evt.u.scnAnnounce.hasPos = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnouncePos(cs, NULL, NULL));
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnounce(cs, NULL, NULL) == NULL);
    UT_ASSERT(!clientSimGetScnAnnouncePos(cs, &x, &y));

    clientSimDestroy(cs);
    return 0;
}

/* The client holds up to SCN_ANNOUNCE_STACK_MAX announcements at once,
 * oldest first, each with its own arrival tick, so a second line does not
 * cover the first. A repeated line replaces its twin and becomes the
 * newest; a line whose time ran out is dropped when the next one lands; a
 * full stack drops its oldest; an empty line takes them all down; the
 * return to the lobby drops them all. */
int run_scn_announce_stack_client(void) {
    ClientSim               *cs;
    ControlEvent             evt;
    const ClientScnAnnounce *a;

    cs = scnClientOnTeam(2, 1);
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 0);
    UT_ASSERT(clientSimGetScnAnnounceAt(cs, 0) == NULL);

    /* Two lines that land together are both held, oldest first. */
    clientSimScnAnnouncePush(cs, "First", 400, 1000, false, 0, 0);
    clientSimScnAnnouncePush(cs, "Second", 300, 1000, true, 10, 20);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 2);
    a = clientSimGetScnAnnounceAt(cs, 0);
    UT_ASSERT(a != NULL && strcmp(a->text, "First") == 0);
    UT_ASSERT(a->ticks == 400 && a->arrivedTick == 1000 && a->hasPos == 0);
    a = clientSimGetScnAnnounceAt(cs, 1);
    UT_ASSERT(a != NULL && strcmp(a->text, "Second") == 0);
    UT_ASSERT(a->hasPos == 1 && a->posX == 10 && a->posY == 20);
    UT_ASSERT(clientSimGetScnAnnounceAt(cs, 2) == NULL);
    UT_ASSERT(clientSimGetScnAnnounceAt(cs, -1) == NULL);
    /* The one-line getters answer for the newest. */
    UT_ASSERT(strcmp(clientSimGetScnAnnounce(cs, NULL, NULL), "Second") == 0);
    UT_ASSERT(clientSimGetScnAnnouncePos(cs, NULL, NULL));

    /* The same text again replaces its twin and moves to the end. */
    clientSimScnAnnouncePush(cs, "First", 500, 1100, false, 0, 0);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 2);
    UT_ASSERT(strcmp(clientSimGetScnAnnounceAt(cs, 0)->text, "Second") == 0);
    a = clientSimGetScnAnnounceAt(cs, 1);
    UT_ASSERT(strcmp(a->text, "First") == 0);
    UT_ASSERT(a->ticks == 500 && a->arrivedTick == 1100);

    /* "Second" ran out at 1300; a line landing at 1300 drops it. */
    clientSimScnAnnouncePush(cs, "Third", 100, 1300, false, 0, 0);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 2);
    UT_ASSERT(strcmp(clientSimGetScnAnnounceAt(cs, 0)->text, "First") == 0);
    UT_ASSERT(strcmp(clientSimGetScnAnnounceAt(cs, 1)->text, "Third") == 0);

    /* A full stack drops its oldest. */
    clientSimScnAnnouncePush(cs, "Fourth", 1000, 1350, false, 0, 0);
    clientSimScnAnnouncePush(cs, "Fifth", 1000, 1350, false, 0, 0);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == SCN_ANNOUNCE_STACK_MAX);
    clientSimScnAnnouncePush(cs, "Sixth", 1000, 1350, false, 0, 0);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == SCN_ANNOUNCE_STACK_MAX);
    UT_ASSERT(strcmp(clientSimGetScnAnnounceAt(cs, 0)->text, "Third") == 0);
    UT_ASSERT(strcmp(clientSimGetScnAnnounceAt(cs, 3)->text, "Sixth") == 0);

    /* An empty line takes every line down. */
    clientSimScnAnnouncePush(cs, "", 0, 1400, false, 0, 0);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 0);
    UT_ASSERT(clientSimGetScnAnnounce(cs, NULL, NULL) == NULL);

    /* Through the control arm: two lines addressed to this client stack,
       and the return to the lobby drops them. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SCN_ANNOUNCE;
    evt.u.scnAnnounce.ticks = 400;
    evt.u.scnAnnounce.destPlayer = 0xFF;
    memcpy(evt.u.scnAnnounce.text, "One", 4);
    clientSimApplyControl(cs, &evt);
    memcpy(evt.u.scnAnnounce.text, "Two", 4);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 2);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnAnnounceCount(cs) == 0);

    clientSimDestroy(cs);
    return 0;
}

/* The client keeps one panel list per script. The panel byte on the wire
 * holds the panel id in the low four bits and the sending script's list
 * position in the high four. clientSimGetScnPanel answers the lowest
 * script whose list has something in it, which is the round's own
 * scenario when it is drawing; clientSimGetScnPanelOf answers one script. */
int run_client_scn_panel_owners(void) {
    ControlEvent evt;
    ClientSim   *cs = scnClientOnTeam(2, 1);
    UT_ASSERT(cs != NULL);

    /* A mod, list position 2, draws first. */
    scnPanelEventFor(&evt, 0, 0xFF);
    evt.u.scnPanel.panel = SCN_PANEL_WIRE(0, 2);
    evt.u.scnPanel.bytes[3] = 7;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 2) != NULL);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 2)->items[0].u.sprite.tile == 7);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0) == NULL);
    UT_ASSERT(clientSimGetScnPanel(cs, 0) == clientSimGetScnPanelOf(cs, 0, 2));

    /* The scenario, list position 0, draws next. The mod's list stays. */
    scnPanelEventFor(&evt, 0, 0xFF);
    evt.u.scnPanel.bytes[3] = 9;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0) != NULL);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0)->items[0].u.sprite.tile == 9);
    UT_ASSERT_MSG(clientSimGetScnPanelOf(cs, 0, 2) != NULL &&
                  clientSimGetScnPanelOf(cs, 0, 2)->items[0].u.sprite.tile == 7,
                  "the scenario's list overwrote the mod's");
    UT_ASSERT(clientSimGetScnPanel(cs, 0) == clientSimGetScnPanelOf(cs, 0, 0));

    /* Out of range asks answer nothing. */
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, SCN_PANEL_OWNERS) == NULL);
    UT_ASSERT(clientSimGetScnPanelOf(cs, SCN_PANEL_IDS, 0) == NULL);

    /* A panel byte naming an owner past the list is dropped. */
    scnPanelEventFor(&evt, 0, 0xFF);
    evt.u.scnPanel.panel = SCN_PANEL_WIRE(0, 15);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0)->items[0].u.sprite.tile == 9);

    /* The scenario clears its panel. The one-slot reader moves on to the
       mod's list, which still has something in it. */
    scnPanelEventFor(&evt, 0, 0xFF);
    evt.u.scnPanel.len = 0;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0) != NULL);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0)->count == 0);
    UT_ASSERT_MSG(clientSimGetScnPanel(cs, 0) ==
                  clientSimGetScnPanelOf(cs, 0, 2),
                  "a cleared scenario panel hid the mod's list");

    /* The mod clears too: the reader answers the lowest cleared list, so a
       cleared panel still reads apart from one never sent. */
    scnPanelEventFor(&evt, 0, 0xFF);
    evt.u.scnPanel.panel = SCN_PANEL_WIRE(0, 2);
    evt.u.scnPanel.len = 0;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanel(cs, 0) == clientSimGetScnPanelOf(cs, 0, 0));
    UT_ASSERT(clientSimGetScnPanel(cs, 0)->count == 0);

    /* The return to lobby drops every script's list. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 0) == NULL);
    UT_ASSERT(clientSimGetScnPanelOf(cs, 0, 2) == NULL);

    clientSimDestroy(cs);
    return 0;
}
