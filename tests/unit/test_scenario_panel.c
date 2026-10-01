/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The scenario panel's display list: the byte layout and the one
 * shared parser every frontend refuses a malformed list with.
 *
 * The decode case reads a hand-written byte array and checks every
 * decoded field against a value written out by hand beside it. The
 * input is deliberately not built with scnPanelWrite: a fixture that
 * reads its own bytes back through the codec under test passes
 * whenever the two drift together, and a wire format only holds if
 * something outside the codec says what the bytes are.
 *
 * The refusal case names the exact ScnPanelResult each rule answers
 * with, and checks a refused list is a refusal rather than a partial
 * draw. The boundary case holds the four caps at their edges, and the
 * round-trip case writes a decoded list back out and parses it again.
 */
#include <stdint.h>
#include <string.h>

#include "global.h"          /* MAX_TANKS, GAME_NUMTOTALTICKS_SEC */
#include "scenario_panel.h"
#include "scenario_panel_draw.h" /* scnPanelTimerText — the drawer's one
                                  * piece of arithmetic, which is plain C
                                  * and needs no renderer behind it */
#include "scenario_defs.h"   /* SCN_PANEL_MAX — the byte cap one list rides in */
#include "test_harness.h"

/* One of each primitive, written out as the bytes that travel. Every
 * value here is repeated in the assertions below as a hand-written
 * number, so the layout is pinned from both ends.
 *
 * Total: 7 + 6 + 12 + 7 + 4 + 10 + 11 = 57 bytes. */
static const uint8_t kGoldenList[] = {
    /* rect:   x=5, y=6, w=16, h=32, colour=RED(5), fill=1 */
    0x01, 0x05, 0x06, 0x10, 0x20, 0x05, 0x01,
    /* line:   x0=0, y0=127, x1=127, y1=0, colour=GREEN(6) */
    0x02, 0x00, 0x7F, 0x7F, 0x00, 0x06,
    /* text:   x=10, y=11, colour=WHITE(2), size=NORMAL(1), align=CENTRE(1),
     *         len=5, "Bolo!" */
    0x03, 0x0A, 0x0B, 0x02, 0x01, 0x01, 0x05, 0x42, 0x6F, 0x6C, 0x6F, 0x21,
    /* name:   x=4, y=12, colour=YELLOW(8), size=SMALL(0), align=RIGHT(2),
     *         slot=3 */
    0x04, 0x04, 0x0C, 0x08, 0x00, 0x02, 0x03,
    /* sprite: x=64, y=65, tile=146 */
    0x05, 0x40, 0x41, 0x92,
    /* bar:    x=2, y=3, w=64, h=6, colour=MAGENTA(11),
     *         value=300 (0x012C), max=1000 (0x03E8) */
    0x06, 0x02, 0x03, 0x40, 0x06, 0x0B, 0x01, 0x2C, 0x03, 0xE8,
    /* timer:  x=80, y=81, colour=CYAN(10), size=NORMAL(1), align=LEFT(0),
     *         mode=UP(1), tick=60000 (0x0000EA60) */
    0x07, 0x50, 0x51, 0x0A, 0x01, 0x00, 0x01, 0x00, 0x00, 0xEA, 0x60
};

#define GOLDEN_LIST_BYTES 57

