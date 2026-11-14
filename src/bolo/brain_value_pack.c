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

/*
 * brain_value_pack.c -- see public/brain_value_pack.h.
 *
 * Why not LuaJIT's string.buffer: the game builds LuaJIT without it
 * (BOLO_LUAJIT_LOCKDOWN_XCFLAGS in cmake/bolo_luajit.cmake, from #653), and
 * the sandbox removes it, because buf:ref() and buf:reserve() hand a brain
 * raw pointers. This file packs only plain data, gives a brain one function
 * that returns a Lua string, and builds on PUC Lua and LuaJIT alike.
 *
 * The encoder writes into a malloc'd buffer that never reaches Lua. Its
 * only Lua calls read values (lua_type, lua_rawgeti, lua_next, ...) and do
 * not raise errors, so the buffer is freed on every path; a Lua error is
 * raised only after the free. The decoder allocates nothing of its own.
 */
#include "brain_value_pack.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>

enum {
  BP_NIL   = 0x01,
  BP_FALSE = 0x02,
  BP_TRUE  = 0x03,
  BP_INT   = 0x04,
  BP_NUM   = 0x05,
  BP_STR   = 0x06,
  BP_TABLE = 0x07,
  BP_END   = 0x08
};

/* Largest array size hint the decoder passes to lua_createtable. */
#define BP_ARRAY_HINT_MAX 4096u

/* ---- encode --------------------------------------------------------- */

typedef struct {
  unsigned char *buf;
  size_t len;
  size_t cap;
  /* The tables being packed, outermost first: a table met again while it is
   * still open is a cycle. Compared only, never handed to Lua. */
  const void *open[BRAIN_VALUE_MAX_DEPTH];
  char err[160];
} PackEnc;

static bool encFail(PackEnc *e, const char *msg) {
  if (e->err[0] == '\0') snprintf(e->err, sizeof(e->err), "%s", msg);
  return false;
}

static bool encReserve(PackEnc *e, size_t n) {
  if (n > BRAIN_VALUE_MAX_BYTES || e->len + n > BRAIN_VALUE_MAX_BYTES) {
    return encFail(e, "the packed value is larger than the limit");
  }
  if (e->len + n <= e->cap) return true;
  size_t cap = e->cap ? e->cap : 4096;
  while (cap < e->len + n) cap *= 2;
  if (cap > BRAIN_VALUE_MAX_BYTES) cap = BRAIN_VALUE_MAX_BYTES;
  unsigned char *nb = (unsigned char *)realloc(e->buf, cap);
  if (nb == NULL) return encFail(e, "out of memory");
  e->buf = nb;
  e->cap = cap;
  return true;
}

static bool encByte(PackEnc *e, unsigned char b) {
  if (!encReserve(e, 1)) return false;
  e->buf[e->len++] = b;
  return true;
}

static bool encVarint(PackEnc *e, uint64_t v) {
  if (!encReserve(e, 10)) return false;
  while (v >= 0x80) {
    e->buf[e->len++] = (unsigned char)(v | 0x80);
    v >>= 7;
  }
  e->buf[e->len++] = (unsigned char)v;
  return true;
}

static bool encInt(PackEnc *e, int64_t i) {
  uint64_t z = i < 0 ? ~((uint64_t)i << 1) : ((uint64_t)i << 1);
  return encByte(e, BP_INT) && encVarint(e, z);
}

static bool encNumber(lua_State *L, PackEnc *e, int idx) {
#if LUA_VERSION_NUM >= 503
  if (lua_isinteger(L, idx)) return encInt(e, (int64_t)lua_tointeger(L, idx));
#endif
  double d = (double)lua_tonumber(L, idx);
#if LUA_VERSION_NUM < 503
  /* LuaJIT has one number type: an integral value packs short. The range
   * test comes first, so NaN and the infinities never reach the cast. */
  if (d >= -9007199254740992.0 && d <= 9007199254740992.0 &&
      d == (double)(int64_t)d && !(d == 0.0 && signbit(d))) {
    return encInt(e, (int64_t)d);
  }
#endif
  uint64_t u;
  memcpy(&u, &d, sizeof u);
  if (!encByte(e, BP_NUM) || !encReserve(e, 8)) return false;
  for (int k = 0; k < 8; k++) e->buf[e->len++] = (unsigned char)(u >> (8 * k));
  return true;
}

static bool encValue(lua_State *L, PackEnc *e, int idx, int depth);

