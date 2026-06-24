/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * glyphs.c — Path A (Steam Input) and Path B (Xelu atlas) glyph
 * lookups.
 *
 * Path A: pointer-equality cache keyed on the const char * Steam
 * returns for a given binding.  Steam returns the same pointer for
 * the same binding across calls so equality is sufficient.
 *
 * Path B: filename-keyed cache.  The keyboardmouse + xbox + ps5 +
 * switch atlases together contain ~120 PNGs but a single tutorial
 * run touches at most a couple of dozen, so the cache is sized
 * generously and never evicts during a session.  Cache misses are
 * silent — callers (tutorial renderer) handle NULL by drawing a
 * procedural fallback.
 *
 * PNG decode mirrors the crosshair load at sdl3draw.c:951-982:
 * SDL_IOFromFile -> SDL_GetIOSize -> SDL_ReadIO -> stbi_load_from_memory
 * -> SDL_CreateSurfaceFrom -> SDL_CreateTextureFromSurface.
 */

#include "glyphs.h"

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "stb_image.h"
#include "input_gamepad.h"
#include "../../steam/steam_wrapper.h"

#define GLYPH_CACHE_MAX 64
#define PATH_B_CACHE_MAX 128

typedef struct {
  const char  *path;
  SDL_Texture *tex;
} GlyphCacheEntry;

typedef struct {
  char         filename[64];   /* relative to data/controller/ */
  SDL_Texture *tex;
} PathBEntry;

static SDL_Renderer    *s_renderer = NULL;
static GlyphCacheEntry  s_cache[GLYPH_CACHE_MAX];
static int              s_cache_count = 0;

static PathBEntry       s_pb_cache[PATH_B_CACHE_MAX];
static int              s_pb_cache_count = 0;

static GamepadGlyphSet  s_active_set = GAMEPAD_GLYPH_SET_XBOX;

bool glyphsInit(SDL_Renderer *renderer) {
  s_renderer = renderer;
  s_cache_count = 0;
  for (int i = 0; i < GLYPH_CACHE_MAX; ++i) {
    s_cache[i].path = NULL;
    s_cache[i].tex  = NULL;
  }
  s_pb_cache_count = 0;
  for (int i = 0; i < PATH_B_CACHE_MAX; ++i) {
    s_pb_cache[i].filename[0] = '\0';
    s_pb_cache[i].tex = NULL;
  }
  s_active_set = GAMEPAD_GLYPH_SET_XBOX;
  return true;
}

void glyphsShutdown(void) {
  for (int i = 0; i < s_cache_count; ++i) {
    if (s_cache[i].tex) {
      SDL_DestroyTexture(s_cache[i].tex);
    }
    s_cache[i].path = NULL;
    s_cache[i].tex  = NULL;
  }
  s_cache_count = 0;
  for (int i = 0; i < s_pb_cache_count; ++i) {
    if (s_pb_cache[i].tex) {
      SDL_DestroyTexture(s_pb_cache[i].tex);
    }
    s_pb_cache[i].filename[0] = '\0';
    s_pb_cache[i].tex = NULL;
  }
  s_pb_cache_count = 0;
  s_renderer    = NULL;
}

static SDL_Texture *load_glyph_png(const char *path) {
  SDL_IOStream *io = SDL_IOFromFile(path, "rb");
  if (!io) return NULL;

  Sint64 sz = SDL_GetIOSize(io);
  if (sz <= 0) { SDL_CloseIO(io); return NULL; }

  unsigned char *buf = (unsigned char *)SDL_malloc((size_t)sz);
  if (!buf) { SDL_CloseIO(io); return NULL; }
  SDL_ReadIO(io, buf, (size_t)sz);
  SDL_CloseIO(io);

  int imgW = 0, imgH = 0, ch = 0;
  unsigned char *pix = stbi_load_from_memory(buf, (int)sz, &imgW, &imgH, &ch, 4);
  SDL_free(buf);
  if (!pix) return NULL;

  SDL_Surface *surf = SDL_CreateSurfaceFrom(imgW, imgH, SDL_PIXELFORMAT_RGBA32,
                                            pix, imgW * 4);
  if (!surf) { stbi_image_free(pix); return NULL; }

  SDL_Texture *tex = SDL_CreateTextureFromSurface(s_renderer, surf);
  SDL_DestroySurface(surf);
  stbi_image_free(pix);

  if (tex) {
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
  }
  return tex;
}

