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
 * Name:          imgui_keysetup.cpp
 * Purpose:       Standalone blocking key setup dialog.
 *                Runs its own ImGui context and SDL event
 *                loop, same pattern as imgui_settings.cpp.
 *********************************************************/

#include <SDL3/SDL.h>

#include "imgui.h"
#include "../../imgui_theme.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_dialog_utils.h"
#include "imgui_nav_outline.h"
#include "imgui_controller_prompt.h"
#include "imgui_keycap.h"
#include "../imgui_steam_nav.h"
#include "dialog_footer.h"

extern "C" {
#include "../sdl3draw.h"
#include "../sdl3imgui.h"      /* sdl3ImguiGetUiScale */
#include "global.h"
#include "../bg_game.h"
#include "../glyphs.h"
#include "../input.h"
#include "../ping_binding.h"   /* the smart-ping chord slots */
#include "../../ping_kinds.h"  /* pingKindNameId — the direct rows' labels */
#include "../input_gamepad.h"  /* GamepadBindings — controller tab */
#include "../build_cursor.h"   /* build-cursor behaviour option flags */
#include "../../winbolo.h"
#include "../../lang.h"
#include "../../ui_mode.h"
#include "../../gamefront.h"   /* gameFrontPutPrefs — persist on OK */
#include "client_sim.h"        /* clientSim{Get,Set}Tank{AutoSlowdown,AutoHideGunsight} */
#include "imgui_keysetup.h"

extern bool useAutoslow;
extern bool useAutohide;
}


/* -------------------------------------------------------
 * Key Setup field enum and helpers — ported from
 * sdl3imgui.cpp.  Duplicated here because the two
 * contexts (blocking dialog vs. in-game modal) have
 * different lifecycle requirements.
 * ------------------------------------------------------- */

enum KeySetupField {
    ksNone = -1,
    ksForward, ksBackward, ksTurnLeft, ksTurnRight,
    ksShoot, ksLayMine, ksGunIncrease, ksGunDecrease,
    ksTankView, ksPillView, ksBaseView, ksAllyView, ksOverviewZoom,
    ksOverviewFollow, ksOverviewZoomIn, ksOverviewZoomOut,
    ksScrollUp, ksScrollDown, ksScrollLeft, ksScrollRight,
    ksQuickTree, ksQuickRoad, ksQuickWall, ksQuickPillbox, ksQuickMine,
    ksPushToTalk, ksMuteMic,
};

static keyItems      s_keys;
static bool          s_autoSlowdown;
static bool          s_autoGunsight;
static KeySetupField s_waiting = ksNone;

static const char *scancodeLabel(int scancode) {
    const char *name = SDL_GetScancodeName((SDL_Scancode)scancode);
    if (name && name[0] != '\0') return name;
    return langGetText(STR_DLGKEYSETUP_NONE_VAL);
}

static int *fieldPtr(KeySetupField f, keyItems *ki) {
    switch (f) {
        case ksForward:     return &ki->kiForward;
        case ksBackward:    return &ki->kiBackward;
        case ksTurnLeft:    return &ki->kiLeft;
        case ksTurnRight:   return &ki->kiRight;
        case ksShoot:       return &ki->kiShoot;
        case ksLayMine:     return &ki->kiLayMine;
        case ksGunIncrease: return &ki->kiGunIncrease;
        case ksGunDecrease: return &ki->kiGunDecrease;
        case ksTankView:    return &ki->kiTankView;
        case ksPillView:    return &ki->kiPillView;
        case ksBaseView:    return &ki->kiBaseView;
        case ksAllyView:    return &ki->kiAllyView;
        case ksOverviewZoom:return &ki->kiOverviewZoom;
        case ksOverviewFollow:  return &ki->kiOverviewFollow;
        case ksOverviewZoomIn:  return &ki->kiOverviewZoomIn;
        case ksOverviewZoomOut: return &ki->kiOverviewZoomOut;
        case ksScrollUp:    return &ki->kiScrollUp;
        case ksScrollDown:  return &ki->kiScrollDown;
        case ksScrollLeft:  return &ki->kiScrollLeft;
        case ksScrollRight: return &ki->kiScrollRight;
        case ksQuickTree:   return &ki->kiQuickTree;
        case ksQuickRoad:   return &ki->kiQuickRoad;
        case ksQuickWall:   return &ki->kiQuickWall;
        case ksQuickPillbox:return &ki->kiQuickPillbox;
        case ksQuickMine:   return &ki->kiQuickMine;
        case ksPushToTalk:  return &ki->kiPushToTalk;
        case ksMuteMic:     return &ki->kiMuteMic;
        default:            return nullptr;
    }
}

static void keyRow(const char *label, KeySetupField field) {
    int *ptr = fieldPtr(field, &s_keys);
    if (!ptr) return;

    bool waiting = (s_waiting == field);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);

    ImGui::TableSetColumnIndex(1);
    if (waiting) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_DLGKEYSETUP_PRESSAKEY));
    } else {
        const char  *name      = scancodeLabel(*ptr);
        float        textLineH = ImGui::GetTextLineHeight();
        float        glyphSize = textLineH * 1.5f;
        SDL_Texture *glyph     = glyphForKeyboardScancode((SDL_Scancode)*ptr);
        /* Lift the glyph by half its overshoot so its vertical centre
           aligns with the row text baseline; otherwise the cap sits
           below the line. */
        float        glyphYOff = (glyphSize - textLineH) * 0.5f;
        float        cursorY   = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(cursorY - glyphYOff);
        if (glyph) {
            ImGui::Image((ImTextureID)glyph, ImVec2(glyphSize, glyphSize));
        } else {
            drawProceduralKeycapAt(ImGui::GetCursorScreenPos(), glyphSize, name);
            ImGui::Dummy(ImVec2(glyphSize, glyphSize));
        }
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetCursorPosY(cursorY);
        ImGui::TextUnformatted(name);
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID((int)field);
    if (waiting) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) {
            s_waiting = ksNone;
        }
        imguiHandOnHover();
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_waiting = field;
        }
        imguiHandOnHover();
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::SmallButton("X")) {
            *ptr = 0;   /* SDL_SCANCODE_UNKNOWN — unbound */
        }
        imguiHandOnHover();
        imguiHelpTooltip("Clear");
    }
    ImGui::PopID();
}

/* -------------------------------------------------------
 * Smart-ping chord rows.
 *
 * These slots do not hold a scancode like every row above them: a ping
 * binding is modifiers plus a key OR a mouse button, packed into one int by
 * ping_binding.h. So they get their own row renderer, their own capture arm,
 * and a capture that also accepts a mouse button.
 *
 * Two groups, one after the other and numbered as one list so a single
 * "which row is armed" index covers both:
 *
 *   0 .. PING_BIND_SLOTS-1   the chords that open the pie menu
 *   then one per ping kind   the direct pings, which send that kind with no
 *                            menu at all
 * ------------------------------------------------------- */
