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
#include <float.h>
#include <stdint.h>
#include <string.h>

#include "global.h"          /* MAX_TANKS, GAME_NUMTOTALTICKS_SEC */
#include "scenario_panel.h"
#include "scenario_panel_draw.h" /* scnPanelTimerText — the drawer's one
                                  * piece of arithmetic, which is plain C
                                  * and needs no renderer behind it */
#include "scenario_panel_slot.h" /* scnPanelSlotRect, for the tablet band */
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

/* Two floats the same to within a hundredth of a pixel. */
static bool scnNear(float a, float b) {
    float d = a - b;
    if (d < 0.0f) d = -d;
    return d < 0.01f;
}

/* Where an announcement goes across the Scenario Text window's source
 * rectangle.
 *
 * Its centre goes at the share across the view that the script named, as
 * a share across the source rectangle's width, and the line is then moved
 * the least it takes to stay one pad inside the edges. A line too wide for
 * the room between the pads is made smaller by itself until it fits.
 *
 * The source rectangle is SCN_TEXT_SRC_W_UNITS (240) wide and the pad is
 * SCN_TEXT_PAD_UNITS (2), so the room for a line is 236. */
int run_scenario_announce_position(void) {
    const float srcW = SCN_TEXT_SRC_W_UNITS;
    const float pad  = SCN_TEXT_PAD_UNITS;
    float x      = -1.0f;
    float shrink = -1.0f;

    UT_ASSERT(srcW == 240.0f);
    UT_ASSERT(pad == 2.0f);

    /* The shares: 127 is exactly the middle, 254 the far edge, and 255
       reads as 254. */
    UT_ASSERT(scnAnnouncePosShare(0) == 0.0f);
    UT_ASSERT(scnAnnouncePosShare((uint8_t)SCN_ANNOUNCE_POS_MID) == 0.5f);
    UT_ASSERT(scnAnnouncePosShare((uint8_t)SCN_ANNOUNCE_POS_MAX) == 1.0f);
    UT_ASSERT(scnAnnouncePosShare(255) == 1.0f);

    /* The middle: a 100 wide line centred at 120 starts at 70, full size. */
    scnTextLineAcross(srcW, 100.0f, 0.5f, &x, &shrink);
    UT_ASSERT_MSG(x == 70.0f && shrink == 1.0f,
                  "the centred line is at %.2f, drawn at %.2f", x, shrink);

    /* The left edge: the centre at 0 would put half the line outside, so it
       is moved in to the pad. */
    scnTextLineAcross(srcW, 100.0f, 0.0f, &x, &shrink);
    UT_ASSERT(x == pad && shrink == 1.0f);

    /* The right edge: one pad in from the far edge. */
    scnTextLineAcross(srcW, 100.0f, 1.0f, &x, &shrink);
    UT_ASSERT_MSG(x == srcW - pad - 100.0f, "the right line is at %.2f", x);
    UT_ASSERT(shrink == 1.0f);

    /* A point well inside is not moved: three quarters across. */
    scnTextLineAcross(srcW, 100.0f, 0.75f, &x, &shrink);
    UT_ASSERT(scnNear(x, 180.0f - 50.0f));

    /* A share outside 0 to 1 is held to the edges. */
    scnTextLineAcross(srcW, 100.0f, -3.0f, &x, &shrink);
    UT_ASSERT(x == pad);
    scnTextLineAcross(srcW, 100.0f, 7.0f, &x, &shrink);
    UT_ASSERT(x == srcW - pad - 100.0f);

    /* A line exactly the room wide is not made smaller and fills it. */
    scnTextLineAcross(srcW, srcW - 2.0f * pad, 0.5f, &x, &shrink);
    UT_ASSERT(x == pad && shrink == 1.0f);

    /* A line twice the room wide is drawn at half size, filling the room
       from pad to pad wherever the script asked for it. */
    scnTextLineAcross(srcW, 2.0f * (srcW - 2.0f * pad), 0.5f, &x, &shrink);
    UT_ASSERT_MSG(scnNear(shrink, 0.5f) && scnNear(x, pad),
                  "the wide line is at %.2f, drawn at %.2f", x, shrink);
    scnTextLineAcross(srcW, 2.0f * (srcW - 2.0f * pad), 0.0f, &x, &shrink);
    UT_ASSERT(scnNear(shrink, 0.5f) && scnNear(x, pad));
    scnTextLineAcross(srcW, 2.0f * (srcW - 2.0f * pad), 1.0f, &x, &shrink);
    UT_ASSERT(scnNear(shrink, 0.5f) && scnNear(x, pad));

    /* NULL outputs are allowed. */
    scnTextLineAcross(srcW, 100.0f, 0.5f, NULL, NULL);

    return 0;
}

