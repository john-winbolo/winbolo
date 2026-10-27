/*
 * The shipped tutorials' text, checked before it ships: every entry of
 * every language's TEXT table in data/maps/tutorials/Tutorial.scenario.lua
 * and Classic Tutorial.scenario.lua, for each input it is worded for, with
 * the widest numbers filled in.
 *
 *   tutorial_text_fits — each text is inside the limit the server takes
 *       for the call that sends it (popup 511 bytes a part, status line
 *       128, announcement 125, panel text 48 and one row), so no line is
 *       refused in play. A popup longer than one part is a table of parts.
 *       For a script with a STEPS table (the Classic Tutorial, which sends
 *       each step's messages at once), no step sends more popups, counting
 *       parts, than a client queues (SCN_POPUP_QUEUE_MAX), in any language
 *       or wording, so none is dropped.
 *   tutorial_text_well_formed — each text is valid UTF-8, every "{" starts
 *       a key token the client knows ({FIRE}, {GUN_UP} ...) and every "}"
 *       ends one, so a typo such as "{FIRE" or "{GUN_DWN}" fails here and
 *       not as a raw brace on a player's screen. An entry worded by input
 *       has a keyboard wording and no input but keyboard, touch and
 *       controller; a language's entry for a key English words by input is
 *       worded by input too; and no language has a key English lacks.
 *
 * The script runs in a plain Lua state with a stub game table, the checker
 * appended to its source so it can read the file's local TEXT. The token
 * and UTF-8 checks are C (tutorialTokenMatch, the client's own matcher).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "../../src/gui/sdl3/tutorial_tokens.h"
#include "scenario_panel.h"   /* SCN_POPUP_QUEUE_MAX */
#include "test_harness.h"

#ifndef WB_REPO_ROOT_DIR
#define WB_REPO_ROOT_DIR "."
#endif

/* The scripts checked, with the chunk name each error names. */
static const struct {
    const char *path;
    const char *chunk;
} kTtcScripts[] = {
    { WB_REPO_ROOT_DIR "/data/maps/tutorials/Tutorial.scenario.lua",
      "=Tutorial.scenario.lua" },
    { WB_REPO_ROOT_DIR "/data/maps/tutorials/Classic Tutorial.scenario.lua",
      "=Classic Tutorial.scenario.lua" },
};
#define TTC_SCRIPT_COUNT ((int)(sizeof(kTtcScripts) / sizeof(kTtcScripts[0])))

/* The checker, run after the script in the same chunk. It answers two
   lists of faults: over a limit, and badly formed. */