#define PING_CHORD_ROWS (PING_BIND_SLOTS + PING_BIND_DIRECT_SLOTS)

static int s_pingWaitSlot = -1;      /* index into the row list above, or -1 */

/* The working copy's binding for one row of that list, or nullptr when the
 * index is out of range. */
static int *pingChordPtr(int row) {
    if (row < 0 || row >= PING_CHORD_ROWS) return nullptr;
    if (row < PING_BIND_SLOTS) return &s_keys.kiPing[row];
    return &s_keys.kiPingDirect[row - PING_BIND_SLOTS];
}

/* True while the capture is looking for a chord, so the mouse path in the
 * event pump knows a click belongs to the dialog rather than to the game. */
static bool pingCapturing(void) { return s_pingWaitSlot >= 0; }

/* Is this scancode one of the modifiers a chord is built from? Those must not
 * end the capture — the player presses Ctrl on the way to pressing the key
 * that the chord is actually for. */
static bool isChordModifier(int scancode) {
    switch (scancode) {
        case SDL_SCANCODE_LCTRL:  case SDL_SCANCODE_RCTRL:
        case SDL_SCANCODE_LALT:   case SDL_SCANCODE_RALT:
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT:
            return true;
        default:
            return false;
    }
}

/* Which modifiers are held right now, in ping_binding's vocabulary. */
static int pingCaptureMods(void) {
    SDL_Keymod km = SDL_GetModState();
    int mods = 0;
    if (km & SDL_KMOD_CTRL)  mods |= PING_BIND_MOD_CTRL;
    if (km & SDL_KMOD_ALT)   mods |= PING_BIND_MOD_ALT;
    if (km & SDL_KMOD_SHIFT) mods |= PING_BIND_MOD_SHIFT;
    return mods;
}

/* Is the pointer resting on an ImGui widget? A press there belongs to the
 * dialog — this row's own Cancel, another row's Change — and must not be
 * taken as the chord, or the slot silently becomes a bare "Left Mouse", every
 * later click opens the pie, and Cancel can never be reached with the mouse.
 *
 * The answer is the hover from the frame already drawn, which is the only one
 * either capture path can have: the standalone dialog asks before
 * ImGui_ImplSDL3_ProcessEvent and the in-game popup after it, but ImGui only
 * settles hovering inside NewFrame, so both read the frame the player was
 * looking at when they pressed. A pointer that lands on a button and clicks
 * inside the same frame reads one frame stale, and costs that click.
 *
 * Why the item test and not the two obvious flags: io.WantCaptureMouse is
 * true over the whole screen while any popup is open, and the in-game dialog
 * IS a popup, so nothing could ever be bound in game. IsWindowHovered
 * (AnyWindow) is true everywhere in the standalone dialog, which lays a
 * transparent full-screen host window under its panel, so nothing could ever
 * be bound there. "On a widget" is false over the game map, false over the
 * map overview's pan item while the modal blocks it, and false over the
 * dialog's own empty space — and true over exactly the buttons a press has to
 * be left to. */
static bool pingPointerOnWidget(void) {
    if (ImGui::GetCurrentContext() == nullptr) return false;
    return ImGui::IsAnyItemHovered();
}

/* Commit a captured code (a scancode, or pingBindingMouseCode of a button)
 * into the armed slot, with whatever modifiers are down at that moment. */
static void pingAssignCaptured(int code) {
    int *slot = pingChordPtr(s_pingWaitSlot);
    if (slot == nullptr) return;
    *slot = pingBindingEncode(pingCaptureMods(), code);
    s_pingWaitSlot = -1;
}

/* A mouse press offered to the armed row. True when it became the chord;
 * false when the pointer was on a widget, in which case the caller has to let
 * the click through to that widget instead of swallowing it. */
static bool pingCaptureFromMouse(int sdlMouseButton) {
    if (pingPointerOnWidget()) return false;
    pingAssignCaptured(pingBindingMouseCode(sdlMouseButton));
    return true;
}

/* The chord as display text: the modifier words and the key or button name,
 * all through the lang system. */
static void pingBindingLabel(int binding, char *out, size_t outLen) {
    const char *codeName = nullptr;
    if (pingBindingIsMouse(binding)) {
        switch (pingBindingMouseButton(binding)) {
            case SDL_BUTTON_LEFT:   codeName = langGetText(STR_PING_MOUSE_LEFT);   break;
            case SDL_BUTTON_MIDDLE: codeName = langGetText(STR_PING_MOUSE_MIDDLE); break;
            case SDL_BUTTON_RIGHT:  codeName = langGetText(STR_PING_MOUSE_RIGHT);  break;
            case SDL_BUTTON_X1:     codeName = langGetText(STR_PING_MOUSE_X1);     break;
            case SDL_BUTTON_X2:     codeName = langGetText(STR_PING_MOUSE_X2);     break;
            default:                codeName = langGetText(STR_DLGKEYSETUP_NONE_VAL); break;
        }
    } else if (pingBindingScancode(binding) != 0) {
        codeName = SDL_GetScancodeName((SDL_Scancode)pingBindingScancode(binding));
        if (codeName == nullptr || codeName[0] == '\0') {
            codeName = langGetText(STR_DLGKEYSETUP_NONE_VAL);
        }
    }
    pingBindingFormat(binding,
                      langGetText(STR_PING_MOD_CTRL),
                      langGetText(STR_PING_MOD_ALT),
                      langGetText(STR_PING_MOD_SHIFT),
                      codeName,
                      langGetText(STR_DLGKEYSETUP_NONE_VAL),
                      out, outLen);
}

/* One chord row. Same three columns as keyRow — label, current value,
 * Change/X — but no keycap glyph: a chord has no single cap to draw. `row`
 * indexes the combined list above, so the same renderer serves the menu
 * chords and the direct pings. */
static void pingRow(int row, langid label) {
    char value[96];
    bool waiting = (s_pingWaitSlot == row);
    int *slot    = pingChordPtr(row);

    if (slot == nullptr) return;

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(langGetText(label));

    ImGui::TableSetColumnIndex(1);
    if (waiting) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_DLGKEYSETUP_PRESSACHORD));
    } else {
        pingBindingLabel(*slot, value, sizeof(value));
        ImGui::TextUnformatted(value);
    }

    ImGui::TableSetColumnIndex(2);
    ImGui::PushID(1000 + row);
    if (waiting) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) s_pingWaitSlot = -1;
        imguiHandOnHover();
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_pingWaitSlot = row;
        }
        imguiHandOnHover();
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::SmallButton("X")) {
            *slot = PING_BIND_NONE;
        }
        imguiHandOnHover();
        imguiHelpTooltip("Clear");
    }
    ImGui::PopID();
}