/* The Scenario Text window's fit: the source rectangle scaled into the
 * window by one factor for both axes, as large as it goes, and centred.
 *
 * The source rectangle is 240 units wide. With one row it is 2 + 22 + 2 =
 * 26 units high, and with two rows 2 + 22 + 2 + 22 + 2 = 50. */
int run_scenario_text_fit(void) {
    const float srcW = SCN_TEXT_SRC_W_UNITS;
    ScnTextFit  fit;

    /* The rows. */
    UT_ASSERT(SCN_TEXT_ROW_UNITS == 22.0f);
    UT_ASSERT(scnTextSourceHeight(1) == 26.0f);
    UT_ASSERT(scnTextSourceHeight(2) == 50.0f);
    /* Fewer than one row is one, and more than SCN_TEXT_MAX_ROWS is that
       many, so a round with no line yet and a wrong count both give a usable
       rectangle. Three rows: the status line and two announcements. */
    UT_ASSERT(scnTextSourceHeight(0) == 26.0f);
    UT_ASSERT(scnTextSourceHeight(-4) == 26.0f);
    UT_ASSERT(scnTextSourceHeight(3) == 74.0f);
    UT_ASSERT(scnTextSourceHeight(SCN_TEXT_MAX_ROWS + 7) ==
              scnTextSourceHeight(SCN_TEXT_MAX_ROWS));
    UT_ASSERT(scnTextRowTop(0) == 2.0f);
    UT_ASSERT(scnTextRowTop(1) == 26.0f);
    UT_ASSERT(scnTextRowTop(-1) == 2.0f);
    /* The second row ends one pad above the bottom. */
    UT_ASSERT(scnTextRowTop(1) + SCN_TEXT_ROW_UNITS + SCN_TEXT_PAD_UNITS ==
              scnTextSourceHeight(2));

    /* A window the same shape as the source, twice the size: scale 2, no
       spare space. */
    fit = scnTextFitRect(srcW, 50.0f, 10.0f, 20.0f, 480.0f, 100.0f);
    UT_ASSERT(fit.scale == 2.0f);
    UT_ASSERT(fit.originX == 10.0f && fit.originY == 20.0f);

    /* A window wider than the source's shape: the height sets the scale,
       and the spare width is split evenly left and right. 100 high over 50
       units is 2; 480 wide is used, 1000 - 480 = 520 spare, 260 a side. */
    fit = scnTextFitRect(srcW, 50.0f, 0.0f, 0.0f, 1000.0f, 100.0f);
    UT_ASSERT(fit.scale == 2.0f);
    UT_ASSERT_MSG(fit.originX == 260.0f && fit.originY == 0.0f,
                  "the wide window's origin is (%.2f, %.2f)", fit.originX,
                  fit.originY);

    /* A window taller than the source's shape: the width sets the scale,
       and the spare height is split evenly above and below. 240 wide is
       scale 1, 50 high used of 150, 50 a side. */
    fit = scnTextFitRect(srcW, 50.0f, 5.0f, 5.0f, 240.0f, 150.0f);
    UT_ASSERT(fit.scale == 1.0f);
    UT_ASSERT(fit.originX == 5.0f && fit.originY == 55.0f);

    /* The shape is kept: one scale for both axes, so the source drawn at
       that scale fits inside the window on both axes and fills it on at
       least one. */
    {
        static const float sizes[][2] = {
            { 64.0f, 24.0f }, { 333.0f, 41.0f }, { 1920.0f, 60.0f },
            { 90.0f, 700.0f }, { 481.0f, 99.0f }, { 240.0f, 26.0f },
        };
        size_t i;
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            const float w = sizes[i][0];
            const float h = sizes[i][1];
            const float srcH = scnTextSourceHeight((int)(i % 2) + 1);
            const float usedW = srcW * scnTextFitRect(srcW, srcH, 0.0f, 0.0f,
                                                      w, h).scale;
            const float usedH = srcH * scnTextFitRect(srcW, srcH, 0.0f, 0.0f,
                                                      w, h).scale;
            fit = scnTextFitRect(srcW, srcH, 0.0f, 0.0f, w, h);
            UT_ASSERT_MSG(usedW <= w + 0.01f && usedH <= h + 0.01f,
                          "%gx%g: the text runs outside the window", w, h);
            UT_ASSERT_MSG(scnNear(usedW, w) || scnNear(usedH, h),
                          "%gx%g: the text does not fill the window", w, h);
            /* Centred: the spare space on each side is the same. */
            UT_ASSERT(scnNear(fit.originX, w - (fit.originX + usedW)));
            UT_ASSERT(scnNear(fit.originY, h - (fit.originY + usedH)));
        }
    }

    /* Nothing to fit into, or nothing to fit: scale 0 at the window's
       corner, so the caller draws nothing. */
    fit = scnTextFitRect(srcW, 50.0f, 7.0f, 8.0f, 0.0f, 100.0f);
    UT_ASSERT(fit.scale == 0.0f && fit.originX == 7.0f && fit.originY == 8.0f);
    fit = scnTextFitRect(srcW, 50.0f, 7.0f, 8.0f, 100.0f, -1.0f);
    UT_ASSERT(fit.scale == 0.0f);
    fit = scnTextFitRect(0.0f, 50.0f, 7.0f, 8.0f, 100.0f, 100.0f);
    UT_ASSERT(fit.scale == 0.0f);

    return 0;
}

