/*
 * Lobby map-upload size-cap enforcement.
 *
 * The .map RLE format encodes the worst-case legitimate playable map
 * in ~27 KiB and a fully-pathological 256×256 file in ~37 KiB. The
 * upload path caps at LOBBY_MAP_UPLOAD_MAX_BYTES (64 KiB) — well
 * above any real map, well below where the historical 1 MiB cap left
 * an attacker free to ship arbitrary 1 MiB blobs.
 *
 * Three sites enforce the cap:
 *   1. PACKET_LOBBY_MAP_UPLOAD_BEGIN handler in transport_udp_server.c
 *   2. PACKET_LOBBY_MAP_USE_LOCAL handler (same constant)
 *   3. serverSimReadMapFile() — used by the USE_LOCAL handler to read
 *      bytes for MD5 verification before installing the cached copy
 *
 * This test pins the cap value and exercises (3) directly with a
 * temp file at the exact cap and one byte over. The wire handlers in
 * (1)/(2) reference the same constant, so a constant-bump regression
 * shifts all three sites together and the value-equality assertion
 * below catches it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "wire_limits.h"
#include "everard_map.h"
#include "test_harness.h"

/* Use a dedicated subdirectory under data/maps/ so we never collide
 * with a real .map file, and so failed runs leave a recognisable
 * footprint. relPath is the path serverSimReadMapFile receives;
 * fullPath is what we open() against. */
#define TEST_SUBDIR  ".test_upload_cap"
#define TEST_NAME    "cap_probe.map"
#define TEST_RELPATH TEST_SUBDIR "/" TEST_NAME
#define TEST_FULLPATH "data/maps/" TEST_RELPATH

static bool write_file_of_size(const char *path, size_t bytes) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    /* Bytes don't need to be a valid .map — serverSimReadMapFile only
     * reads them; it doesn't parse. Use a recognisable filler so a
     * hex-dump of the file makes it obvious where it came from. */
    static const char filler[] = "BOLOUPLOADCAPPROBE";
    size_t fillerLen = sizeof(filler) - 1;
    size_t written = 0;
    while (written < bytes) {
        size_t want = bytes - written;
        if (want > fillerLen) want = fillerLen;
        if (fwrite(filler, 1, want, fp) != want) { fclose(fp); return false; }
        written += want;
    }
    fclose(fp);
    return true;
}

static void cleanup(void) {
    SDL_RemovePath(TEST_FULLPATH);
    SDL_RemovePath("data/maps/" TEST_SUBDIR);
}

int run_upload_cap_enforced(void) {
    /* 1. Pin the constant. If someone bumps this back to 1 MiB the
     *    test fails loudly and the rest of the cap-related code in
     *    transport_udp_server.c / server_sim.c / imgui_lobby.cpp
     *    will follow. */
    UT_ASSERT_MSG(LOBBY_MAP_UPLOAD_MAX_BYTES == 64u * 1024u,
                  "upload cap must stay at 64 KiB — see wire_limits.h");

    /* Build a minimal ServerSim so we have a target for the API call.
     * serverSimReadMapFile ignores the sim pointer (it's a static
     * filesystem helper) but the signature requires one. */
    ServerSim *sim = ut_make_running_sim("Probe");
    UT_ASSERT(sim != NULL);

    /* Make sure the test data directory exists and is clean. */
    SDL_CreateDirectory("data/maps");
    SDL_CreateDirectory("data/maps/" TEST_SUBDIR);
    cleanup();
    SDL_CreateDirectory("data/maps/" TEST_SUBDIR);

    uint8_t *bytes = NULL;
    size_t   byteLen = 0;

    /* 2. Exactly at the cap → must be accepted. */
    UT_ASSERT(write_file_of_size(TEST_FULLPATH,
                                  LOBBY_MAP_UPLOAD_MAX_BYTES));
    UT_ASSERT_MSG(serverSimReadMapFile(sim, TEST_RELPATH,
                                        &bytes, &byteLen),
                  "file at exact cap must be readable");
    UT_ASSERT_MSG(byteLen == LOBBY_MAP_UPLOAD_MAX_BYTES,
                  "byteLen=%zu expected=%u",
                  byteLen, (unsigned)LOBBY_MAP_UPLOAD_MAX_BYTES);
    UT_ASSERT(bytes != NULL);
    free(bytes);
    bytes = NULL;
    byteLen = 0;

    /* 3. One byte over the cap → must be rejected, and the helper
     *    must leave the out params cleared. */
    UT_ASSERT(write_file_of_size(TEST_FULLPATH,
                                  LOBBY_MAP_UPLOAD_MAX_BYTES + 1));
    UT_ASSERT_MSG(!serverSimReadMapFile(sim, TEST_RELPATH,
                                         &bytes, &byteLen),
                  "file one byte over cap must be rejected");
    UT_ASSERT_MSG(bytes == NULL,
                  "rejected read must leave out-pointer NULL");
    UT_ASSERT_MSG(byteLen == 0,
                  "rejected read must leave out-len 0");

    /* 4. Far over the cap (the historical 1 MiB value) → must be
     *    rejected. Pins the *intent* of the change — not just "1
     *    above the cap" but "the old cap is now strictly over". */
    UT_ASSERT(write_file_of_size(TEST_FULLPATH, 1024u * 1024u));
    UT_ASSERT_MSG(!serverSimReadMapFile(sim, TEST_RELPATH,
                                         &bytes, &byteLen),
                  "1 MiB file (the old cap) must now be rejected");

    cleanup();
    serverSimDestroy(sim);
    return 0;
}
