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
#include "../../gamefront.h"   /* gameFrontGetLanguageCode: accent set */
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

/* Max grid rows across all layouts.  The text-mode symbols page is the
   tallest: three fixed symbol rows + up to three appended accent rows + the
   control row. */
#define OSK_MAX_ROWS 8

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
static const OskLayout kNumeric = {
    {kNumericR0, kNumericR1, kNumericR2, kNumericR3, kNumericR4},
    {NELEMS(kNumericR0), NELEMS(kNumericR1), NELEMS(kNumericR2), NELEMS(kNumericR3), NELEMS(kNumericR4)},
    5
};

/* --- Per-language accents --------------------------------------------- */

/* Lowercase accent codepoints offered on the symbols page, keyed by UI
   language code (the lowercased BCP-47 string from gameFrontGetLanguageCode).
   English and any unlisted code get no accents. */
typedef struct {
    const char     *lang;
    const uint32_t *cps;
    int             count;
} OskAccentSet;

static const uint32_t kAccentEs[] = {
    0x00E1,0x00E9,0x00ED,0x00F3,0x00FA,0x00FC,0x00F1,0x00BF,0x00A1 };          /* á é í ó ú ü ñ ¿ ¡ */
static const uint32_t kAccentFr[] = {
    0x00E0,0x00E2,0x00E6,0x00E7,0x00E9,0x00E8,0x00EA,0x00EB,0x00EE,0x00EF,
    0x00F4,0x0153,0x00F9,0x00FB,0x00FC,0x00FF };                               /* à â æ ç é è ê ë î ï ô œ ù û ü ÿ */
static const uint32_t kAccentIt[] = {
    0x00E0,0x00E8,0x00E9,0x00EC,0x00ED,0x00EE,0x00F2,0x00F3,0x00F9 };          /* à è é ì í î ò ó ù */
static const uint32_t kAccentDe[] = {
    0x00E4,0x00F6,0x00FC,0x00DF };                                            /* ä ö ü ß */
static const uint32_t kAccentNl[] = {
    0x00E1,0x00E9,0x00ED,0x00F3,0x00FA,0x00E8,0x00EB,0x00EF,0x00F6,0x00FC };   /* á é í ó ú è ë ï ö ü */
static const uint32_t kAccentPl[] = {
    0x0105,0x0107,0x0119,0x0142,0x0144,0x00F3,0x015B,0x017A,0x017C };          /* ą ć ę ł ń ó ś ź ż */
static const uint32_t kAccentPtBr[] = {
    0x00E1,0x00E0,0x00E2,0x00E3,0x00E7,0x00E9,0x00EA,0x00ED,0x00F3,0x00F4,
    0x00F5,0x00FA,0x00FC };                                                    /* á à â ã ç é ê í ó ô õ ú ü */
static const uint32_t kAccentSv[] = {
    0x00E5,0x00E4,0x00F6 };                                                    /* å ä ö */
static const uint32_t kAccentTr[] = {
    0x00E7,0x011F,0x0131,0x00F6,0x015F,0x00FC };                              /* ç ğ ı ö ş ü */
static const uint32_t kAccentCs[] = {
    0x00E1,0x010D,0x010F,0x00E9,0x011B,0x00ED,0x0148,0x00F3,0x0159,0x0161,
    0x0165,0x00FA,0x016F,0x00FD,0x017E };                                      /* á č ď é ě í ň ó ř š ť ú ů ý ž */

static const OskAccentSet kAccentSets[] = {
    { "es",    kAccentEs,   NELEMS(kAccentEs)   },
    { "fr",    kAccentFr,   NELEMS(kAccentFr)   },
    { "it",    kAccentIt,   NELEMS(kAccentIt)   },
    { "de",    kAccentDe,   NELEMS(kAccentDe)   },
    { "nl",    kAccentNl,   NELEMS(kAccentNl)   },
    { "pl",    kAccentPl,   NELEMS(kAccentPl)   },
    { "pt-br", kAccentPtBr, NELEMS(kAccentPtBr) },
    { "sv",    kAccentSv,   NELEMS(kAccentSv)   },
    { "tr",    kAccentTr,   NELEMS(kAccentTr)   },
    { "cs",    kAccentCs,   NELEMS(kAccentCs)   },
};

