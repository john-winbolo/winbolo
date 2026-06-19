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
#include "../../ui_mode.h"     /* uiShouldUseControllerMode: arbitration */
#include "../../lang.h"        /* langGetText: localized key/legend captions */
}

/* Steam floating-keyboard hooks (declared rather than pulling in the steam
   header).  Stub/non-Steam builds return false / no-op. */
extern "C" bool steam_show_floating_keyboard(int x, int y, int w, int h);
extern "C" void steam_dismiss_floating_keyboard(void);
extern "C" bool steam_input_has_active_controller(void);

/* --- Layout data ------------------------------------------------------ */

typedef enum {
    OSK_K_CHAR = 0,
    OSK_K_BACKSPACE,
    OSK_K_ENTER,
    OSK_K_SPACE,
    OSK_K_SHIFT,
    OSK_K_MORE,
    OSK_K_SCRIPT
} OskKeyKind;

/* For OSK_K_CHAR, base/shift are both the caption and the emitted text
   (lowercase / shifted), and cp is the base codepoint (ASCII = the single
   char).  shiftCp is the explicit uppercase codepoint for non-ASCII keys; 0
   means "derive from shift[0]" (the ASCII case).  Special keys carry only a
   caption in base; cp / shiftCp are unused. */
typedef struct {
    OskKeyKind  kind;
    const char *base;
    const char *shift;
    uint32_t    cp;
    uint32_t    shiftCp;
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
    {OSK_K_CHAR,"q","Q",'q',0},{OSK_K_CHAR,"w","W",'w',0},{OSK_K_CHAR,"e","E",'e',0},
    {OSK_K_CHAR,"r","R",'r',0},{OSK_K_CHAR,"t","T",'t',0},{OSK_K_CHAR,"y","Y",'y',0},
    {OSK_K_CHAR,"u","U",'u',0},{OSK_K_CHAR,"i","I",'i',0},{OSK_K_CHAR,"o","O",'o',0},
    {OSK_K_CHAR,"p","P",'p',0}
};
static const OskKey kLettersR1[] = {
    {OSK_K_CHAR,"a","A",'a',0},{OSK_K_CHAR,"s","S",'s',0},{OSK_K_CHAR,"d","D",'d',0},
    {OSK_K_CHAR,"f","F",'f',0},{OSK_K_CHAR,"g","G",'g',0},{OSK_K_CHAR,"h","H",'h',0},
    {OSK_K_CHAR,"j","J",'j',0},{OSK_K_CHAR,"k","K",'k',0},{OSK_K_CHAR,"l","L",'l',0}
};
static const OskKey kLettersR2[] = {
    {OSK_K_SHIFT,"Shift","Shift",0,0},
    {OSK_K_CHAR,"z","Z",'z',0},{OSK_K_CHAR,"x","X",'x',0},{OSK_K_CHAR,"c","C",'c',0},
    {OSK_K_CHAR,"v","V",'v',0},{OSK_K_CHAR,"b","B",'b',0},{OSK_K_CHAR,"n","N",'n',0},
    {OSK_K_CHAR,"m","M",'m',0},
    {OSK_K_BACKSPACE,"Bksp","Bksp",0,0}
};
/* Russian ЙЦУКЕН letters (lower/upper codepoints).  For U+0430–044F the
   uppercase is cp − 0x20; ё is the U+0451→U+0401 exception. */
