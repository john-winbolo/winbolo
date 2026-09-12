/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * Body-codec coverage for CTRL_SIM_RULES — the gameplay numbers the server
 * states to its clients. The event is delivered body-only (no full-packet
 * wrapper, no PACKET_* type), so the functions are resolved through the body
 * tables the live control path uses.
 *
 *   run_sim_rules_codec_roundtrip — every carried rule out equals the one
 *       that went in, compared field by field and named on failure. A
 *       memcmp would pass a decoder that left a field at zero when the
 *       value that went in happened to be zero, so nothing here is
 *       compared in bulk. Includes rates a fixed-point scale could not
 *       carry, and a short body being refused.
 *   run_sim_rules_codec_golden   — the body's bytes against bytes written
 *       out by hand. The round-trip above passes whenever the encoder and
 *       the decoder agree with each other; this is what fails when they
 *       agree on something other than the layout that shipped.
 *
 * The golden case assigns every field by name rather than through the
 * event's own field lists, so a rule that moves in those lists moves in the
 * bytes and is caught here.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* What the four width groups add up to: 33 one-byte rules, one two-byte
 * rule, three four-byte rules and fourteen four-byte rates. Written out
 * rather than taken from the macro, so a rule added to the event without a
 * thought about the control segment fails here and is looked at. */
#define SR_EXPECTED_BODY_LEN 103

/* ── 1. Round trip ─────────────────────────────────────────────────────── */

/* Distinct values, so a pair of fields swapped anywhere between the encoder
 * and the decoder shows up as two failures rather than cancelling out. The
 * integer counter walks the one-byte rules inside a byte; the wider rules
 * take values that only fit their own width. */
static void srFillCounted(ControlEvent *evt) {
    int n = 0;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_SIM_RULES;
#define SR_FILL_U8(name)  evt->u.simRules.name = (int32_t)(++n);
    CTRL_SIM_RULES_U8_FIELDS(SR_FILL_U8)
#undef SR_FILL_U8
    evt->u.simRules.tank_death_ticks       = 65535;
    evt->u.simRules.shell_start_add        = 2147483647;
    evt->u.simRules.base_regen_ticks       = 1000000;
    evt->u.simRules.tree_grow_initial_ticks = 30000;
    /* Rates a ×256 fixed point could not carry: 0.01 is the bottom of the
       tank rates' range and scales to 2.56, which rounds to 3 and comes
       back as 0.0117 — the client would then predict movement with a number
       the server is not simulating with. Four raw bytes carry it exactly. */
    evt->u.simRules.tank_accel_rate    = 0.01f;
    evt->u.simRules.tank_decel_rate    = 0.33f;
    evt->u.simRules.tank_brake_rate    = 15.99f;
    evt->u.simRules.tank_autoslow_rate = 0.25f;
    evt->u.simRules.turn_road       = 1.0f;
    evt->u.simRules.turn_grass      = 0.9f;
    evt->u.simRules.turn_forest     = 0.5f;
    evt->u.simRules.turn_river      = 0.25f;
    evt->u.simRules.turn_swamp      = 0.125f;
    evt->u.simRules.turn_crater     = 0.0625f;
    evt->u.simRules.turn_rubble     = 0.03125f;
    evt->u.simRules.turn_boat       = 2.5f;
    evt->u.simRules.turn_deep_sea   = 0.0f;
    evt->u.simRules.turn_refuel_base = 16.0f;
}

int run_sim_rules_codec_roundtrip(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SIM_RULES);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SIM_RULES);
    ControlEvent in, out;
    uint8_t      buf[MAX_CONTROL_PACKET];
    size_t       outLen = 0;

    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_SIM_RULES");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_SIM_RULES");
    UT_ASSERT_MSG(CTRL_SIM_RULES_BODY_LEN == SR_EXPECTED_BODY_LEN,
                  "the body is %u bytes, expected %d — a rule has been added "
                  "or its width has changed",
                  (unsigned)CTRL_SIM_RULES_BODY_LEN, SR_EXPECTED_BODY_LEN);

    srFillCounted(&in);
    UT_ASSERT_MSG(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "the encoder refused a table that fits");
    UT_ASSERT_MSG(outLen == CTRL_SIM_RULES_BODY_LEN,
                  "the encoder wrote %u bytes, expected %u",
                  (unsigned)outLen, (unsigned)CTRL_SIM_RULES_BODY_LEN);

    memset(&out, 0xAB, sizeof(out));   /* not zero: a field the decoder
                                          leaves alone must not read as the
                                          value that went in by accident */
    UT_ASSERT_MSG(dec(buf, outLen, &out), "the decoder refused its own body");
    UT_ASSERT_MSG(out.type == CTRL_SIM_RULES,
                  "the decoder produced event type %d", (int)out.type);

    /* Field by field, each named. */
#define SR_CHECK_INT(name)                                                   \
    UT_ASSERT_MSG(out.u.simRules.name == in.u.simRules.name,                 \
                  #name " decoded as %ld, expected %ld",                     \
                  (long)out.u.simRules.name, (long)in.u.simRules.name);
