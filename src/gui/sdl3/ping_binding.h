/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          Ping Binding
 *Filename:      ping_binding.h
 *Purpose:
 *  One smart-ping binding packed into the same plain int
 *  every other keyItems field is, so the prefs file, the
 *  key-setup dialog and the wasm defaults keep storing an
 *  int and nothing else has to learn a new type.
 *
 *  A binding is a chord: zero or more of Ctrl/Alt/Shift plus
 *  exactly one key OR one mouse button.
 *
 *    bits  0..15  code: 0 = unbound,
 *                 1..PING_BIND_MOUSE_BASE-1 = SDL scancode,
 *                 PING_BIND_MOUSE_BASE + n = mouse button n
 *    bit   16     Ctrl
 *    bit   17     Alt
 *    bit   18     Shift
 *
 *  PING_BIND_MOUSE_BASE sits at 512, one past the largest
 *  scancode SDL defines (SDL_SCANCODE_COUNT is 512), so a
 *  mouse code can never be read as a key and a saved binding
 *  cannot change meaning if SDL adds scancodes below that.
 *
 *  Deliberately free of SDL and of the lang system: this is
 *  the part a unit test can drive with no window, no
 *  renderer and no string table. The caller resolves the
 *  display words (its own "Ctrl", the scancode's name) and
 *  hands them to pingBindingFormat.
 *********************************************************/

#ifndef WINBOLO_PING_BINDING_H
#define WINBOLO_PING_BINDING_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many bindings one ping action may hold. The two defaults (Ctrl and Alt
 * with the right mouse button) take the first two; the rest start unbound. */
#define PING_BIND_SLOTS 4

#define PING_BIND_CODE_MASK   0xFFFF
#define PING_BIND_MOUSE_BASE  512      /* one past SDL_SCANCODE_COUNT */
#define PING_BIND_MOUSE_MAX   8        /* SDL reports buttons 1..5; leave room */

#define PING_BIND_MOD_CTRL    0x00010000
#define PING_BIND_MOD_ALT     0x00020000
#define PING_BIND_MOD_SHIFT   0x00040000
#define PING_BIND_MOD_MASK    (PING_BIND_MOD_CTRL | PING_BIND_MOD_ALT | \
                               PING_BIND_MOD_SHIFT)

#define PING_BIND_NONE        0

/* Pack a chord. `mods` is a mask of PING_BIND_MOD_*; `code` is a scancode or
 * a mouse code from pingBindingMouseCode. A zero or out-of-range code gives
 * PING_BIND_NONE, so a caller cannot accidentally store a binding that fires
 * on nothing but still reads as bound. */
static inline int pingBindingEncode(int mods, int code) {
    if (code <= 0 || code > PING_BIND_CODE_MASK) return PING_BIND_NONE;
    if (code >= PING_BIND_MOUSE_BASE &&
        code >= PING_BIND_MOUSE_BASE + PING_BIND_MOUSE_MAX) {
        return PING_BIND_NONE;
    }
    return (mods & PING_BIND_MOD_MASK) | (code & PING_BIND_CODE_MASK);
}

static inline int pingBindingCode(int binding) {
    return binding & PING_BIND_CODE_MASK;
}

static inline int pingBindingMods(int binding) {
    return binding & PING_BIND_MOD_MASK;
}

static inline bool pingBindingIsSet(int binding) {
    return pingBindingCode(binding) != 0;
}

/* The code for mouse button `button` (SDL's 1-based numbering: 1 = left,
 * 2 = middle, 3 = right). Out of range gives 0, which encodes as unbound. */
static inline int pingBindingMouseCode(int button) {
    if (button <= 0 || button >= PING_BIND_MOUSE_MAX) return 0;
    return PING_BIND_MOUSE_BASE + button;
}

static inline bool pingBindingIsMouse(int binding) {
    int code = pingBindingCode(binding);
    return code >= PING_BIND_MOUSE_BASE &&
           code < PING_BIND_MOUSE_BASE + PING_BIND_MOUSE_MAX;
}

/* The mouse button a mouse binding names, or 0 when it is a key binding. */
static inline int pingBindingMouseButton(int binding) {
    if (!pingBindingIsMouse(binding)) return 0;
    return pingBindingCode(binding) - PING_BIND_MOUSE_BASE;
}

/* The scancode a key binding names, or 0 when it is a mouse binding. */
static inline int pingBindingScancode(int binding) {
    int code = pingBindingCode(binding);
    if (code == 0 || code >= PING_BIND_MOUSE_BASE) return 0;
    return code;
}

/* Does the chord `binding` fire for this event? `eventCode` is the code the
 * event carries (a scancode, or pingBindingMouseCode of the button pressed)
 * and `heldMods` is what Ctrl/Alt/Shift are doing right now.
 *
 * The modifier test is exact, not "at least": Ctrl+Right Mouse must not fire
 * on a bare right click, and a bare right-click binding must not fire while
 * the player is holding Ctrl for something else. An unbound slot matches
 * nothing. */
static inline bool pingBindingMatches(int binding, int eventCode, int heldMods) {
    if (!pingBindingIsSet(binding)) return false;
    if (pingBindingCode(binding) != (eventCode & PING_BIND_CODE_MASK)) return false;
    return pingBindingMods(binding) == (heldMods & PING_BIND_MOD_MASK);
}

/* Does any slot of `bindings` fire for this event? Returns the index of the
 * first that does, or -1. */
static inline int pingBindingMatchAny(const int *bindings, int count,
                                      int eventCode, int heldMods) {
    int i;
    if (bindings == NULL) return -1;
    for (i = 0; i < count; i++) {
        if (pingBindingMatches(bindings[i], eventCode, heldMods)) return i;
    }
    return -1;
}

/* Write the chord as display text: the modifiers in a fixed order joined by
 * "+", then the code's name. `codeName` is what the caller resolved for the
 * key or button (SDL_GetScancodeName, or the lang string for a mouse button);
 * `unboundName` is used on its own for an unbound slot. Any of the four name
 * arguments may be NULL, in which case that piece is skipped. Always
 * NUL-terminates when outLen > 0. */
static inline void pingBindingFormat(int binding,
                                     const char *ctrlName,
                                     const char *altName,
                                     const char *shiftName,
                                     const char *codeName,
                                     const char *unboundName,
                                     char *out, size_t outLen) {
    size_t used = 0;
    int mods;
    const char *parts[4];
    int nParts = 0;
    int i;

    if (out == NULL || outLen == 0) return;
    out[0] = '\0';

    if (!pingBindingIsSet(binding)) {
        if (unboundName == NULL) return;
        for (used = 0; used + 1 < outLen && unboundName[used]; used++) {
            out[used] = unboundName[used];
        }
        out[used] = '\0';
        return;
    }

    mods = pingBindingMods(binding);
    if ((mods & PING_BIND_MOD_CTRL)  && ctrlName)  parts[nParts++] = ctrlName;
    if ((mods & PING_BIND_MOD_ALT)   && altName)   parts[nParts++] = altName;
    if ((mods & PING_BIND_MOD_SHIFT) && shiftName) parts[nParts++] = shiftName;
    if (codeName) parts[nParts++] = codeName;

    for (i = 0; i < nParts; i++) {
        const char *p = parts[i];
        if (i > 0 && used + 1 < outLen) out[used++] = '+';
        while (*p && used + 1 < outLen) out[used++] = *p++;
    }
    out[used] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif /* WINBOLO_PING_BINDING_H */
