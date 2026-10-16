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

/*********************************************************
*Name:          Byte Order
*Filename:      byte_order.h
*Purpose:
*  Little-endian fields in a byte buffer, by shifting, so
*  they read and write the same on every host. Several
*  formats - the replay stream's long counts,
*  attribution.trk, INFO_PACKET's numbers, map editor
*  stamps - took their byte order from the hosts that
*  wrote them, which have always been little-endian, so
*  little-endian is what they are. Big-endian fields have
*  packU16 / packU32 in the transport code.
*********************************************************/

#ifndef BYTE_ORDER_H
#define BYTE_ORDER_H

#include <stdint.h>
#include <string.h>

static inline void byteOrderPutLe16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t) (v & 0xFF);
  p[1] = (uint8_t) (v >> 8);
}

static inline void byteOrderPutLe32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t) (v & 0xFF);
  p[1] = (uint8_t) ((v >> 8) & 0xFF);
  p[2] = (uint8_t) ((v >> 16) & 0xFF);
  p[3] = (uint8_t) (v >> 24);
}

static inline uint16_t byteOrderGetLe16(const uint8_t *p) {
  return (uint16_t) (p[0] | (p[1] << 8));
}

static inline uint32_t byteOrderGetLe32(const uint8_t *p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
         ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* A float as its IEEE-754 bit pattern, little-endian. Assumes, as
   every supported target does, that a float's bytes are ordered like
   a uint32_t's. */
static inline void byteOrderPutLeFloat(uint8_t *p, float v) {
  uint32_t bits;
  memcpy(&bits, &v, sizeof bits);
  byteOrderPutLe32(p, bits);
}

static inline float byteOrderGetLeFloat(const uint8_t *p) {
  uint32_t bits = byteOrderGetLe32(p);
  float v;
  memcpy(&v, &bits, sizeof v);
  return v;
}

#endif /* BYTE_ORDER_H */