#define SR_CHECK_FLT(name)                                                   \
    UT_ASSERT_MSG(out.u.simRules.name == in.u.simRules.name,                 \
                  #name " decoded as %.9g, expected %.9g",                   \
                  (double)out.u.simRules.name, (double)in.u.simRules.name);

    CTRL_SIM_RULES_U8_FIELDS(SR_CHECK_INT)
    CTRL_SIM_RULES_U16_FIELDS(SR_CHECK_INT)
    CTRL_SIM_RULES_U32_FIELDS(SR_CHECK_INT)
    CTRL_SIM_RULES_F32_FIELDS(SR_CHECK_FLT)

#undef SR_CHECK_INT
#undef SR_CHECK_FLT

    /* The rate a scale would have rounded, stated on its own so a failure
       says which property broke rather than only naming a field. */
    UT_ASSERT_MSG(out.u.simRules.tank_accel_rate == 0.01f,
                  "tank_accel_rate came back as %.9g, not the 0.01 that went "
                  "in — the rate is being scaled rather than carried",
                  (double)out.u.simRules.tank_accel_rate);

    /* A body of any other length is refused rather than read short. */
    {
        ControlEvent shortOut;
        memset(&shortOut, 0, sizeof(shortOut));
        UT_ASSERT_MSG(!dec(buf, CTRL_SIM_RULES_BODY_LEN - 1, &shortOut),
                      "the decoder accepted a body one byte short");
        UT_ASSERT_MSG(!dec(buf, 0, &shortOut),
                      "the decoder accepted an empty body");
    }

    /* And a buffer too small to hold the body is refused rather than
       overrun. */
    {
        uint8_t tiny[SR_EXPECTED_BODY_LEN - 1];
        size_t  tinyLen = 0;
        UT_ASSERT_MSG(enc(&in, NULL, tiny, sizeof(tiny), &tinyLen) ==
                          ENCODE_OVERFLOW,
                      "the encoder wrote into a buffer that cannot hold the "
                      "body");
    }

    return 0;
}

/* ── 2. Golden bytes ───────────────────────────────────────────────────── */

/* The body the values below must produce, byte for byte.
 *
 * The one-byte rules carry their own 1-based position in the wire order, so
 * the first 33 bytes read 0x01..0x21 and a rule that moves in the list moves
 * a byte here. Then tank_death_ticks big-endian, the three four-byte rules
 * big-endian, and the fourteen rates as their IEEE-754 bit patterns, most
 * significant byte first. Every value below is a power-of-two fraction or a
 * small whole number, so each bit pattern is exact and was written out by
 * hand rather than taken from the encoder. */
static const uint8_t kSrGolden[SR_EXPECTED_BODY_LEN] = {
    /* the 33 one-byte rules, in wire order */
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    0x21,
    /* tank_death_ticks = 0x0102 */
    0x01, 0x02,
    /* shell_start_add = 0x00010203 */
    0x00, 0x01, 0x02, 0x03,
    /* base_regen_ticks = 0x04050607 */
    0x04, 0x05, 0x06, 0x07,
    /* tree_grow_initial_ticks = 0x08090A0B */
    0x08, 0x09, 0x0A, 0x0B,
    /* tank_accel_rate    = 0.25   */ 0x3E, 0x80, 0x00, 0x00,
    /* tank_decel_rate    = 0.5    */ 0x3F, 0x00, 0x00, 0x00,
    /* tank_brake_rate    = 1.0    */ 0x3F, 0x80, 0x00, 0x00,
    /* tank_autoslow_rate = 2.0    */ 0x40, 0x00, 0x00, 0x00,
    /* turn_road          = 4.0    */ 0x40, 0x80, 0x00, 0x00,
    /* turn_grass         = 8.0    */ 0x41, 0x00, 0x00, 0x00,
    /* turn_forest        = 0.125  */ 0x3E, 0x00, 0x00, 0x00,
    /* turn_river         = 0.0625 */ 0x3D, 0x80, 0x00, 0x00,
    /* turn_swamp         = 3.0    */ 0x40, 0x40, 0x00, 0x00,
    /* turn_crater        = 5.0    */ 0x40, 0xA0, 0x00, 0x00,
    /* turn_rubble        = 6.0    */ 0x40, 0xC0, 0x00, 0x00,
    /* turn_boat          = 7.0    */ 0x40, 0xE0, 0x00, 0x00,
    /* turn_deep_sea      = 1.5    */ 0x3F, 0xC0, 0x00, 0x00,
    /* turn_refuel_base   = 2.5    */ 0x40, 0x20, 0x00, 0x00
};