/* Tablet mode's scenario text band: the source rectangle at the zoom's
 * scale, at the top middle of the view, made smaller to fit, and kept one
 * inset left of the scenario panel's square when that is shown. */
int run_scenario_text_tablet_band(void) {
    const float srcW = SCN_TEXT_SRC_W_UNITS;
    const float srcH = 26.0f;
    ScnTextBand b;

    /* No square: 240 x 26 at scale 3 is 720 x 78, centred in a 960 wide
       view, (960 - 720) / 2 = 120 from its left, one inset (12) down. */
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, false,
                          0.0f);
    UT_ASSERT_MSG(b.x == 120.0f && b.y == 12.0f && b.w == 720.0f &&
                      b.h == 78.0f,
                  "no square: %.2f,%.2f %.2fx%.2f", b.x, b.y, b.w, b.h);
    /* The square's place is not read when it is not shown. */
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, false,
                          500.0f);
    UT_ASSERT(b.x == 120.0f && b.w == 720.0f);

    /* A square at 800: the room ends at 800 - 12 = 788, and the band is
       centred in it, (788 - 720) / 2 = 34 from the left. */
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, true,
                          800.0f);
    UT_ASSERT_MSG(b.x == 34.0f && b.w == 720.0f && b.h == 78.0f,
                  "square at 800: %.2f %.2fx%.2f", b.x, b.w, b.h);

    /* A square at 700: the room is 688, less than 720, so the band is made
       smaller to 688 wide and keeps its shape, 78 * 688 / 720 high. */
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, true,
                          700.0f);
    UT_ASSERT_MSG(b.x == 0.0f && b.w == 688.0f &&
                      scnNear(b.h, 78.0f * 688.0f / 720.0f),
                  "square at 700: %.2f %.2fx%.2f", b.x, b.w, b.h);

    /* A view that does not start at 0, with the square where the tablet
       layout puts it on a phone: the band's right edge stays one inset left
       of the square. 2400 x 1080 gives a side of 270, at
       200 + 1800 - 16 - 270 = 1714. */
    {
        float sx = 0.0f, sy = 0.0f, side = 0.0f;
        UT_ASSERT(scnPanelSlotRect(2400, 1080, 200.0f, 0.0f, 1800.0f, 1080.0f,
                                   16.0f, &sx, &sy, &side));
        UT_ASSERT(sx == 1714.0f && side == 270.0f);
        b = scnTextTabletBand(200.0f, 0.0f, 1800.0f, srcW, srcH, 4.0f, 16.0f,
                              true, sx);
        UT_ASSERT_MSG(b.x + b.w <= sx - 16.0f + 0.01f,
                      "the band reaches %.2f, the square is at %.2f",
                      b.x + b.w, sx);
        UT_ASSERT_MSG(b.x == 469.0f && b.w == 960.0f,
                      "phone: %.2f %.2f", b.x, b.w);
        /* A narrow phone view: the band is made smaller and still stops
           short of the square. */
        b = scnTextTabletBand(200.0f, 0.0f, 800.0f, srcW, srcH, 4.0f, 16.0f,
                              true, 200.0f + 800.0f - 16.0f - 270.0f);
        UT_ASSERT(b.x == 200.0f && b.w == 800.0f - 16.0f - 270.0f - 16.0f);
        UT_ASSERT(b.x + b.w <= 200.0f + 800.0f - 16.0f - 270.0f - 16.0f);
    }

    /* A square outside the view's width does not change the room. */
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, true,
                          960.0f);
    UT_ASSERT(b.x == 120.0f && b.w == 720.0f);
    b = scnTextTabletBand(100.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, true,
                          50.0f);
    UT_ASSERT(b.x == 220.0f && b.w == 720.0f);

    /* No room left of the square, or nothing to draw: no band. */
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 3.0f, 12.0f, true,
                          10.0f);
    UT_ASSERT(b.w == 0.0f && b.h == 0.0f);
    b = scnTextTabletBand(0.0f, 0.0f, 0.0f, srcW, srcH, 3.0f, 12.0f, false,
                          0.0f);
    UT_ASSERT(b.w == 0.0f && b.h == 0.0f);
    b = scnTextTabletBand(0.0f, 0.0f, 960.0f, srcW, srcH, 0.0f, 12.0f, false,
                          0.0f);
    UT_ASSERT(b.w == 0.0f && b.h == 0.0f);

    /* A negative inset counts as none. */
    b = scnTextTabletBand(0.0f, 5.0f, 960.0f, srcW, srcH, 3.0f, -4.0f, true,
                          800.0f);
    UT_ASSERT(b.y == 5.0f && b.x == 40.0f);

    return 0;
}

