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
 *Name:          dialog_footer
 *Filename:      dialog_footer.h
 *Purpose:
 *  Standardised footer button row for modal dialogs.
 *  One place to control: centering, equal-width buttons,
 *  muted-grey Cancel styling, separator above row, and
 *  the Esc/Enter/Ctrl+W/Cmd+. key bindings.
 *
 *  Conventions:
 *    - 2-button: [Cancel] [Confirm]   (Cancel-left, Confirm-right)
 *    - 3-button: [Destructive] [Cancel] [Primary]
 *    - Esc / Ctrl+W / Cmd+. always cancel
 *    - Enter only confirms when enterConfirms=true
 *      (use only for dialogs containing a text input)
 *    - Separator+spacing drawn above the row by default;
 *      skip on tiny confirmation modals
 *********************************************************/

#ifndef WB_DIALOG_FOOTER_H
#define WB_DIALOG_FOOTER_H

#include "imgui.h"

namespace WBUI {

enum FooterResult {
    FOOTER_NONE        = 0,
    FOOTER_CANCEL      = 1,
    FOOTER_CONFIRM     = 2,
    FOOTER_DESTRUCTIVE = 3,
};

/* Two-button footer: [Cancel] [Confirm].
 *
 * cancelLabel  : button label, e.g. STR_CANCEL. If NULL, only Confirm
 *                is shown (centered). Esc/Ctrl+W/Cmd+. still return
 *                FOOTER_CANCEL so callers can treat as dismiss.
 * confirmLabel : primary action label, e.g. STR_OK, STR_SAVE.
 * enterConfirms: set true ONLY for dialogs whose body has a text input.
 *                Enter key returns FOOTER_CONFIRM when true.
 * showSeparator: draw Separator+Spacing above the row. Default true.
 *                Set false on tiny confirmation modals (one-line body).
 *
 * Returns FOOTER_NONE / FOOTER_CANCEL / FOOTER_CONFIRM. */
int DialogFooter(const char *cancelLabel,
                 const char *confirmLabel,
                 bool enterConfirms = false,
                 bool showSeparator = true);

/* Three-button footer: [Cancel] [Destructive] [Primary].
 *
 * cancelLabel      : far-left, e.g. "Cancel" — consistent with 2-button order
 * destructiveLabel : middle, e.g. "Discard", "Don't Save"
 * primaryLabel     : far-right, e.g. "Save"
 *
 * Returns FOOTER_NONE / FOOTER_DESTRUCTIVE / FOOTER_CANCEL / FOOTER_CONFIRM. */
int DialogFooter3(const char *cancelLabel,
                  const char *destructiveLabel,
                  const char *primaryLabel,
                  bool enterConfirms = false,
                  bool showSeparator = true);

/* Apply just the muted-grey Cancel button colors (Push/Pop pair).
 *
 * For panel screens (Game Browser, UDP Setup, WBN Browser) that keep
 * their existing multi-button layout but want the new Cancel styling.
 *
 * Usage:
 *   WBUI::PushCancelStyle();
 *   if (ImGui::Button("Cancel", sz) || escPressed) { ... }
 *   WBUI::PopCancelStyle();
 */
void PushCancelStyle();
void PopCancelStyle();

/* Returns true once per frame if any "cancel-equivalent" key was pressed
 * (Esc / Ctrl+W / Cmd+W / Cmd+. on macOS) AND no child popup is open.
 *
 * Use at panel-screen call sites that don't use DialogFooter (because
 * their button layout is custom). The DialogFooter functions check this
 * internally — you only need to call this directly for the panel screens. */
bool CancelKeyPressed();

/* Draws a custom close-X glyph in the top-right corner of the current
 * ImGui window. For full-screen panels (NoTitleBar) that can't use
 * ImGui's built-in window close button.
 *
 * Returns true if clicked. Caller wires the click to the same handler
 * as their Cancel/Close button. Esc is NOT handled here — call
 * CancelKeyPressed() separately if you want Esc to also dismiss.
 *
 * Place the call early in the panel body, before other widgets that
 * might consume the top-right area. */
bool DrawPanelCloseX();

}  /* namespace WBUI */

#endif  /* WB_DIALOG_FOOTER_H */
