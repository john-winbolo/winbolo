/*
 * Copyright (c) 1998-2008 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 * Name:          imgui_keyboard.cpp
 * Purpose:       On-screen keyboard for controller text
 *                entry without Steam.  A keycap grid drawn
 *                in a NoNav / NoFocusOnAppearing window
 *                anchored to the bottom of the viewport, so
 *                the focused InputText stays active while we
 *                drive a keycap cursor ourselves from the
 *                raw SDL pad.  Activations are injected into
 *                the IO queue (AddInputCharacter for printable
 *                keys, a down+up AddKeyEvent for Backspace and
 *                Enter), which whatever field is active next
 *                frame consumes with no per-field wiring.
 *                Layouts (QWERTY base/shift, a symbols page,
 *                and a numeric pad) live in data tables.
 *********************************************************/

#include "imgui.h"
#include "imgui_internal.h"   /* ImGui::ClearActiveID */
#include "imgui_keyboard.h"
#include "../glyphs.h"

extern "C" {
#include "../input_gamepad.h"
}

/* --- Layout data ------------------------------------------------------ */

typedef enum {
    OSK_K_CHAR = 0,
    OSK_K_BACKSPACE,
    OSK_K_ENTER,
    OSK_K_SPACE,
    OSK_K_SHIFT,
    OSK_K_MORE
} OskKeyKind;

/* For OSK_K_CHAR, base/shift are both the caption and the emitted text
   (lowercase / shifted), and cp is the base codepoint (ASCII = the single
   char).  Special keys carry only a caption in base; cp is unused. */
typedef struct {
    OskKeyKind  kind;
    const char *base;
    const char *shift;
    uint32_t    cp;
} OskKey;

#define OSK_MAX_ROWS 5

typedef struct {
    const OskKey *rows[OSK_MAX_ROWS];
    int           rowLen[OSK_MAX_ROWS];
    int           numRows;
} OskLayout;

/* QWERTY letters (base = lowercase, shift = uppercase). */
static const OskKey kLettersR0[] = {
    {OSK_K_CHAR,"q","Q",'q'},{OSK_K_CHAR,"w","W",'w'},{OSK_K_CHAR,"e","E",'e'},
    {OSK_K_CHAR,"r","R",'r'},{OSK_K_CHAR,"t","T",'t'},{OSK_K_CHAR,"y","Y",'y'},
    {OSK_K_CHAR,"u","U",'u'},{OSK_K_CHAR,"i","I",'i'},{OSK_K_CHAR,"o","O",'o'},
    {OSK_K_CHAR,"p","P",'p'}
};
static const OskKey kLettersR1[] = {
    {OSK_K_CHAR,"a","A",'a'},{OSK_K_CHAR,"s","S",'s'},{OSK_K_CHAR,"d","D",'d'},
    {OSK_K_CHAR,"f","F",'f'},{OSK_K_CHAR,"g","G",'g'},{OSK_K_CHAR,"h","H",'h'},
    {OSK_K_CHAR,"j","J",'j'},{OSK_K_CHAR,"k","K",'k'},{OSK_K_CHAR,"l","L",'l'}
};
static const OskKey kLettersR2[] = {
    {OSK_K_SHIFT,"Shift","Shift",0},
    {OSK_K_CHAR,"z","Z",'z'},{OSK_K_CHAR,"x","X",'x'},{OSK_K_CHAR,"c","C",'c'},
    {OSK_K_CHAR,"v","V",'v'},{OSK_K_CHAR,"b","B",'b'},{OSK_K_CHAR,"n","N",'n'},
    {OSK_K_CHAR,"m","M",'m'},
    {OSK_K_BACKSPACE,"Bksp","Bksp",0}
};
static const OskKey kLettersR3[] = {
    {OSK_K_MORE,"?123","?123",0},
    {OSK_K_SPACE,"Space","Space",' '},
    {OSK_K_ENTER,"Enter","Enter",0}
};

