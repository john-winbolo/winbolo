/*
 * pool_render_host.c -- see public/pool_render_host.h.
 *
 * One plain lua_State (standard libraries and brain_value_decode, no brain
 * globals), kept open between calls. Its package.path holds only the brain directory, so
 * require "pool_render" finds the brain's own pool_render.lua, json.lua,
 * bitcompat.lua and constants.lua. A change of brain directory opens a new
 * state.
 */
#include "pool_render_host.h"
#include "brain_value_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static lua_State *g_L = NULL;
static char g_dir[1024];
static int g_renderRef = LUA_NOREF;

bool poolRenderIsRaw(const char *blob) {
    return blob && (strncmp(blob, POOL_RENDER_RAW_TAG,
                            sizeof(POOL_RENDER_RAW_TAG) - 1) == 0 ||
                    strncmp(blob, POOL_RENDER_RAW_TAG_V1,
                            sizeof(POOL_RENDER_RAW_TAG_V1) - 1) == 0);
}

char *poolRenderDupBrainString(const char *s, size_t len) {
    if (!s) return NULL;
    size_t tagLen = sizeof(POOL_RENDER_RAW_TAG) - 1;
    if (len < tagLen || memcmp(s, POOL_RENDER_RAW_TAG, tagLen) != 0) {
        size_t n = strlen(s);
        char *o = (char *)malloc(n + 1);
        if (o) memcpy(o, s, n + 1);
        return o;
    }
    size_t extra = 0;
    for (size_t i = tagLen; i < len; i++) {
        if ((unsigned char)s[i] <= 1) extra++;
    }
    char *o = (char *)malloc(len + extra + 1);
    if (!o) return NULL;
    memcpy(o, s, tagLen);
    size_t j = tagLen;
    for (size_t i = tagLen; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0)      { o[j++] = 1; o[j++] = 2; }
        else if (c == 1) { o[j++] = 1; o[j++] = 3; }
        else             { o[j++] = (char)c; }
    }
    o[j] = '\0';
    return o;
}

void poolRenderHostShutdown(void) {
    if (g_L) lua_close(g_L);
    g_L = NULL;
    g_dir[0] = '\0';
    g_renderRef = LUA_NOREF;
}

static char *dupStr(const char *s) {
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1);
    if (o) memcpy(o, s, n + 1);
    return o;
}

static void setErr(char *err, size_t errLen, const char *msg) {
    if (err && errLen) snprintf(err, errLen, "%s", msg ? msg : "");
}

/* A one-row panel that shows msg, in the shape BrainTest's pool panel reads. */
static char *errorPanel(const char *msg) {
    char clean[512];
    size_t j = 0;
    for (size_t i = 0; msg && msg[i] && j + 1 < sizeof(clean); i++) {
        char c = msg[i];
        if (c == '"' || c == '\\') c = '\'';
        if ((unsigned char)c < 0x20) c = ' ';
        clean[j++] = c;
    }
    clean[j] = '\0';
    size_t n = strlen(clean) + 256;
    char *o = (char *)malloc(n);
    if (!o) return NULL;
    snprintf(o, n,
        "{\"phase\":\"?\",\"tick\":0,\"replan_left\":0,\"bot\":0,"
        "\"sections\":[{\"id\":\"error\",\"label\":\"Pool render error\","
        "\"rows\":[{\"id\":0,\"mx\":0,\"my\":0,\"cost\":0,\"formula\":\"%s\","
        "\"stale\":-1,\"active\":false,\"imminent\":false,\"reject\":null}]}]}",
        clean);
    return o;
}

/* Opens the state for brainDir and keeps a reference to pool_render's
 * render_blob. Returns NULL on success, else the error text (static). */
static const char *openState(const char *brainDir) {
    static char msg[512];
    if (g_L && strcmp(g_dir, brainDir) == 0) return NULL;
    poolRenderHostShutdown();
    lua_State *L = luaL_newstate();
    if (!L) return "pool render: out of memory";
    luaL_openlibs(L);
    /* pool_render.unpack reads a raw block with this (see brain_value_pack.h). */
    brainValueRegisterDecode(L);
    lua_getglobal(L, "package");
    lua_pushfstring(L, "%s/?.lua", brainDir);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);
    lua_getglobal(L, "require");
    lua_pushstring(L, "pool_render");
    if (lua_pcall(L, 1, 1, 0) != 0) {
        snprintf(msg, sizeof(msg), "pool render: cannot load pool_render.lua from %s: %s",
                 brainDir, lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_close(L);
        return msg;
    }
    lua_getfield(L, -1, "render_blob");
    if (!lua_isfunction(L, -1)) {
        snprintf(msg, sizeof(msg), "pool render: %s/pool_render.lua has no render_blob",
                 brainDir);
        lua_close(L);
        return msg;
    }
    g_renderRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
    g_L = L;
    snprintf(g_dir, sizeof(g_dir), "%s", brainDir);
    return NULL;
}

char *poolRenderToJson(const char *brainDir, const char *blob,
                       char *err, size_t errLen) {
    setErr(err, errLen, "");
    if (!blob) blob = "";
    if (!poolRenderIsRaw(blob)) return dupStr(blob);
    const char *e = openState(brainDir ? brainDir : "brains/GoalHunter");
    if (e) {
        setErr(err, errLen, e);
        return errorPanel(e);
    }
    lua_State *L = g_L;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_renderRef);
    lua_pushstring(L, blob);
    char *out;
    if (lua_pcall(L, 1, 1, 0) != 0 || !lua_isstring(L, -1)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "pool render: %s",
                 lua_tostring(L, -1) ? lua_tostring(L, -1) : "no result");
        setErr(err, errLen, msg);
        out = errorPanel(msg);
    } else {
        out = dupStr(lua_tostring(L, -1));
    }
    lua_settop(L, top);
    return out;
}
