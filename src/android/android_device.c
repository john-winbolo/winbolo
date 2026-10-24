/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
*Name:          Android Device
*Filename:      android_device.c
*Purpose:
*  See android_device.h.
*********************************************************/

#include <jni.h>
#include <SDL3/SDL.h>

#include "../common/wb_log.h"
#include "android_device.h"

#define DEVICE_JAVA_CLASS "com/winbolo/android/WinBoloActivity"

/* Android's own large-screen line: a configuration whose smallest width is
   this many dp or more is a tablet. */
#define DEVICE_TABLET_MIN_WIDTH_DP 600

static jclass    s_deviceClass = NULL;   /* global reference */
static jmethodID s_midImeHeight = NULL;
static jmethodID s_midShowKeyboard = NULL;
static jmethodID s_midSafeInsets = NULL;
static jmethodID s_midMulticastHold = NULL;
static bool      s_isTablet = false;

static bool deviceClearException(JNIEnv *env, const char *what) {
  if ((*env)->ExceptionCheck(env)) {
    WB_LOG_WARN(WB_LOG_CAT_PLATFORM, "device JNI: Java exception in %s", what);
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return true;
  }
  return false;
}

/* One static method's ID, or NULL with the NoSuchMethodError it raised
   cleared: another JNI call with that error still pending is not allowed,
   so each lookup clears its own before the next. */
static jmethodID deviceStaticMethod(JNIEnv *env, jclass cls, const char *name,
                                    const char *sig) {
  jmethodID mid = (*env)->GetStaticMethodID(env, cls, name, sig);
  if (mid == NULL) {
    deviceClearException(env, name);
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "device JNI: method %s not found", name);
  }
  return mid;
}

bool androidDeviceInit(void) {
  JNIEnv *env;
  jclass local;
  jmethodID midWidth;
  jint widthDp;

  if (s_deviceClass != NULL) return true;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) {
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "device JNI: no JNI environment: %s", SDL_GetError());
    return false;
  }
  if ((*env)->PushLocalFrame(env, 8) < 0) return false;

  /* FindClass sees the app's classes from this thread, which Java created
     (android_netpath.c resolves its class the same way). */
  local = (*env)->FindClass(env, DEVICE_JAVA_CLASS);
  if (local == NULL) {
    (*env)->ExceptionClear(env);
    WB_LOG_ERROR(WB_LOG_CAT_PLATFORM, "device JNI: class %s not found", DEVICE_JAVA_CLASS);
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  midWidth = deviceStaticMethod(env, local, "smallestWidthDp", "()I");
  s_midImeHeight = deviceStaticMethod(env, local, "imeHeightPx", "()I");
  s_midShowKeyboard = deviceStaticMethod(env, local, "showKeyboardAgain", "()V");
  s_midSafeInsets = deviceStaticMethod(env, local, "safeInsetsPx", "()[I");
  s_midMulticastHold = deviceStaticMethod(env, local, "multicastHold", "(Z)V");
  if (midWidth == NULL || s_midImeHeight == NULL || s_midShowKeyboard == NULL ||
      s_midSafeInsets == NULL || s_midMulticastHold == NULL) {
    s_midImeHeight = NULL;
    s_midShowKeyboard = NULL;
    s_midSafeInsets = NULL;
    s_midMulticastHold = NULL;
    (*env)->PopLocalFrame(env, NULL);
    return false;
  }
  widthDp = (*env)->CallStaticIntMethod(env, local, midWidth);
  if (deviceClearException(env, "smallestWidthDp")) widthDp = 0;
  s_isTablet = (widthDp >= DEVICE_TABLET_MIN_WIDTH_DP);
  s_deviceClass = (jclass)(*env)->NewGlobalRef(env, local);
  (*env)->PopLocalFrame(env, NULL);
  if (s_deviceClass == NULL) {
    s_midImeHeight = NULL;
    s_midShowKeyboard = NULL;
    s_midSafeInsets = NULL;
    return false;
  }
  WB_LOG_INFO(WB_LOG_CAT_PLATFORM, "[Android] smallest width %d dp: %s",
              (int)widthDp, s_isTablet ? "tablet" : "phone");
  return true;
}

bool androidDeviceIsTablet(void) {
  return s_isTablet;
}

bool androidDeviceAppPath(const char *rel, char *out, size_t cap) {
  const char *base = SDL_GetAndroidInternalStoragePath();
  if (base == NULL || base[0] == '\0' || rel == NULL || out == NULL || cap == 0) {
    return false;
  }
  return SDL_snprintf(out, cap, "%s/%s/%s", base, ANDROID_APP_DIR, rel) < (int)cap;
}

int androidDeviceKeyboardHeight(void) {
  JNIEnv *env;
  jint height;
  if (s_deviceClass == NULL || s_midImeHeight == NULL) return 0;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) return 0;
  height = (*env)->CallStaticIntMethod(env, s_deviceClass, s_midImeHeight);
  if (deviceClearException(env, "imeHeightPx")) return 0;
  return (int)height;
}

bool androidDeviceSafeInsets(int *left, int *top, int *right, int *bottom) {
  JNIEnv *env;
  jintArray arr;
  jint v[4];
  if (s_deviceClass == NULL || s_midSafeInsets == NULL) return false;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) return false;
  arr = (jintArray)(*env)->CallStaticObjectMethod(env, s_deviceClass, s_midSafeInsets);
  if (deviceClearException(env, "safeInsetsPx") || arr == NULL) return false;
  if ((*env)->GetArrayLength(env, arr) != 4) {
    (*env)->DeleteLocalRef(env, arr);
    return false;
  }
  (*env)->GetIntArrayRegion(env, arr, 0, 4, v);
  (*env)->DeleteLocalRef(env, arr);
  if (deviceClearException(env, "safeInsetsPx region")) return false;
  if (left) *left = (int)v[0];
  if (top) *top = (int)v[1];
  if (right) *right = (int)v[2];
  if (bottom) *bottom = (int)v[3];
  return true;
}

void androidDeviceShowKeyboard(void) {
  JNIEnv *env;
  if (s_deviceClass == NULL || s_midShowKeyboard == NULL) return;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) return;
  (*env)->CallStaticVoidMethod(env, s_deviceClass, s_midShowKeyboard);
  deviceClearException(env, "showKeyboardAgain");
}

void androidDeviceMulticastHold(bool on) {
  JNIEnv *env;
  if (s_deviceClass == NULL || s_midMulticastHold == NULL) return;
  env = (JNIEnv *)SDL_GetAndroidJNIEnv();
  if (env == NULL) return;
  (*env)->CallStaticVoidMethod(env, s_deviceClass, s_midMulticastHold,
                               (jboolean)(on ? JNI_TRUE : JNI_FALSE));
  deviceClearException(env, "multicastHold");
}
