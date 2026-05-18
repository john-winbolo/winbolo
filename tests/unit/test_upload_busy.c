/*
 * Concurrent-upload rejection predicate.
 *
 * The sim has one preview / pending-upload slot. Letting two
 * clients race PACKET_LOBBY_MAP_UPLOAD_BEGIN truncates the loser's
 * bytes on the global .pending_upload.map temp file and overwrites
 * their pending paths inside the sim, silently losing the loser's
 * upload work. The UPLOAD_BEGIN and USE_LOCAL handlers in
 * transport_udp_server.c gate on lobbyAnyOtherUploadActive — if any
 * client other than the sender has an active upload, the request
 * is rejected with LOBBY_REJECT_UPLOAD_BUSY.
 *
 * The predicate is a pure function so tests can drive it with
 * hand-built state arrays without seeding udpServer.
 *
 * The "same-client retry isn't busy" invariant is critical:
 * UPLOAD_BEGIN clears the sender's own clientUploadBuf at
 * transport_udp_server.c:3319, so the existing retry path stays
 * intact (sender re-sends BEGIN, server frees their old buffer,
 * allocates a new one). The busy gate must not block that path.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "transport_udp.h"
#include "test_harness.h"

int run_upload_busy_predicate(void) {
    bool active[MAX_TANKS];
    memset(active, 0, sizeof(active));

    /* 1. No one uploading: no client sees a busy slot. */
    UT_ASSERT_MSG(!lobbyAnyOtherUploadActive(active, 0),
                  "empty state must not be busy for any client");
    UT_ASSERT(!lobbyAnyOtherUploadActive(active, 7));
    UT_ASSERT(!lobbyAnyOtherUploadActive(active, MAX_TANKS - 1));

    /* 2. Client 2 uploading: everyone else sees busy, but client
     *    2's own retry path doesn't trip on its own state. */
    active[2] = true;
    UT_ASSERT_MSG(lobbyAnyOtherUploadActive(active, 0),
                  "client 0 must see client 2's upload as busy");
    UT_ASSERT_MSG(lobbyAnyOtherUploadActive(active, 5),
                  "client 5 must see client 2's upload as busy");
    UT_ASSERT_MSG(!lobbyAnyOtherUploadActive(active, 2),
                  "client 2 must NOT see its own upload as busy "
                  "(would break the same-client BEGIN retry path)");

    /* 3. Multiple uploaders: the predicate stays true for every
     *    other slot. (Production code never lets this happen,
     *    but the predicate has to be robust to it.) */
    active[5] = true;
    UT_ASSERT(lobbyAnyOtherUploadActive(active, 0));
    UT_ASSERT(lobbyAnyOtherUploadActive(active, 2));  /* sees 5 */
    UT_ASSERT(lobbyAnyOtherUploadActive(active, 5));  /* sees 2 */
    UT_ASSERT(lobbyAnyOtherUploadActive(active, 7));

    /* 4. Cleanup path: after both flags clear (the disconnect-
     *    cleanup or UPLOAD_DONE branch fires), the slot is free
     *    again for the next client. */
    active[2] = false;
    active[5] = false;
    UT_ASSERT_MSG(!lobbyAnyOtherUploadActive(active, 0),
                  "post-cleanup state must un-busy the slot");

    /* 5. NULL state defaults to "not busy" — defensive guard for
     *    callers that pass a partially-initialised struct. */
    UT_ASSERT(!lobbyAnyOtherUploadActive(NULL, 0));

    /* 6. exceptIdx outside [0, MAX_TANKS) doesn't crash and treats
     *    all slots as "other". A real call site can't hit this
     *    (serverFindClient returns < 0 before the handler runs)
     *    but the predicate should still behave consistently. */
    active[3] = true;
    UT_ASSERT_MSG(lobbyAnyOtherUploadActive(active, -1),
                  "exceptIdx=-1 must still report client 3 as busy");
    UT_ASSERT_MSG(lobbyAnyOtherUploadActive(active, MAX_TANKS),
                  "exceptIdx beyond range must still report client 3 as busy");

    return 0;
}
