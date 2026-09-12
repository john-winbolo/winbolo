/*
 * INFO_PACKET view-policy byte, and the voice mode in the flags byte.
 *
 * The browser advertisement grew a trailing byte carrying the server's
 * three visibility rules, two bits each, plus the classic-mode flag in
 * bit 6 and the allies-in-trees flag in bit 7, and then a second byte
 * carrying the overview window in bits 0-1 and line of sight in bits
 * 2-3. Two things can break quietly here: the packed layout (a compiler
 * that pads the struct would push every consumer's offsets out), and the
 * length tier each decoder uses — an advertisement that stops before a
 * byte must report the built-in defaults for that byte rather than
 * whatever the short read left behind, and must still report the real
 * values for the bytes it does carry.
 *
 * The voice mode is packed into the top two bits of the separate flags
 * byte, which the single-bit INFO_FLAG_* values share. Two encodings in
 * one byte is the third thing that can break quietly, so the cases below
 * check each side leaves the other alone.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "global.h"
#include "netpacks.h"
#include "view_policy.h"
#include "server_voice_mode.h"
#include "test_harness.h"

int run_info_packet_view_policy_layout(void) {
    UT_ASSERT_MSG(sizeof(INFO_PACKET) == 113,
                  "INFO_PACKET is %zu bytes, want 113", sizeof(INFO_PACKET));
    UT_ASSERT_MSG(offsetof(INFO_PACKET, view_policies) == 111,
                  "view_policies is at offset %zu, want 111",
                  offsetof(INFO_PACKET, view_policies));
    UT_ASSERT_MSG(offsetof(INFO_PACKET, view_policies2) == 112,
                  "view_policies2 is at offset %zu, want 112",
                  offsetof(INFO_PACKET, view_policies2));
    UT_ASSERT_MSG(INFO_PACKET_PRE_VIEWS_SIZE == 111,
                  "INFO_PACKET_PRE_VIEWS_SIZE = %d, want 111",
                  (int)INFO_PACKET_PRE_VIEWS_SIZE);
    UT_ASSERT_MSG(INFO_PACKET_PRE_VIEWS2_SIZE == 112,
                  "INFO_PACKET_PRE_VIEWS2_SIZE = %d, want 112",
                  (int)INFO_PACKET_PRE_VIEWS2_SIZE);

    /* Each category owns its own two bits and nothing bleeds across. */
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyAlways, false, false) == 0x00);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyAlways,
                                         viewPolicyAlways, false, false) == 0x03);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyOff,
                                         viewPolicyAlways, false, false) == 0x0C);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyOff, false, false) == 0x30);

    /* Classic mode owns bit 6 and allies in trees bit 7, each alone. */
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyAlways, true, false) == 0x40);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyAlways, false, true) == 0x80);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyAlways, viewPolicyAlways,
                                         viewPolicyAlways, true, true) == 0xC0);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyOff,
                                         viewPolicyOff, true, false) == 0x7F);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyOff,
                                         viewPolicyOff, false, false) == 0x3F);
    UT_ASSERT(infoPacketPackViewPolicies(viewPolicyOff, viewPolicyOff,
                                         viewPolicyOff, true, true) == 0xFF);

    /* The three policies still read back correctly with both flags set,
     * so neither top bit disturbs bits 0-5. */
    {
        INFO_PACKET pkt;
        ViewPolicy pill, base, ally;
        bool classic, alliesInTrees;

        memset(&pkt, 0, sizeof(pkt));
        pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                       viewPolicyDecay,
                                                       viewPolicyOff, true,
                                                       true);
        infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally,
                                   &classic, &alliesInTrees);
        UT_ASSERT_MSG(pill == viewPolicyKey, "pill = %d, want key", (int)pill);
        UT_ASSERT_MSG(base == viewPolicyDecay, "base = %d, want decay", (int)base);
        UT_ASSERT_MSG(ally == viewPolicyOff, "ally = %d, want off", (int)ally);
        UT_ASSERT(classic);
        UT_ASSERT(alliesInTrees);
    }

    /* The second byte: the overview window owns bits 0-1 and line of
     * sight bits 2-3, every combination survives a round-trip, and the
     * four spare bits stay clear. */
    {
        INFO_PACKET pkt;
        uint8_t window, sight;

        UT_ASSERT(infoPacketPackViewPolicies2((uint8_t)overviewWindowExpanded,
                                              (uint8_t)lineOfSightOff) == 0x00);
        UT_ASSERT(infoPacketPackViewPolicies2((uint8_t)overviewWindowClassic,
                                              (uint8_t)lineOfSightOff) == 0x01);
        UT_ASSERT(infoPacketPackViewPolicies2(
                      (uint8_t)overviewWindowExpanded,
                      (uint8_t)lineOfSightBuildingsAndTrees) == 0x04);
        UT_ASSERT(infoPacketPackViewPolicies2(
                      (uint8_t)overviewWindowClassic,
                      (uint8_t)lineOfSightBuildingsAndTrees) == 0x05);

        memset(&pkt, 0, sizeof(pkt));
        for (int w = 0; w < (int)OVERVIEW_WINDOW_COUNT; w++) {
            for (int sg = 0; sg < (int)LINE_OF_SIGHT_COUNT; sg++) {
                BYTE packed = infoPacketPackViewPolicies2((uint8_t)w,
                                                          (uint8_t)sg);
                UT_ASSERT_MSG((packed & 0xF0u) == 0,
                              "packing %d/%d set a spare bit (0x%02X)",
                              w, sg, (unsigned)packed);
                pkt.view_policies2 = packed;
                infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight);
                UT_ASSERT_MSG((int)window == w && (int)sight == sg,
                              "round-trip %d/%d came back %d/%d",
                              w, sg, (int)window, (int)sight);
            }
        }

        /* The two bytes sit side by side, so neither may disturb the
         * other: both read back their own values with both packed. */
        {
            ViewPolicy pill, base, ally;
            bool classic, alliesInTrees;

            pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                           viewPolicyDecay,
                                                           viewPolicyOff,
                                                           true, true);
            pkt.view_policies2 = infoPacketPackViewPolicies2(
                (uint8_t)overviewWindowClassic,
                (uint8_t)lineOfSightBuildingsAndTrees);
            infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally,
                                       &classic, &alliesInTrees);
            infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight);
            UT_ASSERT(pill == viewPolicyKey && base == viewPolicyDecay &&
                      ally == viewPolicyOff && classic && alliesInTrees);
            UT_ASSERT(window == (uint8_t)overviewWindowClassic &&
                      sight == (uint8_t)lineOfSightBuildingsAndTrees);
        }
    }

    /* The voice mode owns bits 6-7 of the flags byte. On packs as zero,
     * so a server that never sent these bits reads as on. */
    UT_ASSERT(infoPacketPackVoiceMode(serverVoiceOn) == 0x00);
    UT_ASSERT(infoPacketPackVoiceMode(serverVoiceOff) == 0x40);
    UT_ASSERT(infoPacketPackVoiceMode(serverVoiceProximity) == 0x80);
    UT_ASSERT(infoPacketReadVoiceMode(0x00) == serverVoiceOn);
    UT_ASSERT(infoPacketReadVoiceMode(0x40) == serverVoiceOff);
    UT_ASSERT(infoPacketReadVoiceMode(0x80) == serverVoiceProximity);

    /* The fourth value the two bits can hold is not a mode; it reads as
     * on rather than as something the enum does not name. */
    UT_ASSERT_MSG(infoPacketReadVoiceMode(0xC0) == serverVoiceOn,
                  "reserved voice value read back as %d, want on (%d)",
                  (int)infoPacketReadVoiceMode(0xC0), (int)serverVoiceOn);

    /* The two encodings share one byte, so neither may disturb the other:
     * every INFO_FLAG_* bit set alongside a mode still reads that mode
     * back, and packing a mode leaves those flags exactly as they were. */
    {
        const BYTE otherFlags = (BYTE)(INFO_FLAG_ALLOW_NEW_PLAYERS |
                                       INFO_FLAG_LOCKED |
                                       INFO_FLAG_RANKED |
                                       INFO_FLAG_RANDOM_MAP |
                                       INFO_FLAG_ALLOW_SPECTATORS |
                                       INFO_FLAG_IN_LOBBY);
        for (int m = (int)serverVoiceOn; m <= (int)serverVoiceProximity; m++) {
            BYTE packed = infoPacketPackVoiceMode((ServerVoiceMode)m);
            BYTE flags  = (BYTE)(otherFlags | packed);
            UT_ASSERT_MSG((packed & otherFlags) == 0,
                          "voice %d packed 0x%02X into a bit another flag owns",
                          m, (unsigned)packed);
            UT_ASSERT_MSG((BYTE)(flags & ~INFO_FLAG_VOICE_MASK) == otherFlags,
                          "packing voice %d left the other flags as 0x%02X, want 0x%02X",
                          m, (unsigned)(flags & ~INFO_FLAG_VOICE_MASK),
                          (unsigned)otherFlags);
            UT_ASSERT_MSG((int)infoPacketReadVoiceMode(flags) == m,
                          "voice %d read back as %d with every other flag set",
                          m, (int)infoPacketReadVoiceMode(flags));
        }
    }
    return 0;
}