static const OskKey kCyrRuR0[] = {
    {OSK_K_CHAR,"й","Й",0x0439,0x0419},{OSK_K_CHAR,"ц","Ц",0x0446,0x0426},
    {OSK_K_CHAR,"у","У",0x0443,0x0423},{OSK_K_CHAR,"к","К",0x043A,0x041A},
    {OSK_K_CHAR,"е","Е",0x0435,0x0415},{OSK_K_CHAR,"н","Н",0x043D,0x041D},
    {OSK_K_CHAR,"г","Г",0x0433,0x0413},{OSK_K_CHAR,"ш","Ш",0x0448,0x0428},
    {OSK_K_CHAR,"щ","Щ",0x0449,0x0429},{OSK_K_CHAR,"з","З",0x0437,0x0417},
    {OSK_K_CHAR,"х","Х",0x0445,0x0425},{OSK_K_CHAR,"ъ","Ъ",0x044A,0x042A}
};
static const OskKey kCyrRuR1[] = {
    {OSK_K_CHAR,"ф","Ф",0x0444,0x0424},{OSK_K_CHAR,"ы","Ы",0x044B,0x042B},
    {OSK_K_CHAR,"в","В",0x0432,0x0412},{OSK_K_CHAR,"а","А",0x0430,0x0410},
    {OSK_K_CHAR,"п","П",0x043F,0x041F},{OSK_K_CHAR,"р","Р",0x0440,0x0420},
    {OSK_K_CHAR,"о","О",0x043E,0x041E},{OSK_K_CHAR,"л","Л",0x043B,0x041B},
    {OSK_K_CHAR,"д","Д",0x0434,0x0414},{OSK_K_CHAR,"ж","Ж",0x0436,0x0416},
    {OSK_K_CHAR,"э","Э",0x044D,0x042D},{OSK_K_CHAR,"ё","Ё",0x0451,0x0401}
};
static const OskKey kCyrRuR2[] = {
    {OSK_K_SHIFT,"Shift","Shift",0,0},
    {OSK_K_CHAR,"я","Я",0x044F,0x042F},{OSK_K_CHAR,"ч","Ч",0x0447,0x0427},
    {OSK_K_CHAR,"с","С",0x0441,0x0421},{OSK_K_CHAR,"м","М",0x043C,0x041C},
    {OSK_K_CHAR,"и","И",0x0438,0x0418},{OSK_K_CHAR,"т","Т",0x0442,0x0422},
    {OSK_K_CHAR,"ь","Ь",0x044C,0x042C},{OSK_K_CHAR,"б","Б",0x0431,0x0411},
    {OSK_K_CHAR,"ю","Ю",0x044E,0x042E},
    {OSK_K_BACKSPACE,"Bksp","Bksp",0,0}
};

/* Ukrainian ЙЦУКЕН: ru with ы→і, э→є, ъ→ї, ё dropped, ґ added. */
static const OskKey kCyrUkR0[] = {
    {OSK_K_CHAR,"й","Й",0x0439,0x0419},{OSK_K_CHAR,"ц","Ц",0x0446,0x0426},
    {OSK_K_CHAR,"у","У",0x0443,0x0423},{OSK_K_CHAR,"к","К",0x043A,0x041A},
    {OSK_K_CHAR,"е","Е",0x0435,0x0415},{OSK_K_CHAR,"н","Н",0x043D,0x041D},
    {OSK_K_CHAR,"г","Г",0x0433,0x0413},{OSK_K_CHAR,"ш","Ш",0x0448,0x0428},
    {OSK_K_CHAR,"щ","Щ",0x0449,0x0429},{OSK_K_CHAR,"з","З",0x0437,0x0417},
    {OSK_K_CHAR,"х","Х",0x0445,0x0425},{OSK_K_CHAR,"ї","Ї",0x0457,0x0407}
};
static const OskKey kCyrUkR1[] = {
    {OSK_K_CHAR,"ф","Ф",0x0444,0x0424},{OSK_K_CHAR,"і","І",0x0456,0x0406},
    {OSK_K_CHAR,"в","В",0x0432,0x0412},{OSK_K_CHAR,"а","А",0x0430,0x0410},
    {OSK_K_CHAR,"п","П",0x043F,0x041F},{OSK_K_CHAR,"р","Р",0x0440,0x0420},
    {OSK_K_CHAR,"о","О",0x043E,0x041E},{OSK_K_CHAR,"л","Л",0x043B,0x041B},
    {OSK_K_CHAR,"д","Д",0x0434,0x0414},{OSK_K_CHAR,"ж","Ж",0x0436,0x0416},
    {OSK_K_CHAR,"є","Є",0x0454,0x0404},{OSK_K_CHAR,"ґ","Ґ",0x0491,0x0490}
};
static const OskKey kCyrUkR2[] = {
    {OSK_K_SHIFT,"Shift","Shift",0,0},
    {OSK_K_CHAR,"я","Я",0x044F,0x042F},{OSK_K_CHAR,"ч","Ч",0x0447,0x0427},
    {OSK_K_CHAR,"с","С",0x0441,0x0421},{OSK_K_CHAR,"м","М",0x043C,0x041C},
    {OSK_K_CHAR,"и","И",0x0438,0x0418},{OSK_K_CHAR,"т","Т",0x0442,0x0422},
    {OSK_K_CHAR,"ь","Ь",0x044C,0x042C},{OSK_K_CHAR,"б","Б",0x0431,0x0411},
    {OSK_K_CHAR,"ю","Ю",0x044E,0x042E},
    {OSK_K_BACKSPACE,"Bksp","Bksp",0,0}
};