int run_scenario_panel_parses_each_primitive(void) {
    ScnPanelList list;
    const ScnPanelItem *it;

    UT_ASSERT(sizeof(kGoldenList) == GOLDEN_LIST_BYTES);
    UT_ASSERT(scnPanelParse(kGoldenList, (uint16_t)sizeof(kGoldenList), &list) ==
              SCN_PANEL_OK);
    UT_ASSERT(list.count == 7);

    it = &list.items[0];
    UT_ASSERT(it->op == SCN_PANEL_OP_RECT);
    UT_ASSERT(it->u.rect.x == 5);
    UT_ASSERT(it->u.rect.y == 6);
    UT_ASSERT(it->u.rect.w == 16);
    UT_ASSERT(it->u.rect.h == 32);
    UT_ASSERT(it->u.rect.colour == SCN_PANEL_COLOUR_RED);
    UT_ASSERT(it->u.rect.fill == 1);

    it = &list.items[1];
    UT_ASSERT(it->op == SCN_PANEL_OP_LINE);
    UT_ASSERT(it->u.line.x0 == 0);
    UT_ASSERT(it->u.line.y0 == 127);
    UT_ASSERT(it->u.line.x1 == 127);
    UT_ASSERT(it->u.line.y1 == 0);
    UT_ASSERT(it->u.line.colour == SCN_PANEL_COLOUR_GREEN);

    it = &list.items[2];
    UT_ASSERT(it->op == SCN_PANEL_OP_TEXT);
    UT_ASSERT(it->u.text.x == 10);
    UT_ASSERT(it->u.text.y == 11);
    UT_ASSERT(it->u.text.colour == SCN_PANEL_COLOUR_WHITE);
    UT_ASSERT(it->u.text.size == SCN_PANEL_SIZE_NORMAL);
    UT_ASSERT(it->u.text.align == SCN_PANEL_ALIGN_CENTRE);
    UT_ASSERT(it->u.text.len == 5);
    UT_ASSERT(strcmp(it->u.text.text, "Bolo!") == 0);
    UT_ASSERT(it->u.text.text[5] == '\0');

    it = &list.items[3];
    UT_ASSERT(it->op == SCN_PANEL_OP_NAME);
    UT_ASSERT(it->u.name.x == 4);
    UT_ASSERT(it->u.name.y == 12);
    UT_ASSERT(it->u.name.colour == SCN_PANEL_COLOUR_YELLOW);
    UT_ASSERT(it->u.name.size == SCN_PANEL_SIZE_SMALL);
    UT_ASSERT(it->u.name.align == SCN_PANEL_ALIGN_RIGHT);
    UT_ASSERT(it->u.name.slot == 3);

    it = &list.items[4];
    UT_ASSERT(it->op == SCN_PANEL_OP_SPRITE);
    UT_ASSERT(it->u.sprite.x == 64);
    UT_ASSERT(it->u.sprite.y == 65);
    UT_ASSERT(it->u.sprite.tile == 146);

    it = &list.items[5];
    UT_ASSERT(it->op == SCN_PANEL_OP_BAR);
    UT_ASSERT(it->u.bar.x == 2);
    UT_ASSERT(it->u.bar.y == 3);
    UT_ASSERT(it->u.bar.w == 64);
    UT_ASSERT(it->u.bar.h == 6);
    UT_ASSERT(it->u.bar.colour == SCN_PANEL_COLOUR_MAGENTA);
    UT_ASSERT(it->u.bar.value == 300);   /* big-endian, so not 0x2C01 */
    UT_ASSERT(it->u.bar.max == 1000);

    it = &list.items[6];
    UT_ASSERT(it->op == SCN_PANEL_OP_TIMER);
    UT_ASSERT(it->u.timer.x == 80);
    UT_ASSERT(it->u.timer.y == 81);
    UT_ASSERT(it->u.timer.colour == SCN_PANEL_COLOUR_CYAN);
    UT_ASSERT(it->u.timer.size == SCN_PANEL_SIZE_NORMAL);
    UT_ASSERT(it->u.timer.align == SCN_PANEL_ALIGN_LEFT);
    UT_ASSERT(it->u.timer.mode == SCN_PANEL_TIMER_UP);
    UT_ASSERT(it->u.timer.tick == 60000u);

    /* The opcode values travel in recordings, so they are held here too. */
    UT_ASSERT(SCN_PANEL_OP_RECT == 1);
    UT_ASSERT(SCN_PANEL_OP_LINE == 2);
    UT_ASSERT(SCN_PANEL_OP_TEXT == 3);
    UT_ASSERT(SCN_PANEL_OP_NAME == 4);
    UT_ASSERT(SCN_PANEL_OP_SPRITE == 5);
    UT_ASSERT(SCN_PANEL_OP_BAR == 6);
    UT_ASSERT(SCN_PANEL_OP_TIMER == 7);
    UT_ASSERT(SCN_PANEL_UNITS == 128);
    UT_ASSERT(SCN_PANEL_IDS == 1);
    UT_ASSERT(SCN_PANEL_COLOURS == 16);

    return 0;
}

/* Parse `bytes` expecting `want`, and check the refusal left the list
 * alone. A caller's list is only ever replaced whole, so a refused
 * parse must not leave a half-decoded one behind for a frontend to
 * draw. 0 on success, 1 on failure — callers return it straight up. */
static int refuses(const char *what, const uint8_t *bytes, uint16_t len,
                   ScnPanelResult want) {
    ScnPanelList list;
    ScnPanelResult got;

    memset(&list, 0, sizeof(list));
    list.count = 0xAB;                 /* a value no parse would write */
    got = scnPanelParse(bytes, len, &list);
    UT_ASSERT_MSG(got == want, "%s: wanted result %d, got %d", what,
                  (int)want, (int)got);
    UT_ASSERT_MSG(list.count == 0xAB, "%s: refusal wrote a partial list", what);
    return 0;
}