/* -------------------------------------------------------
 * Controller (gamepad) binding tab.
 *
 * Edits a working copy of the runtime GamepadBindings (12 actions x
 * primary/secondary slots). Both keyboard and controller stay live at
 * runtime — this just lets the player view/rebind the controller half.
 * Capture is armed per (action, slot) and resolved from a gamepad button
 * down or a trigger crossing its threshold, fed in by the standalone
 * event loop or the in-game event pump (mirroring the scancode path).
 * ------------------------------------------------------- */
static GamepadBindings s_pad;                       /* working copy */
static int             s_padWaitAction = -1;        /* GamepadAction, or -1 */
static GamepadSlot     s_padWaitSlot   = GP_SLOT_PRIMARY;

static const char *padBindingName(const GamepadBinding *b) {
    if (!b || b->kind == GP_BIND_NONE) return "-";
    if (b->kind == GP_BIND_BUTTON) {
        const char *n = SDL_GetGamepadStringForButton((SDL_GamepadButton)b->code);
        return (n && n[0]) ? n : "?";
    }
    const char *n = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)b->code);
    return (n && n[0]) ? n : "?";
}

static SDL_Texture *padBindingGlyph(const GamepadBinding *b) {
    if (!b) return nullptr;
    if (b->kind == GP_BIND_BUTTON)  return glyphForGamepadButton((SDL_GamepadButton)b->code);
    if (b->kind == GP_BIND_TRIGGER) return glyphForGamepadAxis((SDL_GamepadAxis)b->code);
    return nullptr;
}

/* Commit a captured button/trigger into the slot currently armed. */
static void padAssignCaptured(GamepadBindKind kind, int code) {
    if (s_padWaitAction < 0 || s_padWaitAction >= GP_ACT_COUNT) return;
    GamepadBinding b; b.kind = kind; b.code = code;
    GamepadActionBindings *ab = &s_pad.b[s_padWaitAction];
    if (s_padWaitSlot == GP_SLOT_PRIMARY) ab->pri = b; else ab->sec = b;
    s_padWaitAction = -1;
}

/* Binding display for one slot (glyph + name, or the capture prompt). */
static void padSlotDisplay(GamepadAction act, GamepadSlot slot) {
    const GamepadBinding *b = (slot == GP_SLOT_PRIMARY) ? &s_pad.b[act].pri
                                                        : &s_pad.b[act].sec;
    bool waiting = (s_padWaitAction == (int)act && s_padWaitSlot == slot);
    if (waiting) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s",
                           langGetText(STR_GP_REBIND_PROMPT));
        return;
    }
    const char  *name = padBindingName(b);
    float        h    = ImGui::GetTextLineHeight();
    float        gs   = h * 1.5f;
    SDL_Texture *g    = padBindingGlyph(b);
    if (g) {
        float yoff = (gs - h) * 0.5f, cy = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(cy - yoff);
        ImGui::Image((ImTextureID)g, ImVec2(gs, gs));
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::SetCursorPosY(cy);
    }
    ImGui::TextUnformatted(name);
}

/* Change/Cancel button for one slot — kept in its own table column so the
   buttons line up across all rows. */
static void padSlotChange(GamepadAction act, GamepadSlot slot) {
    bool waiting = (s_padWaitAction == (int)act && s_padWaitSlot == slot);
    ImGui::PushID((int)act * 2 + (int)slot);
    if (waiting) {
        if (ImGui::SmallButton(langGetText(STR_CANCEL))) s_padWaitAction = -1;
        imguiHandOnHover();
    } else {
        if (ImGui::SmallButton(langGetText(STR_DLGKEYSETUP_CHANGE))) {
            s_padWaitAction = (int)act;
            s_padWaitSlot   = slot;
        }
        imguiHandOnHover();
        ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::SmallButton("X")) {
            GamepadBinding none; none.kind = GP_BIND_NONE; none.code = 0;
            if (slot == GP_SLOT_PRIMARY) s_pad.b[act].pri = none;
            else                         s_pad.b[act].sec = none;
        }
        imguiHandOnHover();
        imguiHelpTooltip("Clear");
    }
    ImGui::PopID();
}

/* Action | Primary glyph | Change | Secondary glyph | Change */
static void controllerRow(const char *label, GamepadAction act) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1); padSlotDisplay(act, GP_SLOT_PRIMARY);
    ImGui::TableSetColumnIndex(2); padSlotChange (act, GP_SLOT_PRIMARY);
    ImGui::TableSetColumnIndex(3); padSlotDisplay(act, GP_SLOT_SECONDARY);
    ImGui::TableSetColumnIndex(4); padSlotChange (act, GP_SLOT_SECONDARY);
}

/* Full-width sensitivity slider row: label in the Action column, slider in the
   Primary column. Lower = finer control (slower movement per stick deflection).
   Binds the global live; the OK handler flushes it to prefs. */
/* Checkbox row, indented under the related binding.  Optional hover tooltip. */
/* Build-cursor behaviour options as plain checkboxes (not table rows), shared
   by both controller tabs.  Rendered below the per-path content -- the bindings
   table on the native path, the sliders under Steam Input.  These are game
   rules, not button remaps, so they apply identically on both input paths. */
static void renderBuildBehaviorOptions() {
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s",
                       "Toggle Build Cursor Additional Options");
    ImGui::Spacing();
    auto cb = [](const char *label, bool *v, const char *tip) {
        ImGui::Checkbox(label, v);
        if (tip && *tip) imguiHelpTooltip(tip);
    };
    cb("Hold to build, release to exit (momentary)", &g_buildHoldMomentary,
       "Hold the build-toggle button (>200ms) to temporarily enter build mode; "
       "releasing it exits again -- a quick way to pop in, build, and pop back "
       "to driving. A quick tap still toggles build mode normally (stays on "
       "until pressed again).");
    cb("Double-tap builds a road under the tank", &g_buildDoubleTapRoad,
       "Double-tap the build-toggle button to instantly drop a road on the "
       "tank's own tile -- without changing your selected build type or moving "
       "the build cursor.");
    cb("Exiting build mode executes the build where the cursor is",
       &g_buildExitExecutes, nullptr);
    if (g_buildExitExecutes) {
        ImGui::Indent();
        cb("Only when exiting press-and-hold (momentary) build mode",
           &g_buildExitExecutesMomentaryOnly, nullptr);
        ImGui::Unindent();
    }
    cb("Auto-close build mode when a build is executed",
       &g_buildAutoCloseOnExecute,
       "When on, using Execute Build automatically exits build cursor mode "
       "afterwards, returning you to driving.");
}