/* Symbols / punctuation page, reached via the MORE key. */
static const OskKey kSymbolsR0[] = {
    {OSK_K_CHAR,"1","1",'1',0},{OSK_K_CHAR,"2","2",'2',0},{OSK_K_CHAR,"3","3",'3',0},
    {OSK_K_CHAR,"4","4",'4',0},{OSK_K_CHAR,"5","5",'5',0},{OSK_K_CHAR,"6","6",'6',0},
    {OSK_K_CHAR,"7","7",'7',0},{OSK_K_CHAR,"8","8",'8',0},{OSK_K_CHAR,"9","9",'9',0},
    {OSK_K_CHAR,"0","0",'0',0}
};
static const OskKey kSymbolsR1[] = {
    {OSK_K_CHAR,"@","@",'@',0},{OSK_K_CHAR,"#","#",'#',0},{OSK_K_CHAR,"$","$",'$',0},
    {OSK_K_CHAR,"_","_",'_',0},{OSK_K_CHAR,"&","&",'&',0},{OSK_K_CHAR,"-","-",'-',0},
    {OSK_K_CHAR,"+","+",'+',0},{OSK_K_CHAR,"(","(",'(',0},{OSK_K_CHAR,")",")",')',0},
    {OSK_K_CHAR,"/","/",'/',0}
};
static const OskKey kSymbolsR2[] = {
    {OSK_K_CHAR,"*","*",'*',0},{OSK_K_CHAR,"\"","\"",'"',0},{OSK_K_CHAR,"'","'",'\'',0},
    {OSK_K_CHAR,":",":",':',0},{OSK_K_CHAR,";",";",';',0},{OSK_K_CHAR,"!","!",'!',0},
    {OSK_K_CHAR,"?","?",'?',0},
    {OSK_K_BACKSPACE,"Bksp","Bksp",0,0}
};
static const OskKey kSymbolsR3[] = {
    {OSK_K_MORE,"ABC","ABC",0,0},
    {OSK_K_SPACE,"Space","Space",' ',0},
    {OSK_K_ENTER,"Enter","Enter",0,0}
};

/* Compact numeric pad (no shift / no symbols page). */
static const OskKey kNumericR0[] = {
    {OSK_K_CHAR,"1","1",'1',0},{OSK_K_CHAR,"2","2",'2',0},{OSK_K_CHAR,"3","3",'3',0}
};
static const OskKey kNumericR1[] = {
    {OSK_K_CHAR,"4","4",'4',0},{OSK_K_CHAR,"5","5",'5',0},{OSK_K_CHAR,"6","6",'6',0}
};
static const OskKey kNumericR2[] = {
    {OSK_K_CHAR,"7","7",'7',0},{OSK_K_CHAR,"8","8",'8',0},{OSK_K_CHAR,"9","9",'9',0}
};
static const OskKey kNumericR3[] = {
    {OSK_K_CHAR,".",".",'.',0},{OSK_K_CHAR,"0","0",'0',0},{OSK_K_CHAR,"-","-",'-',0}
};
static const OskKey kNumericR4[] = {
    {OSK_K_BACKSPACE,"Bksp","Bksp",0,0},
    {OSK_K_ENTER,"Enter","Enter",0,0}
};