SDL_Texture *glyphForAction(const char *action_name) {
  if (!s_renderer || !action_name) return NULL;

  const char *path = steam_input_get_glyph_path(action_name);
  if (!path) return NULL;

  /* Pointer-equality cache lookup — Steam returns the same const char *
     for the same binding across calls. */
  for (int i = 0; i < s_cache_count; ++i) {
    if (s_cache[i].path == path) return s_cache[i].tex;
  }

  if (s_cache_count >= GLYPH_CACHE_MAX) {
    fprintf(stderr, "glyphs: cache full (%d entries), dropping '%s'\n",
            GLYPH_CACHE_MAX, action_name);
    return NULL;
  }

  SDL_Texture *tex = load_glyph_png(path);
  s_cache[s_cache_count].path = path;
  s_cache[s_cache_count].tex  = tex;
  ++s_cache_count;
  return tex;
}

/* --- Path B --- */

GamepadGlyphSet selectGamepadGlyphSet(SDL_Gamepad *active) {
  SDL_GamepadType t = SDL_GAMEPAD_TYPE_UNKNOWN;
  if (active) {
    t = SDL_GetGamepadType(active);
  } else {
    /* Fall back to the input_gamepad-tracked active type — same
       answer the gamepad module would give a frame after this. */
    t = inputGamepadGetActiveType();
  }
  switch (t) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return GAMEPAD_GLYPH_SET_PS5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return GAMEPAD_GLYPH_SET_SWITCH;
    default:
      return GAMEPAD_GLYPH_SET_XBOX;
  }
}

/* Re-evaluate the active set on connect/disconnect.  Called from
   inputGamepadProcessEvent via the wrapper below — see header. */
static void glyphsRefreshActiveSet(void) {
  s_active_set = selectGamepadGlyphSet(inputGamepadGetActiveHandle());
}

static SDL_Texture *load_pathb(const char *subdir, const char *filename) {
  if (!s_renderer || !subdir || !filename) return NULL;

  /* Look up by filename only — same PNG name from any subdir is a
     rare collision; in our atlases the prefix differs (Switch_*,
     PS5_*, XboxSeriesX_*, *_Key_Light) so collisions cannot occur. */
  for (int i = 0; i < s_pb_cache_count; ++i) {
    if (strcmp(s_pb_cache[i].filename, filename) == 0) {
      return s_pb_cache[i].tex;
    }
  }
  if (s_pb_cache_count >= PATH_B_CACHE_MAX) {
    fprintf(stderr, "glyphs: pathB cache full, dropping '%s'\n", filename);
    return NULL;
  }

  char path[1024];
  const char *base = SDL_GetBasePath();
  if (!base) base = "./";
  SDL_snprintf(path, sizeof(path), "%sdata/controller/%s/%s",
               base, subdir, filename);

  SDL_Texture *tex = load_glyph_png(path);

  PathBEntry *e = &s_pb_cache[s_pb_cache_count++];
  SDL_strlcpy(e->filename, filename, sizeof(e->filename));
  e->tex = tex;
  return tex;
}

/* --- Keyboard scancode -> PNG filename table ---
 *
 * Filenames follow Xelu's keyboardmouse pack convention:
 * <Name>_Key_Light.png.  Letter keys, digits, function keys,
 * modifiers, and the cursor cluster cover every default Bolo
 * binding.  Unmapped scancodes return NULL; the renderer draws a
 * procedural keycap with the SDL_GetScancodeName() label. */

typedef struct {
  SDL_Scancode sc;
  const char  *file;
} ScancodeMap;

