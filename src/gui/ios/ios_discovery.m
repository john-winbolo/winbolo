/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*********************************************************
 * Name:          ios_discovery.m
 * Purpose:
 *  The LAN game search on iOS. The raw-socket mDNS browser
 *  in discovery_mdns.c sends to 224.0.0.251 itself, which
 *  iOS allows only with Apple's multicast entitlement. The
 *  system's Bonjour API needs no entitlement for a service
 *  type listed under NSBonjourServices in Info.plist
 *  (_winbolo._udp is), so on iOS discovery runs this browser
 *  in its place (discoverySetMdnsBrowser).
 *
 *  For each _winbolo._udp instance DNSServiceBrowse reports,
 *  DNSServiceResolve gives the game port and the TXT record,
 *  which discoveryServerFromMdns reads with the same key
 *  schema as the raw-socket browser. The address comes from
 *  the instance name: the server's advertiser
 *  (mdns_advertise.c) names each instance
 *  WinBolo-<a>-<b>-<c>-<d>-<port> after its LAN address,
 *  while every server gives the same host name,
 *  winbolo.local, so resolving that name could return
 *  another server's address. The advertiser answers only
 *  questions for _winbolo._udp.local itself and sends its
 *  SRV, A and TXT records along with the PTR answer; the
 *  system keeps those, which is what lets the resolve
 *  succeed. An instance that does not resolve within the
 *  search window is still listed, from its name alone,
 *  without the details the TXT record would add.
 *
 *  The same file advertises a game this device hosts, for
 *  the same reason: mdns_advertise.c answers queries from
 *  its own multicast socket, so on iOS the server hands its
 *  instance name, port and TXT pairs to DNSServiceRegister
 *  instead (mdnsAdvertiseSetPlatform). The system then
 *  answers for the game with this device's own host name
 *  and address records.
 *
 *  Built with ARC (see CMakeLists.txt).
 *********************************************************/

#if !__has_feature(objc_arc)
#error "ios_discovery.m must be compiled with -fobjc-arc"
#endif

#import <Foundation/Foundation.h>
#include <dns_sd.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "discovery.h"
#include "mdns_advertise.h"
#include "ios_discovery.h"
#include "../../common/wb_log.h"

/* The search window, the same as the raw-socket browser's. */
#define IOS_DISCOVERY_SEARCH_MS 5000
/* How often the waiting thread checks for an abort. */
#define IOS_DISCOVERY_POLL_MS 50

/* Most TXT pairs kept from one record; the advertiser writes 23. */
#define IOS_DISCOVERY_TXT_MAX 32

/* Most resolves started in one search. Each resolve is its own connection
 * to mDNSResponder, so without a limit one device announcing thousands of
 * instance names would use up file descriptors and memory. A LAN has far
 * fewer WinBolo games than this. */
#define IOS_DISCOVERY_RESOLVE_MAX 64

/* One search. Touched only on its serial queue, except stopped, which the
 * waiting thread sets inside a dispatch_sync on that queue. */
@interface IosDiscoverySearch : NSObject {
@public
  dispatch_queue_t queue;
  DNSServiceRef browseRef;
  NSMutableArray<NSValue *> *resolveRefs;  /* DNSServiceRef each */
  NSMutableSet<NSString *> *seen;          /* instance names found */
  NSMutableSet<NSString *> *resolved;      /* instance names delivered */
  DiscoveryServerCallback callback;
  void *userData;
  bool stopped;
  int resolveCount;      /* resolves started, up to IOS_DISCOVERY_RESOLVE_MAX */
  bool resolveCapLogged;
}
@end

@implementation IosDiscoverySearch
@end

/* The address and port the advertiser writes into an instance name:
 * WinBolo-<a>-<b>-<c>-<d>-<port>. */
static bool iosDiscoveryParseInstance(const char *name, char *addrOut,
                                      size_t addrCap, unsigned short *portOut) {
  unsigned a, b, c, d, p;
  char tail;
  if (sscanf(name, "WinBolo-%u-%u-%u-%u-%u%c", &a, &b, &c, &d, &p, &tail) != 5) {
    return false;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255 || p == 0 || p > 65535) {
    return false;
  }
  snprintf(addrOut, addrCap, "%u.%u.%u.%u", a, b, c, d);
  *portOut = (unsigned short)p;
  return true;
}

/* Builds the row for one instance from its TXT record (may be empty) and
 * hands it to the caller's callback. */