int run_scenario_panel_refuses_malformed(void) {
    /* A rect whose operands run off the end. */
    static const uint8_t shortRect[] = { 0x01, 0x05, 0x06 };
    /* A text declaring five bytes with two behind it. */
    static const uint8_t shortText[] = {
        0x03, 0x00, 0x00, 0x02, 0x00, 0x00, 0x05, 'a', 'b'
    };
    static const uint8_t opcodeZero[] = { 0x00 };
    static const uint8_t opcodeEight[] = { 0x08, 0, 0, 0, 0, 0, 0 };
    /* rect with colour one past the palette. */
    static const uint8_t badColour[] = { 0x01, 0, 0, 4, 4, 0x10, 0x00 };
    /* rect with a fill that is neither outlined nor filled. */
    static const uint8_t badFill[] = { 0x01, 0, 0, 4, 4, 0x02, 0x02 };
    /* text with a size that is neither small nor normal. */
    static const uint8_t badSize[] = { 0x03, 0, 0, 0x02, 0x02, 0x00, 0x00 };
    /* text aligned past right. */
    static const uint8_t badAlign[] = { 0x03, 0, 0, 0x02, 0x00, 0x03, 0x00 };
    /* timer counting neither down nor up. */
    static const uint8_t badMode[] = {
        0x07, 0, 0, 0x02, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00
    };
    /* name pointing at a slot the roster has not got. */
    static const uint8_t badSlot[] = {
        0x04, 0, 0, 0x02, 0x00, 0x00, (uint8_t)MAX_TANKS
    };
    /* text carrying a newline: a script must not write a control byte
     * into a frontend's draw. */
    static const uint8_t controlByte[] = {
        0x03, 0, 0, 0x02, 0x00, 0x00, 0x02, 'a', '\n'
    };
    uint8_t tooMany[(SCN_PANEL_ITEMS_MAX + 1) * 4];
    int i;

    if (refuses("truncated rect", shortRect, (uint16_t)sizeof(shortRect),
                SCN_PANEL_ERR_TRUNCATED)) return 1;
    if (refuses("truncated text body", shortText, (uint16_t)sizeof(shortText),
                SCN_PANEL_ERR_TRUNCATED)) return 1;
    if (refuses("opcode 0", opcodeZero, (uint16_t)sizeof(opcodeZero),
                SCN_PANEL_ERR_OPCODE)) return 1;
    if (refuses("opcode 8", opcodeEight, (uint16_t)sizeof(opcodeEight),
                SCN_PANEL_ERR_OPCODE)) return 1;
    if (refuses("colour 16", badColour, (uint16_t)sizeof(badColour),
                SCN_PANEL_ERR_RANGE)) return 1;
    if (refuses("fill 2", badFill, (uint16_t)sizeof(badFill),
                SCN_PANEL_ERR_RANGE)) return 1;
    if (refuses("size 2", badSize, (uint16_t)sizeof(badSize),
                SCN_PANEL_ERR_RANGE)) return 1;
    if (refuses("align 3", badAlign, (uint16_t)sizeof(badAlign),
                SCN_PANEL_ERR_RANGE)) return 1;
    if (refuses("mode 2", badMode, (uint16_t)sizeof(badMode),
                SCN_PANEL_ERR_RANGE)) return 1;
    if (refuses("slot past the roster", badSlot, (uint16_t)sizeof(badSlot),
                SCN_PANEL_ERR_RANGE)) return 1;
    if (refuses("newline in text", controlByte, (uint16_t)sizeof(controlByte),
                SCN_PANEL_ERR_TEXT)) return 1;

    /* One sprite past the item cap. */
    for (i = 0; i < SCN_PANEL_ITEMS_MAX + 1; i++) {
        tooMany[i * 4 + 0] = SCN_PANEL_OP_SPRITE;
        tooMany[i * 4 + 1] = 1;
        tooMany[i * 4 + 2] = 2;
        tooMany[i * 4 + 3] = 3;
    }
    if (refuses("129 primitives", tooMany, (uint16_t)sizeof(tooMany),
                SCN_PANEL_ERR_TOO_MANY)) return 1;

    return 0;
}

/* Write one text primitive of `textLen` printable bytes at *off. */
static void putText(uint8_t *buf, uint16_t *off, uint8_t textLen) {
    uint8_t i;
    buf[(*off)++] = SCN_PANEL_OP_TEXT;
    buf[(*off)++] = 0;                          /* x */
    buf[(*off)++] = 0;                          /* y */
    buf[(*off)++] = SCN_PANEL_COLOUR_WHITE;
    buf[(*off)++] = SCN_PANEL_SIZE_SMALL;
    buf[(*off)++] = SCN_PANEL_ALIGN_LEFT;
    buf[(*off)++] = textLen;
    for (i = 0; i < textLen; i++) {
        buf[(*off)++] = (uint8_t)('a' + (i % 26));
    }
}