static void sensitivityRow(const char *label, float *value,
                           float vmin, float vmax, const char *fmt, bool indent) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (indent) ImGui::Indent();
    ImGui::TextUnformatted(label);
    if (indent) ImGui::Unindent();
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(value);
    ImGui::SetNextItemWidth(180.0f);
    if (ImGui::SliderFloat("##sens", value, vmin, vmax, fmt)) {
        if (*value < vmin) *value = vmin;
        if (*value > vmax) *value = vmax;
    }
    ImGui::PopID();
}

/* -------------------------------------------------------
 * Binding rows + the two checkboxes — the visible body of the
 * key-setup form, minus any footer or commit logic. Shared by
 * renderFormBody (standalone dialog + in-game popup) and the
 * embedded wizard path (imguiKeySetupRenderEmbedded), so the
 * onboarding wizard can draw the same controls inside its own
 * already-running ImGui context without the OK/Cancel footer.
 * Operates purely on the shared file-static form state.
 * ------------------------------------------------------- */
/* Set when the dialog opens; the tab logic consumes it once to land on the
   Controller tab when a pad is connected (so controller users start there). */
static bool s_requestControllerTabDefault = false;

static void renderKeyRows(float extraFooterReserve = 0.0f) {
    /* Scrollable region containing all binding rows. footerH reserves space
     * for the two checkboxes below the child; extraFooterReserve lets an
     * embedding caller (the onboarding wizard) also reserve room for its own
     * button row beneath, so the rows scroll inside the child rather than
     * pushing the host window past its fixed height. */
    float footerH = ImGui::GetFrameHeightWithSpacing() * 3.0f +
                    ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginChild("##bindings", ImVec2(0.0f, -(footerH + extraFooterReserve)), false);

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit |
        ImGuiTableFlags_RowBg;

    auto section = [&](const char *sectionTitle) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "%s", sectionTitle);
        ImGui::BeginTable(sectionTitle, 3, tflags, ImVec2(0, 0));
        ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_ACTION),
                                ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_KEY),
                                ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed);
    };
    auto endSection = [&]() { ImGui::EndTable(); };

    /* Shoulder buttons (L1 / R1) cycle the tabs. The SDL3 backend feeds
       ImGuiKey_GamepadL1/R1 even while gamepad nav is disabled, so this works
       on the native path. s_forceTab is set for one frame to drive a tab via
       ImGuiTabItemFlags_SetSelected; s_activeTab tracks the open tab so the
       next cycle starts from the right place (and mouse clicks stay honoured). */
    static int s_activeTab = 0;
    int s_forceTab = -1;
    int tabCount = inputGamepadIsConnected() ? 2 : 1;
    /* On open, default to the Controller tab when a pad is connected. */
    if (s_requestControllerTabDefault) {
        s_requestControllerTabDefault = false;
        if (tabCount > 1) { s_activeTab = 1; s_forceTab = 1; }
    }
    /* Don't cycle tabs while a binding is being captured — a shoulder press
       then belongs to the binding, not to tab switching.  Also suppress on the
       frame a capture *ends*: the very button press that completes the capture
       (e.g. assigning L1/RT) clears s_padWaitAction the same frame ImGui still
       reports that button's edge, which would otherwise tab us out of the
       Controller tab the instant the bind is set. */
    bool capturing = (s_waiting != ksNone) || (s_padWaitAction != -1) ||
                     pingCapturing();
    static bool s_wasCapturing = false;
    bool suppressTabCycle = capturing || s_wasCapturing;
    s_wasCapturing = capturing;
    if (tabCount > 1 && !suppressTabCycle) {
        /* Native pad: L1/R1 (backend feeds ImGuiKey_Gamepad* even with nav off).
           Steam Input: the pad is hidden from SDL, so L1/R1 never arrive — use
           the menu_tab_left/right Steam actions (mapped to triggers) instead. */
        int shift = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ? 1 : 0)
                  - (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ? 1 : 0);
        if (shift == 0)
            shift = imguiSteamNavConsumeMenuTabShift();
        if (shift != 0)
            s_forceTab = (s_activeTab + shift + tabCount) % tabCount;
    }

    if (ImGui::BeginTabBar("##keysetuptabs")) {
        if (ImGui::BeginTabItem(langGetText(STR_DLGKEYSETUP_TAB_KEYBOARD), nullptr,
                                s_forceTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_activeTab = 0;
            section(langGetText(STR_DLGKEYSETUP_DRIVETANK));
            keyRow(langGetText(STR_DLGKEYSETUP_FASTER),    ksForward);
            keyRow(langGetText(STR_DLGKEYSETUP_SLOWER),    ksBackward);
            keyRow(langGetText(STR_DLGKEYSETUP_TURNLEFT),  ksTurnLeft);
            keyRow(langGetText(STR_DLGKEYSETUP_TURNRIGHT), ksTurnRight);
            endSection();

            section(langGetText(STR_DLGKEYSETUP_WEAPONS));
            keyRow(langGetText(STR_DLGKEYSETUP_SHOOT),    ksShoot);
            keyRow(langGetText(STR_DLGKEYSETUP_LAYMINE),  ksLayMine);
            endSection();

            section(langGetText(STR_DLGKEYSETUP_GUNRANGE));
            keyRow(langGetText(STR_DLGKEYSETUP_INCREASE), ksGunIncrease);
            keyRow(langGetText(STR_DLGKEYSETUP_DECREASE), ksGunDecrease);
            endSection();

            section(langGetText(STR_DLGKEYSETUP_VIEW));
            keyRow(langGetText(STR_DLGKEYSETUP_TANKVIEW), ksTankView);
            keyRow(langGetText(STR_DLGKEYSETUP_PILLVIEW), ksPillView);
            keyRow(langGetText(STR_DLGKEYSETUP_BASEVIEW), ksBaseView);
            keyRow(langGetText(STR_DLGKEYSETUP_ALLYVIEW), ksAllyView);
            keyRow(langGetText(STR_DLGKEYSETUP_OVERVIEWZOOM), ksOverviewZoom);
            keyRow(langGetText(STR_DLGKEYSETUP_OVERVIEWZOOMIN),  ksOverviewZoomIn);
            keyRow(langGetText(STR_DLGKEYSETUP_OVERVIEWZOOMOUT), ksOverviewZoomOut);
            keyRow(langGetText(STR_DLGKEYSETUP_OVERVIEWFOLLOW),  ksOverviewFollow);
            endSection();

            section(langGetText(STR_DLGKEYSETUP_SCROLL));
            keyRow(langGetText(STR_DLGKEYSETUP_SCROLLUP),    ksScrollUp);
            keyRow(langGetText(STR_DLGKEYSETUP_SCROLLDOWN),  ksScrollDown);
            keyRow(langGetText(STR_DLGKEYSETUP_SCROLLLEFT),  ksScrollLeft);
            keyRow(langGetText(STR_DLGKEYSETUP_SCROLLRIGHT), ksScrollRight);
            endSection();

            section(langGetText(STR_DLGKEYSETUP_QUICKKEYS));
            keyRow(langGetText(STR_DLGKEYSETUP_TREE),         ksQuickTree);
            keyRow(langGetText(STR_DLGKEYSETUP_ROAD),         ksQuickRoad);
            keyRow(langGetText(STR_DLGKEYSETUP_WALL),         ksQuickWall);
            keyRow(langGetText(STR_DLGKEYSETUP_QUICKPILLBOX), ksQuickPillbox);
            keyRow(langGetText(STR_DLGKEYSETUP_QUICKMINE),    ksQuickMine);
            endSection();

#if defined(WINBOLO_VOICE)
            /* Unbound by default — guessing a key here would silently steal
               one of the bindings above from players who never wanted voice. */
            section(langGetText(STR_DLGSETTINGS_VOICE));
            keyRow(langGetText(STR_DLGKEYSETUP_PUSHTOTALK), ksPushToTalk);
            keyRow(langGetText(STR_DLGKEYSETUP_MUTEMIC),    ksMuteMic);
            endSection();
#endif

            section(langGetText(STR_DLGKEYSETUP_PING));
            /* The three chords that open the pie, then one row per kind for
               the direct pings — same order the kinds are numbered in, so the
               standard ping heads the list the way it is the pie's centre.
               A chord on a direct row wins over the same chord on a menu row
               (pingBindingDirectKind). */
            pingRow(0, STR_DLGKEYSETUP_PING);
            pingRow(1, STR_DLGKEYSETUP_PING_ALT);
            pingRow(2, STR_DLGKEYSETUP_PING_ALT2);
            for (int pk = 0; pk < PING_BIND_DIRECT_SLOTS; pk++) {
                pingRow(PING_BIND_SLOTS + pk,
                        pingKindNameId((unsigned char)pk));
            }
            endSection();
            ImGui::EndTabItem();
        }

        /* Controller tab — only shown when a gamepad is connected. */
        if (inputGamepadIsConnected() &&
            ImGui::BeginTabItem(langGetText(STR_GP_SECTION), nullptr,
                                s_forceTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            s_activeTab = 1;
            ImGui::Spacing();
          if (inputGamepadIsSteamInput()) {
            /* Steam Input owns the button/stick mapping via its own
               configurator (and the action manifest we ship), so the
               per-button remaps below would do nothing -- hide them and
               say why.  The sensitivity sliders and build-rule options are
               game behaviour (not remaps), so they still apply and stay. */
            ImGui::TextWrapped(
                "This controller is running through Steam Input. Button and "
                "trigger mappings are configured in Steam "
                "(Steam \xE2\x86\x92 Settings \xE2\x86\x92 Controller, or the in-game "
                "Steam overlay), not here, so WinBolo's per-button remaps are "
                "hidden. To rebind, open the Steam controller configurator for "
                "WinBolo. The options below are game behaviour and still apply.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextDisabled("Game options (still apply under Steam Input):");
            ImGui::Spacing();
            ImGui::TextUnformatted("Tank turn sensitivity");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("##sitanksens", &g_gamepadTankSensitivity,
                                   0.10f, 1.00f, "%.2f")) {
                if (g_gamepadTankSensitivity < 0.10f) g_gamepadTankSensitivity = 0.10f;
                if (g_gamepadTankSensitivity > 1.00f) g_gamepadTankSensitivity = 1.00f;
            }
            ImGui::TextUnformatted("Build cursor sensitivity");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::SliderFloat("##sibuildsens", &g_gamepadBuildCursorSensitivity,
                                   0.25f, 2.00f, "%.2fx")) {
                if (g_gamepadBuildCursorSensitivity < 0.25f) g_gamepadBuildCursorSensitivity = 0.25f;
                if (g_gamepadBuildCursorSensitivity > 2.00f) g_gamepadBuildCursorSensitivity = 2.00f;
            }
          } else {
            /* Action column gets at least ~33% of the table width. */
            ImGui::BeginTable("##padbindings", 5, tflags, ImVec2(0, 0));
            ImGui::TableSetupColumn(langGetText(STR_DLGKEYSETUP_COL_ACTION),
                                    ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Primary",   ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableHeadersRow();
            /* Left stick = tank move; its sensitivity slider sits at the top.
               10%-100%: 100% = snap (current), 50% = turn tracks stick 1:1. */
            sensitivityRow("Tank turn sensitivity", &g_gamepadTankSensitivity,
                           0.10f, 1.00f, "%.2f", /*indent=*/false);
            controllerRow("Lock direction (while pressed)",              GP_ACT_LOCK_HEADING);
            controllerRow(langGetText(STR_GP_ACTION_FIRE),                GP_ACT_FIRE);
            controllerRow(langGetText(STR_GP_ACTION_MINE),                GP_ACT_MINE);
            controllerRow(langGetText(STR_GP_ACTION_GUNSIGHT_INC),        GP_ACT_GUNSIGHT_INC);
            controllerRow(langGetText(STR_GP_ACTION_GUNSIGHT_DEC),        GP_ACT_GUNSIGHT_DEC);
            controllerRow(langGetText(STR_GP_ACTION_BUILD_CONFIRM),       GP_ACT_BUILD_CONFIRM);
            controllerRow(langGetText(STR_GP_ACTION_BUILD_PREV),          GP_ACT_BUILD_PREV);
            controllerRow(langGetText(STR_GP_ACTION_BUILD_NEXT),          GP_ACT_BUILD_NEXT);
            controllerRow(langGetText(STR_GP_ACTION_BUILD_CURSOR_TOGGLE), GP_ACT_BUILD_CURSOR_TOGGLE);
            sensitivityRow("Build cursor sensitivity", &g_gamepadBuildCursorSensitivity,
                           0.25f, 2.00f, "%.2fx", /*indent=*/true);
            controllerRow("Exit build mode, no build",
                          GP_ACT_BUILD_CANCEL);
            controllerRow(langGetText(STR_GP_ACTION_VIEW_CYCLE),          GP_ACT_VIEW_CYCLE);
            controllerRow("Tank view",                                    GP_ACT_TANK_VIEW);
            controllerRow(langGetText(STR_GP_ACTION_VIEW_PLAYERS),        GP_ACT_VIEW_PLAYERS);
            controllerRow(langGetText(STR_GP_ACTION_QUICK_CHAT),          GP_ACT_QUICK_CHAT);
            controllerRow(langGetText(STR_GP_ACTION_PAUSE),               GP_ACT_PAUSE);
            /* Smart ping, same order as the keyboard tab: the held chord that
               opens the pie (the right stick or the d-pad then picks a sector
               and letting go sends it), then one row per kind that sends
               straight away. All unbound until the player binds them, and all
               aimed at the build cursor's square — a pad has no pointer. */
            controllerRow(langGetText(STR_DLGKEYSETUP_PING),               GP_ACT_PING_MENU);
            for (int pk = 0; pk < PING_BIND_DIRECT_SLOTS; pk++) {
                controllerRow(langGetText(pingKindNameId((unsigned char)pk)),
                              (GamepadAction)(GP_ACT_PING_DIRECT_FIRST + pk));
            }
            ImGui::EndTable();
          }
            /* Build-cursor behaviour options, shared by both paths, below the
               per-path content (bindings table on native, sliders on Steam). */
            ImGui::Spacing();
            renderBuildBehaviorOptions();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_AUTOSLOWDOWN), &s_autoSlowdown);
    ImGui::SameLine();
    ImGui::Checkbox(langGetText(STR_DLGKEYSETUP_AUTOGUNSIGHT), &s_autoGunsight);
    ImGui::Spacing();
}

/* -------------------------------------------------------
 * Shared form body — the binding rows (renderKeyRows) plus the
 * press-to-bind indicator, controller input-path line, and the
 * OK/Cancel footer with commit logic. Drawn by BOTH the
 * standalone blocking dialog (imguiKeySetupShow) AND the in-game
 * popup wrapper (imguiKeySetupRenderInGamePopup).
 *
 * Returns:  1 = OK clicked (state has been committed)
 *          -1 = Cancel clicked / Escape pressed
 *           0 = still showing this frame
 *
 * On OK, this function commits the shared file-static state
 * to the frontend globals (useAutoslow / useAutohide), pushes
 * the typed keys via windowSetKeys, optionally pushes the
 * auto-slowdown / auto-gunsight flags onto the live tank when
 * cs != NULL (in-game path — pre-game cs is always NULL since
 * no tank exists yet), and flushes everything to INI via
 * gameFrontPutPrefs so the choice is durable immediately.
 * ------------------------------------------------------- */
static int renderFormBody(struct ClientSim *cs) {
    /* If the controller vanished mid-capture, drop the capture so the
     * dialog can't get stuck "busy" with the Controller tab gone. */
    if (s_padWaitAction != -1 && !inputGamepadIsConnected()) s_padWaitAction = -1;

    bool busy = (s_waiting != ksNone) || (s_padWaitAction != -1) ||
                pingCapturing();

    /* Plain-text input-path indicator, always visible while a controller is
     * connected (not relying on colour to convey it). */
    if (inputGamepadIsConnected()) {
        ImGui::Text("Controller input: %s",
                    inputGamepadIsSteamInput()
                        ? "Steam Input (Path A) - bindings managed by Steam"
                        : "Native SDL (Path B)");
        ImGui::Spacing();
    }

    renderKeyRows();

    int result = 0;

    if (busy) ImGui::BeginDisabled();
    int footer = WBUI::DialogFooter(langGetText(STR_CANCEL),
                                    langGetText(STR_OK));
    if (busy) ImGui::EndDisabled();

    /* Skip key/click action while capturing a binding — the footer
     * suppresses itself visually via BeginDisabled but bindings still fire. */
    if (!busy) {
        if (footer == WBUI::FOOTER_CONFIRM) {
            windowSetKeys(&s_keys);
            inputGamepadBindingsSetAll(&s_pad);  /* push controller bindings live */
            useAutoslow = s_autoSlowdown;
            useAutohide = s_autoGunsight;
            if (cs != NULL) {
                /* In-game path: push the new flags onto the live tank so
                 * the next sim tick respects them. Pre-game (cs==NULL) skips
                 * this — no tank exists yet, and the next clientSimSetupSelf
                 * path applies useAutoslow via frontEndApplyLocalTankPrefs. */
                clientSimSetTankAutoSlowdown(cs, s_autoSlowdown);
                clientSimSetTankAutoHideGunsight(cs, s_autoGunsight);
            }
            /* gameFrontPutPrefs writes both [KEYS] and [GAMEPAD] (the latter
             * read back from the global we just set above). */
            gameFrontPutPrefs(&s_keys);
            s_waiting = ksNone;
            s_padWaitAction = -1;
            result = 1;
        } else if (footer == WBUI::FOOTER_CANCEL) {
            s_waiting = ksNone;
            s_padWaitAction = -1;
            result = -1;
        }
    }

    return result;
}

/* -------------------------------------------------------
 * Main blocking dialog
 * ------------------------------------------------------- */

extern "C" int imguiKeySetupShow(void) {
    SDL_Window *window = sdl3DrawGetWindow();
    SDL_Renderer *renderer = sdl3DrawGetRenderer();
    if (!window || !renderer) return 0;
    s_requestControllerTabDefault = true;

    /* Save logical presentation */
    int savedLogW = 0, savedLogH = 0;
    SDL_RendererLogicalPresentation savedLogMode = SDL_LOGICAL_PRESENTATION_DISABLED;
    dialogSaveLogicalPresentation(renderer, &savedLogW, &savedLogH, &savedLogMode);

    int screenW, screenH;
    SDL_GetWindowSize(window, &screenW, &screenH);
    if (screenW <= 0 || screenH <= 0) { screenW = 1024; screenH = 768; }
    float s = dialogComputeScale(screenW, screenH);

#if !BOLO_MOBILE
    if (!uiModeIsSteamDeck()) {
        dialogSetWindowSize(window, 1024, 768);
        SDL_SetWindowResizable(window, true);
    }
    /* On Deck: keep the existing fullscreen 1280×800 window — the
       panel sizes itself via dialogComputeScale + the SetNextWindowSize
       calls below, with a 95% clamp that fits 800px height. */
    dialogSetWindowTitle(window, langGetText(STR_DLGKEYSETUP_WINTITLE));
#endif
    dialogRestorePosition(window);
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    imguiRegisterPlatformOpenUrl();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAlways = true;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();
    imguiApplyBoloTheme();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    dialogApplyScaling(s);

    /* Ensure the gamepad subsystem is up and any connected pad is opened.
     * In-game this happened in the main loop; the pre-game standalone
     * dialog runs its own loop, so without this inputGamepadIsConnected()
     * is false here and the Controller tab never appears. Safe to call
     * again — the main loop re-inits cleanly when a game starts. */
    inputGamepadInit();

    /* Load current bindings */
    windowGetKeys(&s_keys);
    inputGamepadBindingsGetAll(&s_pad);
    s_autoSlowdown = useAutoslow;
    s_autoGunsight = useAutohide;
    s_waiting = ksNone;
    s_padWaitAction = -1;
    s_pingWaitSlot = -1;

    /* Background game */
    BgGame *bg = bgGameGetShared();
    bool hasBg = (bg != nullptr);
    Uint64 lastTickTime = SDL_GetTicks();

    bool running = true;
    int result = 0;

    while (running) {
        Uint64 frameCapStart = dialogFrameCapBegin();
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Smart-ping chord capture — a key or a mouse button, with
               whatever modifiers are down. Before the scancode arm below so
               an armed ping row wins the keystroke, and it must ignore the
               modifier keys themselves: the player presses Ctrl on the way to
               the key the chord is for. */
            if (pingCapturing()) {
                if (ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.windowID == SDL_GetWindowID(window)) {
                    if (ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                        s_pingWaitSlot = -1;
                    } else if (!isChordModifier((int)ev.key.scancode)) {
                        pingAssignCaptured((int)ev.key.scancode);
                    }
                    continue;
                }
                /* A press on the dialog's own buttons is not a chord: it falls
                   through to ImGui below so Cancel cancels and another row's
                   Change arms that row instead. */
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    pingCaptureFromMouse((int)ev.button.button)) {
                    continue;
                }
            }

            /* Key capture — intercept before ImGui sees it */
            if (s_waiting != ksNone && ev.type == SDL_EVENT_KEY_DOWN &&
                ev.key.windowID == SDL_GetWindowID(window)) {
                SDL_Scancode sc = ev.key.scancode;
                if (sc == SDL_SCANCODE_ESCAPE) {
                    s_waiting = ksNone;
                } else {
                    int *ptr = fieldPtr(s_waiting, &s_keys);
                    if (ptr) *ptr = (int)sc;
                    s_waiting = ksNone;
                }
                continue;
            }

            /* Controller binding capture — intercept a gamepad button down
             * or a trigger crossing its threshold before ImGui sees it.
             * Escape (keyboard) cancels. */
            if (s_padWaitAction != -1) {
                if (ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    s_padWaitAction = -1;
                    continue;
                }
                if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                    padAssignCaptured(GP_BIND_BUTTON, (int)ev.gbutton.button);
                    continue;
                }
                if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                    (ev.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                     ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) &&
                    ev.gaxis.value > 16384 /* ~0.5 of 32767 */) {
                    padAssignCaptured(GP_BIND_TRIGGER, (int)ev.gaxis.axis);
                    continue;
                }
            }

            /* Keep gamepad connection state current so the Controller tab
             * appears/disappears live on hot-plug while the dialog is up. */
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED ||
                ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
                inputGamepadProcessEvent(&ev);
            }

            ImGui_ImplSDL3_ProcessEvent(&ev);
            dialogHandleGamepadCancelEvent(window, &ev);
            if (dialogHandleDevicePresetEvent(window, &ev)) continue;
            dialogHandleWindowMoveResize(window, &ev);
            if (dialogHandleQuitEvent(window, &ev)) {
                running = false;
            }
        }

        /* Tick the background game */
        if (hasBg && !bg->paused) {
            bgGameTickFixed(bg, &lastTickTime);
        } else if (hasBg && bg->paused) {
            lastTickTime = SDL_GetTicks();
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        dialogResetTextInputArea(window);
        dialogOverrideFramebufferScale(renderer);
        ImGui::NewFrame();
        imguiSteamNavActivateMenuSet();
        imguiSteamNavFeedCurrentContext();
        controllerDialogsRenderMenu();

        int winW, winH;
        SDL_GetWindowSize(window, &winW, &winH);

        /* Transparent full-screen host window */
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)winW, (float)winH));
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##KeySetupBg", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        /* Centered overlay panel. Wide enough to fit the Controller tab's
         * widest row (the build-option checkboxes); clamped to screen below. */
        float panelW = 920.0f * s, panelH = 560.0f * s;
        if (panelW > (float)winW * 0.95f) panelW = (float)winW * 0.95f;
        if (panelH > (float)winH * 0.95f) panelH = (float)winH * 0.95f;

        ImGui::SetNextWindowPos(ImVec2(((float)winW - panelW) * 0.5f, ((float)winH - panelH) * 0.5f));
        ImGui::SetNextWindowSize(ImVec2(panelW, panelH));
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::Begin("##KeySetupPanel", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);

        /* Top-right close X — same effect as Cancel in the form body. */
        if (WBUI::DrawPanelCloseX()) {
            result = 0;
            running = false;
        }

        /* Title */
        {
            ImGui::SetWindowFontScale(1.4f);
            const char *title = langGetText(STR_DLGKEYSETUP_TITLE);
            ImVec2 textSize = ImGui::CalcTextSize(title);
            ImGui::SetCursorPosX((panelW - textSize.x) * 0.5f);
            ImGui::Text("%s", title);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        int formRc = renderFormBody(/*cs=*/nullptr);
        if (formRc == 1)  { result = 1;  running = false; }
        if (formRc == -1) { result = 0;  running = false; }

        ImGui::End(); /* ##KeySetupPanel */
        ImGui::End(); /* ##KeySetupBg */

        dialogDrawNavOutline();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);

        /* Render background game with overlay */
        if (hasBg) {
            bgGameRender(bg, renderer, winW, winH);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 140);
            SDL_FRect overlayRect = { 0, 0, (float)winW, (float)winH };
            SDL_RenderFillRect(renderer, &overlayRect);
        }

        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
        dialogFrameCapEnd(frameCapStart);
    }

    dialogDismissKeyboard(window);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    dialogRestoreLogicalPresentation(renderer, savedLogW, savedLogH, savedLogMode);

