/*
 * Deferred WBN PLAYER_JOIN core (test_wbn_deferred_join.c).
 *
 * On an internet join the player's WBN identity isn't known at
 * JOIN_ACCEPT time — it arrives later, after the server hands the
 * joiner the session server_key (PACKET_WBN_REKEY) and the client mints
 * a player_key and re-auths (PACKET_WBN_REAUTH). So the join event is
 * deferred: emitted keyed the moment a reauth fills the slot's key, or
 * anonymous if a grace window elapses with no reauth. Because
 * winbolonetEndSession empties every key at a round boundary, the next
 * reauth re-fires the join for the new session ("return to lobby = new
 * game").
 *
 * transport_udp_server.c hangs one WbnJoinState off each connected slot
 * and drives it from four sites (join, reauth, the per-tick grace
 * sweep, disconnect). These pin the value-only core that decides what
 * each site does, without standing up a socket or the WBN HTTP layer.
 * They also pin wbnRekeyTargetSelected — the rotation rekey gate that
 * must read the durable PLAYER_FLAG_WBN_VERIFIED, never the per-slot key
 * that the round-boundary teardown has already wiped.
 */

#include <stdint.h>
#include <stdbool.h>

#include "global.h"
#include "transport_udp.h"   /* WbnJoinState + wbnJoin* / wbnRekeyTargetSelected */
#include "player_flags.h"    /* PLAYER_FLAG_WBN_VERIFIED etc. */
#include "test_harness.h"

/* A reauth that lands inside the grace window registers the player
 * keyed (key was absent->present) and cancels the anonymous fallback. */
int run_wbn_join_keyed_on_reauth(void) {
    WbnJoinState s = {0};

    wbnJoinArm(&s, 100, 250);
    UT_ASSERT(s.pending);

    /* wasParticipant == false: the verify just filled the key, so the
     * caller must emit a keyed PLAYER_JOIN. */
    UT_ASSERT_MSG(wbnJoinOnReauth(&s, false) == true,
                  "fresh reauth (key absent->present) must emit keyed join");
    UT_ASSERT_MSG(!s.pending, "a resolved join must clear the pending debt");

    /* With the debt cleared, the grace sweep must never fire — no
     * duplicate anonymous join after the keyed one. */
    UT_ASSERT_MSG(wbnJoinOnTick(&s, 100000) == false,
                  "grace sweep must not fire once reauth resolved the join");
    return 0;
}

/* No reauth arrives: the grace sweep emits an anonymous join exactly
 * once, at the deadline, and never again. */
int run_wbn_join_anonymous_on_grace(void) {
    WbnJoinState s = {0};

    wbnJoinArm(&s, 100, 250);   /* deadline tick = 350 */

    UT_ASSERT_MSG(wbnJoinOnTick(&s, 349) == false,
                  "must not fire before the grace deadline");
    UT_ASSERT_MSG(wbnJoinOnTick(&s, 350) == true,
                  "must fire an anonymous join at the grace deadline");
    UT_ASSERT_MSG(wbnJoinOnTick(&s, 351) == false,
                  "must fire at most once (pending cleared on expiry)");
    UT_ASSERT_MSG(wbnJoinOnTick(&s, 100000) == false,
                  "must stay quiet long after expiry");
    return 0;
}

/* An idempotent rekey resend re-verifies an already-keyed slot. That
 * must NOT emit a second join — wasParticipant gates it off — while
 * still clearing any stray pending debt. */
int run_wbn_join_idempotent_reauth_no_double(void) {
    WbnJoinState s = {0};

    wbnJoinArm(&s, 100, 250);
    UT_ASSERT(wbnJoinOnReauth(&s, false) == true);   /* first, keyed */

    /* Second verify, slot already a participant -> no emit. */
    UT_ASSERT_MSG(wbnJoinOnReauth(&s, true) == false,
                  "repeat verify on an already-keyed slot must not re-emit");
    UT_ASSERT_MSG(!s.pending, "repeat reauth must leave no pending debt");
    UT_ASSERT(wbnJoinOnTick(&s, 100000) == false);
    return 0;
}

/* A slot that leaves before its join resolves drops the owed event:
 * no orphan anonymous join (and so no leave it would need to pair). */
int run_wbn_join_disconnect_drops(void) {
    WbnJoinState s = {0};

    wbnJoinArm(&s, 100, 250);
    UT_ASSERT(s.pending);

    wbnJoinClear(&s);
    UT_ASSERT_MSG(!s.pending, "disconnect must drop the pending join");
    UT_ASSERT_MSG(wbnJoinOnTick(&s, 100000) == false,
                  "no anonymous join may fire for a slot that left early");
    return 0;
}

/* Each round is a new WBN game: after a keyed join is resolved, a
 * session rotation re-arms the slot, and the next reauth (key wiped, so
 * absent->present again) emits a fresh keyed join. */
int run_wbn_join_rearm_per_session(void) {
    WbnJoinState s = {0};

    /* Round 1 join. */
    wbnJoinArm(&s, 100, 250);
    UT_ASSERT(wbnJoinOnReauth(&s, false) == true);

    /* Round boundary: winbolonetEndSession wiped the key, the rekey
     * broadcast re-arms the slot. */
    wbnJoinArm(&s, 5000, 250);
    UT_ASSERT(s.pending);

    /* The post-rotation reauth sees an empty key again -> keyed join
     * for the new session. */
    UT_ASSERT_MSG(wbnJoinOnReauth(&s, false) == true,
                  "post-rotation reauth must emit a fresh keyed join");
    UT_ASSERT(!s.pending);
    return 0;
}

/* The rotation rekey gate must select connected, WBN-verified slots and
 * nothing else — and must key off the verified flag, not the per-slot
 * WBN key (which winbolonetEndSession has already wiped by the time the
 * broadcast runs). */
int run_wbn_join_rekey_target_gate(void) {
    UT_ASSERT_MSG(wbnRekeyTargetSelected(true, PLAYER_FLAG_WBN_VERIFIED) == true,
                  "connected + verified must be rekeyed");
    UT_ASSERT_MSG(wbnRekeyTargetSelected(true, 0) == false,
                  "connected but unverified must not be rekeyed");
    UT_ASSERT_MSG(wbnRekeyTargetSelected(false, PLAYER_FLAG_WBN_VERIFIED) == false,
                  "a disconnected slot must not be rekeyed");

    /* Other flags riding alongside VERIFIED don't change the decision;
     * a non-VERIFIED flag alone doesn't qualify. */
    UT_ASSERT(wbnRekeyTargetSelected(true,
                  (uint8_t)(PLAYER_FLAG_WBN_VERIFIED | PLAYER_FLAG_SUPPORTER)) == true);
    UT_ASSERT_MSG(wbnRekeyTargetSelected(true, PLAYER_FLAG_STEAM_BUILD) == false,
                  "a non-verified hint flag must not qualify for rekey");
    return 0;
}
