/*
 * JOIN name-collision verdict core (test_join_collision.c).
 *
 * When an incoming joiner's validated name already matches a connected
 * slot, the server must decide whether to reject the joiner or admit it
 * provisionally. The policy is value-only so it can be pinned without a
 * socket or the WBN layer: a verified slot always wins (the client's
 * "will authenticate" assertion is irrelevant and can never displace it);
 * an unverified slot is rejected as in-use unless the joiner claims it
 * will authenticate, in which case it is admitted provisionally. The
 * will-auth flag is client-asserted and only ever downgrades a reject to
 * a provisional admit — it never grants priority.
 *
 * The reauth-time resolve core (claimResolveDecide) is pinned here too:
 * given whether the desired bare name is held and, if so, whether the
 * holder is verified, it picks promote-free / preempt-squatter / keep-temp;
 * an absent holder collapses to promote-free regardless of the verified
 * flag.
 */

#include <stdint.h>
#include <stdbool.h>

#include "global.h"
#include "transport_udp.h"   /* JoinCollisionVerdict + joinCollisionDecide */
#include "test_harness.h"

/* Unverified squatter and a joiner that won't authenticate: the name is
 * in use, so the joiner is rejected. */
int run_join_collision_unverified_no_auth_rejects(void) {
    UT_ASSERT_MSG(joinCollisionDecide(false, false) == JOIN_COLLISION_REJECT_IN_USE,
                  "unverified slot + non-auth joiner must reject as in-use");
    return 0;
}

/* Unverified squatter and a joiner that will authenticate: the joiner
 * gets a provisional admit, to be confirmed once it authenticates. */
int run_join_collision_unverified_will_auth_admits(void) {
    UT_ASSERT_MSG(joinCollisionDecide(true, false) == JOIN_COLLISION_ADMIT_PROVISIONAL,
                  "unverified slot + will-auth joiner must admit provisionally");
    return 0;
}

/* Verified squatter: the joiner is rejected even when it claims it will
 * authenticate. */
int run_join_collision_verified_rejects_will_auth(void) {
    UT_ASSERT_MSG(joinCollisionDecide(true, true) == JOIN_COLLISION_REJECT_VERIFIED,
                  "verified slot must reject a will-auth joiner");
    return 0;
}

/* A verified slot rejects regardless of the will-auth flag: the
 * client-asserted flag cannot displace a verified slot either way. */
int run_join_collision_verified_flag_irrelevant(void) {
    UT_ASSERT_MSG(joinCollisionDecide(false, true) == JOIN_COLLISION_REJECT_VERIFIED &&
                  joinCollisionDecide(true, true) == JOIN_COLLISION_REJECT_VERIFIED,
                  "the will-auth flag cannot displace a verified slot");
    return 0;
}

/* Bare name free: nobody holds the desired name, so the reclaiming slot is
 * promoted straight to it. */
int run_claim_resolve_free_promotes(void) {
    UT_ASSERT_MSG(claimResolveDecide(false, false) == CLAIM_RESOLVE_PROMOTE_FREE,
                  "a free bare name must promote the reclaiming slot");
    return 0;
}

/* Unverified holder still squats the bare name: it must be renamed off
 * before the reclaiming slot is promoted. */
int run_claim_resolve_unverified_preempts(void) {
    UT_ASSERT_MSG(claimResolveDecide(true, false) == CLAIM_RESOLVE_PREEMPT_SQUATTER,
                  "an unverified holder must be preempted off the bare name");
    return 0;
}

/* Verified holder won the bare name: the reclaiming slot keeps its temp
 * name permanently. */
int run_claim_resolve_verified_keeps_temp(void) {
    UT_ASSERT_MSG(claimResolveDecide(true, true) == CLAIM_RESOLVE_KEEP_TEMP,
                  "a verified holder must leave the reclaiming slot on its temp name");
    return 0;
}

/* With no holder, the verified flag is irrelevant: both flag values
 * collapse to promote-free. */
int run_claim_resolve_free_ignores_holder_flag(void) {
    UT_ASSERT_MSG(claimResolveDecide(false, false) == CLAIM_RESOLVE_PROMOTE_FREE &&
                  claimResolveDecide(false, true) == CLAIM_RESOLVE_PROMOTE_FREE,
                  "an absent holder makes the verified flag irrelevant");
    return 0;
}