/* The Scenario Text window takes the mouse only on its bar and its grip, so
 * a click or the wheel on the text goes to the game. The bar sits directly
 * above the text area and covers none of it. A text area at 100,70,
 * 300 x 120, with a 20 high bar and a 14 square grip: the bar is
 * 100..400 x 50..70, the text area is 100..400 x 70..190 and the grip is
 * 386..400 x 176..190. */
int run_scenario_text_pointer_on_handle(void) {
    const float x = 100.0f, y = 70.0f, w = 300.0f, h = 120.0f;
    const float bar = 20.0f, grip = 14.0f;

    /* The bar, across the full width: its left end, its middle, and the
       gear and the X at its right end. */
    UT_ASSERT(scnTextPointerOnHandle(x, y, w, h, bar, grip, 100.0f, 50.0f));
    UT_ASSERT(scnTextPointerOnHandle(x, y, w, h, bar, grip, 250.0f, 60.0f));
    UT_ASSERT(scnTextPointerOnHandle(x, y, w, h, bar, grip, 399.0f, 69.0f));
    /* Its bottom edge is the text area's top edge, and is not in it: the
       first row of text is not under the bar. */
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 250.0f, 70.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 100.0f, 71.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 399.0f, 75.0f));

    /* The grip, and the pixels just outside it. */
    UT_ASSERT(scnTextPointerOnHandle(x, y, w, h, bar, grip, 386.0f, 176.0f));
    UT_ASSERT(scnTextPointerOnHandle(x, y, w, h, bar, grip, 399.5f, 189.5f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 385.9f, 180.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 390.0f, 175.9f));

    /* The text body: the middle, the left and right edges at the top, and
       the bottom-left corner. None of these takes the mouse. */
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 250.0f, 130.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 100.0f, 120.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 399.0f, 120.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 100.0f, 189.0f));

    /* Off the window: left, above the bar, right of the bar, below the
       grip. */
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 99.9f, 60.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 250.0f, 49.9f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 400.0f, 60.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, 395.0f, 190.0f));
    /* The point ImGui gives when there is no mouse. */
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, grip, -FLT_MAX,
                                      -FLT_MAX));

    /* A text area with no area has no bar and no grip, and a bar or grip of
       size 0 is not there. With no bar, the strip above the text area does
       not take the mouse. */
    UT_ASSERT(!scnTextPointerOnHandle(x, y, 0.0f, h, bar, grip, 100.0f, 50.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, -1.0f, bar, grip, 100.0f,
                                      50.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, 0.0f, grip, 250.0f, 60.0f));
    UT_ASSERT(!scnTextPointerOnHandle(x, y, w, h, bar, 0.0f, 399.0f, 189.0f));

    return 0;
}

