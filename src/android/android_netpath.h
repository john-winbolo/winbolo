/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          android_netpath.h
 * Purpose:
 *  Which kind of network the phone's traffic goes out on,
 *  read from WinBoloNet.java (ConnectivityManager) through
 *  JNI and handed to the game front as its net path query,
 *  so the mobile-data warning before hosting an Internet
 *  game works as it does on iOS (ios_netpath.m).
 *********************************************************/

#ifndef ANDROID_NETPATH_H
#define ANDROID_NETPATH_H

#include <stdbool.h>

/* Binds the Java class, starts its watch on the default network and
 * installs the query with gameFrontSetNetPathQuery. Called once from the
 * driver on the main thread, whose class loader can see the app's classes.
 * False leaves the query uninstalled, which gives no warning. */
bool androidNetPathInstall(void);

#endif /* ANDROID_NETPATH_H */