#if !BOLO_MOBILE
    SDL_SetWindowResizable(window, true);
#endif

    SDL_FlushEvent(SDL_EVENT_QUIT);

    return result;
}

/* -------------------------------------------------------
 * In-game popup wrapper — same form, BeginPopupModal style
 * so it can sit inside the running game's ImGui context.
 *
 * sdl3ImguiShowKeySetup() (or the equivalent menu hook)
 * calls imguiKeySetupOpenInGame to flip the pending flag,
 * then each main-game render frame calls
 * imguiKeySetupRenderInGamePopup(cs) to draw / dismiss the
 * popup. State seeding happens once on the first render
 * frame after the trigger so we can read the live tank's
 * current values (not yet known at trigger time).
 *
 * Key capture is split across the event pump and renderForm.
 * sdl3ImguiProcessEvents intercepts SDL_EVENT_KEY_DOWN while
 * a row is in capture mode and routes it via
 * imguiKeySetupHandleInGameScancode — same pattern the
 * standalone dialog uses in its own event loop, just plumbed
 * through helper functions because we don't own the loop.
 * ------------------------------------------------------- */
static bool  s_inGameShowRequested = false;
static bool  s_inGameOpen          = false;
static float s_inGameFadeAlpha     = 0.0f;

extern "C" void imguiKeySetupOpenInGame(void) {
    /* Ignore re-triggers while the popup is already showing — calling
     * ImGui::OpenPopup() again on a non-consecutive frame makes ImGui
     * close and reopen the modal, which flickers and eats the click that
     * was meant for OK/Cancel/X. */
    if (s_inGameOpen) return;
    s_inGameShowRequested = true;
}

