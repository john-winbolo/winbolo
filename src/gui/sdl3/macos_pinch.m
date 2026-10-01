/*
 * Copyright (c) 1998-2008 John Morrison.
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
 * Name:          macos_pinch.m
 * Purpose:       macOS trackpad pinch-to-zoom support.
 *                Uses NSEvent local monitoring to capture
 *                magnification (pinch) gestures and expose
 *                them as a consumable zoom delta.
 *********************************************************/

#import <Cocoa/Cocoa.h>
#include "macos_pinch.h"

static id  pinchMonitor = nil;
static float pinchAccum = 0.0f;

void macOSDisablePressAndHold(void) {
    /* Stop macOS from showing the accent-character picker when a key is held
     * down; games rely on key repeat from a held key instead.  This writes
     * into the running app's own defaults domain, so it only affects WinBolo
     * and leaves accent input untouched in every other app. */
    [[NSUserDefaults standardUserDefaults]
        setBool:NO forKey:@"ApplePressAndHoldEnabled"];
}

void macOSPinchZoomInit(void) {
    if (pinchMonitor) return; /* already initialised */
    pinchMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
        handler:^NSEvent *(NSEvent *event) {
            pinchAccum += (float)[event magnification];
            return event;
        }];
}

float macOSPinchZoomConsume(void) {
    float delta = pinchAccum;
    pinchAccum = 0.0f;
    return delta;
}

void macOSPinchZoomDestroy(void) {
    if (pinchMonitor) {
        [NSEvent removeMonitor:pinchMonitor];
        pinchMonitor = nil;
    }
    pinchAccum = 0.0f;
}
