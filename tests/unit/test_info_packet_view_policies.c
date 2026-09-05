/*
 * INFO_PACKET view-policy byte.
 *
 * The browser advertisement grew a trailing byte carrying the server's
 * three visibility rules, two bits each, plus the classic-mode flag in
 * bit 6. Two things can break quietly here: the packed layout (a
 * compiler that pads the struct would push every consumer's offsets
 * out), and the length tier the decoder uses — an advertisement that
 * stops before the new byte must report the built-in defaults rather
 * than whatever the short read left behind.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "netpacks.h"
#include "view_policy.h"
#include "test_harness.h"

int run_info_packet_view_policy_layout(void) {
    UT_ASSERT_MSG(sizeof(INFO_PACKET) == 112,
                  "INFO_PACKET is %zu bytes, want 112", sizeof(INFO_PACKET));
    UT_ASSERT_MSG(offsetof(INFO_PACKET, view_policies) == 111,
                  "view_policies is at offset %zu, want 111",
                  offsetof(INFO_PACKET, view_policies));
    UT_ASSERT_MSG(INFO_PACKET_PRE_VIEWS_SIZE == 111,
                  "INFO_PACKET_PRE_VIEWS_SIZE = %d, want 111",
                  (int)INFO_PACKET_PRE_VIEWS_SIZE);

    /* Each category owns its own two bits and nothing bleeds across. */
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyAlways, false) == 0x00);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyAlways,
                                         viewPolicyAlways, false) == 0x03);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyOff,
                                         viewPolicyAlways, false) == 0x0C);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyOff, false) == 0x30);

    /* Classic mode owns bit 6 alone; bit 7 stays clear either way. */
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyAlways, true) == 0x40);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyOff,
                                         viewPolicyOff, true) == 0x7F);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyOff,
                                         viewPolicyOff, false) == 0x3F);

    /* The three policies still read back correctly with classic set, so
     * the new bit leaves bits 0-5 alone. */
    {
        INFO_PACKET pkt;
        ViewPolicy pill, base, ally;
        bool classic;

        memset(&pkt, 0, sizeof(pkt));
        pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                       viewPolicyDecay,
                                                       viewPolicyOff, true);
        infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally,
                                   &classic);
        UT_ASSERT_MSG(pill == viewPolicyKey, "pill = %d, want key", (int)pill);
        UT_ASSERT_MSG(base == viewPolicyDecay, "base = %d, want decay", (int)base);
        UT_ASSERT_MSG(ally == viewPolicyOff, "ally = %d, want off", (int)ally);
        UT_ASSERT(classic);
    }
    return 0;
}

int run_info_packet_view_policy_length_tier(void) {
    INFO_PACKET pkt;
    ViewPolicy pill, base, ally;
    bool classic;

    memset(&pkt, 0, sizeof(pkt));
    pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                   viewPolicyDecay,
                                                   viewPolicyOff, true);

    /* Full-length packet: the encoded policies and classic mode come
     * back out. */
    infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally, &classic);
    UT_ASSERT_MSG(pill == viewPolicyKey, "pill = %d, want key", (int)pill);
    UT_ASSERT_MSG(base == viewPolicyDecay, "base = %d, want decay", (int)base);
    UT_ASSERT_MSG(ally == viewPolicyOff, "ally = %d, want off", (int)ally);
    UT_ASSERT_MSG(classic, "classic mode did not survive the round-trip");

    /* A 111-byte packet predates the byte: the defaults win, even
     * though the struct still holds a fully populated view_policies. */
    infoPacketReadViewPolicies(&pkt, INFO_PACKET_PRE_VIEWS_SIZE,
                               &pill, &base, &ally, &classic);
    UT_ASSERT_MSG(pill == viewPolicyAlways, "short pill = %d, want always", (int)pill);
    UT_ASSERT_MSG(base == viewPolicyOff, "short base = %d, want off", (int)base);
    UT_ASSERT_MSG(ally == viewPolicyAlways, "short ally = %d, want always", (int)ally);
    UT_ASSERT_MSG(!classic, "short packet reported classic mode on");

    /* Same for the legacy 76-byte layout and for a NULL packet. */
    infoPacketReadViewPolicies(&pkt, INFO_PACKET_LEGACY_SIZE, &pill, &base, &ally,
                               &classic);
    UT_ASSERT(pill == viewPolicyAlways && base == viewPolicyOff &&
              ally == viewPolicyAlways && !classic);
    infoPacketReadViewPolicies(NULL, sizeof(pkt), &pill, &base, &ally, &classic);
    UT_ASSERT(pill == viewPolicyAlways && base == viewPolicyOff &&
              ally == viewPolicyAlways && !classic);

    /* A full-length packet with classic mode clear reports it off. */
    pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                   viewPolicyDecay,
                                                   viewPolicyOff, false);
    infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally, &classic);
    UT_ASSERT_MSG(!classic, "classic mode read back on when packed off");

    /* Every combination survives a pack/read round-trip, with classic
     * mode set and clear. */
    for (int p = 0; p <= (int)viewPolicyOff; p++) {
        for (int b = 0; b <= (int)viewPolicyOff; b++) {
            for (int a = 0; a <= (int)viewPolicyOff; a++) {
                for (int c = 0; c <= 1; c++) {
                    pkt.view_policies = infoPacketPackViewPolicies(
                        (ViewPolicy)p, (ViewPolicy)b, (ViewPolicy)a, c != 0);
                    infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base,
                                               &ally, &classic);
                    UT_ASSERT_MSG((int)pill == p && (int)base == b &&
                                  (int)ally == a && (int)classic == c,
                                  "round-trip %d/%d/%d/%d came back %d/%d/%d/%d",
                                  p, b, a, c,
                                  (int)pill, (int)base, (int)ally, (int)classic);
                }
            }
        }
    }
    return 0;
}
