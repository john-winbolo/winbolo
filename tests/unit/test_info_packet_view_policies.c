/*
 * INFO_PACKET view-policy byte, and the voice mode in the flags byte.
 *
 * The browser advertisement grew a trailing byte carrying the server's
 * three visibility rules, two bits each, plus the classic-mode flag in
 * bit 6 and the allies-in-trees flag in bit 7, and then a second byte
 * carrying the overview window in bits 0-1, line of sight in bits 2-3
 * and the positional-sound flag in bit 4. Two things can break quietly here: the packed layout (a compiler
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
#include "visibility_presets.h"
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

    /* The second byte: the overview window owns bits 0-1, line of sight
     * bits 2-3 and positional sound bit 4, every combination survives a
     * round-trip, and the three spare bits stay clear. */
    {
        INFO_PACKET pkt;
        uint8_t window, sight;
        bool sound;

        UT_ASSERT(infoPacketPackViewPolicies2((uint8_t)overviewWindowExpanded,
                                              (uint8_t)lineOfSightOff,
                                              false) == 0x00);
        UT_ASSERT(infoPacketPackViewPolicies2((uint8_t)overviewWindowClassic,
                                              (uint8_t)lineOfSightOff,
                                              false) == 0x01);
        UT_ASSERT(infoPacketPackViewPolicies2(
                      (uint8_t)overviewWindowExpanded,
                      (uint8_t)lineOfSightBuildingsAndTrees, false) == 0x04);
        UT_ASSERT(infoPacketPackViewPolicies2(
                      (uint8_t)overviewWindowClassic,
                      (uint8_t)lineOfSightBuildingsAndTrees, false) == 0x05);

        /* Positional sound owns bit 4 alone, and sets it beside either
         * mode without moving them. */
        UT_ASSERT(infoPacketPackViewPolicies2((uint8_t)overviewWindowExpanded,
                                              (uint8_t)lineOfSightOff,
                                              true) == 0x10);
        UT_ASSERT(infoPacketPackViewPolicies2(
                      (uint8_t)overviewWindowClassic,
                      (uint8_t)lineOfSightBuildingsAndTrees, true) == 0x15);

        memset(&pkt, 0, sizeof(pkt));
        for (int w = 0; w < (int)OVERVIEW_WINDOW_COUNT; w++) {
            for (int sg = 0; sg < (int)LINE_OF_SIGHT_COUNT; sg++) {
                for (int ps = 0; ps <= 1; ps++) {
                    BYTE packed = infoPacketPackViewPolicies2((uint8_t)w,
                                                              (uint8_t)sg,
                                                              ps != 0);
                    UT_ASSERT_MSG((packed & 0xE0u) == 0,
                                  "packing %d/%d/%d set a spare bit (0x%02X)",
                                  w, sg, ps, (unsigned)packed);
                    UT_ASSERT_MSG(((packed & 0x10u) != 0) == (ps != 0),
                                  "packing %d/%d/%d left bit 4 as %d",
                                  w, sg, ps, (int)((packed >> 4) & 1u));
                    pkt.view_policies2 = packed;
                    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window,
                                                &sight, &sound);
                    UT_ASSERT_MSG((int)window == w && (int)sight == sg &&
                                  (int)sound == ps,
                                  "round-trip %d/%d/%d came back %d/%d/%d",
                                  w, sg, ps, (int)window, (int)sight,
                                  (int)sound);
                }
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
                (uint8_t)lineOfSightBuildingsAndTrees, true);
            infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally,
                                       &classic, &alliesInTrees);
            infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight,
                                        &sound);
            UT_ASSERT(pill == viewPolicyKey && base == viewPolicyDecay &&
                      ally == viewPolicyOff && classic && alliesInTrees);
            UT_ASSERT(window == (uint8_t)overviewWindowClassic &&
                      sight == (uint8_t)lineOfSightBuildingsAndTrees &&
                      sound);
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
    bool sound;

    memset(&pkt, 0, sizeof(pkt));
    pkt.view_policies = infoPacketPackViewPolicies(viewPolicyKey,
                                                   viewPolicyDecay,
                                                   viewPolicyOff, true, true);
    pkt.view_policies2 = infoPacketPackViewPolicies2(
        (uint8_t)overviewWindowClassic,
        (uint8_t)lineOfSightBuildingsAndTrees, true);

    /* Full-length packet: the encoded policies, classic mode and allies
     * in trees all come back out. */
    infoPacketReadViewPolicies(&pkt, sizeof(pkt), &pill, &base, &ally, &classic,
                               &alliesInTrees);
    UT_ASSERT_MSG(pill == viewPolicyKey, "pill = %d, want key", (int)pill);
    UT_ASSERT_MSG(base == viewPolicyDecay, "base = %d, want decay", (int)base);
    UT_ASSERT_MSG(ally == viewPolicyOff, "ally = %d, want off", (int)ally);
    UT_ASSERT_MSG(classic, "classic mode did not survive the round-trip");
    UT_ASSERT_MSG(alliesInTrees, "allies in trees did not survive the round-trip");
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowClassic,
                  "window = %u, want classic", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightBuildingsAndTrees,
                  "sight = %u, want buildings and trees", (unsigned)sight);
    UT_ASSERT_MSG(sound, "positional sound did not survive the round-trip");

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
                                &window, &sight, &sound);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowExpanded,
                  "112-byte window = %u, want expanded", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightOff,
                  "112-byte sight = %u, want off", (unsigned)sight);
    UT_ASSERT_MSG(!sound, "112-byte packet reported positional sound on");

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
                                &window, &sight, &sound);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff && !sound);
    infoPacketReadViewPolicies2(&pkt, INFO_PACKET_LEGACY_SIZE, &window, &sight,
                                &sound);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff && !sound);
    infoPacketReadViewPolicies2(NULL, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff && !sound);
    /* Every out-pointer may be NULL. */
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), NULL, NULL, NULL);

    /* Two bits can hold a value neither enum names. It reads back as the
     * default rather than as a mode the browser has no name for. */
    pkt.view_policies2 = 0x0F;
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowExpanded,
                  "an unnamed window value read back as %u", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightOff,
                  "an unnamed sight value read back as %u", (unsigned)sight);
    UT_ASSERT_MSG(!sound, "unnamed modes set positional sound");
    /* The same unnamed modes beside bit 4: the flag is read on its own
     * and the clamp on the modes does not clear it. */
    pkt.view_policies2 = 0x1F;
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT(window == (uint8_t)overviewWindowExpanded &&
              sight == (uint8_t)lineOfSightOff);
    UT_ASSERT_MSG(sound, "unnamed modes cleared positional sound");
    /* Each field is clamped on its own, so an unnamed value in one does
     * not drag its named neighbour back to the default with it. */
    pkt.view_policies2 = 0x09;   /* window classic, sight unnamed */
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowClassic,
                  "an unnamed sight value moved the window to %u",
                  (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightOff,
                  "an unnamed sight value read back as %u", (unsigned)sight);
    pkt.view_policies2 = 0x07;   /* window unnamed, sight buildings and trees */
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowExpanded,
                  "an unnamed window value read back as %u", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightBuildingsAndTrees,
                  "an unnamed window value moved sight to %u", (unsigned)sight);
    /* Two is the third window, not an unnamed value, and the two bits it
     * rides in were always wide enough for it. Pinned here because it is on
     * the wire: a browser row has to read None back as None. */
    pkt.view_policies2 = 0x06;   /* window none, sight buildings and trees */
    infoPacketReadViewPolicies2(&pkt, sizeof(pkt), &window, &sight, &sound);
    UT_ASSERT_MSG(window == (uint8_t)overviewWindowNone,
                  "the none window read back as %u", (unsigned)window);
    UT_ASSERT_MSG(sight == (uint8_t)lineOfSightBuildingsAndTrees,
                  "the none window moved sight to %u", (unsigned)sight);
    pkt.view_policies2 = infoPacketPackViewPolicies2(
        (uint8_t)overviewWindowClassic,
        (uint8_t)lineOfSightBuildingsAndTrees, true);

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