int run_scenario_panel_boundaries(void) {
    uint8_t buf[SCN_PANEL_MAX];
    uint8_t sprites[(SCN_PANEL_ITEMS_MAX + 1) * 4];
    ScnPanelList list;
    uint16_t off;
    int i;

    /* The item cap, both sides of it. */
    for (i = 0; i < SCN_PANEL_ITEMS_MAX + 1; i++) {
        sprites[i * 4 + 0] = SCN_PANEL_OP_SPRITE;
        sprites[i * 4 + 1] = (uint8_t)i;
        sprites[i * 4 + 2] = (uint8_t)(i + 1);
        sprites[i * 4 + 3] = 0;
    }
    UT_ASSERT(scnPanelParse(sprites, SCN_PANEL_ITEMS_MAX * 4, &list) ==
              SCN_PANEL_OK);
    UT_ASSERT(list.count == SCN_PANEL_ITEMS_MAX);
    UT_ASSERT(list.items[SCN_PANEL_ITEMS_MAX - 1].u.sprite.x ==
              (uint8_t)(SCN_PANEL_ITEMS_MAX - 1));
    if (refuses("one past the item cap", sprites, (uint16_t)sizeof(sprites),
                SCN_PANEL_ERR_TOO_MANY)) return 1;

    /* The text cap, both sides of it. */
    off = 0;
    putText(buf, &off, SCN_PANEL_TEXT_MAX);
    UT_ASSERT(off == 7 + SCN_PANEL_TEXT_MAX);
    UT_ASSERT(scnPanelParse(buf, off, &list) == SCN_PANEL_OK);
    UT_ASSERT(list.count == 1);
    UT_ASSERT(list.items[0].u.text.len == SCN_PANEL_TEXT_MAX);
    UT_ASSERT(list.items[0].u.text.text[SCN_PANEL_TEXT_MAX] == '\0');
    UT_ASSERT(strlen(list.items[0].u.text.text) == (size_t)SCN_PANEL_TEXT_MAX);

    off = 0;
    putText(buf, &off, SCN_PANEL_TEXT_MAX + 1);
    if (refuses("one byte past the text cap", buf, off,
                SCN_PANEL_ERR_TEXT)) return 1;

    /* A list filling a whole control segment: eighteen texts at the
     * text cap (55 bytes each) and one of twenty bytes makes 1017. */
    off = 0;
    for (i = 0; i < 18; i++) {
        putText(buf, &off, SCN_PANEL_TEXT_MAX);
    }
    putText(buf, &off, 20);
    UT_ASSERT(off == SCN_PANEL_MAX);
    UT_ASSERT(scnPanelParse(buf, off, &list) == SCN_PANEL_OK);
    UT_ASSERT(list.count == 19);
    UT_ASSERT(list.items[18].u.text.len == 20);

    /* An empty list is a list: it clears the panel. */
    memset(&list, 0, sizeof(list));
    list.count = 0xAB;
    UT_ASSERT(scnPanelParse(buf, 0, &list) == SCN_PANEL_OK);
    UT_ASSERT(list.count == 0);

    return 0;
}

