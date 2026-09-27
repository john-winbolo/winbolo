/*
 * The mDNS view TXT key (test_mdns_view_txt.c).
 *
 * The LAN record carries the server's visibility rules in one key,
 * view=<4 hex chars>: the three view policies with the classic-mode and
 * allies-in-trees flags in the first byte, the overview window, line of
 * sight and the positional-sound flag in the second. This drives the
 * browser's parse seam directly — no socket — packing the bytes the way
 * mdns_advertise.c packs them and handing the formatted value to
 * discoveryMdnsFillServer.
 *
 * Two things are pinned. All eight values survive the trip, and a record
 * whose value is missing or malformed reports the defaults instead of
 * whatever the failed parse left behind. The base view is the one the
 * zero-fill gets wrong — viewPolicyOff is 3, not 0 — so it is asserted
 * in every default case.
 */

#include <stdio.h>
#include <string.h>

#include "global.h"
#include "netpacks.h"         /* infoPacketPackViewPolicies / ...2 */
#include "view_policy.h"
#include "discovery.h"        /* DiscoveryServer */
#include "discovery_mdns.h"   /* DiscoveryMdnsResolved + the parse seam */
#include "test_harness.h"

/* A resolved record carrying one view value, or none when viewVal is
 * NULL. haveSrv is what makes discoveryMdnsFillServer accept it. */
static void resolvedWithView(DiscoveryMdnsResolved *r, const char *viewVal) {
    memset(r, 0, sizeof(*r));
    r->haveSrv = true;
    r->port    = 27500;
    if (viewVal != NULL) {
        snprintf(r->txt[0].key, sizeof(r->txt[0].key), "%s", "view");
        snprintf(r->txt[0].value, sizeof(r->txt[0].value), "%s", viewVal);
        r->txtCount = 1;
    }
}

/* Every value at its default, which is what an absent or unreadable key
 * means. */
static int isAllDefault(const DiscoveryServer *s) {
    return s->pillView == viewPolicyAlways &&
           s->baseView == viewPolicyOff &&
           s->allyView == viewPolicyAlways &&
           s->classicMode == false &&
           s->alliesInTrees == false &&
           s->overviewWindow == (uint8_t)overviewWindowExpanded &&
           s->lineOfSight == (uint8_t)lineOfSightOff &&
           s->positionalSound == false;
}