/* ── The name a browser row puts on a listed game ─────────────────
 * The server browser reads a game's rules out of the INFO packet and runs
 * them through the same preset table the lobby's dropdown uses, so a game
 * is named the same before you join it as after. That only holds if every
 * field the match needs survives the packet, which is what this pins: each
 * preset is packed into the two view bytes, read back out, and matched.
 *
 * The decay seconds are not in the packet and take no part in the match,
 * so the round trip is exact without them. */
static void packPreset(INFO_PACKET *pkt, const VisibilitySettings *v) {
    memset(pkt, 0, sizeof(*pkt));
    pkt->view_policies = infoPacketPackViewPolicies(
        (ViewPolicy)v->policy[viewCategoryPill],
        (ViewPolicy)v->policy[viewCategoryBase],
        (ViewPolicy)v->policy[viewCategoryAlly],
        v->classicMode, v->alliesInTrees);
    pkt->view_policies2 = infoPacketPackViewPolicies2(v->overviewWindow,
                                                      v->lineOfSight,
                                                      v->positionalSound);
}

static VisibilitySettings readPacket2(const INFO_PACKET *pkt, size_t len) {
    VisibilitySettings out;
    ViewPolicy pill, base, ally;
    bool classic, trees;
    uint8_t window, sight;
    bool sound;

    memset(&out, 0, sizeof(out));
    infoPacketReadViewPolicies(pkt, len, &pill, &base, &ally,
                               &classic, &trees);
    infoPacketReadViewPolicies2(pkt, len, &window, &sight, &sound);
    out.policy[viewCategoryPill] = (uint8_t)pill;
    out.policy[viewCategoryBase] = (uint8_t)base;
    out.policy[viewCategoryAlly] = (uint8_t)ally;
    out.classicMode    = classic;
    out.alliesInTrees  = trees;
    out.overviewWindow = window;
    out.lineOfSight    = sight;
    out.positionalSound = sound;
    return out;
}