static void iosDiscoveryDeliver(IosDiscoverySearch *search, NSString *instance,
                                unsigned short port, uint16_t txtLen,
                                const unsigned char *txt) {
  if (search->stopped || [search->resolved containsObject:instance]) {
    return;
  }
  char address[64];
  unsigned short namePort = 0;
  if (!iosDiscoveryParseInstance(instance.UTF8String, address, sizeof(address),
                                 &namePort)) {
    WB_LOG_DEBUG(WB_LOG_CAT_NET, "ios discovery: instance '%s' has no address in its name",
                 instance.UTF8String);
    return;
  }
  if (port == 0) {
    port = namePort;
  }

  /* TXTRecordGetItemAtIndex values are not NUL-terminated, so each is
   * copied into its own buffer. */
  char keyBuf[IOS_DISCOVERY_TXT_MAX][256];
  char valBuf[IOS_DISCOVERY_TXT_MAX][256];
  const char *keys[IOS_DISCOVERY_TXT_MAX];
  const char *values[IOS_DISCOVERY_TXT_MAX];
  size_t count = 0;
  if (txt != NULL && txtLen > 0) {
    uint16_t n = TXTRecordGetCount(txtLen, txt);
    for (uint16_t i = 0; i < n && count < IOS_DISCOVERY_TXT_MAX; i++) {
      uint8_t valueLen = 0;
      const void *value = NULL;
      if (TXTRecordGetItemAtIndex(txtLen, txt, i, sizeof(keyBuf[count]),
                                  keyBuf[count], &valueLen, &value) !=
          kDNSServiceErr_NoError) {
        continue;
      }
      size_t vl = valueLen < sizeof(valBuf[count]) - 1 ? valueLen
                                                       : sizeof(valBuf[count]) - 1;
      if (value != NULL && vl > 0) {
        memcpy(valBuf[count], value, vl);
      }
      valBuf[count][vl] = '\0';
      keys[count] = keyBuf[count];
      values[count] = valBuf[count];
      count++;
    }
  }

  DiscoveryServer server;
  if (discoveryServerFromMdns(address, port, keys, values, count, &server)) {
    [search->resolved addObject:instance];
    search->callback(&server, search->userData);
  }
}

typedef struct {
  void *search;        /* IosDiscoverySearch, retained by the search's array */
  void *instance;      /* NSString, bridged retained */
} IosResolveCtx;

static void DNSSD_API iosDiscoveryResolveReply(DNSServiceRef sdRef,
                                               DNSServiceFlags flags,
                                               uint32_t interfaceIndex,
                                               DNSServiceErrorType errorCode,
                                               const char *fullname,
                                               const char *hosttarget,
                                               uint16_t port,
                                               uint16_t txtLen,
                                               const unsigned char *txtRecord,
                                               void *context) {
  (void)sdRef; (void)flags; (void)interfaceIndex; (void)fullname; (void)hosttarget;
  IosResolveCtx *rc = (IosResolveCtx *)context;
  if (errorCode != kDNSServiceErr_NoError || rc == NULL) {
    return;
  }
  IosDiscoverySearch *search = (__bridge IosDiscoverySearch *)rc->search;
  NSString *instance = (__bridge NSString *)rc->instance;
  iosDiscoveryDeliver(search, instance, ntohs(port), txtLen, txtRecord);
}

static void DNSSD_API iosDiscoveryBrowseReply(DNSServiceRef sdRef,
                                              DNSServiceFlags flags,
                                              uint32_t interfaceIndex,
                                              DNSServiceErrorType errorCode,
                                              const char *serviceName,
                                              const char *regtype,
                                              const char *replyDomain,
                                              void *context) {
  (void)sdRef;
  IosDiscoverySearch *search = (__bridge IosDiscoverySearch *)context;
  if (errorCode != kDNSServiceErr_NoError) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "ios discovery: browse error %d", (int)errorCode);
    return;
  }
  if (search->stopped || serviceName == NULL) {
    return;
  }
  NSString *instance = [NSString stringWithUTF8String:serviceName];
  if (instance == nil) {
    return;
  }
  if ((flags & kDNSServiceFlagsAdd) == 0) {
    /* The instance has gone away: the end-of-search pass must not list it
     * from its name. A row already delivered stays; the browser's callback
     * only adds. If it comes back it is resolved again. */
    [search->seen removeObject:instance];
    return;
  }
  if ([search->seen containsObject:instance]) {
    return;
  }
  if (search->resolveCount >= IOS_DISCOVERY_RESOLVE_MAX) {
    /* Not added to seen either, so the end-of-search pass does not list
     * it from its name. */
    if (!search->resolveCapLogged) {
      WB_LOG_INFO(WB_LOG_CAT_NET,
                  "ios discovery: reached the limit of %d resolves, ignoring "
                  "further instances in this search", IOS_DISCOVERY_RESOLVE_MAX);
      search->resolveCapLogged = true;
    }
    return;
  }
  [search->seen addObject:instance];
  search->resolveCount++;

  IosResolveCtx *rc = (IosResolveCtx *)SDL_calloc(1, sizeof(IosResolveCtx));
  if (rc == NULL) {
    return;
  }
  rc->search = (__bridge void *)search;
  rc->instance = (__bridge_retained void *)instance;
  DNSServiceRef resolveRef = NULL;
  DNSServiceErrorType err = DNSServiceResolve(&resolveRef, 0, interfaceIndex,
                                              serviceName, regtype, replyDomain,
                                              iosDiscoveryResolveReply, rc);
  if (err != kDNSServiceErr_NoError ||
      DNSServiceSetDispatchQueue(resolveRef, search->queue) != kDNSServiceErr_NoError) {
    if (resolveRef != NULL) DNSServiceRefDeallocate(resolveRef);
    CFBridgingRelease(rc->instance);
    SDL_free(rc);
    return;
  }
  /* The ref and its context go together; both are released at the end of
   * the search. */
  [search->resolveRefs addObject:[NSValue valueWithPointer:resolveRef]];
  [search->resolveRefs addObject:[NSValue valueWithPointer:rc]];
}