/* Symbols / punctuation page, reached via the MORE key. */
static const OskKey kSymbolsR0[] = {
    {OSK_K_CHAR,"1","1",'1'},{OSK_K_CHAR,"2","2",'2'},{OSK_K_CHAR,"3","3",'3'},
    {OSK_K_CHAR,"4","4",'4'},{OSK_K_CHAR,"5","5",'5'},{OSK_K_CHAR,"6","6",'6'},
    {OSK_K_CHAR,"7","7",'7'},{OSK_K_CHAR,"8","8",'8'},{OSK_K_CHAR,"9","9",'9'},
    {OSK_K_CHAR,"0","0",'0'}
};
static const OskKey kSymbolsR1[] = {
    {OSK_K_CHAR,"@","@",'@'},{OSK_K_CHAR,"#","#",'#'},{OSK_K_CHAR,"$","$",'$'},
    {OSK_K_CHAR,"_","_",'_'},{OSK_K_CHAR,"&","&",'&'},{OSK_K_CHAR,"-","-",'-'},
    {OSK_K_CHAR,"+","+",'+'},{OSK_K_CHAR,"(","(",'('},{OSK_K_CHAR,")",")",')'},
    {OSK_K_CHAR,"/","/",'/'}
};
static const OskKey kSymbolsR2[] = {
    {OSK_K_CHAR,"*","*",'*'},{OSK_K_CHAR,"\"","\"",'"'},{OSK_K_CHAR,"'","'",'\''},
    {OSK_K_CHAR,":",":",':'},{OSK_K_CHAR,";",";",';'},{OSK_K_CHAR,"!","!",'!'},
    {OSK_K_CHAR,"?","?",'?'},
    {OSK_K_BACKSPACE,"Bksp","Bksp",0}
};
static const OskKey kSymbolsR3[] = {
    {OSK_K_MORE,"ABC","ABC",0},
    {OSK_K_SPACE,"Space","Space",' '},
    {OSK_K_ENTER,"Enter","Enter",0}
};

/* Compact numeric pad (no shift / no symbols page). */
static const OskKey kNumericR0[] = {
    {OSK_K_CHAR,"1","1",'1'},{OSK_K_CHAR,"2","2",'2'},{OSK_K_CHAR,"3","3",'3'}
};
static const OskKey kNumericR1[] = {
    {OSK_K_CHAR,"4","4",'4'},{OSK_K_CHAR,"5","5",'5'},{OSK_K_CHAR,"6","6",'6'}
};
static const OskKey kNumericR2[] = {
    {OSK_K_CHAR,"7","7",'7'},{OSK_K_CHAR,"8","8",'8'},{OSK_K_CHAR,"9","9",'9'}
};
static const OskKey kNumericR3[] = {
    {OSK_K_CHAR,".",".",'.'},{OSK_K_CHAR,"0","0",'0'},{OSK_K_CHAR,"-","-",'-'}
};
static const OskKey kNumericR4[] = {
    {OSK_K_BACKSPACE,"Bksp","Bksp",0},
    {OSK_K_ENTER,"Enter","Enter",0}
};

#define NELEMS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const OskLayout kLetters = {
    {kLettersR0, kLettersR1, kLettersR2, kLettersR3, nullptr},
    {NELEMS(kLettersR0), NELEMS(kLettersR1), NELEMS(kLettersR2), NELEMS(kLettersR3), 0},
    4
};
static const OskLayout kSymbols = {
    {kSymbolsR0, kSymbolsR1, kSymbolsR2, kSymbolsR3, nullptr},
    {NELEMS(kSymbolsR0), NELEMS(kSymbolsR1), NELEMS(kSymbolsR2), NELEMS(kSymbolsR3), 0},
    4
};
static const OskLayout kNumeric = {
    {kNumericR0, kNumericR1, kNumericR2, kNumericR3, kNumericR4},
    {NELEMS(kNumericR0), NELEMS(kNumericR1), NELEMS(kNumericR2), NELEMS(kNumericR3), NELEMS(kNumericR4)},
    5
};

