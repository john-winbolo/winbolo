/*
 * Copyright (c) 1998-2026 John Morrison.
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
 * Name:          macos_pinch.h
 * Purpose:       macOS trackpad pinch-to-zoom support.
 *                Monitors NSMagnificationGesture events
 *                and exposes accumulated zoom delta.
 *********************************************************/

#ifndef MACOS_PINCH_H
#define MACOS_PINCH_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && !TARGET_OS_IPHONE

/* Start monitoring trackpad magnification (pinch) gestures.
 * Call once after the SDL window is created. */
void macOSPinchZoomInit(void);

/* Return the accumulated pinch magnification delta since the last call
 * and reset the accumulator to zero.  Positive = zoom in, negative = zoom out.
 * Returns 0.0f on non-macOS platforms. */
float macOSPinchZoomConsume(void);

/* Stop monitoring and clean up. */
void macOSPinchZoomDestroy(void);

#else

static inline void  macOSPinchZoomInit(void)    {}
static inline float macOSPinchZoomConsume(void)  { return 0.0f; }
static inline void  macOSPinchZoomDestroy(void)  {}

#endif

#ifdef __cplusplus
}
#endif

#endif /* MACOS_PINCH_H */