int run_scenario_panel_roundtrip(void) {
    ScnPanelList out;
    ScnPanelList back;
    uint8_t buf[SCN_PANEL_MAX];
    uint16_t written;

    memset(&out, 0, sizeof(out));
    out.count = 7;

    out.items[0].op = SCN_PANEL_OP_RECT;
    out.items[0].u.rect.x = 5;
    out.items[0].u.rect.y = 6;
    out.items[0].u.rect.w = 16;
    out.items[0].u.rect.h = 32;
    out.items[0].u.rect.colour = SCN_PANEL_COLOUR_RED;
    out.items[0].u.rect.fill = 1;

    out.items[1].op = SCN_PANEL_OP_LINE;
    out.items[1].u.line.x0 = 0;
    out.items[1].u.line.y0 = 127;
    out.items[1].u.line.x1 = 127;
    out.items[1].u.line.y1 = 0;
    out.items[1].u.line.colour = SCN_PANEL_COLOUR_GREEN;

    out.items[2].op = SCN_PANEL_OP_TEXT;
    out.items[2].u.text.x = 10;
    out.items[2].u.text.y = 11;
    out.items[2].u.text.colour = SCN_PANEL_COLOUR_WHITE;
    out.items[2].u.text.size = SCN_PANEL_SIZE_NORMAL;
    out.items[2].u.text.align = SCN_PANEL_ALIGN_CENTRE;
    out.items[2].u.text.len = 5;
    memcpy(out.items[2].u.text.text, "Bolo!", 6);

    out.items[3].op = SCN_PANEL_OP_NAME;
    out.items[3].u.name.x = 4;
    out.items[3].u.name.y = 12;
    out.items[3].u.name.colour = SCN_PANEL_COLOUR_YELLOW;
    out.items[3].u.name.size = SCN_PANEL_SIZE_SMALL;
    out.items[3].u.name.align = SCN_PANEL_ALIGN_RIGHT;
    out.items[3].u.name.slot = 3;

    out.items[4].op = SCN_PANEL_OP_SPRITE;
    out.items[4].u.sprite.x = 64;
    out.items[4].u.sprite.y = 65;
    out.items[4].u.sprite.tile = 146;

    out.items[5].op = SCN_PANEL_OP_BAR;
    out.items[5].u.bar.x = 2;
    out.items[5].u.bar.y = 3;
    out.items[5].u.bar.w = 64;
    out.items[5].u.bar.h = 6;
    out.items[5].u.bar.colour = SCN_PANEL_COLOUR_MAGENTA;
    out.items[5].u.bar.value = 300;
    out.items[5].u.bar.max = 1000;

    out.items[6].op = SCN_PANEL_OP_TIMER;
    out.items[6].u.timer.x = 80;
    out.items[6].u.timer.y = 81;
    out.items[6].u.timer.colour = SCN_PANEL_COLOUR_CYAN;
    out.items[6].u.timer.size = SCN_PANEL_SIZE_NORMAL;
    out.items[6].u.timer.align = SCN_PANEL_ALIGN_LEFT;
    out.items[6].u.timer.mode = SCN_PANEL_TIMER_UP;
    out.items[6].u.timer.tick = 60000u;

    written = scnPanelWrite(&out, buf, (uint16_t)sizeof(buf));
    UT_ASSERT(written == GOLDEN_LIST_BYTES);
    /* And those are the bytes the decode case reads, so the writer
     * cannot drift away from the layout on its own. */
    UT_ASSERT(memcmp(buf, kGoldenList, GOLDEN_LIST_BYTES) == 0);

    UT_ASSERT(scnPanelParse(buf, written, &back) == SCN_PANEL_OK);
    UT_ASSERT(back.count == out.count);

    UT_ASSERT(back.items[0].op == SCN_PANEL_OP_RECT);
    UT_ASSERT(back.items[0].u.rect.x == out.items[0].u.rect.x);
    UT_ASSERT(back.items[0].u.rect.y == out.items[0].u.rect.y);
    UT_ASSERT(back.items[0].u.rect.w == out.items[0].u.rect.w);
    UT_ASSERT(back.items[0].u.rect.h == out.items[0].u.rect.h);
    UT_ASSERT(back.items[0].u.rect.colour == out.items[0].u.rect.colour);
    UT_ASSERT(back.items[0].u.rect.fill == out.items[0].u.rect.fill);

    UT_ASSERT(back.items[1].op == SCN_PANEL_OP_LINE);
    UT_ASSERT(back.items[1].u.line.x0 == out.items[1].u.line.x0);
    UT_ASSERT(back.items[1].u.line.y0 == out.items[1].u.line.y0);
    UT_ASSERT(back.items[1].u.line.x1 == out.items[1].u.line.x1);
    UT_ASSERT(back.items[1].u.line.y1 == out.items[1].u.line.y1);
    UT_ASSERT(back.items[1].u.line.colour == out.items[1].u.line.colour);

    UT_ASSERT(back.items[2].op == SCN_PANEL_OP_TEXT);
    UT_ASSERT(back.items[2].u.text.x == out.items[2].u.text.x);
    UT_ASSERT(back.items[2].u.text.y == out.items[2].u.text.y);
    UT_ASSERT(back.items[2].u.text.colour == out.items[2].u.text.colour);
    UT_ASSERT(back.items[2].u.text.size == out.items[2].u.text.size);
    UT_ASSERT(back.items[2].u.text.align == out.items[2].u.text.align);
    UT_ASSERT(back.items[2].u.text.len == out.items[2].u.text.len);
    UT_ASSERT(strcmp(back.items[2].u.text.text, out.items[2].u.text.text) == 0);

    UT_ASSERT(back.items[3].op == SCN_PANEL_OP_NAME);
    UT_ASSERT(back.items[3].u.name.x == out.items[3].u.name.x);
    UT_ASSERT(back.items[3].u.name.y == out.items[3].u.name.y);
    UT_ASSERT(back.items[3].u.name.colour == out.items[3].u.name.colour);
    UT_ASSERT(back.items[3].u.name.size == out.items[3].u.name.size);
    UT_ASSERT(back.items[3].u.name.align == out.items[3].u.name.align);
    UT_ASSERT(back.items[3].u.name.slot == out.items[3].u.name.slot);

    UT_ASSERT(back.items[4].op == SCN_PANEL_OP_SPRITE);
    UT_ASSERT(back.items[4].u.sprite.x == out.items[4].u.sprite.x);
    UT_ASSERT(back.items[4].u.sprite.y == out.items[4].u.sprite.y);
    UT_ASSERT(back.items[4].u.sprite.tile == out.items[4].u.sprite.tile);

    UT_ASSERT(back.items[5].op == SCN_PANEL_OP_BAR);
    UT_ASSERT(back.items[5].u.bar.x == out.items[5].u.bar.x);
    UT_ASSERT(back.items[5].u.bar.y == out.items[5].u.bar.y);
    UT_ASSERT(back.items[5].u.bar.w == out.items[5].u.bar.w);
    UT_ASSERT(back.items[5].u.bar.h == out.items[5].u.bar.h);
    UT_ASSERT(back.items[5].u.bar.colour == out.items[5].u.bar.colour);
    UT_ASSERT(back.items[5].u.bar.value == out.items[5].u.bar.value);
    UT_ASSERT(back.items[5].u.bar.max == out.items[5].u.bar.max);

    UT_ASSERT(back.items[6].op == SCN_PANEL_OP_TIMER);
    UT_ASSERT(back.items[6].u.timer.x == out.items[6].u.timer.x);
    UT_ASSERT(back.items[6].u.timer.y == out.items[6].u.timer.y);
    UT_ASSERT(back.items[6].u.timer.colour == out.items[6].u.timer.colour);
    UT_ASSERT(back.items[6].u.timer.size == out.items[6].u.timer.size);
    UT_ASSERT(back.items[6].u.timer.align == out.items[6].u.timer.align);
    UT_ASSERT(back.items[6].u.timer.mode == out.items[6].u.timer.mode);
    UT_ASSERT(back.items[6].u.timer.tick == out.items[6].u.timer.tick);

    /* The writer refuses what the parser would: a list that will not
     * fit, and an item outside the palette. */
    UT_ASSERT(scnPanelWrite(&out, buf, (uint16_t)(GOLDEN_LIST_BYTES - 1)) == 0);
    out.items[5].u.bar.colour = SCN_PANEL_COLOURS;
    UT_ASSERT(scnPanelWrite(&out, buf, (uint16_t)sizeof(buf)) == 0);

    return 0;
}

