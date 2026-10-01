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

/*********************************************************
 *Name:          Scenario Panel Display List
 *Filename:      scenario_panel.c
 *Author:        John Morrison
 *Purpose:
 *  The one shared parser and writer for the scenario panel's display
 *  list. scnPanelParse turns a byte list into decoded primitives or
 *  says why it will not; scnPanelWrite turns decoded primitives back
 *  into bytes. Every frontend calls the same parser, so a malformed
 *  list is refused identically everywhere.
 *
 *  Both directions answer to one operand check (panelItemCheck), which
 *  is what keeps an item the writer emits an item the parser takes
 *  back.
 *
 *  Pure C over the public panel types — no sim state, no globals, no
 *  SDL — so it compiles into the standalone log viewer, the wasm
 *  target, Android and iOS, none of which link the bolo sim. The list
 *  is untrusted: it is written by a scenario script and arrives over
 *  the wire, so every bound is checked here rather than assumed.
 *********************************************************/
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "scenario_panel.h"

/* Operand bytes each primitive carries after its opcode byte. text is
 * the variable one: this is its fixed part, and its len bytes follow. */
static const uint8_t kPanelOperandBytes[SCN_PANEL_OP_TIMER + 1] = {
    0,   /* 0 is not a primitive */
    6,   /* rect:   x, y, w, h, colour, fill */
    5,   /* line:   x0, y0, x1, y1, colour */
    6,   /* text:   x, y, colour, size, align, len */
    6,   /* name:   x, y, colour, size, align, slot */
    3,   /* sprite: x, y, tile */
    9,   /* bar:    x, y, w, h, colour, value, max */
    10   /* timer:  x, y, colour, size, align, mode, tick */
};

/* Multi-byte operands are big-endian, the way log.h writes its records. */
static uint16_t panelReadU16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t panelReadU32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void panelWriteU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void panelWriteU32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

/* A text byte a frontend will draw. Control bytes and DEL are refused
 * so a script cannot write a newline or an escape into anyone's draw —
 * the same reason the operator console bounds what it prints. */
static bool panelTextByteOk(uint8_t c) {
    return c >= 0x20 && c != 0x7F;
}

/* The size mark's bytes, opcode included (see scenario_panel.h). */
#define PANEL_MARK_BYTES 7

int scnPanelIsSizeMark(const ScnPanelItem *item) {
    return item != NULL && item->op == SCN_PANEL_OP_RECT &&
           item->u.rect.x == SCN_PANEL_SIZE_LARGE && item->u.rect.y == 0 &&
           item->u.rect.w == 0 && item->u.rect.h == 0 &&
           item->u.rect.colour == SCN_PANEL_COLOUR_NONE &&
           item->u.rect.fill == 0;
}

/* Whether the bytes at off are a size mark. */
static bool panelMarkAt(const uint8_t *bytes, uint16_t len, uint16_t off) {
    if (off >= len || (uint16_t)(len - off) < PANEL_MARK_BYTES) {
        return false;
    }
    return bytes[off] == SCN_PANEL_OP_RECT &&
           bytes[off + 1] == SCN_PANEL_SIZE_LARGE && bytes[off + 2] == 0 &&
           bytes[off + 3] == 0 && bytes[off + 4] == 0 &&
           bytes[off + 5] == SCN_PANEL_COLOUR_NONE && bytes[off + 6] == 0;
}

/* The size field of a text, name or timer, or NULL for a primitive that
 * has none. */
static uint8_t *panelItemSize(ScnPanelItem *item) {
    switch (item->op) {
    case SCN_PANEL_OP_TEXT:  return &item->u.text.size;
    case SCN_PANEL_OP_NAME:  return &item->u.name.size;
    case SCN_PANEL_OP_TIMER: return &item->u.timer.size;
    default:                 return NULL;
    }
}

static bool panelItemIsLarge(const ScnPanelItem *item) {
    const uint8_t *size = panelItemSize((ScnPanelItem *)item);
    return size != NULL && *size == SCN_PANEL_SIZE_LARGE;
}