static bool encTable(lua_State *L, PackEnc *e, int idx, int depth) {
  if (depth >= BRAIN_VALUE_MAX_DEPTH) {
    snprintf(e->err, sizeof(e->err), "tables nested deeper than %d",
             BRAIN_VALUE_MAX_DEPTH);
    return false;
  }
  const void *self = lua_topointer(L, idx);
  for (int i = 0; i < depth; i++) {
    if (e->open[i] == self) return encFail(e, "a table that holds itself (a cycle)");
  }
  e->open[depth] = self;
  if (!lua_checkstack(L, 4)) return encFail(e, "the Lua stack is full");

  if (!encByte(e, BP_TABLE) || !encReserve(e, 4)) return false;
  size_t countAt = e->len;
  e->len += 4;

  /* The array part: keys 1..n up to the first nil. */
  uint32_t narr = 0;
  size_t n = (size_t)lua_rawlen(L, idx);
  for (size_t i = 1; i <= n; i++) {
    lua_rawgeti(L, idx, (int)i);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      break;
    }
    if (!encValue(L, e, lua_gettop(L), depth + 1)) return false;
    lua_pop(L, 1);
    narr++;
  }
  for (int k = 0; k < 4; k++) e->buf[countAt + k] = (unsigned char)(narr >> (8 * k));

  /* Every other pair. lua_tolstring is never called on a key here (it would
   * turn a number key into a string and break lua_next). */
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    int kt = lua_type(L, -2);
    if (kt == LUA_TNUMBER) {
      double k = (double)lua_tonumber(L, -2);
      if (k >= 1.0 && k <= (double)narr && k == floor(k)) {
        lua_pop(L, 1);
        continue;
      }
    } else if (kt != LUA_TSTRING && kt != LUA_TBOOLEAN) {
      snprintf(e->err, sizeof(e->err), "cannot pack a %s as a table key",
               lua_typename(L, kt));
      return false;
    }
    int top = lua_gettop(L);
    if (!encValue(L, e, top - 1, depth + 1)) return false;
    if (!encValue(L, e, top, depth + 1)) return false;
    lua_pop(L, 1);
  }
  return encByte(e, BP_END);
}

static bool encValue(lua_State *L, PackEnc *e, int idx, int depth) {
  int t = lua_type(L, idx);
  switch (t) {
  case LUA_TNIL:
    return encByte(e, BP_NIL);
  case LUA_TBOOLEAN:
    return encByte(e, lua_toboolean(L, idx) ? BP_TRUE : BP_FALSE);
  case LUA_TNUMBER:
    return encNumber(L, e, idx);
  case LUA_TSTRING: {
    size_t n = 0;
    const char *s = lua_tolstring(L, idx, &n);
    if (!encByte(e, BP_STR) || !encVarint(e, (uint64_t)n) || !encReserve(e, n)) {
      return false;
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
    return true;
  }
  case LUA_TTABLE:
    return encTable(L, e, idx, depth);
  default:
    snprintf(e->err, sizeof(e->err), "cannot pack a %s", lua_typename(L, t));
    return false;
  }
}

/* brain_value_encode(value) -> string */
static int l_brain_value_encode(lua_State *L) {
  PackEnc e;
  luaL_argcheck(L, lua_gettop(L) == 1, 1, "takes exactly one value");
  memset(&e, 0, sizeof e);
  bool ok = encValue(L, &e, 1, 0);
  lua_settop(L, 1);
  if (!ok) {
    free(e.buf);
    return luaL_error(L, "brain_value_encode: %s", e.err);
  }
  /* lua_pushlstring copies the bytes; only an out-of-memory error there
   * could skip the free below. */
  lua_pushlstring(L, e.buf ? (const char *)e.buf : "", e.len);
  free(e.buf);
  return 1;
}

/* ---- decode --------------------------------------------------------- */

typedef struct {
  const unsigned char *p;
  size_t len;
  size_t pos;
  char err[160];
} PackDec;

static bool decFail(PackDec *d, const char *msg) {
  if (d->err[0] == '\0') {
    snprintf(d->err, sizeof(d->err), "%s at byte %u", msg, (unsigned)d->pos);
  }
  return false;
}

static bool decVarint(PackDec *d, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (d->pos >= d->len) return decFail(d, "the block ends inside a number");
    unsigned char b = d->p[d->pos++];
    if (shift == 63 && b > 1) return decFail(d, "a number that is too large");
    v |= (uint64_t)(b & 0x7F) << shift;
    if ((b & 0x80) == 0) {
      *out = v;
      return true;
    }
  }
  return decFail(d, "a number that is too long");
}

