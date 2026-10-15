/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          ios_discovery.h
 * Purpose:       The LAN game search on iOS, through the
 *                system's Bonjour API. See ios_discovery.m.
 *********************************************************/

#ifndef IOS_DISCOVERY_H
#define IOS_DISCOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hands the Bonjour browser to discovery as its mDNS browser
 * (discoverySetMdnsBrowser). Call once at startup, before any search. */
void iosDiscoveryInstall(void);

#ifdef __cplusplus
}
#endif

#endif /* IOS_DISCOVERY_H */