static const char *const kChecker =
    "\n"
    "do\n"
    "  local LIMIT = { pop = 511, step = 128, line = 128, say = 125,\n"
    "                  title = 48, list = 48, info = 48, hint = 48,\n"
    "                  terrain = 48 }\n"
    "  local INPUTS = { keyboard = true, touch = true, controller = true }\n"
    "  local over, bad = {}, {}\n"
    "  local function by_input(v)\n"
    "    return type(v) == 'table' and v.keyboard ~= nil\n"
    "  end\n"
    "  -- The longest terrain word a language has, for the info line.\n"
    "  local function widest_word(T)\n"
    "    local word = ''\n"
    "    for _, w in pairs(T.terrain or TEXT.en.terrain) do\n"
    "      if type(w) == 'string' and #w > #word then word = w end\n"
    "    end\n"
    "    return word\n"
    "  end\n"
    "  local function one(where, lim, s, T, section, key)\n"
    "    if type(s) == 'function' then s = s(99, 15) end\n"
    "    if type(s) ~= 'string' then\n"
    "      bad[#bad + 1] = where .. ' is a ' .. type(s); return\n"
    "    end\n"
    "    local ok, out = pcall(string.format, s, 99, 15)\n"
    "    if section == 'info' and (key == 'on' or key == 'on_plain') then\n"
    "      ok, out = pcall(string.format, s, widest_word(T), 255)\n"
    "    end\n"
    "    if not ok then out = s end\n"
    "    if #out > lim then\n"
    "      over[#over + 1] = where .. ' is ' .. #out .. ' bytes, limit ' .. lim\n"
    "    elseif lim == 48 and string.find(out, '\\n') then\n"
    "      over[#over + 1] = where .. ' has a second row'\n"
    "    end\n"
    "    local fault = ttc_text_fault(out)\n"
    "    if fault then bad[#bad + 1] = where .. ': ' .. fault end\n"
    "  end\n"
    "  local function parts(where, lim, v, T, section, key)\n"
    "    if type(v) == 'table' and not by_input(v) then\n"
    "      if section ~= 'pop' then\n"
    "        bad[#bad + 1] = where .. ' is in parts, which only a popup may be'\n"
    "      end\n"
    "      if #v == 0 then bad[#bad + 1] = where .. ' has no parts' end\n"
    "      for i, part in ipairs(v) do\n"
    "        one(where .. '[' .. i .. ']', lim, part, T, section, key)\n"
    "      end\n"
    "    else\n"
    "      one(where, lim, v, T, section, key)\n"
    "    end\n"
    "  end\n"
    "  for code, T in pairs(TEXT) do\n"
    "    for section, entries in pairs(T) do\n"
    "      local lim = LIMIT[section]\n"
    "      local en = TEXT.en[section]\n"
    "      if lim == nil then\n"
    "        bad[#bad + 1] = code .. ' has a section ' .. tostring(section) ..\n"
    "                        ' with no limit known'\n"
    "      else\n"
    "        for key, v in pairs(entries) do\n"
    "          local where = code .. ' ' .. section .. '.' .. tostring(key)\n"
    "          if en == nil or en[key] == nil then\n"
    "            bad[#bad + 1] = where .. ' is a key English does not have'\n"
    "          elseif by_input(en[key]) and not by_input(v) then\n"
    "            bad[#bad + 1] = where .. ' is one wording; English words it '\n"
    "                            .. 'for each input'\n"
    "          end\n"
    "          if by_input(v) then\n"
    "            for input, w in pairs(v) do\n"
    "              if not INPUTS[input] then\n"
    "                bad[#bad + 1] = where .. ' has an input named ' ..\n"
    "                                tostring(input)\n"
    "              else\n"
    "                parts(where .. '.' .. input, lim, w, T, section, key)\n"
    "              end\n"
    "            end\n"
    "          elseif type(v) == 'table' and next(v) ~= nil and v[1] == nil then\n"
    "            bad[#bad + 1] = where .. ' is worded by input with no keyboard'\n"
    "          else\n"
    "            parts(where, lim, v, T, section, key)\n"
    "          end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  if STEPS ~= nil then\n"
    "    local function count(T, key, input)\n"
    "      local v = (T.pop and T.pop[key]) or TEXT.en.pop[key]\n"
    "      if by_input(v) then v = v[input] or v.keyboard end\n"
    "      if type(v) == 'table' then return #v end\n"
    "      return 1\n"
    "    end\n"
    "    for code, T in pairs(TEXT) do\n"
    "      for _, input in ipairs({ 'keyboard', 'touch', 'controller' }) do\n"
    "        for i, s in ipairs(STEPS) do\n"
    "          local n = 0\n"
    "          for _, key in ipairs(s.msgs) do n = n + count(T, key, input) end\n"
    "          if n > ttc_popup_queue then\n"
    "            over[#over + 1] = code .. ' step ' .. i .. ' (' .. input ..\n"
    "              ') sends ' .. n .. ' popups, a client queues ' .. ttc_popup_queue\n"
    "          end\n"
    "        end\n"
    "      end\n"
    "    end\n"
    "  end\n"
    "  table.sort(over); table.sort(bad)\n"
    "  local n = 0\n"
    "  for _ in pairs(TEXT) do n = n + 1 end\n"
    "  return over, bad, n\n"
    "end\n";