static bool decValue(lua_State *L, PackDec *d, int depth, bool isKey) {
  if (d->pos >= d->len) return decFail(d, "the block ends early");
  unsigned char tag = d->p[d->pos++];
  switch (tag) {
  case BP_NIL:
    if (depth != 0 || isKey) return decFail(d, "a nil inside a table");
    lua_pushnil(L);
    return true;
  case BP_FALSE:
  case BP_TRUE:
    lua_pushboolean(L, tag == BP_TRUE);
    return true;
  case BP_INT: {
    uint64_t z;
    if (!decVarint(d, &z)) return false;
    int64_t i = (z & 1) ? -(int64_t)(z >> 1) - 1 : (int64_t)(z >> 1);
#if LUA_VERSION_NUM >= 503
    lua_pushinteger(L, (lua_Integer)i);
#else
    lua_pushnumber(L, (lua_Number)i);
#endif
    return true;
  }
  case BP_NUM: {
    if (d->len - d->pos < 8) return decFail(d, "the block ends inside a number");
    uint64_t u = 0;
    for (int k = 0; k < 8; k++) u |= (uint64_t)d->p[d->pos + k] << (8 * k);
    d->pos += 8;
    double v;
    memcpy(&v, &u, sizeof v);
    if (isKey && v != v) return decFail(d, "a NaN table key");
    lua_pushnumber(L, (lua_Number)v);
    return true;
  }
  case BP_STR: {
    uint64_t n;
    if (!decVarint(d, &n)) return false;
    if (n > (uint64_t)(d->len - d->pos)) return decFail(d, "a string past the end");
    lua_pushlstring(L, (const char *)d->p + d->pos, (size_t)n);
    d->pos += (size_t)n;
    return true;
  }
  case BP_TABLE: {
    if (isKey) return decFail(d, "a table as a table key");
    if (depth >= BRAIN_VALUE_MAX_DEPTH) return decFail(d, "tables nested too deep");
    if (!lua_checkstack(L, 4)) return decFail(d, "the Lua stack is full");
    if (d->len - d->pos < 4) return decFail(d, "the block ends inside a table");
    uint32_t narr = 0;
    for (int k = 0; k < 4; k++) narr |= (uint32_t)d->p[d->pos + k] << (8 * k);
    d->pos += 4;
    /* Every item takes at least one byte, so a count past the end is a
     * lie; checking it first keeps a bad block from a huge allocation. */
    if (narr > d->len - d->pos) return decFail(d, "a table count past the end");
    /* The count is only a size hint. Clamp it to the bytes left and to a
     * fixed limit: nested tables each check against the same bytes left,
     * so without the fixed limit 64 levels could each ask for ~16M slots
     * up front. Lua grows the array as the items arrive. */
    {
      size_t left = d->len - d->pos;
      size_t hint = narr < left ? (size_t)narr : left;
      if (hint > BP_ARRAY_HINT_MAX) hint = BP_ARRAY_HINT_MAX;
      lua_createtable(L, (int)hint, 0);
    }
    for (uint32_t i = 1; i <= narr; i++) {
      if (!decValue(L, d, depth + 1, false)) return false;
      lua_rawseti(L, -2, (int)i);
    }
    for (;;) {
      if (d->pos >= d->len) return decFail(d, "a table with no end mark");
      if (d->p[d->pos] == BP_END) {
        d->pos++;
        return true;
      }
      if (!decValue(L, d, depth + 1, true)) return false;
      if (!decValue(L, d, depth + 1, false)) return false;
      lua_rawset(L, -3);
    }
  }
  default:
    d->pos--;
    return decFail(d, "an unknown type byte");
  }
}

/* brain_value_decode(string) -> value */
static int l_brain_value_decode(lua_State *L) {
  luaL_argcheck(L, lua_gettop(L) == 1, 1, "takes exactly one string");
  PackDec d;
  memset(&d, 0, sizeof d);
  /* Checked before luaL_checklstring, which would take a number too. */
  if (lua_type(L, 1) != LUA_TSTRING) return luaL_argerror(L, 1, "not a string");
  d.p = (const unsigned char *)lua_tolstring(L, 1, &d.len);
  if (d.len > BRAIN_VALUE_MAX_BYTES) {
    return luaL_error(L, "brain_value_decode: the block is larger than the limit");
  }
  int base = lua_gettop(L);
  bool ok = decValue(L, &d, 0, false);
  if (ok && d.pos != d.len) ok = decFail(&d, "bytes left after the value");
  if (!ok) {
    lua_settop(L, base);
    return luaL_error(L, "brain_value_decode: %s", d.err);
  }
  return 1;
}

void brainValueRegisterEncode(lua_State *L) {
  lua_pushcfunction(L, l_brain_value_encode);
  lua_setglobal(L, "brain_value_encode");
}

void brainValueRegisterDecode(lua_State *L) {
  lua_pushcfunction(L, l_brain_value_decode);
  lua_setglobal(L, "brain_value_decode");
}
