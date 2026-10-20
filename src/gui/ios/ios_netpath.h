/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          ios_netpath.h
 * Purpose:       Which kind of network the device is on,
 *                for gamefront.c's network kind query. See
 *                ios_netpath.m.
 *********************************************************/

#ifndef IOS_NETPATH_H
#define IOS_NETPATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts watching the device's network with NWPathMonitor and installs the
 * answer as gamefront.c's network kind query (gameFrontSetNetPathQuery).
 * Call once at startup. */
void iosNetPathInstall(void);

#ifdef __cplusplus
}
#endif

#endif /* IOS_NETPATH_H */