int run_info_packet_view_policy_length_tier(void) {
    INFO_PACKET pkt;
    ViewPolicy pill, base, ally;
    bool classic, alliesInTrees;
    uint8_t window, sight;

    memset(&pkt, 0, sizeof(pkt));
    pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                   viewPolicyDecay,
                                                   viewPolicyOff, true, true);
    pkt.view_policies2 = infoPacketPackViewPolicies2(
        (uint8_t)overviewWindowClassic,
        (uint8_t)lineOfSightBuildingsAndTrees);

    /* Full-length packet: the encoded policies, classic mode and allies
     * in trees all come back out. */
    infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally, &classic,
                               &alliesInTrees);
    UT_ASSERT_MSG(pill == viewPolicyKey, "pill = %d, want key", (int)pill);
    UT_ASSERT_MSG(base == viewPolicyDecay, "base = %d, want decay", (int)base);
    UT_ASSERT_MSG(ally == viewPolicyOff, "ally = %d, want off", (int)ally);
    UT_ASSERT_MSG(classic, "classic mode did not survive the round-trip");
    UT_ASSERT_MSG(alliesInTrees, "allies in trees did not survive the round-trip");
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowClassic,
                  "window = %u, want classic", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightBuildingsAndTrees,
                  "sight = %u, want buildings and trees", (unsigned)sight);

    /* A 112-byte packet carries view_policies but not view_policies2.
     * The five values in the byte it has must still be the real ones:
     * reading them against the full length instead hands every one of
     * them back as a default. */
    infoPacketReadViewPolicies(&pkt, INFO_PACKET_PRE_VIEWS2_SIZE, &pill, &base,
                               &ally, &classic, &alliesInTrees);
    UT_ASSERT_MSG(pill == viewPolicyKey,
                  "112-byte pill = %d, want key", (int)pill);
    UT_ASSERT_MSG(base == viewPolicyDecay,
                  "112-byte base = %d, want decay", (int)base);
    UT_ASSERT_MSG(ally == viewPolicyOff,
                  "112-byte ally = %d, want off", (int)ally);
    UT_ASSERT_MSG(classic, "112-byte packet lost classic mode");
    UT_ASSERT_MSG(alliesInTrees, "112-byte packet lost allies in trees");
    infoPacketReadViewPolicies2(&pkt, INFO_PACKET_PRE_VIEWS2_SIZE,
                                &window, &sight);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowExpanded,
                  "112-byte window = %u, want expanded", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightOff,
                  "112-byte sight = %u, want off", (unsigned)sight);

    /* A 111-byte packet predates the byte: the defaults win, even
     * though the struct still holds a fully populated view_policies. */
    infoPacketReadViewPolicies(&pkt, INFO_PACKET_PRE_VIEWS_SIZE,
                               &pill, &base, &ally, &classic, &alliesInTrees);
    UT_ASSERT_MSG(pill == viewPolicyAlways, "short pill = %d, want always", (int)pill);
    UT_ASSERT_MSG(base == viewPolicyOff, "short base = %d, want off", (int)base);
    UT_ASSERT_MSG(ally == viewPolicyAlways, "short ally = %d, want always", (int)ally);
    UT_ASSERT_MSG(!classic, "short packet reported classic mode on");
    UT_ASSERT_MSG(!alliesInTrees, "short packet reported allies in trees on");

    /* Same for the legacy 76-byte layout and for a NULL packet. */
    infoPacketReadViewPolicies(&pkt, INFO_PACKET_LEGACY_SIZE, &pill, &base, &ally,
                               &classic, &alliesInTrees);
    UT_ASSERT(pill == viewPolicyAlways && base == viewPolicyOff &&
              ally == viewPolicyAlways && !classic && !alliesInTrees);
    infoPacketReadViewPolicies(NULL, sizeof(pkt), &pill, &base, &ally, &classic,
                               &alliesInTrees);
    UT_ASSERT(pill == viewPolicyAlways && base == viewPolicyOff &&
              ally == viewPolicyAlways && !classic && !alliesInTrees);

    /* The second byte defaults the same way at every shorter length. */
    infoPacketReadViewPolicies2(&pkt, INFO_PACKET_PRE_VIEWS_SIZE,
                                &window, &sight);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff);
    infoPacketReadViewPolicies2(&pkt, INFO_PACKET_LEGACY_SIZE, &window, &sight);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff);
    infoPacketReadViewPolicies2(NULL, sizeof(pkt), &window, &sight);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff);

    /* Two bits can hold a value neither enum names. It reads back as the
     * default rather than as a mode the browser has no name for. */
    pkt.view_policies2 = 0x0F;
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowExpanded,
                  "an unnamed window value read back as %u", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightOff,
                  "an unnamed sight value read back as %u", (unsigned)sight);
    /* Each field is clamped on its own, so an unnamed value in one does
     * not drag its named neighbour back to the default with it. */
    pkt.view_policies2 = 0x09;   /* window classic, sight unnamed */
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowClassic,
                  "an unnamed sight value moved the window to %u",
                  (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightOff,
                  "an unnamed sight value read back as %u", (unsigned)sight);
    pkt.view_policies2 = 0x06;   /* window unnamed, sight buildings and trees */
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowExpanded,
                  "an unnamed window value read back as %u", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightBuildingsAndTrees,
                  "an unnamed window value moved sight to %u", (unsigned)sight);
    pkt.view_policies2 = infoPacketPackViewPolicies2(
        (uint8_t)overviewWindowClassic,
        (uint8_t)lineOfSightBuildingsAndTrees);

    /* A full-length packet with both flags clear reports them off. */
    pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                   viewPolicyDecay,
                                                   viewPolicyOff, false, false);
    infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally, &classic,
                               &alliesInTrees);
    UT_ASSERT_MSG(!classic, "classic mode read back on when packed off");
    UT_ASSERT_MSG(!alliesInTrees, "allies in trees read back on when packed off");

    /* Every combination survives a pack/read round-trip, with classic
     * mode and allies in trees each set and clear. */
    for (int p = 0; p <= (int)viewPolicyOff; p++) {
        for (int b = 0; b <= (int)viewPolicyOff; b++) {
            for (int a = 0; a <= (int)viewPolicyOff; a++) {
                for (int c = 0; c <= 1; c++) {
                    for (int t = 0; t <= 1; t++) {
                        pkt.view_policies = infoPacketPackViewPolicies(
                            (ViewPolicy)p, (ViewPolicy)b, (ViewPolicy)a,
                            c != 0, t != 0);
                        infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill,
                                                   &base, &ally, &classic,
                                                   &alliesInTrees);
                        UT_ASSERT_MSG((int)pill == p && (int)base == b &&
                                      (int)ally == a && (int)classic == c &&
                                      (int)alliesInTrees == t,
                                      "round-trip %d/%d/%d/%d/%d came back %d/%d/%d/%d/%d",
                                      p, b, a, c, t,
                                      (int)pill, (int)base, (int)ally,
                                      (int)classic, (int)alliesInTrees);
                    }
                }
            }
        }
    }
    return 0;
}