/* The operand rules, in one place because both directions answer to
 * them. Coordinates, widths, heights and tile ids are deliberately not
 * here: a rect may start inside the square and run past its edge, and
 * an id the skin does not define draws nothing, so clipping and
 * skipping are the drawer's business. */
static ScnPanelResult panelItemCheck(const ScnPanelItem *item) {
    switch (item->op) {
    case SCN_PANEL_OP_RECT:
        if (item->u.rect.colour >= SCN_PANEL_COLOURS) return SCN_PANEL_ERR_RANGE;
        if (item->u.rect.fill > 1)                    return SCN_PANEL_ERR_RANGE;
        /* On the wire this rect is the size mark, so as an item of its own
         * it would come back as a large flag on its neighbour. */
        if (scnPanelIsSizeMark(item))                 return SCN_PANEL_ERR_RANGE;
        return SCN_PANEL_OK;
    case SCN_PANEL_OP_LINE:
        if (item->u.line.colour >= SCN_PANEL_COLOURS) return SCN_PANEL_ERR_RANGE;
        return SCN_PANEL_OK;
    case SCN_PANEL_OP_TEXT: {
        uint8_t i;
        if (item->u.text.colour >= SCN_PANEL_COLOURS)      return SCN_PANEL_ERR_RANGE;
        if (item->u.text.size > SCN_PANEL_SIZE_LARGE)      return SCN_PANEL_ERR_RANGE;
        if (item->u.text.align > SCN_PANEL_ALIGN_RIGHT)    return SCN_PANEL_ERR_RANGE;
        if (item->u.text.len > SCN_PANEL_TEXT_MAX)         return SCN_PANEL_ERR_TEXT;
        for (i = 0; i < item->u.text.len; i++) {
            if (!panelTextByteOk((uint8_t)item->u.text.text[i])) {
                return SCN_PANEL_ERR_TEXT;
            }
        }
        return SCN_PANEL_OK;
    }
    case SCN_PANEL_OP_NAME:
        if (item->u.name.colour >= SCN_PANEL_COLOURS)   return SCN_PANEL_ERR_RANGE;
        if (item->u.name.size > SCN_PANEL_SIZE_LARGE)   return SCN_PANEL_ERR_RANGE;
        if (item->u.name.align > SCN_PANEL_ALIGN_RIGHT) return SCN_PANEL_ERR_RANGE;
        if (item->u.name.slot >= MAX_TANKS)             return SCN_PANEL_ERR_RANGE;
        return SCN_PANEL_OK;
    case SCN_PANEL_OP_SPRITE:
        return SCN_PANEL_OK;
    case SCN_PANEL_OP_BAR:
        if (item->u.bar.colour >= SCN_PANEL_COLOURS) return SCN_PANEL_ERR_RANGE;
        return SCN_PANEL_OK;
    case SCN_PANEL_OP_TIMER:
        if (item->u.timer.colour >= SCN_PANEL_COLOURS)   return SCN_PANEL_ERR_RANGE;
        if (item->u.timer.size > SCN_PANEL_SIZE_LARGE)   return SCN_PANEL_ERR_RANGE;
        if (item->u.timer.align > SCN_PANEL_ALIGN_RIGHT) return SCN_PANEL_ERR_RANGE;
        if (item->u.timer.mode > SCN_PANEL_TIMER_UP)     return SCN_PANEL_ERR_RANGE;
        return SCN_PANEL_OK;
    default:
        return SCN_PANEL_ERR_OPCODE;
    }
}

/* Bytes one decoded primitive occupies on the wire, 0 for an opcode
 * that is not a primitive. */
static uint16_t panelItemBytes(const ScnPanelItem *item) {
    if (item->op < SCN_PANEL_OP_RECT || item->op > SCN_PANEL_OP_TIMER) {
        return 0;
    }
    if (item->op == SCN_PANEL_OP_TEXT) {
        return (uint16_t)(1 + kPanelOperandBytes[SCN_PANEL_OP_TEXT] +
                          item->u.text.len +
                          (panelItemIsLarge(item) ? PANEL_MARK_BYTES : 0));
    }
    return (uint16_t)(1 + kPanelOperandBytes[item->op] +
                      (panelItemIsLarge(item) ? PANEL_MARK_BYTES : 0));
}