static const ScancodeMap kKbMap[] = {
  /* Letters */
  { SDL_SCANCODE_A, "A_Key_Light.png" },
  { SDL_SCANCODE_B, "B_Key_Light.png" },
  { SDL_SCANCODE_C, "C_Key_Light.png" },
  { SDL_SCANCODE_D, "D_Key_Light.png" },
  { SDL_SCANCODE_E, "E_Key_Light.png" },
  { SDL_SCANCODE_F, "F_Key_Light.png" },
  { SDL_SCANCODE_G, "G_Key_Light.png" },
  { SDL_SCANCODE_H, "H_Key_Light.png" },
  { SDL_SCANCODE_I, "I_Key_Light.png" },
  { SDL_SCANCODE_J, "J_Key_Light.png" },
  { SDL_SCANCODE_K, "K_Key_Light.png" },
  { SDL_SCANCODE_L, "L_Key_Light.png" },
  { SDL_SCANCODE_M, "M_Key_Light.png" },
  { SDL_SCANCODE_N, "N_Key_Light.png" },
  { SDL_SCANCODE_O, "O_Key_Light.png" },
  { SDL_SCANCODE_P, "P_Key_Light.png" },
  { SDL_SCANCODE_Q, "Q_Key_Light.png" },
  { SDL_SCANCODE_R, "R_Key_Light.png" },
  { SDL_SCANCODE_S, "S_Key_Light.png" },
  { SDL_SCANCODE_T, "T_Key_Light.png" },
  { SDL_SCANCODE_U, "U_Key_Light.png" },
  { SDL_SCANCODE_V, "V_Key_Light.png" },
  { SDL_SCANCODE_W, "W_Key_Light.png" },
  { SDL_SCANCODE_X, "X_Key_Light.png" },
  { SDL_SCANCODE_Y, "Y_Key_Light.png" },
  { SDL_SCANCODE_Z, "Z_Key_Light.png" },
  /* Top-row digits — Xelu pack ships 0_Key_Light.png .. 9_Key_Light.png */
  { SDL_SCANCODE_0, "0_Key_Light.png" },
  { SDL_SCANCODE_1, "1_Key_Light.png" },
  { SDL_SCANCODE_2, "2_Key_Light.png" },
  { SDL_SCANCODE_3, "3_Key_Light.png" },
  { SDL_SCANCODE_4, "4_Key_Light.png" },
  { SDL_SCANCODE_5, "5_Key_Light.png" },
  { SDL_SCANCODE_6, "6_Key_Light.png" },
  { SDL_SCANCODE_7, "7_Key_Light.png" },
  { SDL_SCANCODE_8, "8_Key_Light.png" },
  { SDL_SCANCODE_9, "9_Key_Light.png" },
  /* Function row */
  { SDL_SCANCODE_F1,  "F1_Key_Light.png"  },
  { SDL_SCANCODE_F2,  "F2_Key_Light.png"  },
  { SDL_SCANCODE_F3,  "F3_Key_Light.png"  },
  { SDL_SCANCODE_F4,  "F4_Key_Light.png"  },
  { SDL_SCANCODE_F5,  "F5_Key_Light.png"  },
  { SDL_SCANCODE_F6,  "F6_Key_Light.png"  },
  { SDL_SCANCODE_F7,  "F7_Key_Light.png"  },
  { SDL_SCANCODE_F8,  "F8_Key_Light.png"  },
  { SDL_SCANCODE_F9,  "F9_Key_Light.png"  },
  { SDL_SCANCODE_F10, "F10_Key_Light.png" },
  { SDL_SCANCODE_F11, "F11_Key_Light.png" },
  { SDL_SCANCODE_F12, "F12_Key_Light.png" },
  /* Modifiers + control cluster */
  { SDL_SCANCODE_RETURN,    "Enter_Key_Light.png"        },
  { SDL_SCANCODE_KP_ENTER,  "Enter_Key_Light.png"        },
  { SDL_SCANCODE_ESCAPE,    "Esc_Key_Light.png"          },
  { SDL_SCANCODE_BACKSPACE, "Backspace_Key_Light.png"    },
  { SDL_SCANCODE_TAB,       "Tab_Key_Light.png"          },
  { SDL_SCANCODE_SPACE,     "Space_Key_Light.png"        },
  { SDL_SCANCODE_LCTRL,     "Ctrl_Key_Light.png"         },
  { SDL_SCANCODE_RCTRL,     "Ctrl_Key_Light.png"         },
  { SDL_SCANCODE_LSHIFT,    "Shift_Key_Light.png"        },
  { SDL_SCANCODE_RSHIFT,    "Shift_Key_Light.png"        },
  { SDL_SCANCODE_LALT,      "Alt_Key_Light.png"          },
  { SDL_SCANCODE_RALT,      "Alt_Key_Light.png"          },
  { SDL_SCANCODE_LGUI,      "Win_Key_Light.png"          },
  { SDL_SCANCODE_RGUI,      "Win_Key_Light.png"          },
  { SDL_SCANCODE_CAPSLOCK,  "Caps_Lock_Key_Light.png"    },
  { SDL_SCANCODE_NUMLOCKCLEAR, "Num_Lock_Key_Light.png"  },
  { SDL_SCANCODE_PRINTSCREEN, "Print_Screen_Key_Light.png" },
  { SDL_SCANCODE_INSERT,    "Insert_Key_Light.png"       },
  { SDL_SCANCODE_DELETE,    "Del_Key_Light.png"          },
  { SDL_SCANCODE_HOME,      "Home_Key_Light.png"         },
  { SDL_SCANCODE_END,       "End_Key_Light.png"          },
  { SDL_SCANCODE_PAGEUP,    "Page_Up_Key_Light.png"      },
  { SDL_SCANCODE_PAGEDOWN,  "Page_Down_Key_Light.png"    },
  /* Cursor arrows */
  { SDL_SCANCODE_UP,    "Arrow_Up_Key_Light.png"    },
  { SDL_SCANCODE_DOWN,  "Arrow_Down_Key_Light.png"  },
  { SDL_SCANCODE_LEFT,  "Arrow_Left_Key_Light.png"  },
  { SDL_SCANCODE_RIGHT, "Arrow_Right_Key_Light.png" },
  /* Punctuation that the Xelu pack ships explicit caps for */
  { SDL_SCANCODE_MINUS,        "Minus_Key_Light.png"     },
  { SDL_SCANCODE_EQUALS,       "Plus_Key_Light.png"      },
  { SDL_SCANCODE_LEFTBRACKET,  "Bracket_Left_Key_Light.png"  },
  { SDL_SCANCODE_RIGHTBRACKET, "Bracket_Right_Key_Light.png" },
  { SDL_SCANCODE_SEMICOLON,    "Semicolon_Key_Light.png" },
  { SDL_SCANCODE_APOSTROPHE,   "Quote_Key_Light.png"     },
  { SDL_SCANCODE_GRAVE,        "Tilda_Key_Light.png"     },
  { SDL_SCANCODE_SLASH,        "Slash_Key_Light.png"     },
  /* Keypad — default scroll bindings live here.  Xelu ships only
     digit caps (no "Keypad 4" art), so we reuse the plain digit
     PNGs.  Visually identical numerals; the cap art doesn't depend
     on whether the digit was pressed on the main row or the pad. */
  { SDL_SCANCODE_KP_0, "0_Key_Light.png" },
  { SDL_SCANCODE_KP_1, "1_Key_Light.png" },
  { SDL_SCANCODE_KP_2, "2_Key_Light.png" },
  { SDL_SCANCODE_KP_3, "3_Key_Light.png" },
  { SDL_SCANCODE_KP_4, "4_Key_Light.png" },
  { SDL_SCANCODE_KP_5, "5_Key_Light.png" },
  { SDL_SCANCODE_KP_6, "6_Key_Light.png" },
  { SDL_SCANCODE_KP_7, "7_Key_Light.png" },
  { SDL_SCANCODE_KP_8, "8_Key_Light.png" },
  { SDL_SCANCODE_KP_9, "9_Key_Light.png" },
  { SDL_SCANCODE_KP_PLUS,  "Plus_Key_Light.png"   },
  { SDL_SCANCODE_KP_MINUS, "Minus_Key_Light.png"  },
};
static const int kKbMapCount = (int)(sizeof(kKbMap) / sizeof(kKbMap[0]));