/* Forget a pending open and any armed row. Called by a host that owns its
 * own event loop as that loop ends: the flags below outlive the ImGui
 * context the popup was drawn in, and the next context to run reads them.
 * Keys and pad bindings are pushed out only when OK is pressed, and both
 * working copies are re-read on the next open, so this is a Cancel — it
 * drops nothing that was saved. Touches no ImGui state: it can be called
 * outside a frame, and after a context has been destroyed. */
extern "C" void imguiKeySetupCancelInGame(void) {
    s_inGameShowRequested = false;
    s_inGameOpen          = false;
    s_inGameFadeAlpha     = 0.0f;
    s_waiting             = ksNone;
    s_padWaitAction       = -1;
}

extern "C" void imguiKeySetupRenderInGamePopup(struct ClientSim *cs) {
    char title[128];
    snprintf(title, sizeof(title), "%s###keysetup",
             langGetText(STR_DLGKEYSETUP_TITLE));

    if (s_inGameShowRequested) {
        ImGui::OpenPopup(title);
        s_inGameShowRequested = false;
        s_inGameOpen = true;
        s_requestControllerTabDefault = true;
        windowGetKeys(&s_keys);
        inputGamepadBindingsGetAll(&s_pad);
        s_padWaitAction = -1;
        /* Seed checkboxes from the live tank so the dialog opens
         * showing what the tank currently has — this matches the
         * old in-game popup's behavior. */
        s_autoSlowdown = cs ? clientSimGetTankAutoSlowdown(cs) : useAutoslow;
        s_autoGunsight = cs ? clientSimGetTankAutoHideGunsight(cs) : useAutohide;
        s_waiting      = ksNone;
        s_pingWaitSlot = -1;
    }

    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    float sc = sdl3ImguiGetUiScale();
    float pw = 920.0f * sc, ph = 560.0f * sc;
    if (pw > io.DisplaySize.x * 0.95f) pw = io.DisplaySize.x * 0.95f;
    if (ph > io.DisplaySize.y * 0.95f) ph = io.DisplaySize.y * 0.95f;
    ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);

    /* s_inGameOpen doubles as the close-box (X) flag and the "already
     * showing" guard read by imguiKeySetupOpenInGame; reset it whenever the
     * modal is no longer open (X, Escape, or click-through) so a later
     * Ctrl+K can reopen it. */
    if (!ImGui::BeginPopupModal(title, &s_inGameOpen,
                                ImGuiWindowFlags_NoResize |
                                ImGuiWindowFlags_NoMove)) {
        s_inGameOpen = false;
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                        imguiPopupFadeAlpha(&s_inGameFadeAlpha));

    int rc = renderFormBody(cs);
    if (rc != 0) {
        ImGui::CloseCurrentPopup();
        s_inGameOpen = false;
    }

    ImGui::PopStyleVar();
    ImGui::EndPopup();
}