/* When the desktop Scenario Text window is up: while the player has it
 * shown and a line was on screen a short time ago; while there is a line;
 * and while the player is busy with it. Never while it is closed. */
int run_scenario_text_window_shown(void) {
    /* A recent line, no line now, not busy: up, so it can be grabbed. */
    UT_ASSERT(scnTextWindowShown(true, true, false, false));
    /* A recent line and a line now. */
    UT_ASSERT(scnTextWindowShown(true, true, true, false));
    /* No recent line, no line now, not busy: not up, no empty window. */
    UT_ASSERT(!scnTextWindowShown(true, false, false, false));
    /* A line or the player busy with it keeps it up. */
    UT_ASSERT(scnTextWindowShown(true, false, true, false));
    UT_ASSERT(scnTextWindowShown(true, false, false, true));
    /* Closed by the player: never up. */
    UT_ASSERT(!scnTextWindowShown(false, true, true, true));
    UT_ASSERT(!scnTextWindowShown(false, true, false, false));
    UT_ASSERT(!scnTextWindowShown(false, false, true, false));
    return 0;
}

/* A line on screen within the hold time keeps the window up; after the
 * hold time, or with no line this round, it does not. */
int run_scenario_text_printed_recently(void) {
    const uint64_t hold = 5000u;
    /* No line this round: never recent, also at the first tick. */
    UT_ASSERT(!scnTextPrintedRecently(false, 0u, 0u, hold));
    UT_ASSERT(!scnTextPrintedRecently(false, 1000u, 1000u, hold));
    /* On screen this frame. */
    UT_ASSERT(scnTextPrintedRecently(true, 1000u, 1000u, hold));
    /* Just inside and at the edge of the hold time. */
    UT_ASSERT(scnTextPrintedRecently(true, 1000u, 5999u, hold));
    UT_ASSERT(!scnTextPrintedRecently(true, 1000u, 6000u, hold));
    UT_ASSERT(!scnTextPrintedRecently(true, 1000u, 60000u, hold));
    /* A stamp ahead of now counts as recent. */
    UT_ASSERT(scnTextPrintedRecently(true, 2000u, 1000u, hold));
    /* A zero hold time: never up after the line goes. */
    UT_ASSERT(!scnTextPrintedRecently(true, 1000u, 1000u, 0u));
    return 0;
}