/* The DiscoveryMdnsBrowser handed to discovery. Blocks for the search
 * window on the caller's worker thread while the queue does the work. */
static bool iosDiscoveryBrowse(DiscoveryServerCallback callback, void *userData) {
  if (callback == NULL) return false;

  IosDiscoverySearch *search = [[IosDiscoverySearch alloc] init];
  search->queue = dispatch_queue_create("WinBolo LAN search", DISPATCH_QUEUE_SERIAL);
  search->resolveRefs = [NSMutableArray array];
  search->seen = [NSMutableSet set];
  search->resolved = [NSMutableSet set];
  search->callback = callback;
  search->userData = userData;
  search->stopped = false;
  search->resolveCount = 0;
  search->resolveCapLogged = false;

  __block bool started = false;
  dispatch_sync(search->queue, ^{
    DNSServiceRef ref = NULL;
    DNSServiceErrorType err = DNSServiceBrowse(&ref, 0, kDNSServiceInterfaceIndexAny,
                                               "_winbolo._udp", "local.",
                                               iosDiscoveryBrowseReply,
                                               (__bridge void *)search);
    if (err == kDNSServiceErr_NoError &&
        DNSServiceSetDispatchQueue(ref, search->queue) == kDNSServiceErr_NoError) {
      search->browseRef = ref;
      started = true;
    } else {
      WB_LOG_WARN(WB_LOG_CAT_NET, "ios discovery: DNSServiceBrowse failed (%d)", (int)err);
      if (ref != NULL) DNSServiceRefDeallocate(ref);
    }
  });
  if (!started) {
    return false;
  }

  Uint64 start = SDL_GetTicks();
  while (SDL_GetTicks() - start < IOS_DISCOVERY_SEARCH_MS) {
    if (discoveryMdnsAbortRequested()) {
      WB_LOG_DEBUG(WB_LOG_CAT_NET, "ios discovery: search aborted by caller");
      break;
    }
    SDL_Delay(IOS_DISCOVERY_POLL_MS);
  }

  /* Stop on the queue, so no reply runs after this: list what never
   * resolved from its name alone, then release every ref. */
  dispatch_sync(search->queue, ^{
    if (!discoveryMdnsAbortRequested()) {
      for (NSString *instance in search->seen) {
        iosDiscoveryDeliver(search, instance, 0, 0, NULL);
      }
    }
    search->stopped = true;
    if (search->browseRef != NULL) {
      DNSServiceRefDeallocate(search->browseRef);
      search->browseRef = NULL;
    }
    NSArray<NSValue *> *refs = search->resolveRefs;
    for (NSUInteger i = 0; i + 1 < refs.count; i += 2) {
      DNSServiceRef ref = (DNSServiceRef)refs[i].pointerValue;
      IosResolveCtx *rc = (IosResolveCtx *)refs[i + 1].pointerValue;
      DNSServiceRefDeallocate(ref);
      CFBridgingRelease(rc->instance);
      SDL_free(rc);
    }
    [search->resolveRefs removeAllObjects];
  });
  return true;
}

/* ---- Advertising a hosted game ----
 *
 * One registration at a time, as the server runs one advertiser. Every
 * DNSService call on it runs on its serial queue; the hooks are called from
 * the server tick on the host timer thread, which waits for the queue on
 * start and stop and hands it a copy of the record on update. */

static dispatch_queue_t s_advertiseQueue;
static DNSServiceRef s_advertiseRef;

/* The TXT record from the server's pairs, as DNS-SD bytes. NULL when a
 * pair does not fit. */