/* The large size, and how it travels.
 *
 * A large item goes on the wire as a normal one followed by the size mark,
 * a colourless empty rect at x 2, so a build from before the large size
 * draws it at normal size instead of refusing the list. The bytes are
 * written out by hand, so the layout is pinned from outside the writer. */
int run_scenario_panel_large_size(void) {
    static const uint8_t kLarge[] = {
        /* text: x=4, y=30, colour=CYAN(10), size byte NORMAL(1), align=LEFT,
         *       len=2, "Hi" ... */
        0x03, 0x04, 0x1E, 0x0A, 0x01, 0x00, 0x02, 'H', 'i',
        /* ... and the size mark that makes it large */
        0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* name: x=4, y=50, colour=CYAN, size byte NORMAL, align=LEFT, slot=2,
         *       then the mark */
        0x04, 0x04, 0x32, 0x0A, 0x01, 0x00, 0x02,
        0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* timer: x=124, y=50, colour=WHITE, size byte NORMAL, align=RIGHT,
         *        mode=DOWN, tick=1000, then the mark */
        0x07, 0x7C, 0x32, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x03, 0xE8,
        0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* a plain normal text after them, with no mark */
        0x03, 0x04, 0x46, 0x02, 0x01, 0x00, 0x01, 'x'
    };
    ScnPanelList out;
    ScnPanelList back;
    uint8_t      buf[SCN_PANEL_MAX];
    uint8_t      bad[32];
    uint16_t     written;
    uint16_t     off;
    int          i;

    memset(&out, 0, sizeof(out));
    out.count = 4;
    out.items[0].op = SCN_PANEL_OP_TEXT;
    out.items[0].u.text.x = 4;
    out.items[0].u.text.y = 30;
    out.items[0].u.text.colour = SCN_PANEL_COLOUR_CYAN;
    out.items[0].u.text.size = SCN_PANEL_SIZE_LARGE;
    out.items[0].u.text.align = SCN_PANEL_ALIGN_LEFT;
    out.items[0].u.text.len = 2;
    memcpy(out.items[0].u.text.text, "Hi", 3);
    out.items[1].op = SCN_PANEL_OP_NAME;
    out.items[1].u.name.x = 4;
    out.items[1].u.name.y = 50;
    out.items[1].u.name.colour = SCN_PANEL_COLOUR_CYAN;
    out.items[1].u.name.size = SCN_PANEL_SIZE_LARGE;
    out.items[1].u.name.align = SCN_PANEL_ALIGN_LEFT;
    out.items[1].u.name.slot = 2;
    out.items[2].op = SCN_PANEL_OP_TIMER;
    out.items[2].u.timer.x = 124;
    out.items[2].u.timer.y = 50;
    out.items[2].u.timer.colour = SCN_PANEL_COLOUR_WHITE;
    out.items[2].u.timer.size = SCN_PANEL_SIZE_LARGE;
    out.items[2].u.timer.align = SCN_PANEL_ALIGN_RIGHT;
    out.items[2].u.timer.mode = SCN_PANEL_TIMER_DOWN;
    out.items[2].u.timer.tick = 1000u;
    out.items[3].op = SCN_PANEL_OP_TEXT;
    out.items[3].u.text.x = 4;
    out.items[3].u.text.y = 70;
    out.items[3].u.text.colour = SCN_PANEL_COLOUR_WHITE;
    out.items[3].u.text.size = SCN_PANEL_SIZE_NORMAL;
    out.items[3].u.text.align = SCN_PANEL_ALIGN_LEFT;
    out.items[3].u.text.len = 1;
    memcpy(out.items[3].u.text.text, "x", 2);

    UT_ASSERT(scnPanelWireCount(&out) == 7);
    written = scnPanelWrite(&out, buf, (uint16_t)sizeof(buf));
    UT_ASSERT_MSG(written == sizeof(kLarge), "wrote %u bytes, expected %u",
                  (unsigned)written, (unsigned)sizeof(kLarge));
    UT_ASSERT(memcmp(buf, kLarge, sizeof(kLarge)) == 0);

    /* Back apart: four items, the mark folded into each large one. */
    memset(&back, 0, sizeof(back));
    UT_ASSERT(scnPanelParse(kLarge, (uint16_t)sizeof(kLarge), &back) ==
              SCN_PANEL_OK);
    UT_ASSERT_MSG(back.count == 4, "decoded %u items", (unsigned)back.count);
    UT_ASSERT(back.items[0].op == SCN_PANEL_OP_TEXT &&
              back.items[0].u.text.size == SCN_PANEL_SIZE_LARGE &&
              strcmp(back.items[0].u.text.text, "Hi") == 0);
    UT_ASSERT(back.items[1].op == SCN_PANEL_OP_NAME &&
              back.items[1].u.name.size == SCN_PANEL_SIZE_LARGE &&
              back.items[1].u.name.slot == 2);
    UT_ASSERT(back.items[2].op == SCN_PANEL_OP_TIMER &&
              back.items[2].u.timer.size == SCN_PANEL_SIZE_LARGE &&
              back.items[2].u.timer.tick == 1000u);
    UT_ASSERT(back.items[3].op == SCN_PANEL_OP_TEXT &&
              back.items[3].u.text.size == SCN_PANEL_SIZE_NORMAL);

    /* A size byte of 2 on the wire is refused: large has one spelling. */
    off = 0;
    bad[off++] = 0x03; bad[off++] = 4; bad[off++] = 4; bad[off++] = 2;
    bad[off++] = SCN_PANEL_SIZE_LARGE; bad[off++] = 0; bad[off++] = 1;
    bad[off++] = 'a';
    UT_ASSERT(scnPanelParse(bad, off, &back) == SCN_PANEL_ERR_RANGE);
    /* And a size past large is refused on the wire and by the writer. */
    bad[4] = 3;
    UT_ASSERT(scnPanelParse(bad, off, &back) == SCN_PANEL_ERR_RANGE);
    out.items[0].u.text.size = 3;
    UT_ASSERT(scnPanelWrite(&out, buf, (uint16_t)sizeof(buf)) == 0);
    out.items[0].u.text.size = SCN_PANEL_SIZE_LARGE;

    /* A mark on its own, after a rect, and after a small text: refused. */
    UT_ASSERT(scnPanelParse(kLarge + 9, 7, &back) == SCN_PANEL_ERR_RANGE);
    off = 0;
    bad[off++] = 0x01; bad[off++] = 1; bad[off++] = 1; bad[off++] = 4;
    bad[off++] = 4; bad[off++] = 2; bad[off++] = 1;
    memcpy(bad + off, kLarge + 9, 7);
    off = (uint16_t)(off + 7);
    UT_ASSERT(scnPanelParse(bad, off, &back) == SCN_PANEL_ERR_RANGE);
    memcpy(bad, kLarge, 16);
    bad[4] = SCN_PANEL_SIZE_SMALL;
    UT_ASSERT(scnPanelParse(bad, 16, &back) == SCN_PANEL_ERR_RANGE);
    /* Two marks after one item: the second follows a large item, which is
       not a normal one, so it is refused as a rect. */
    memcpy(bad, kLarge, 16);
    memcpy(bad + 16, kLarge + 9, 7);
    UT_ASSERT(scnPanelParse(bad, 23, &back) == SCN_PANEL_ERR_RANGE);

    /* The writer will not emit a mark-shaped rect as an item of its own. */
    UT_ASSERT(scnPanelParse(kLarge, 16, &back) == SCN_PANEL_OK);
    back.count = 2;
    back.items[1].op = SCN_PANEL_OP_RECT;
    memset(&back.items[1].u, 0, sizeof(back.items[1].u));
    back.items[1].u.rect.x = SCN_PANEL_SIZE_LARGE;
    UT_ASSERT(scnPanelIsSizeMark(&back.items[1]));
    UT_ASSERT(scnPanelWrite(&back, buf, (uint16_t)sizeof(buf)) == 0);
    /* Any other colourless empty rect is still an ordinary rect. */
    back.items[1].u.rect.x = 3;
    UT_ASSERT(!scnPanelIsSizeMark(&back.items[1]));
    UT_ASSERT(scnPanelWrite(&back, buf, (uint16_t)sizeof(buf)) == 23);

    /* The item limit counts wire primitives: 64 large texts are 128 on the
       wire and fit, 65 do not, in either direction. */
    memset(&out, 0, sizeof(out));
    for (i = 0; i < 65; i++) {
        out.items[i].op = SCN_PANEL_OP_TEXT;
        out.items[i].u.text.colour = SCN_PANEL_COLOUR_WHITE;
        out.items[i].u.text.size = SCN_PANEL_SIZE_LARGE;
    }
    out.count = 64;
    written = scnPanelWrite(&out, buf, (uint16_t)sizeof(buf));
    UT_ASSERT(written == 64 * 14);
    UT_ASSERT(scnPanelParse(buf, written, &back) == SCN_PANEL_OK);
    UT_ASSERT(back.count == 64);
    out.count = 65;
    UT_ASSERT(scnPanelWireCount(&out) == 130);
    UT_ASSERT(scnPanelWrite(&out, buf, (uint16_t)sizeof(buf)) == 0);
    for (i = 0; i < 65; i++) {
        memcpy(buf + i * 14, kLarge, 7);    /* a text with a normal byte */
        buf[i * 14 + 6] = 0;                /* and no string */
        memcpy(buf + i * 14 + 7, kLarge + 9, 7);
    }
    UT_ASSERT(scnPanelParse(buf, (uint16_t)(65 * 14), &back) ==
              SCN_PANEL_ERR_TOO_MANY);

    return 0;
}