static VisibilitySettings readPacket(const INFO_PACKET *pkt) {
    return readPacket2(pkt, sizeof(*pkt));
}

int run_info_packet_preset_round_trip(void) {
    INFO_PACKET pkt;
    VisibilitySettings want;
    VisibilitySettings got;
    int p;

    for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
        UT_ASSERT(visibilityPresetSettings((VisibilityPreset)p, &want));
        packPreset(&pkt, &want);
        got = readPacket(&pkt);
        UT_ASSERT_MSG(visibilityPresetMatch(&got) == (VisibilityPreset)p,
                      "preset %d came back off the wire as %d", p,
                      (int)visibilityPresetMatch(&got));
    }

    /* And a set that is none of them still reads as Custom rather than
     * being rounded to the nearest preset by a field the packet dropped. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetMaxView, &want));
    want.policy[viewCategoryBase] = (uint8_t)viewPolicyOff;
    packPreset(&pkt, &want);
    got = readPacket(&pkt);
    UT_ASSERT_MSG(visibilityPresetMatch(&got) == visibilityPresetCustom,
                  "a hand-made set came back off the wire as %d",
                  (int)visibilityPresetMatch(&got));

    /* Allies in trees is the field most easily lost - it is a single bit
     * beside classic mode rather than a value of its own - so it is shown
     * to survive on its own account: Max view with it off is Custom. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetMaxView, &want));
    want.alliesInTrees = false;
    packPreset(&pkt, &want);
    got = readPacket(&pkt);
    UT_ASSERT(visibilityPresetMatch(&got) == visibilityPresetCustom);

    /* Positional sound is a single bit too: Max view with it off is
     * Custom, so the bit really is carried rather than assumed. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetMaxView, &want));
    UT_ASSERT(want.positionalSound);
    want.positionalSound = false;
    packPreset(&pkt, &want);
    got = readPacket(&pkt);
    UT_ASSERT(!got.positionalSound);
    UT_ASSERT(visibilityPresetMatch(&got) == visibilityPresetCustom);

    return 0;
}

/* ── A listing that advertised nothing reads as Classic ───────────
 * The browser names a game by running its advertised rules through the
 * preset table, but an old server sends no rules at all and the decoder
 * fills the back-compatibility set in their place. That set matches no
 * preset, so naming it by the match would call such a server "Custom" —
 * a word that reads as a host having chosen something. The browser names
 * it Classic instead, which is what it plays like, and asks hasViewInfo
 * rather than the values to tell the two apart.
 *
 * This pins the two halves of that: the back-compatibility set really
 * does match nothing (so the fallback is load-bearing, not decoration),
 * and the length that decides hasViewInfo is the one the view byte needs.
 */
int run_info_packet_absent_views_read_classic(void) {
    INFO_PACKET pkt;
    VisibilitySettings got;

    /* A packet too short for the view byte. Both readers hand back their
     * back-compatibility answers. */
    memset(&pkt, 0, sizeof(pkt));
    got = readPacket2(&pkt, (size_t)INFO_PACKET_PRE_VIEWS_SIZE);
    UT_ASSERT_MSG(got.policy[viewCategoryPill] == (uint8_t)viewPolicyAlways &&
                  got.policy[viewCategoryBase] == (uint8_t)viewPolicyOff &&
                  got.policy[viewCategoryAlly] == (uint8_t)viewPolicyAlways &&
                  !got.classicMode && !got.alliesInTrees &&
                  got.overviewWindow == (uint8_t)overviewWindowExpanded &&
                  got.lineOfSight == (uint8_t)lineOfSightOff &&
                  !got.positionalSound,
                  "the back-compatibility reading moved");
    UT_ASSERT_MSG(visibilityPresetMatch(&got) == visibilityPresetCustom,
                  "the back-compatibility set matched preset %d, so the "
                  "browser's Classic fallback would never be reached",
                  (int)visibilityPresetMatch(&got));

    /* One byte short of the view byte is still nothing advertised; the
     * full length is the first that carries an answer. */
    UT_ASSERT(INFO_PACKET_PRE_VIEWS2_SIZE > INFO_PACKET_PRE_VIEWS_SIZE);

    /* And with the bytes present the label comes from the match, not from
     * the fallback: every preset survives the round trip. */
    {
        VisibilitySettings want;
        int p;
        for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
            UT_ASSERT(visibilityPresetSettings((VisibilityPreset)p, &want));
            packPreset(&pkt, &want);
            got = readPacket2(&pkt, sizeof(pkt));
            UT_ASSERT_MSG(visibilityPresetMatch(&got) == (VisibilityPreset)p,
                          "preset %d with the bytes present read as %d", p,
                          (int)visibilityPresetMatch(&got));
        }
    }
    return 0;
}