extern "C" bool imguiKeySetupIsCapturingInGameKey(void) {
    /* The ping rows capture keys too, so the event pump has to route a
       keystroke here while one of them is armed as well. */
    return s_waiting != ksNone || pingCapturing();
}

extern "C" void imguiKeySetupHandleInGameScancode(int scancode) {
    if (pingCapturing()) {
        if (scancode == SDL_SCANCODE_ESCAPE) {
            s_pingWaitSlot = -1;
        } else if (!isChordModifier(scancode)) {
            pingAssignCaptured(scancode);
        }
        return;
    }
    if (s_waiting == ksNone) return;
    if (scancode == SDL_SCANCODE_ESCAPE) {
        s_waiting = ksNone;
        return;
    }
    int *ptr = fieldPtr(s_waiting, &s_keys);
    if (ptr) *ptr = scancode;
    s_waiting = ksNone;
}

/* Mouse half of the in-game ping capture. Keys arrive through the scancode
 * hook above; a mouse button has no equivalent, so the event pump asks
 * whether a click belongs to the dialog and hands the button over if so. */
extern "C" bool imguiKeySetupIsCapturingInGamePing(void) {
    return pingCapturing();
}

extern "C" bool imguiKeySetupHandleInGamePingMouse(int sdlMouseButton) {
    return pingCaptureFromMouse(sdlMouseButton);
}