/* The timer primitive's text.
 *
 * A timer carries one tick and a direction, and every client works the
 * minutes and seconds out against its own clock — which is what makes a
 * countdown one message rather than one a tick. The strings below are
 * written out by hand rather than computed, so the arithmetic is pinned
 * from outside itself.
 *
 * Ticks run at GAME_NUMTOTALTICKS_SEC, a hundred a second; the seconds in
 * the table are multiplied by it here so each row reads as the time it
 * stands for. */
#define TICKS_PER_SEC ((uint32_t)GAME_NUMTOTALTICKS_SEC)

int run_scenario_panel_timer_text(void) {
    static const struct {
        uint32_t    nowSecs;
        uint32_t    targetSecs;
        uint8_t     mode;
        const char *expect;
    } kRows[] = {
        /* Counting down, with time left. */
        { 10,    70,    SCN_PANEL_TIMER_DOWN, "1:00" },
        { 0,     65,    SCN_PANEL_TIMER_DOWN, "1:05" },
        { 0,     5,     SCN_PANEL_TIMER_DOWN, "0:05" },
        /* Counting down past its tick reads 0:00 rather than going
           negative: a wave whose time has run out sits at zero until the
           script says something else. */
        { 80,    70,    SCN_PANEL_TIMER_DOWN, "0:00" },
        { 100000, 1,    SCN_PANEL_TIMER_DOWN, "0:00" },
        /* Exactly its tick, both ways. */
        { 70,    70,    SCN_PANEL_TIMER_DOWN, "0:00" },
        { 70,    70,    SCN_PANEL_TIMER_UP,   "0:00" },
        /* Counting up from its tick. */
        { 70,    10,    SCN_PANEL_TIMER_UP,   "1:00" },
        { 125,   0,     SCN_PANEL_TIMER_UP,   "2:05" },
        /* A count-up whose tick has not arrived yet is held at zero the
           same way. */
        { 10,    70,    SCN_PANEL_TIMER_UP,   "0:00" },
        /* Past an hour the minutes keep counting rather than wrapping: a
           panel reading 30:00 for an hour and a half would be lying about
           the hour. */
        { 0,     5400,  SCN_PANEL_TIMER_DOWN, "90:00" },
        { 3661,  0,     SCN_PANEL_TIMER_UP,   "61:01" },
    };
    size_t i;

    for (i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++) {
        char out[SCN_PANEL_TIMER_TEXT_MAX];
        out[0] = 'x';
        scnPanelTimerText(kRows[i].nowSecs * TICKS_PER_SEC,
                          kRows[i].targetSecs * TICKS_PER_SEC,
                          kRows[i].mode, out, sizeof(out));
        UT_ASSERT(strcmp(out, kRows[i].expect) == 0);
    }

    /* A tick difference that is not a whole second is the second it has
       finished, not the one it is in: 5.99 seconds left reads 0:05. */
    {
        char out[SCN_PANEL_TIMER_TEXT_MAX];
        scnPanelTimerText(0, 6 * TICKS_PER_SEC - 1, SCN_PANEL_TIMER_DOWN, out,
                          sizeof(out));
        UT_ASSERT(strcmp(out, "0:05") == 0);
    }

    /* Nowhere to write it is not a crash. */
    scnPanelTimerText(0, TICKS_PER_SEC, SCN_PANEL_TIMER_DOWN, NULL, 16);

    return 0;
}