#define NELEMS(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Three-row letter sources (no control row — buildLettersPage appends it). */
static const OskLayout kLatinLetters = {
    {kLettersR0, kLettersR1, kLettersR2, nullptr, nullptr},
    {NELEMS(kLettersR0), NELEMS(kLettersR1), NELEMS(kLettersR2), 0, 0},
    3
};
static const OskLayout kCyrillicRu = {
    {kCyrRuR0, kCyrRuR1, kCyrRuR2, nullptr, nullptr},
    {NELEMS(kCyrRuR0), NELEMS(kCyrRuR1), NELEMS(kCyrRuR2), 0, 0},
    3
};
static const OskLayout kCyrillicUk = {
    {kCyrUkR0, kCyrUkR1, kCyrUkR2, nullptr, nullptr},
    {NELEMS(kCyrUkR0), NELEMS(kCyrUkR1), NELEMS(kCyrUkR2), 0, 0},
    3
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

/* --- Script (Latin / Cyrillic) ---------------------------------------- */

enum { SCRIPT_LATIN = 0, SCRIPT_CYRILLIC };

/* Languages whose letters page defaults to Cyrillic; the globe key then
   cycles to Latin (and back) so names/addresses can still be typed in ASCII.
   cyrLabel is the globe caption shown while the Latin page is up. */
typedef struct {
    const char      *lang;
    const OskLayout *layout;
    const char      *cyrLabel;
} OskScript;

static const OskScript kScripts[] = {
    { "ru", &kCyrillicRu, "РУ" },
    { "uk", &kCyrillicUk, "УК" },
};

static int              s_script      = SCRIPT_LATIN;
static bool             s_scriptCycle = false;   /* globe key offered */
static const OskLayout *s_cyrLayout   = NULL;     /* chosen ru/uk source */
static const char      *s_cyrLabel    = NULL;

/* ru/ru-* and uk/uk-* default to Cyrillic with the globe key enabled; every
   other code (including empty / English) stays Latin with no globe. */
static void resolveScript(const char *code) {
    s_script      = SCRIPT_LATIN;
    s_scriptCycle = false;
    s_cyrLayout   = NULL;
    s_cyrLabel    = NULL;
    if (!code || !code[0]) return;

    const OskScript *m = NULL;
    for (int i = 0; i < NELEMS(kScripts) && !m; i++)
        if (SDL_strcasecmp(code, kScripts[i].lang) == 0) m = &kScripts[i];
    if (!m) {
        int codeBase = langBaseLen(code);
        for (int i = 0; i < NELEMS(kScripts) && !m; i++) {
            int setBase = langBaseLen(kScripts[i].lang);
            if (codeBase == setBase &&
                SDL_strncasecmp(code, kScripts[i].lang, (size_t)codeBase) == 0)
                m = &kScripts[i];
        }
    }
    if (m) {
        s_script      = SCRIPT_CYRILLIC;
        s_scriptCycle = true;
        s_cyrLayout   = m->layout;
        s_cyrLabel    = m->cyrLabel;
    }
}

/* --- Assembled letters page ------------------------------------------- */

static OskLayout s_letters;
static OskKey    s_lettersBottom[4];   /* ?123, [globe], Space, Enter */
static char      s_globeCap[8];        /* "EN" / "РУ" / "УК" + NUL */

static void setKey(OskKey *k, OskKeyKind kind, const char *cap, uint32_t cp) {
    k->kind = kind; k->base = cap; k->shift = cap; k->cp = cp; k->shiftCp = 0;
}

/* Assemble s_letters: the active script's three letter rows plus a built
   control row (?123, the globe key for ru/uk, Space, Enter).  Rebuilt on open
   and on every globe toggle. */
static void buildLettersPage(void) {
    const OskLayout *src = (s_script == SCRIPT_CYRILLIC && s_cyrLayout)
                               ? s_cyrLayout : &kLatinLetters;
    for (int r = 0; r < 3; r++) {
        s_letters.rows[r]   = src->rows[r];
        s_letters.rowLen[r] = src->rowLen[r];
    }

    int n = 0;
    setKey(&s_lettersBottom[n++], OSK_K_MORE, "?123", 0);
    if (s_scriptCycle) {
        /* Caption names the script the globe switches TO. */
        const char *label = (s_script == SCRIPT_CYRILLIC)
                                ? "EN" : (s_cyrLabel ? s_cyrLabel : "");
        SDL_strlcpy(s_globeCap, label, sizeof(s_globeCap));
        setKey(&s_lettersBottom[n++], OSK_K_SCRIPT, s_globeCap, 0);
    }
    setKey(&s_lettersBottom[n++], OSK_K_SPACE, "Space", ' ');
    setKey(&s_lettersBottom[n++], OSK_K_ENTER, "Enter", 0);

    s_letters.rows[3]   = s_lettersBottom;
    s_letters.rowLen[3] = n;
    s_letters.numRows   = 4;
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
    return s_symbols ? &s_more : &s_letters;
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
            uint32_t cp;
            if (s_shift && lettersActive())
                cp = k->shiftCp ? k->shiftCp : (uint32_t)(unsigned char)k->shift[0];
            else
                cp = k->cp;
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
        case OSK_K_SCRIPT:
            if (s_scriptCycle) {
                s_script = (s_script == SCRIPT_LATIN) ? SCRIPT_CYRILLIC
                                                      : SCRIPT_LATIN;
                s_shift  = false;
                buildLettersPage();
                clampCursor(currentLayout());
            }
            break;
    }
}