SDL_Texture *glyphForKeyboardScancode(SDL_Scancode sc) {
  if (sc <= 0) return NULL;
  for (int i = 0; i < kKbMapCount; ++i) {
    if (kKbMap[i].sc == sc) return load_pathb("keyboardmouse", kKbMap[i].file);
  }
  return NULL;
}

/* --- Pseudo-action -> per-set PNG table ---
 *
 * Pseudo-actions are namespaced "glyph_*" so they cannot be
 * confused with Steam Input manifest entries.  Each row resolves to
 * one filename per controller set.  Analog directional tokens
 * resolve to whole-stick glyphs in every set, including Switch —
 * Switch ships per-direction art (Switch_Up.png etc.) but we
 * deliberately reach for the stick glyph instead so the tutorial
 * UX stays uniform across vendors. */

typedef struct {
  const char *action;
  const char *xbox;
  const char *ps5;
  const char *sw;
} GamepadActionRow;

static const GamepadActionRow kGpActions[] = {
  /* Tank movement — left stick on every set */
  { "glyph_tank_forward", "XboxSeriesX_Left_Stick.png", "PS5_Left_Stick.png", "Switch_Left_Stick.png" },
  { "glyph_tank_back",    "XboxSeriesX_Left_Stick.png", "PS5_Left_Stick.png", "Switch_Left_Stick.png" },
  { "glyph_tank_left",    "XboxSeriesX_Left_Stick.png", "PS5_Left_Stick.png", "Switch_Left_Stick.png" },
  { "glyph_tank_right",   "XboxSeriesX_Left_Stick.png", "PS5_Left_Stick.png", "Switch_Left_Stick.png" },
  /* Triggers.  Retained as Path-B fallback art only — the tutorial now
     routes {FIRE}/{MINE} through glyphForControllerAction so the shown
     glyph follows the live fire/mine binding rather than these fixed
     trigger images. */
  { "glyph_fire", "XboxSeriesX_RT.png", "PS5_R2.png", "Switch_RT.png" },
  { "glyph_mine", "XboxSeriesX_LT.png", "PS5_L2.png", "Switch_LT.png" },
  /* Map scroll — right stick */
  { "glyph_scroll_up",    "XboxSeriesX_Right_Stick.png", "PS5_Right_Stick.png", "Switch_Right_Stick.png" },
  { "glyph_scroll_down",  "XboxSeriesX_Right_Stick.png", "PS5_Right_Stick.png", "Switch_Right_Stick.png" },
  { "glyph_scroll_left",  "XboxSeriesX_Right_Stick.png", "PS5_Right_Stick.png", "Switch_Right_Stick.png" },
  { "glyph_scroll_right", "XboxSeriesX_Right_Stick.png", "PS5_Right_Stick.png", "Switch_Right_Stick.png" },
  /* Start-equivalent button on each pad. */
  { "glyph_pause",        "XboxSeriesX_Menu.png",        "PS5_Options.png",     "Switch_Plus.png" },
  /* Quick chat — D-pad left on every set. */
  { "glyph_quick_chat",   "XboxSeriesX_Dpad_Left.png",   "PS5_Dpad_Left.png",   "Switch_Dpad_Left.png" },
  /* Confirm/dismiss — south face button on each pad.  Switch's
     south face is labelled B (Nintendo's A/B are swapped vs Xbox). */
  { "glyph_dismiss",      "XboxSeriesX_A.png",           "PS5_Cross.png",       "Switch_B.png" },
};
static const int kGpActionCount =
    (int)(sizeof(kGpActions) / sizeof(kGpActions[0]));

