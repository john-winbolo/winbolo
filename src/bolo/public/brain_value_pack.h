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
 * brain_value_pack -- packs plain Lua data (tables, strings, numbers, booleans)
 * into a byte string and back, for the GoalHunter raw pool capture.
 *
 * A brain gets one function, brain_value_encode(value) -> string. It takes
 * no options and gives back only a Lua string: no buffer object, no
 * pointer. Functions, userdata, threads, lightuserdata, tables as keys, a
 * table that holds itself (a cycle), nesting deeper than BRAIN_VALUE_MAX_DEPTH
 * and output larger than BRAIN_VALUE_MAX_BYTES are errors. Metatables are not
 * packed: a table is read raw, and an unpacked table has none.
 *
 * The view side (pool_render_host.c: BrainTest, brainrec_split, the unit
 * tests) gets brain_value_decode(string) -> value. A block can come from a
 * recording or another host, so the decoder checks every byte: an unknown
 * type, a length past the end, a bad key, nesting too deep or bytes left
 * over are a Lua error, never a read outside the string.
 *
 * Byte format (all multi-byte values little-endian):
 *   0x01            nil (top level only)
 *   0x02 / 0x03     false / true
 *   0x04 <varint>   integer (zigzag), any integral number with |n| <= 2^53
 *                   on LuaJIT, any Lua integer on Lua 5.3+
 *   0x05 <8 bytes>  any other number, as an IEEE double
 *   0x06 <varint n> <n bytes>   string
 *   0x07 <u32 n> <n values> {<key> <value>} 0x08
 *                   table: n array items for keys 1..n, then the other
 *                   pairs, then the end mark. A key is a string, a number
 *                   or a boolean.
 */

#ifndef BRAIN_VALUE_PACK_H
#define BRAIN_VALUE_PACK_H

#include <lua.h>

/* Deepest table nesting either way. The pool capture is about 6 deep. */
#define BRAIN_VALUE_MAX_DEPTH 64

/* Largest packed string either way. A pool capture is under 100 KB. */
#define BRAIN_VALUE_MAX_BYTES (16u * 1024u * 1024u)

/* Set the global brain_value_encode. luabrainshandler.c calls this for every
 * brain VM, before the brain loads. */
void brainValueRegisterEncode(lua_State *L);

/* Set the global brain_value_decode. For the view side only: a brain VM does
 * not get it. */
void brainValueRegisterDecode(lua_State *L);

#endif /* BRAIN_VALUE_PACK_H */