/* Decode the primitive at *off, advancing *off past it on success. The
 * caller has already established that *off < len. *item is scratch on
 * a refusal — part of it may have been filled in before the operand
 * check spoke, which is why scnPanelParse validates a whole list
 * against one throwaway item before it writes a caller's list. */
static ScnPanelResult panelItemParse(const uint8_t *bytes, uint16_t len,
                                     uint16_t *off, ScnPanelItem *item) {
    uint16_t at = *off;
    uint8_t op = bytes[at];
    const uint8_t *p;
    uint16_t avail;
    uint16_t need;

    if (op < SCN_PANEL_OP_RECT || op > SCN_PANEL_OP_TIMER) {
        return SCN_PANEL_ERR_OPCODE;
    }
    avail = (uint16_t)(len - at - 1);   /* bytes after the opcode */
    need = kPanelOperandBytes[op];
    if (avail < need) {
        return SCN_PANEL_ERR_TRUNCATED;
    }

    p = bytes + at + 1;
    item->op = op;

    switch (op) {
    case SCN_PANEL_OP_RECT:
        item->u.rect.x      = p[0];
        item->u.rect.y      = p[1];
        item->u.rect.w      = p[2];
        item->u.rect.h      = p[3];
        item->u.rect.colour = p[4];
        item->u.rect.fill   = p[5];
        break;
    case SCN_PANEL_OP_LINE:
        item->u.line.x0     = p[0];
        item->u.line.y0     = p[1];
        item->u.line.x1     = p[2];
        item->u.line.y1     = p[3];
        item->u.line.colour = p[4];
        break;
    case SCN_PANEL_OP_TEXT: {
        uint8_t textLen = p[5];
        /* The length is read before the body is: a length past the
         * buffer cannot be copied at all, so it is refused as a text
         * fault rather than reported as a short stream. */
        if (textLen > SCN_PANEL_TEXT_MAX) {
            return SCN_PANEL_ERR_TEXT;
        }
        if ((uint16_t)(avail - need) < textLen) {
            return SCN_PANEL_ERR_TRUNCATED;
        }
        item->u.text.x      = p[0];
        item->u.text.y      = p[1];
        item->u.text.colour = p[2];
        item->u.text.size   = p[3];
        item->u.text.align  = p[4];
        item->u.text.len    = textLen;
        memset(item->u.text.text, 0, sizeof(item->u.text.text));
        memcpy(item->u.text.text, p + 6, textLen);   /* terminated by the memset */
        need = (uint16_t)(need + textLen);
        break;
    }
    case SCN_PANEL_OP_NAME:
        item->u.name.x      = p[0];
        item->u.name.y      = p[1];
        item->u.name.colour = p[2];
        item->u.name.size   = p[3];
        item->u.name.align  = p[4];
        item->u.name.slot   = p[5];
        break;
    case SCN_PANEL_OP_SPRITE:
        item->u.sprite.x    = p[0];
        item->u.sprite.y    = p[1];
        item->u.sprite.tile = p[2];
        break;
    case SCN_PANEL_OP_BAR:
        item->u.bar.x      = p[0];
        item->u.bar.y      = p[1];
        item->u.bar.w      = p[2];
        item->u.bar.h      = p[3];
        item->u.bar.colour = p[4];
        item->u.bar.value  = panelReadU16(p + 5);
        item->u.bar.max    = panelReadU16(p + 7);
        break;
    case SCN_PANEL_OP_TIMER:
        item->u.timer.x      = p[0];
        item->u.timer.y      = p[1];
        item->u.timer.colour = p[2];
        item->u.timer.size   = p[3];
        item->u.timer.align  = p[4];
        item->u.timer.mode   = p[5];
        item->u.timer.tick   = panelReadU32(p + 6);
        break;
    default:
        return SCN_PANEL_ERR_OPCODE;   /* unreachable: the opcode was bounded above */
    }

    /* A size byte travels as small or normal only; large rides as normal
     * plus the size mark, which panelWireNext folds in. */
    {
        const uint8_t *size = panelItemSize(item);
        if (size != NULL && *size > SCN_PANEL_SIZE_NORMAL) {
            return SCN_PANEL_ERR_RANGE;
        }
    }
    {
        ScnPanelResult r = panelItemCheck(item);
        if (r != SCN_PANEL_OK) {
            return r;
        }
    }
    *off = (uint16_t)(at + 1 + need);
    return SCN_PANEL_OK;
}