int run_mdns_view_txt_roundtrip(void) {
    DiscoveryMdnsResolved r;
    DiscoveryServer s;
    char value[8];

    /* Eight values, none of them the default, packed and formatted the
     * way the advertiser does it. */
    snprintf(value, sizeof(value), "%02X%02X",
             (unsigned)infoPacketPackViewPolicies(viewPolicyKey,
                                                  viewPolicyAlways,
                                                  viewPolicyOff, true, true),
             (unsigned)infoPacketPackViewPolicies2(
                 (uint8_t)overviewWindowClassic,
                 (uint8_t)lineOfSightBuildingsAndTrees, true));
    UT_ASSERT_MSG(strlen(value) == 4, "view value '%s' is not four hex chars",
                  value);

    resolvedWithView(&r, value);
    memset(&s, 0, sizeof(s));
    UT_ASSERT(discoveryMdnsFillServer(&r, &s));
    UT_ASSERT_MSG(s.pillView == viewPolicyKey,
                  "pill = %d, want key", (int)s.pillView);
    UT_ASSERT_MSG(s.baseView == viewPolicyAlways,
                  "base = %d, want always", (int)s.baseView);
    UT_ASSERT_MSG(s.allyView == viewPolicyOff,
                  "ally = %d, want off", (int)s.allyView);
    UT_ASSERT_MSG(s.classicMode, "classic mode did not survive the key");
    UT_ASSERT_MSG(s.alliesInTrees, "allies in trees did not survive the key");
    UT_ASSERT_MSG(s.overviewWindow == (uint8_t)overviewWindowClassic,
                  "window = %u, want classic", (unsigned)s.overviewWindow);
    UT_ASSERT_MSG(s.lineOfSight == (uint8_t)lineOfSightBuildingsAndTrees,
                  "sight = %u, want buildings and trees",
                  (unsigned)s.lineOfSight);
    UT_ASSERT_MSG(s.positionalSound,
                  "positional sound did not survive the key");

    /* Bit 4 on its own, with every other rule at its default: the flag
     * reads back on and nothing else moves. */
    snprintf(value, sizeof(value), "%02X%02X",
             (unsigned)infoPacketPackViewPolicies(viewPolicyAlways,
                                                  viewPolicyOff,
                                                  viewPolicyAlways,
                                                  false, false),
             (unsigned)infoPacketPackViewPolicies2(
                 (uint8_t)overviewWindowExpanded, (uint8_t)lineOfSightOff,
                 true));
    UT_ASSERT_MSG(strcmp(value, "0C10") == 0, "view value '%s', want 0C10",
                  value);
    resolvedWithView(&r, value);
    memset(&s, 0, sizeof(s));
    UT_ASSERT(discoveryMdnsFillServer(&r, &s));
    UT_ASSERT_MSG(s.positionalSound, "view=0C10 read positional sound off");
    UT_ASSERT(s.pillView == viewPolicyAlways && s.baseView == viewPolicyOff &&
              s.allyView == viewPolicyAlways && !s.classicMode &&
              !s.alliesInTrees &&
              s.overviewWindow == (uint8_t)overviewWindowExpanded &&
              s.lineOfSight == (uint8_t)lineOfSightOff);

    /* A server running the defaults still sends a key — the base view's
     * viewPolicyOff is not zero, so the value is not four zeros — and it
     * reads back as the defaults. */
    snprintf(value, sizeof(value), "%02X%02X",
             (unsigned)infoPacketPackViewPolicies(viewPolicyAlways,
                                                  viewPolicyOff,
                                                  viewPolicyAlways,
                                                  false, false),
             (unsigned)infoPacketPackViewPolicies2(
                 (uint8_t)overviewWindowExpanded, (uint8_t)lineOfSightOff,
                 false));
    resolvedWithView(&r, value);
    memset(&s, 0, sizeof(s));
    UT_ASSERT(discoveryMdnsFillServer(&r, &s));
    UT_ASSERT_MSG(isAllDefault(&s),
                  "a default server read back %d/%d/%d/%d/%d/%u/%u",
                  (int)s.pillView, (int)s.baseView, (int)s.allyView,
                  (int)s.classicMode, (int)s.alliesInTrees,
                  (unsigned)s.overviewWindow, (unsigned)s.lineOfSight);

    /* No key at all: the record predates it, or comes from something
     * else entirely. The defaults are what that means. */
    resolvedWithView(&r, NULL);
    memset(&s, 0, sizeof(s));
    UT_ASSERT(discoveryMdnsFillServer(&r, &s));
    UT_ASSERT_MSG(isAllDefault(&s),
                  "a record with no view key read back %d/%d/%d/%d/%d/%u/%u",
                  (int)s.pillView, (int)s.baseView, (int)s.allyView,
                  (int)s.classicMode, (int)s.alliesInTrees,
                  (unsigned)s.overviewWindow, (unsigned)s.lineOfSight);

    /* An empty, short or non-hex value cannot say anything about the
     * server, so each leaves the defaults where they are rather than
     * reporting half a byte. */
    {
        const char *bad[] = { "", "0F", "zzzz", "0Fzz", "  " };
        size_t i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            resolvedWithView(&r, bad[i]);
            memset(&s, 0, sizeof(s));
            UT_ASSERT(discoveryMdnsFillServer(&r, &s));
            UT_ASSERT_MSG(isAllDefault(&s),
                          "view='%s' read back %d/%d/%d/%d/%d/%u/%u",
                          bad[i], (int)s.pillView, (int)s.baseView,
                          (int)s.allyView, (int)s.classicMode,
                          (int)s.alliesInTrees, (unsigned)s.overviewWindow,
                          (unsigned)s.lineOfSight);
        }
    }

    return 0;
}