/* Whether an announcement is still on screen, and for how long.
 *
 * An announcement carries the tick it landed at and how long it was asked to
 * stay up; every viewer works the rest out against its own clock, the same
 * bargain the timer primitive makes. The rows below are the four answers
 * that matter and are written out by hand. */
int run_scenario_announce_remaining(void) {
    uint32_t left;

    /* Still up: landed at tick 1000 for five seconds, and the clock is two
       seconds past it. Three seconds left. */
    left = 0;
    UT_ASSERT(scnAnnounceRemaining("Wave 3", 1000, (uint16_t)(5 * TICKS_PER_SEC),
                                   1000 + 2 * TICKS_PER_SEC, &left));
    UT_ASSERT(left == 3 * TICKS_PER_SEC);

    /* Exactly expired: the tick it runs out on is already off, not the last
       one it is on. */
    left = 99;
    UT_ASSERT(!scnAnnounceRemaining("Wave 3", 1000, (uint16_t)(5 * TICKS_PER_SEC),
                                    1000 + 5 * TICKS_PER_SEC, &left));
    UT_ASSERT(left == 0);

    /* Expired long ago. */
    left = 99;
    UT_ASSERT(!scnAnnounceRemaining("Wave 3", 1000, (uint16_t)(5 * TICKS_PER_SEC),
                                    9999999, &left));
    UT_ASSERT(left == 0);

    /* Nothing to show: no text, an empty one, and a duration of nothing. */
    UT_ASSERT(!scnAnnounceRemaining(NULL, 1000, 500, 1000, NULL));
    UT_ASSERT(!scnAnnounceRemaining("", 1000, 500, 1000, NULL));
    UT_ASSERT(!scnAnnounceRemaining("Wave 3", 1000, 0, 1000, NULL));

    /* A clock behind the arrival has not reached it yet, so the whole
       duration is still to run rather than a negative age wrapping round. */
    left = 0;
    UT_ASSERT(scnAnnounceRemaining("Wave 3", 1000, 500, 900, &left));
    UT_ASSERT(left == 500);

    return 0;
}
