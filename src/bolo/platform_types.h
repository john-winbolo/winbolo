/*
 * Copyright (c) 1998-2008 John Morrison.
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
*Name:          Platform Types
*Filename:      platform_types.h
*Purpose:
*  Fixed-width portable type definitions for WinBolo.
*  Ensures binary compatibility for network packets and
*  log files across 32-bit and 64-bit platforms.
*
*  Include this header (via global.h) instead of relying
*  on platform-specific sizes for fundamental types.
*********************************************************/

#ifndef PLATFORM_TYPES_H
#define PLATFORM_TYPES_H

#include <stdint.h>
#include <stdbool.h>

/* Fundamental byte type — always 8 bits.
 * uint8_t = unsigned char, which matches Windows SDK's BYTE. No conflict. */
typedef uint8_t  BYTE;

/* Map coordinate types (0-255) — WinBolo-specific, not in Windows SDK */
typedef uint8_t  MAP_X;
typedef uint8_t  MAP_Y;

/* World coordinate — 16-bit fixed. WinBolo-specific, not in Windows SDK. */
typedef uint16_t WORLD;

/* Generic 16-bit word.
 * uint16_t = unsigned short, which matches Windows SDK's WORD. No conflict. */
typedef uint16_t WORD;

/* Player bitmap — must be exactly 32 bits for network packet compatibility.
 * Previously 'unsigned long', which is 8 bytes on 64-bit Linux/macOS. */
typedef uint32_t PlayerBitMap;

/* Timer/tick counter — 32-bit unsigned, sufficient for ~49 days of milliseconds.
 * On Windows, DWORD is already typedef'd as 'unsigned long' by <minwindef.h>
 * (which is always 32-bit on Windows regardless of pointer size — LLP64 model).
 * On POSIX (LP64), 'unsigned long' is 64 bits, so we define it explicitly. */
#ifndef _WIN32
typedef uint32_t DWORD;
#endif

/* Floating-point angular and speed types.
 * IEEE 754 single-precision is always 4 bytes on all supported platforms. */
typedef float TURNTYPE;
typedef float SPEEDTYPE;

/* Boolean constants — compatible with C99 <stdbool.h> bool type.
 * The bool type itself comes from <stdbool.h> included above. */
#ifndef TRUE
#  define TRUE  1
#endif
#ifndef FALSE
#  define FALSE 0
#endif

/* Portable compile-time assertion.
 * Usage: BOLO_STATIC_ASSERT(condition, identifier_label)
 *   - condition: boolean expression
 *   - identifier_label: a valid C identifier used as the error label/message
 *
 * _Static_assert is C11; MSVC supports 'static_assert' as a C-mode extension
 * even in C99 mode. Both require a string-literal message, so we stringify the
 * label with the # operator. GCC/Clang use _Static_assert in C11+. The C99
 * fallback uses a negative-size array typedef which produces a readable error. */
#if defined(_MSC_VER)
#  define BOLO_STATIC_ASSERT(cond, msg) static_assert(cond, #msg)
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define BOLO_STATIC_ASSERT(cond, msg) _Static_assert(cond, #msg)
#else
#  define BOLO_STATIC_ASSERT(cond, msg) \
     typedef char bolo_static_assert_##msg[(cond) ? 1 : -1]
#endif

/* Packed-struct attribute.
 * MSVC uses #pragma pack(1) at the call site; GCC/Clang also need
 * __attribute__((packed)) to guarantee no internal padding on macOS/ARM
 * where bare #pragma pack may interact unexpectedly with system headers. */
#ifdef _MSC_VER
#  define BOLO_PACK_ATTR
#else
#  define BOLO_PACK_ATTR __attribute__((packed))
#endif

#endif /* PLATFORM_TYPES_H */
