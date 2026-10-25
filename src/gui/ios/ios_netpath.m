/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          ios_netpath.m
 * Purpose:
 *  Which kind of network the device's traffic goes out on,
 *  read with the Network framework's path monitor
 *  (NWPathMonitor, here through its C API), which needs no
 *  entitlement. The monitor calls back on its own queue
 *  whenever the path changes; the kind is kept in an atomic
 *  that gamefront.c's query reads on the main thread. Until
 *  the first callback, and whenever the path is not usable,
 *  the answer is unknown, which gives no warning.
 *
 *  Built with ARC (see CMakeLists.txt).
 *********************************************************/

#if !__has_feature(objc_arc)
#error "ios_netpath.m must be compiled with -fobjc-arc"
#endif

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#include <stdatomic.h>

#include "../gamefront.h"
#include "ios_netpath.h"
#include "../../common/wb_log.h"

static _Atomic int        s_netPath = GAMEFRONT_NET_PATH_UNKNOWN;
static nw_path_monitor_t  s_monitor;

static const char *iosNetPathName(GameFrontNetPath kind) {
  switch (kind) {
    case GAMEFRONT_NET_PATH_WIFI:     return "Wi-Fi";
    case GAMEFRONT_NET_PATH_WIRED:    return "wired";
    case GAMEFRONT_NET_PATH_CELLULAR: return "mobile data";
    case GAMEFRONT_NET_PATH_UNKNOWN:
    default:                          return "unknown";
  }
}

/* Cellular is tested first: a path that uses it goes out on mobile data
 * whatever else it reports. */
static GameFrontNetPath iosNetPathKind(nw_path_t path) {
  if (nw_path_get_status(path) != nw_path_status_satisfied) {
    return GAMEFRONT_NET_PATH_UNKNOWN;
  }
  if (nw_path_uses_interface_type(path, nw_interface_type_cellular)) {
    return GAMEFRONT_NET_PATH_CELLULAR;
  }
  if (nw_path_uses_interface_type(path, nw_interface_type_wifi)) {
    return GAMEFRONT_NET_PATH_WIFI;
  }
  if (nw_path_uses_interface_type(path, nw_interface_type_wired)) {
    return GAMEFRONT_NET_PATH_WIRED;
  }
  return GAMEFRONT_NET_PATH_UNKNOWN;
}

static GameFrontNetPath iosNetPathQuery(void) {
  return (GameFrontNetPath)atomic_load(&s_netPath);
}

void iosNetPathInstall(void) {
  if (s_monitor != nil) {
    return;
  }
  s_monitor = nw_path_monitor_create();
  dispatch_queue_t queue =
      dispatch_queue_create("com.winbolo.netpath", DISPATCH_QUEUE_SERIAL);
  nw_path_monitor_set_queue(s_monitor, queue);
  nw_path_monitor_set_update_handler(s_monitor, ^(nw_path_t path) {
    GameFrontNetPath kind = iosNetPathKind(path);
    int previous = atomic_exchange(&s_netPath, (int)kind);
    if (previous != (int)kind) {
      WB_LOG_INFO(WB_LOG_CAT_NET, "ios net path: %s", iosNetPathName(kind));
    }
  });
  nw_path_monitor_start(s_monitor);
  gameFrontSetNetPathQuery(iosNetPathQuery);
}