/* --- State ------------------------------------------------------------ */

static bool s_open    = false;
static int  s_mode    = OSK_MODE_TEXT;
static bool s_shift   = false;   /* uppercase layer (letters only) */
static bool s_symbols = false;   /* symbols page (text mode only) */
static int  s_row     = 0;
static int  s_col     = 0;

/* Backspace / Enter inject a clean down on the activation frame and the
   matching up at the top of the next render.  One slot suffices because
   discrete actions are edge-latched to one per frame. */
static ImGuiKey s_pendingUp = ImGuiKey_None;

/* Edge-latch arming: each discrete action fires once per fresh press and
   re-arms on release (the latchedPress pattern from imgui_steam_nav.cpp).
   Direction movement is edge-only — no hold-to-repeat in v1. */
static bool s_armUp = true, s_armDown = true, s_armLeft = true, s_armRight = true;
static bool s_armA = true, s_armB = true, s_armLB = true, s_armRB = true, s_armStart = true;

static bool latched(bool pressed, bool *armed) {
    bool emit = pressed && *armed;
    *armed = !pressed;
    return emit;
}

static const OskLayout *currentLayout(void) {
    if (s_mode == OSK_MODE_NUMERIC) return &kNumeric;
    return s_symbols ? &kSymbols : &kLetters;
}

/* Shift only applies to the letters page. */
static bool lettersActive(void) {
    return s_mode != OSK_MODE_NUMERIC && !s_symbols;
}

static void clampCursor(const OskLayout *L) {
    if (s_row >= L->numRows) s_row = L->numRows - 1;
    if (s_row < 0)           s_row = 0;
    int len = L->rowLen[s_row];
    if (s_col >= len) s_col = len - 1;
    if (s_col < 0)    s_col = 0;
}

static void activateKey(const OskKey *k) {
    ImGuiIO &io = ImGui::GetIO();
    switch (k->kind) {
        case OSK_K_CHAR: {
            uint32_t cp = (s_shift && lettersActive())
                              ? (uint32_t)(unsigned char)k->shift[0]
                              : k->cp;
            io.AddInputCharacter(cp);
            break;
        }
        case OSK_K_SPACE:
            io.AddInputCharacter(' ');
            break;
        case OSK_K_BACKSPACE:
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            s_pendingUp = ImGuiKey_Backspace;
            break;
        case OSK_K_ENTER:
            io.AddKeyEvent(ImGuiKey_Enter, true);
            s_pendingUp = ImGuiKey_Enter;
            break;
        case OSK_K_SHIFT:
            s_shift = !s_shift;
            break;
        case OSK_K_MORE:
            s_symbols = !s_symbols;
            clampCursor(currentLayout());
            break;
    }
}

static void doBackspace(void) {
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
    s_pendingUp = ImGuiKey_Backspace;
}

/* --- API -------------------------------------------------------------- */

void keyboardOpen(int mode) {
    s_open    = true;
    s_mode    = mode;
    s_shift   = false;
    s_symbols = false;
    s_row     = 0;
    s_col     = 0;
    s_armUp = s_armDown = s_armLeft = s_armRight = true;
    s_armA = s_armB = s_armLB = s_armRB = s_armStart = true;
}

void keyboardClose(void) {
    s_open = false;
}

bool keyboardIsOpen(void) {
    return s_open;
}