/* Whether s[0..len) is well-formed UTF-8. */
static bool ttcUtf8Valid(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        const unsigned char c = (unsigned char)s[i];
        size_t n, k;
        if (c < 0x80) { i++; continue; }
        if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        else return false;
        if (i + n > len) return false;
        for (k = 1; k < n; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

/* ttc_text_fault(s): nil for a good text, else what is wrong with it. */
static int ttcTextFault(lua_State *L) {
    size_t      len = 0;
    const char *s   = luaL_checklstring(L, 1, &len);
    size_t      i;
    char        msg[96];

    if (!ttcUtf8Valid(s, len)) {
        lua_pushstring(L, "not valid UTF-8");
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (s[i] == '{') {
            size_t tlen = 0;
            if (tutorialTokenMatch(s + i, &tlen) == TT_COUNT) {
                snprintf(msg, sizeof(msg), "'%.20s' is no key token", s + i);
                lua_pushstring(L, msg);
                return 1;
            }
            i += tlen - 1;   /* past the token's '}' */
        } else if (s[i] == '}') {
            lua_pushstring(L, "a '}' that ends no key token");
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

/* A game table with no server behind it, so the script's top level loads:
   a constant table (game.TERRAIN ...) answers 0 for any name, and every
   other field is a function answering nothing. */
static const char *const kGameStub =
    "local function const()\n"
    "  return setmetatable({}, { __index = function() return 0 end })\n"
    "end\n"
    "game = setmetatable({}, { __index = function(t, k)\n"
    "  local v\n"
    "  if string.match(k, '^%u') then v = const() else v = function() end end\n"
    "  rawset(t, k, v)\n"
    "  return v\n"
    "end })\n";

static char *ttcReadFile(const char *path, size_t *outLen) {
    FILE  *f = fopen(path, "rb");
    char  *buf;
    long   size;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)size + strlen(kChecker) + 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    memcpy(buf + size, kChecker, strlen(kChecker) + 1);
    *outLen = (size_t)size + strlen(kChecker);
    return buf;
}

/* Runs script `which` and the checker. Leaves over, bad and the language
   count on the stack and answers true, or writes why it could not and
   answers false. */
static bool ttcRun(lua_State *L, int which, char *err, size_t errCap) {
    size_t len = 0;
    char  *src = ttcReadFile(kTtcScripts[which].path, &len);
    if (src == NULL) {
        snprintf(err, errCap, "could not read %s", kTtcScripts[which].path);
        return false;
    }
    luaL_openlibs(L);
    lua_register(L, "ttc_text_fault", ttcTextFault);
    lua_pushinteger(L, SCN_POPUP_QUEUE_MAX);
    lua_setglobal(L, "ttc_popup_queue");
    if (luaL_dostring(L, kGameStub) != 0) {
        snprintf(err, errCap, "the game stub: %s", lua_tostring(L, -1));
        free(src);
        return false;
    }
    if (luaL_loadbuffer(L, src, len, kTtcScripts[which].chunk) != 0 ||
        lua_pcall(L, 0, 3, 0) != 0) {
        snprintf(err, errCap, "%s", lua_tostring(L, -1));
        free(src);
        return false;
    }
    free(src);
    return true;
}

/* The faults in the list at stack index idx, the first few joined. */
static int ttcFaults(lua_State *L, int idx, char *out, size_t cap) {
    int n = (int)lua_objlen(L, idx);
    int i;
    out[0] = '\0';
    for (i = 1; i <= n && i <= 6; i++) {
        size_t used = strlen(out);
        lua_rawgeti(L, idx, i);
        snprintf(out + used, cap - used, "%s%s", used ? "; " : "",
                 lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return n;
}

int run_tutorial_text_fits(void) {
    int which;
    for (which = 0; which < TTC_SCRIPT_COUNT; which++) {
        const char *name = kTtcScripts[which].chunk + 1;
        lua_State  *L = luaL_newstate();
        char        err[1024];
        int         n, langs;
        UT_ASSERT(L != NULL);
        if (!ttcRun(L, which, err, sizeof(err))) {
            lua_close(L);
            UT_FAIL("%s: %s", name, err);
        }
        langs = (int)lua_tointeger(L, -1);
        n = ttcFaults(L, -3, err, sizeof(err));
        lua_close(L);
        UT_ASSERT_MSG(langs == 17, "%s has %d languages, not the 17 the game "
                      "ships", name, langs);
        UT_ASSERT_MSG(n == 0, "%s: %d texts over a limit: %s", name, n, err);
    }
    return 0;
}

int run_tutorial_text_well_formed(void) {
    int which;
    for (which = 0; which < TTC_SCRIPT_COUNT; which++) {
        const char *name = kTtcScripts[which].chunk + 1;
        lua_State  *L = luaL_newstate();
        char        err[1024];
        int         n;
        UT_ASSERT(L != NULL);
        if (!ttcRun(L, which, err, sizeof(err))) {
            lua_close(L);
            UT_FAIL("%s: %s", name, err);
        }
        n = ttcFaults(L, -2, err, sizeof(err));
        lua_close(L);
        UT_ASSERT_MSG(n == 0, "%s: %d texts badly formed: %s", name, n, err);
    }
    return 0;
}