/* The faint edge: with a line, only when there is a backing; with no line,
 * only while the handles show, at any background opacity. */
int run_scenario_text_edge_shown(void) {
    UT_ASSERT(scnTextEdgeShown(true, 50, false));
    UT_ASSERT(scnTextEdgeShown(true, 1, true));
    UT_ASSERT(!scnTextEdgeShown(true, 0, false));
    UT_ASSERT(!scnTextEdgeShown(true, 0, true));
    UT_ASSERT(scnTextEdgeShown(false, 0, true));
    UT_ASSERT(scnTextEdgeShown(false, 100, true));
    UT_ASSERT(!scnTextEdgeShown(false, 0, false));
    UT_ASSERT(!scnTextEdgeShown(false, 100, false));
    return 0;
}

/* When the place is written: a drag always; a window moved before also
 * while it has a line; a window with no line never by itself. */
int run_scenario_text_save_place(void) {
    UT_ASSERT(scnTextSavePlace(true, false, false));
    UT_ASSERT(scnTextSavePlace(true, true, false));
    UT_ASSERT(scnTextSavePlace(true, false, true));
    UT_ASSERT(scnTextSavePlace(false, true, true));
    UT_ASSERT(!scnTextSavePlace(false, true, false));
    UT_ASSERT(!scnTextSavePlace(false, false, true));
    UT_ASSERT(!scnTextSavePlace(false, false, false));
    return 0;
}

/* The status line's text: the script's line, and with a countdown four
 * spaces and the time left as a panel countdown shows it. */
int run_scenario_status_line(void) {
    char  out[SCN_STATUS_LINE_MAX];

    /* With a countdown: 2:44 left. */
    UT_ASSERT(scnStatusLineText("Wave 3/10", 1000 + 164 * TICKS_PER_SEC, 1000,
                                out, sizeof(out)));
    UT_ASSERT(strcmp(out, "Wave 3/10    2:44") == 0);

    /* The countdown stops at 0:00 rather than wrapping. */
    UT_ASSERT(scnStatusLineText("Wave 3/10", 1000, 5000, out, sizeof(out)));
    UT_ASSERT(strcmp(out, "Wave 3/10    0:00") == 0);

    /* No countdown: the line alone. */
    UT_ASSERT(scnStatusLineText("Wave 1/5 over", SCN_STATUS_NO_COUNTDOWN, 1000,
                                out, sizeof(out)));
    UT_ASSERT(strcmp(out, "Wave 1/5 over") == 0);

    /* Nothing to draw. */
    out[0] = 'x';
    UT_ASSERT(!scnStatusLineText(NULL, 5, 0, out, sizeof(out)));
    UT_ASSERT(out[0] == '\0');
    UT_ASSERT(!scnStatusLineText("", 5, 0, out, sizeof(out)));
    UT_ASSERT(!scnStatusLineText("x", 5, 0, NULL, 16));

    /* A line the length of the field with a countdown still fits. */
    {
        char big[PACKET_MAX_CHAT_MESSAGE + 1];
        memset(big, 'w', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        UT_ASSERT(scnStatusLineText(big, 90 * 60 * TICKS_PER_SEC, 0, out,
                                    sizeof(out)));
        UT_ASSERT(strlen(out) == sizeof(big) - 1 + 4 + 5);
        UT_ASSERT(strcmp(out + sizeof(big) - 1, "    90:00") == 0);
    }

    return 0;
}