/* Length of the script-tag prefix before the first '-' (whole string if no
   '-').  Used so a region-qualified code matches a bare-language entry and a
   bare code matches a region-qualified entry. */
static int langBaseLen(const char *s) {
    const char *dash = SDL_strchr(s, '-');
    return dash ? (int)(dash - s) : (int)SDL_strlen(s);
}

/* Resolve a language code to an accent set: exact (case-insensitive) match
   first, then a base-tag match (prefix before '-' on both sides).  No match,
   empty code, or English → empty set. */
static void resolveAccents(const char *code, const uint32_t **outCps, int *outN) {
    *outCps = NULL;
    *outN   = 0;
    if (!code || !code[0]) return;
    for (int i = 0; i < NELEMS(kAccentSets); i++) {
        if (SDL_strcasecmp(code, kAccentSets[i].lang) == 0) {
            *outCps = kAccentSets[i].cps;
            *outN   = kAccentSets[i].count;
            return;
        }
    }
    int codeBase = langBaseLen(code);
    for (int i = 0; i < NELEMS(kAccentSets); i++) {
        int setBase = langBaseLen(kAccentSets[i].lang);
        if (codeBase == setBase &&
            SDL_strncasecmp(code, kAccentSets[i].lang, (size_t)codeBase) == 0) {
            *outCps = kAccentSets[i].cps;
            *outN   = kAccentSets[i].count;
            return;
        }
    }
}

/* --- Assembled symbols page ------------------------------------------- */

#define OSK_MAX_ACCENTS    30   /* up to 3 rows of 10 */
#define OSK_ACCENTS_PER_ROW 10

/* The symbols page is rebuilt at open time so the accent rows can vary with
   the UI language.  The accent keycaps and their UTF-8 captions live in
   persistent storage that the layout's row pointers reference. */
static OskKey   s_accentKeys[OSK_MAX_ACCENTS];
static char     s_accentCaps[OSK_MAX_ACCENTS][5];   /* UTF-8 + NUL */
static OskLayout s_more;

static void utf8Encode(uint32_t cp, char out[5]) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        out[4] = 0;
    }
}

/* Assemble s_more: the fixed symbol rows, then the current language's accent
   rows (≤10 keys each, ≤3 rows), then the control row.  An empty accent set
   leaves just the original symbol page. */
static void buildSymbolsPage(void) {
    char code[16];
    gameFrontGetLanguageCode(code, (int)sizeof(code));
    const uint32_t *cps = NULL;
    int n = 0;
    resolveAccents(code, &cps, &n);
    if (n > OSK_MAX_ACCENTS) n = OSK_MAX_ACCENTS;

    int row = 0;
    s_more.rows[row] = kSymbolsR0; s_more.rowLen[row] = NELEMS(kSymbolsR0); row++;
    s_more.rows[row] = kSymbolsR1; s_more.rowLen[row] = NELEMS(kSymbolsR1); row++;
    s_more.rows[row] = kSymbolsR2; s_more.rowLen[row] = NELEMS(kSymbolsR2); row++;

    int idx = 0;
    while (idx < n) {
        int rowCount = n - idx;
        if (rowCount > OSK_ACCENTS_PER_ROW) rowCount = OSK_ACCENTS_PER_ROW;
        for (int c = 0; c < rowCount; c++) {
            uint32_t cp = cps[idx + c];
            utf8Encode(cp, s_accentCaps[idx + c]);
            s_accentKeys[idx + c].kind  = OSK_K_CHAR;
            s_accentKeys[idx + c].base  = s_accentCaps[idx + c];
            s_accentKeys[idx + c].shift = s_accentCaps[idx + c];
            s_accentKeys[idx + c].cp    = cp;
        }
        s_more.rows[row]   = &s_accentKeys[idx];
        s_more.rowLen[row] = rowCount;
        row++;
        idx += rowCount;
    }

    s_more.rows[row] = kSymbolsR3; s_more.rowLen[row] = NELEMS(kSymbolsR3); row++;
    s_more.numRows = row;
}

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
    return s_symbols ? &s_more : &kLetters;
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
    buildSymbolsPage();   /* refresh accent rows for the current UI language */
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