static NSData *iosAdvertiseTxt(const char *const *keys, const char *const *values,
                               size_t count) {
  TXTRecordRef txt;
  TXTRecordCreate(&txt, 0, NULL);
  for (size_t i = 0; i < count; i++) {
    size_t len = strlen(values[i]);
    if (len > 255 ||
        TXTRecordSetValue(&txt, keys[i], (uint8_t)len, values[i]) !=
            kDNSServiceErr_NoError) {
      TXTRecordDeallocate(&txt);
      return nil;
    }
  }
  NSData *data = [NSData dataWithBytes:TXTRecordGetBytesPtr(&txt)
                                length:TXTRecordGetLength(&txt)];
  TXTRecordDeallocate(&txt);
  return data;
}

static void DNSSD_API iosAdvertiseRegisterReply(DNSServiceRef sdRef,
                                                DNSServiceFlags flags,
                                                DNSServiceErrorType errorCode,
                                                const char *name,
                                                const char *regtype,
                                                const char *domain,
                                                void *context) {
  (void)sdRef; (void)flags; (void)regtype; (void)domain; (void)context;
  if (errorCode == kDNSServiceErr_NoError) {
    WB_LOG_INFO(WB_LOG_CAT_NET, "ios advertise: registered '%s'",
                name != NULL ? name : "");
  } else {
    WB_LOG_WARN(WB_LOG_CAT_NET, "ios advertise: registration failed (%d)",
                (int)errorCode);
  }
}

static bool iosAdvertiseStart(const char *instance, unsigned short port,
                              const char *ifName, const char *const *keys,
                              const char *const *values, size_t count) {
  NSData *txt = iosAdvertiseTxt(keys, values, count);
  if (txt == nil || instance == NULL) {
    WB_LOG_WARN(WB_LOG_CAT_NET, "ios advertise: the TXT record did not fit");
    return false;
  }
  /* On the interface the advertised address belongs to, so the address in
   * the instance name and the one the system answers with agree. */
  uint32_t ifIndex = (ifName != NULL && ifName[0] != '\0') ? if_nametoindex(ifName) : 0;
  if (s_advertiseQueue == NULL) {
    s_advertiseQueue = dispatch_queue_create("WinBolo LAN advertise",
                                             DISPATCH_QUEUE_SERIAL);
  }
  __block bool started = false;
  dispatch_sync(s_advertiseQueue, ^{
    if (s_advertiseRef != NULL) {
      DNSServiceRefDeallocate(s_advertiseRef);
      s_advertiseRef = NULL;
    }
    DNSServiceRef ref = NULL;
    /* No auto-rename: the name carries the address and port the LAN search
     * reads, and a renamed instance would not parse. */
    DNSServiceErrorType err = DNSServiceRegister(
        &ref, kDNSServiceFlagsNoAutoRename, ifIndex, instance, "_winbolo._udp",
        NULL, NULL, htons(port), (uint16_t)txt.length, txt.bytes,
        iosAdvertiseRegisterReply, NULL);
    if (err == kDNSServiceErr_NoError &&
        DNSServiceSetDispatchQueue(ref, s_advertiseQueue) == kDNSServiceErr_NoError) {
      s_advertiseRef = ref;
      started = true;
    } else {
      WB_LOG_WARN(WB_LOG_CAT_NET, "ios advertise: DNSServiceRegister failed (%d)",
                  (int)err);
      if (ref != NULL) DNSServiceRefDeallocate(ref);
    }
  });
  return started;
}

static void iosAdvertiseUpdate(const char *const *keys, const char *const *values,
                               size_t count) {
  NSData *txt = iosAdvertiseTxt(keys, values, count);
  if (txt == nil || s_advertiseQueue == NULL) {
    return;
  }
  dispatch_async(s_advertiseQueue, ^{
    if (s_advertiseRef == NULL) {
      return;
    }
    /* NULL record: the registration's own TXT record. */
    DNSServiceErrorType err = DNSServiceUpdateRecord(
        s_advertiseRef, NULL, 0, (uint16_t)txt.length, txt.bytes, 0);
    if (err != kDNSServiceErr_NoError) {
      WB_LOG_WARN(WB_LOG_CAT_NET, "ios advertise: TXT update failed (%d)", (int)err);
    }
  });
}

static void iosAdvertiseStop(void) {
  if (s_advertiseQueue == NULL) {
    return;
  }
  dispatch_sync(s_advertiseQueue, ^{
    if (s_advertiseRef != NULL) {
      DNSServiceRefDeallocate(s_advertiseRef);
      s_advertiseRef = NULL;
      WB_LOG_INFO(WB_LOG_CAT_NET, "ios advertise: stopped");
    }
  });
}

static const MdnsPlatformAdvertiser kIosAdvertiser = {
  iosAdvertiseStart,
  iosAdvertiseUpdate,
  iosAdvertiseStop,
};

void iosDiscoveryInstall(void) {
  discoverySetMdnsBrowser(iosDiscoveryBrowse);
  mdnsAdvertiseSetPlatform(&kIosAdvertiser);
}
