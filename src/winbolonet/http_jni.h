/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          http (JNI)
*Filename:      http_jni.h
*Author:        John Morrison
*Creation Date: 21/10/26
*Last Modified: 21/10/26
*Purpose:
*  The one call the Android driver makes into the JNI
*  transport (http_jni.c) that is not part of http.h.
*********************************************************/

#ifndef __HTTP_JNI_H
#define __HTTP_JNI_H

#include <stdbool.h>

/*********************************************************
*NAME:          httpAndroidInit
*PURPOSE:
* Finds the WinBoloHttp Java class and binds its native
* methods. Call it once from the main thread before any
* WinBolo.net request: that thread's class loader can see
* the app's classes, while a thread SDL created cannot.
* A request made before this call resolves the class
* through the activity's class loader instead, so the
* call is a convenience, not a requirement.
* Returns TRUE when the class is bound.
*********************************************************/
bool httpAndroidInit(void);

#endif /* __HTTP_JNI_H */