/* One decoded item off the wire: a primitive, and the size mark after it
 * when there is one. *wire is how many wire primitives that took, 1 or 2.
 * A mark after anything but a normal text, name or timer is not folded,
 * and panelItemParse then refuses it as a rect of its own. */
static ScnPanelResult panelWireNext(const uint8_t *bytes, uint16_t len,
                                    uint16_t *off, ScnPanelItem *item,
                                    uint16_t *wire) {
    ScnPanelResult r = panelItemParse(bytes, len, off, item);
    uint8_t *size;

    *wire = 1;
    if (r != SCN_PANEL_OK) {
        return r;
    }
    size = panelItemSize(item);
    if (size != NULL && *size == SCN_PANEL_SIZE_NORMAL &&
        panelMarkAt(bytes, len, *off)) {
        *size = SCN_PANEL_SIZE_LARGE;
        *off  = (uint16_t)(*off + PANEL_MARK_BYTES);
        *wire = 2;
    }
    return SCN_PANEL_OK;
}

uint16_t scnPanelWireCount(const ScnPanelList *list) {
    uint16_t n = 0;
    uint16_t i;

    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->count && i < SCN_PANEL_ITEMS_MAX; i++) {
        n = (uint16_t)(n + (panelItemIsLarge(&list->items[i]) ? 2 : 1));
    }
    return n;
}

ScnPanelResult scnPanelParse(const uint8_t *bytes, uint16_t len, ScnPanelList *out) {
    ScnPanelItem item;
    uint16_t off;
    uint16_t count = 0;
    uint16_t wireCount = 0;
    uint16_t i;

    if (out == NULL) {
        return SCN_PANEL_ERR_RANGE;
    }
    if (bytes == NULL) {
        if (len > 0) {
            return SCN_PANEL_ERR_TRUNCATED;
        }
        out->count = 0;
        return SCN_PANEL_OK;
    }

    /* Two walks. The first refuses the whole list without touching
     * *out, so a caller's list is left alone when any byte in the
     * stream is bad; the second decodes what the first proved good.
     *
     * The limit is on wire primitives, a size mark counting as one, since
     * that is what an older parser counts; the decoded count is never more. */
    off = 0;
    while (off < len) {
        ScnPanelResult r;
        uint16_t       wire;
        if (wireCount >= SCN_PANEL_ITEMS_MAX) {
            return SCN_PANEL_ERR_TOO_MANY;
        }
        r = panelWireNext(bytes, len, &off, &item, &wire);
        if (r != SCN_PANEL_OK) {
            return r;
        }
        wireCount = (uint16_t)(wireCount + wire);
        if (wireCount > SCN_PANEL_ITEMS_MAX) {
            return SCN_PANEL_ERR_TOO_MANY;
        }
        count++;
    }

    off = 0;
    for (i = 0; i < count; i++) {
        uint16_t       wire;
        ScnPanelResult r = panelWireNext(bytes, len, &off, &out->items[i],
                                         &wire);
        if (r != SCN_PANEL_OK) {
            return r;   /* unreachable: the first walk took this list whole */
        }
    }
    out->count = (uint8_t)count;
    return SCN_PANEL_OK;
}

/* The size byte an item travels with: large goes out as normal, and the
 * size mark after it says the rest. */