int run_sim_rules_codec_golden(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_SIM_RULES);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_SIM_RULES);
    ControlEvent in, out;
    uint8_t      buf[MAX_CONTROL_PACKET];
    size_t       outLen = 0;
    size_t       i;

    UT_ASSERT_MSG(enc != NULL, "no body encoder registered for CTRL_SIM_RULES");
    UT_ASSERT_MSG(dec != NULL, "no body decoder registered for CTRL_SIM_RULES");

    memset(&in, 0, sizeof(in));
    in.type = CTRL_SIM_RULES;

    /* Every one-byte rule set to its own place in the wire order, by name. */
    in.u.simRules.tank_reload_ticks     = 1;
    in.u.simRules.tank_full_shells      = 2;
    in.u.simRules.tank_full_mines       = 3;
    in.u.simRules.tank_full_trees       = 4;
    in.u.simRules.tank_full_armour      = 5;
    in.u.simRules.tank_water_ticks      = 6;
    in.u.simRules.shell_damage          = 7;
    in.u.simRules.mine_damage           = 8;
    in.u.simRules.just_fired_ticks      = 9;
    in.u.simRules.gunsight_min          = 10;
    in.u.simRules.gunsight_max          = 11;
    in.u.simRules.tank_min_move         = 12;
    in.u.simRules.speed_road            = 13;
    in.u.simRules.speed_grass           = 14;
    in.u.simRules.speed_forest          = 15;
    in.u.simRules.speed_river           = 16;
    in.u.simRules.speed_swamp           = 17;
    in.u.simRules.speed_crater          = 18;
    in.u.simRules.speed_rubble          = 19;
    in.u.simRules.speed_boat            = 20;
    in.u.simRules.speed_deep_sea        = 21;
    in.u.simRules.speed_refuel_base     = 22;
    in.u.simRules.shell_life            = 23;
    in.u.simRules.shell_speed           = 24;
    in.u.simRules.pill_max_armour       = 25;
    in.u.simRules.pill_attack_ticks     = 26;
    in.u.simRules.pill_attack_min_ticks = 27;
    in.u.simRules.pill_cooldown_ticks   = 28;
    in.u.simRules.base_full_armour      = 29;
    in.u.simRules.base_full_shells      = 30;
    in.u.simRules.base_full_mines       = 31;
    in.u.simRules.base_capture_armour   = 32;
    in.u.simRules.base_hit_armour       = 33;

    in.u.simRules.tank_death_ticks        = 0x0102;
    in.u.simRules.shell_start_add         = 0x00010203;
    in.u.simRules.base_regen_ticks        = 0x04050607;
    in.u.simRules.tree_grow_initial_ticks = 0x08090A0B;

    in.u.simRules.tank_accel_rate    = 0.25f;
    in.u.simRules.tank_decel_rate    = 0.5f;
    in.u.simRules.tank_brake_rate    = 1.0f;
    in.u.simRules.tank_autoslow_rate = 2.0f;
    in.u.simRules.turn_road          = 4.0f;
    in.u.simRules.turn_grass         = 8.0f;
    in.u.simRules.turn_forest        = 0.125f;
    in.u.simRules.turn_river         = 0.0625f;
    in.u.simRules.turn_swamp         = 3.0f;
    in.u.simRules.turn_crater        = 5.0f;
    in.u.simRules.turn_rubble        = 6.0f;
    in.u.simRules.turn_boat          = 7.0f;
    in.u.simRules.turn_deep_sea      = 1.5f;
    in.u.simRules.turn_refuel_base   = 2.5f;

    UT_ASSERT_MSG(enc(&in, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK,
                  "the encoder refused the golden table");
    UT_ASSERT_MSG(outLen == sizeof(kSrGolden),
                  "the encoder wrote %u bytes, the golden holds %u",
                  (unsigned)outLen, (unsigned)sizeof(kSrGolden));
    for (i = 0; i < sizeof(kSrGolden); i++) {
        UT_ASSERT_MSG(buf[i] == kSrGolden[i],
                      "body byte %u is 0x%02X, the golden says 0x%02X",
                      (unsigned)i, (unsigned)buf[i], (unsigned)kSrGolden[i]);
    }

    /* And the golden bytes decode back to the values that produced them, so
       the fixture is a statement about both halves rather than only the
       encoder. */
    memset(&out, 0, sizeof(out));
    UT_ASSERT_MSG(dec(kSrGolden, sizeof(kSrGolden), &out),
                  "the decoder refused the golden body");
    UT_ASSERT_MSG(out.u.simRules.tank_reload_ticks == 1 &&
                      out.u.simRules.base_hit_armour == 33,
                  "the golden body decoded its first one-byte rule as %ld and "
                  "its last as %ld, expected 1 and 33",
                  (long)out.u.simRules.tank_reload_ticks,
                  (long)out.u.simRules.base_hit_armour);
    UT_ASSERT_MSG(out.u.simRules.tank_death_ticks == 0x0102,
                  "the golden body decoded tank_death_ticks as %ld",
                  (long)out.u.simRules.tank_death_ticks);
    UT_ASSERT_MSG(out.u.simRules.tree_grow_initial_ticks == 0x08090A0B,
                  "the golden body decoded tree_grow_initial_ticks as %ld",
                  (long)out.u.simRules.tree_grow_initial_ticks);
    UT_ASSERT_MSG(out.u.simRules.turn_refuel_base == 2.5f,
                  "the golden body decoded the last rate as %.9g, expected 2.5",
                  (double)out.u.simRules.turn_refuel_base);

    return 0;
}