static void doBackspace(void) {
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
    s_pendingUp = ImGuiKey_Backspace;
}

/* Caption shown on a keycap.  Word keys (Bksp/Shift/Space/Enter) are
   localized; character keys and the symbolic mode switches (?123/ABC, the
   ru/uk/EN globe) keep their table caption, which is already script-neutral. */
static const char *keycapLabel(const OskKey *k, const char *fallback) {
    switch (k->kind) {
        case OSK_K_BACKSPACE: return langGetText(STR_OSK_BKSP);
        case OSK_K_SHIFT:     return langGetText(STR_OSK_SHIFT);
        case OSK_K_SPACE:     return langGetText(STR_OSK_SPACE);
        case OSK_K_ENTER:     return langGetText(STR_OSK_ENTER);
        default:              return fallback;
    }
}

/* --- API -------------------------------------------------------------- */

void keyboardOpen(int mode) {
    s_open    = true;
    s_mode    = mode;
    s_shift   = false;
    s_symbols = false;
    char code[16];
    gameFrontGetLanguageCode(code, (int)sizeof(code));
    resolveScript(code);  /* pick Latin vs Cyrillic + whether the globe shows */
    buildSymbolsPage();   /* refresh accent rows for the current UI language */
    buildLettersPage();   /* assemble the letters page for the chosen script */
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
                cap = keycapLabel(k, cap);

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
        ImGui::TextUnformatted(langGetText(STR_OSK_LEGEND));
    }
    ImGui::End();
}

/* Pick the layout from the active field's flags: decimal or scientific fields
   get the numeric pad, password fields pick the password mode (the field still
   does its own masking), everything else — or a field whose state can't be
   read — gets single-line text. */
static int computeMode(void) {
    if (ImGuiInputTextState *st = ImGui::GetInputTextState(GImGui->ActiveId)) {
        if (st->Flags & (ImGuiInputTextFlags_CharsDecimal |
                         ImGuiInputTextFlags_CharsScientific))
            return OSK_MODE_NUMERIC;
        if (st->Flags & ImGuiInputTextFlags_Password)
            return OSK_MODE_PASSWORD;
    }
    return OSK_MODE_TEXT;
}

void keyboardUpdate(void) {
    /* Recompute the desired backend every frame so it survives Steam Input
       attach/detach and controller hot-unplug.  The backend is chosen by an
       explicit predicate (never the show() return value): Steam Input owning
       the pad → Steam's floating keyboard; otherwise (raw SDL pad, non-Steam
       launch, stub build) → ours. */
    enum { KB_NONE, KB_STEAM, KB_OURS };
    static int s_activeKb = KB_NONE;
    int desired = KB_NONE;
    if (uiShouldUseControllerMode() && ImGui::GetIO().WantTextInput)
        desired = steam_input_has_active_controller() ? KB_STEAM : KB_OURS;
    if (desired != s_activeKb) {
        if      (s_activeKb == KB_STEAM) steam_dismiss_floating_keyboard();
        else if (s_activeKb == KB_OURS)  keyboardClose();
        if (desired == KB_STEAM) {
            ImGuiViewport *vp = ImGui::GetMainViewport();
            steam_show_floating_keyboard((int)vp->Pos.x, (int)vp->Pos.y,
                                         (int)vp->Size.x, (int)vp->Size.y);
        } else if (desired == KB_OURS) {
            keyboardOpen(computeMode());
        }
        s_activeKb = desired;
    }
    keyboardRender();
}