SDL_Texture *glyphForActionAuto(const char *action_name) {
  if (!action_name) return NULL;
  SDL_Texture *t = glyphForAction(action_name);
  if (t) return t;
  if (strncmp(action_name, "glyph_", 6) == 0) return NULL;
  char buf[80];
  SDL_snprintf(buf, sizeof(buf), "glyph_%s", action_name);
  return glyphForGamepadAction(buf);
}

SDL_Texture *glyphForGamepadAction(const char *glyphAction) {
  if (!glyphAction) return NULL;
  /* Refresh on every call so a connect/disconnect that fires
     between glyphsInit and the first lookup gets picked up.  The
     work is cheap: SDL_GetGamepadType + an enum switch. */
  glyphsRefreshActiveSet();
  for (int i = 0; i < kGpActionCount; ++i) {
    if (strcmp(kGpActions[i].action, glyphAction) != 0) continue;
    const char *file = NULL;
    const char *sub  = NULL;
    switch (s_active_set) {
      case GAMEPAD_GLYPH_SET_PS5:    sub = "ps5";    file = kGpActions[i].ps5;  break;
      case GAMEPAD_GLYPH_SET_SWITCH: sub = "switch"; file = kGpActions[i].sw;   break;
      default:                       sub = "xbox";   file = kGpActions[i].xbox; break;
    }
    return load_pathb(sub, file);
  }
  return NULL;
}

/* --- Per-button + per-trigger table ---
 *
 * Mirrors the per-set glyph naming used above, indexed by SDL gamepad
 * enum.  The entries cover every button and trigger the rebindable
 * action set's defaults reference, plus a few common remap targets
 * (face buttons, shoulder buttons, dpad).  Buttons not listed here
 * resolve to NULL — the rebind UI falls back to a procedural keycap
 * with SDL_GetGamepadStringForButton(). */

typedef struct {
  SDL_GamepadButton btn;
  const char       *xbox;
  const char       *ps5;
  const char       *sw;
} GamepadButtonRow;