/* In-game controller-binding capture — mirror of the scancode hooks above.
 * sdl3ImguiProcessEvents routes gamepad button-down / trigger events here
 * while a controller row is armed. Escape cancels via the scancode path. */
extern "C" bool imguiKeySetupIsCapturingInGamePad(void) {
    return s_padWaitAction != -1;
}

extern "C" void imguiKeySetupHandleInGamePadButton(int sdlGamepadButton) {
    padAssignCaptured(GP_BIND_BUTTON, sdlGamepadButton);
}

extern "C" void imguiKeySetupHandleInGamePadTrigger(int sdlGamepadAxis) {
    padAssignCaptured(GP_BIND_TRIGGER, sdlGamepadAxis);
}

extern "C" void imguiKeySetupCancelInGamePad(void) {
    s_padWaitAction = -1;
}

/* -------------------------------------------------------
 * Embedded form — drawn inside a caller-owned ImGui context
 * (the first-run onboarding wizard) without the key-setup
 * dialog's own OK/Cancel footer. The caller seeds the shared
 * form state once via imguiKeySetupBeginEmbedded, draws the
 * binding rows each frame with imguiKeySetupRenderEmbedded,
 * and persists on exit via imguiKeySetupCommitEmbedded. Key
 * capture reuses the same imguiKeySetupIsCapturingInGameKey /
 * imguiKeySetupHandleInGameScancode hooks as the in-game
 * popup. Pre-game only — no live ClientSim to push onto.
 * ------------------------------------------------------- */
extern "C" void imguiKeySetupBeginEmbedded(void) {
    s_requestControllerTabDefault = true;
    windowGetKeys(&s_keys);
    s_autoSlowdown = useAutoslow;
    s_autoGunsight = useAutohide;
    s_waiting      = ksNone;
    s_pingWaitSlot = -1;
}

extern "C" void imguiKeySetupRenderEmbedded(float reserveBottom) {
    renderKeyRows(reserveBottom);
}

extern "C" void imguiKeySetupCommitEmbedded(void) {
    windowSetKeys(&s_keys);
    useAutoslow = s_autoSlowdown;
    useAutohide = s_autoGunsight;
    gameFrontPutPrefs(&s_keys);
}