static uint8_t panelWireSize(uint8_t size) {
    return (size == SCN_PANEL_SIZE_LARGE) ? (uint8_t)SCN_PANEL_SIZE_NORMAL
                                          : size;
}

uint16_t scnPanelWrite(const ScnPanelList *list, uint8_t *bytes, uint16_t cap) {
    uint16_t off = 0;
    uint8_t i;

    if (list == NULL || bytes == NULL) {
        return 0;
    }
    if (list->count > SCN_PANEL_ITEMS_MAX ||
        scnPanelWireCount(list) > SCN_PANEL_ITEMS_MAX) {
        return 0;
    }

    for (i = 0; i < list->count; i++) {
        const ScnPanelItem *item = &list->items[i];
        uint16_t need = panelItemBytes(item);
        uint8_t *p;

        if (need == 0 || panelItemCheck(item) != SCN_PANEL_OK) {
            return 0;   /* an item the parser would refuse never leaves here */
        }
        if ((uint16_t)(cap - off) < need) {
            return 0;
        }

        bytes[off] = item->op;
        p = bytes + off + 1;
        switch (item->op) {
        case SCN_PANEL_OP_RECT:
            p[0] = item->u.rect.x;
            p[1] = item->u.rect.y;
            p[2] = item->u.rect.w;
            p[3] = item->u.rect.h;
            p[4] = item->u.rect.colour;
            p[5] = item->u.rect.fill;
            break;
        case SCN_PANEL_OP_LINE:
            p[0] = item->u.line.x0;
            p[1] = item->u.line.y0;
            p[2] = item->u.line.x1;
            p[3] = item->u.line.y1;
            p[4] = item->u.line.colour;
            break;
        case SCN_PANEL_OP_TEXT:
            p[0] = item->u.text.x;
            p[1] = item->u.text.y;
            p[2] = item->u.text.colour;
            p[3] = panelWireSize(item->u.text.size);
            p[4] = item->u.text.align;
            p[5] = item->u.text.len;
            memcpy(p + 6, item->u.text.text, item->u.text.len);
            break;
        case SCN_PANEL_OP_NAME:
            p[0] = item->u.name.x;
            p[1] = item->u.name.y;
            p[2] = item->u.name.colour;
            p[3] = panelWireSize(item->u.name.size);
            p[4] = item->u.name.align;
            p[5] = item->u.name.slot;
            break;
        case SCN_PANEL_OP_SPRITE:
            p[0] = item->u.sprite.x;
            p[1] = item->u.sprite.y;
            p[2] = item->u.sprite.tile;
            break;
        case SCN_PANEL_OP_BAR:
            p[0] = item->u.bar.x;
            p[1] = item->u.bar.y;
            p[2] = item->u.bar.w;
            p[3] = item->u.bar.h;
            p[4] = item->u.bar.colour;
            panelWriteU16(p + 5, item->u.bar.value);
            panelWriteU16(p + 7, item->u.bar.max);
            break;
        case SCN_PANEL_OP_TIMER:
            p[0] = item->u.timer.x;
            p[1] = item->u.timer.y;
            p[2] = item->u.timer.colour;
            p[3] = panelWireSize(item->u.timer.size);
            p[4] = item->u.timer.align;
            p[5] = item->u.timer.mode;
            panelWriteU32(p + 6, item->u.timer.tick);
            break;
        default:
            return 0;   /* unreachable: panelItemBytes answered 0 for these */
        }
        if (panelItemIsLarge(item)) {
            /* The size mark, the last of the bytes need counted. */
            uint8_t *m = bytes + off + need - PANEL_MARK_BYTES;
            m[0] = SCN_PANEL_OP_RECT;
            m[1] = SCN_PANEL_SIZE_LARGE;
            m[2] = 0;
            m[3] = 0;
            m[4] = 0;
            m[5] = SCN_PANEL_COLOUR_NONE;
            m[6] = 0;
        }
        off = (uint16_t)(off + need);
    }

    return off;
}