static const GamepadButtonRow kGpButtons[] = {
  { SDL_GAMEPAD_BUTTON_SOUTH,           "XboxSeriesX_A.png",            "PS5_Cross.png",           "Switch_B.png" },
  { SDL_GAMEPAD_BUTTON_EAST,            "XboxSeriesX_B.png",            "PS5_Circle.png",          "Switch_A.png" },
  { SDL_GAMEPAD_BUTTON_WEST,            "XboxSeriesX_X.png",            "PS5_Square.png",          "Switch_Y.png" },
  { SDL_GAMEPAD_BUTTON_NORTH,           "XboxSeriesX_Y.png",            "PS5_Triangle.png",        "Switch_X.png" },
  { SDL_GAMEPAD_BUTTON_BACK,            "XboxSeriesX_View.png",         "PS5_Share.png",           "Switch_Minus.png" },
  { SDL_GAMEPAD_BUTTON_START,           "XboxSeriesX_Menu.png",         "PS5_Options.png",         "Switch_Plus.png" },
  { SDL_GAMEPAD_BUTTON_LEFT_STICK,      "XboxSeriesX_Left_Stick_Click.png",  "PS5_Left_Stick_Click.png",  "Switch_Left_Stick_Click.png" },
  { SDL_GAMEPAD_BUTTON_RIGHT_STICK,     "XboxSeriesX_Right_Stick_Click.png", "PS5_Right_Stick_Click.png", "Switch_Right_Stick_Click.png" },
  { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,   "XboxSeriesX_LB.png",           "PS5_L1.png",              "Switch_LB.png" },
  { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,  "XboxSeriesX_RB.png",           "PS5_R1.png",              "Switch_RB.png" },
  { SDL_GAMEPAD_BUTTON_DPAD_UP,         "XboxSeriesX_Dpad_Up.png",      "PS5_Dpad_Up.png",         "Switch_Dpad_Up.png" },
  { SDL_GAMEPAD_BUTTON_DPAD_DOWN,       "XboxSeriesX_Dpad_Down.png",    "PS5_Dpad_Down.png",       "Switch_Dpad_Down.png" },
  { SDL_GAMEPAD_BUTTON_DPAD_LEFT,       "XboxSeriesX_Dpad_Left.png",    "PS5_Dpad_Left.png",       "Switch_Dpad_Left.png" },
  { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,      "XboxSeriesX_Dpad_Right.png",   "PS5_Dpad_Right.png",      "Switch_Dpad_Right.png" },
};
static const int kGpButtonCount = (int)(sizeof(kGpButtons) / sizeof(kGpButtons[0]));

SDL_Texture *glyphForGamepadButton(SDL_GamepadButton button) {
  glyphsRefreshActiveSet();
  for (int i = 0; i < kGpButtonCount; ++i) {
    if (kGpButtons[i].btn != button) continue;
    const char *file = NULL;
    const char *sub  = NULL;
    switch (s_active_set) {
      case GAMEPAD_GLYPH_SET_PS5:    sub = "ps5";    file = kGpButtons[i].ps5;  break;
      case GAMEPAD_GLYPH_SET_SWITCH: sub = "switch"; file = kGpButtons[i].sw;   break;
      default:                       sub = "xbox";   file = kGpButtons[i].xbox; break;
    }
    return load_pathb(sub, file);
  }
  return NULL;
}

SDL_Texture *glyphForGamepadAxis(SDL_GamepadAxis axis) {
  glyphsRefreshActiveSet();
  const char *xbox = NULL;
  const char *ps5  = NULL;
  const char *sw   = NULL;
  if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
    xbox = "XboxSeriesX_LT.png"; ps5 = "PS5_L2.png"; sw = "Switch_LT.png";
  } else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    xbox = "XboxSeriesX_RT.png"; ps5 = "PS5_R2.png"; sw = "Switch_RT.png";
  } else {
    return NULL;
  }
  switch (s_active_set) {
    case GAMEPAD_GLYPH_SET_PS5:    return load_pathb("ps5",    ps5);
    case GAMEPAD_GLYPH_SET_SWITCH: return load_pathb("switch", sw);
    default:                       return load_pathb("xbox",   xbox);
  }
}

SDL_Texture *glyphForControllerAction(const char *siAction, GamepadAction sdlAction) {
  SDL_Texture *t = siAction ? glyphForAction(siAction) : NULL;   /* Path A: Steam */
  if (t) return t;
  if (sdlAction < GP_ACT_COUNT) {
    const GamepadBinding *b = inputGamepadBindingsGet(sdlAction, GP_SLOT_PRIMARY);
    if (b && b->kind == GP_BIND_BUTTON)  return glyphForGamepadButton((SDL_GamepadButton)b->code);
    if (b && b->kind == GP_BIND_TRIGGER) return glyphForGamepadAxis((SDL_GamepadAxis)b->code);
  }
  return NULL;
}