void keyboardRender(void) {
    ImGuiIO &io = ImGui::GetIO();

    /* Release any key whose down was injected last frame. */
    if (s_pendingUp != ImGuiKey_None) {
        io.AddKeyEvent(s_pendingUp, false);
        s_pendingUp = ImGuiKey_None;
    }

    if (!s_open) return;

    /* Manual gamepad cursor — read the raw SDL pad directly (Path B style),
       not ImGui nav, so the focused field stays active.  No handle (Steam
       Input owns the pad, or no pad) → skip the interactive cursor this
       frame; the mouse path below still works. */
    SDL_Gamepad *gp = inputGamepadGetActiveHandle();
    if (gp) {
        const float DEAD = 0.5f;
        float ax = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float ay = (float)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        bool up    = ay < -DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool down  = ay >  DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        bool left  = ax < -DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        bool right = ax >  DEAD || SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

        const OskLayout *L = currentLayout();
        if (latched(up,    &s_armUp))    { if (s_row > 0)              s_row--; clampCursor(L); }
        if (latched(down,  &s_armDown))  { if (s_row < L->numRows - 1) s_row++; clampCursor(L); }
        if (latched(left,  &s_armLeft))  { if (s_col > 0)              s_col--; }
        if (latched(right, &s_armRight)) { if (s_col < L->rowLen[s_row] - 1) s_col++; }

        if (latched(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH), &s_armA))
            activateKey(&L->rows[s_row][s_col]);
        if (latched(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST), &s_armB))
            doBackspace();
        if (latched(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), &s_armLB)) {
            if (lettersActive()) s_shift = !s_shift;
        }
        if (latched(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), &s_armRB)) {
            if (s_mode != OSK_MODE_NUMERIC) {
                s_symbols = !s_symbols;
                clampCursor(currentLayout());
            }
        }
        if (latched(SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START), &s_armStart)) {
            /* Done: drop the field's active state so WantTextInput clears
               and the per-frame state machine tears us down — closing only
               the window would let it reopen next frame. */
            ImGui::ClearActiveID();
            keyboardClose();
        }
    }

    if (!s_open) return;   /* Start may have closed us above */

    const OskLayout *L = currentLayout();
    clampCursor(L);

    ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("###osk", nullptr, flags)) {
        const float u = ImGui::GetFontSize() * 2.2f;   /* keycap cell */
        const ImVec2 charSz(u, u);

        ImVec2 selMin(0, 0), selMax(0, 0);
        bool   haveSel = false;

        for (int r = 0; r < L->numRows; r++) {
            for (int c = 0; c < L->rowLen[r]; c++) {
                const OskKey *k = &L->rows[r][c];
                if (c > 0) ImGui::SameLine();

                const char *cap =
                    (k->kind == OSK_K_CHAR && s_shift && lettersActive())
                        ? k->shift : k->base;

                ImVec2 sz;
                if (k->kind == OSK_K_CHAR)        sz = charSz;
                else if (k->kind == OSK_K_SPACE)  sz = ImVec2(u * 3.0f, u);
                else                              sz = ImVec2(0.0f, u);

                ImGui::PushID(r * 100 + c);
                /* Mouse-click is a best-effort parallel path; clicking a
                   keycap may steal focus from the field (known v1 limit).
                   The gamepad path injects directly, never through here. */
                if (ImGui::Button(cap, sz))
                    activateKey(k);
                ImGui::PopID();

                if (r == s_row && c == s_col) {
                    selMin  = ImGui::GetItemRectMin();
                    selMax  = ImGui::GetItemRectMax();
                    haveSel = true;
                }
            }
        }

        /* Our own cursor highlight — we don't use ImGui's nav highlight. */
        if (haveSel) {
            ImGui::GetWindowDrawList()->AddRect(
                selMin, selMax, IM_COL32(255, 210, 0, 255),
                ImGui::GetStyle().FrameRounding, 0, 3.0f);
        }

        /* Control legend.  The A/activate glyph resolves via the Xelu atlas;
           the rest have no glyph, so they use text captions. */
        SDL_Texture *aGlyph = glyphForActionAuto("dismiss");
        if (aGlyph) {
            const float h = ImGui::GetFrameHeight();
            ImGui::Image((ImTextureID)aGlyph, ImVec2(h, h));
            ImGui::SameLine();
        }
        ImGui::TextUnformatted("Select   B Backspace   LB Shift   RB ?123   Start Done");
    }
    ImGui::End();
}
