/*
 * Tests for local_map_index.c: finding a .map file on this machine by the
 * md5 of its bytes. The game finder uses it to draw a listed game's map
 * from the player's own copy when the md5 the host advertises matches.
 *
 * Every file is written under utScratchPath, so nothing here reads the
 * maps the game ships with or touches the network.
 */

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "test_harness.h"
#include "local_map_index.h"
#include "common/md5.h"

/* Writes `len` bytes to <scratch>/<leaf> and puts the md5 of those bytes,
 * as the host would advertise it, in md5Out. */
static bool write_file(const char *leaf, const void *data, size_t len,
                       char md5Out[33]) {
    char    path[1024];
    uint8_t digest[16];
    if (!utScratchPath(path, sizeof(path), leaf)) return false;
    if (!SDL_SaveFile(path, data, len)) return false;
    if (md5Out != NULL) {
        md5Compute(data, len, digest);
        md5ToHex(digest, md5Out);
    }
    return true;
}

/* A file that passes the header test: BMAPBOLO and then `tail`. */
static bool write_map(const char *leaf, const char *tail, char md5Out[33]) {
    char   buf[256];
    size_t len = (size_t)snprintf(buf, sizeof(buf), "BMAPBOLO%s", tail);
    return write_file(leaf, buf, len, md5Out);
}

static bool make_dir(const char *leaf, char *out, size_t outSz) {
    if (!utScratchPath(out, outSz, leaf)) return false;
    return SDL_CreateDirectory(out);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* A map in the root and one two folders down are both found by md5, and
 * the bytes that come back are the file's. The game ships maps in
 * subfolders (Tutorial, Workshop), and a host can be on any of them. */
static int finds_maps_in_root_and_subfolders(void) {
    char root[1024], sub[1024];
    char md5Top[33], md5Deep[33];
    const char *roots[1];
    uint8_t *bytes = NULL;
    size_t len = 0;

    UT_ASSERT(make_dir("a", root, sizeof(root)));
    UT_ASSERT(make_dir("a/Sub", sub, sizeof(sub)));
    UT_ASSERT(make_dir("a/Sub/Deeper", sub, sizeof(sub)));
    UT_ASSERT(write_map("a/Top.map", "top", md5Top));
    UT_ASSERT(write_map("a/Sub/Deeper/Deep.MAP", "deep", md5Deep));

    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = root;
    UT_ASSERT_MSG(localMapIndexScan(idx, roots, 1) == 2, "first scan hashes both");
    UT_ASSERT_MSG(localMapIndexCount(idx) == 2, "count=%d", localMapIndexCount(idx));

    const char *p = localMapIndexFind(idx, md5Top);
    UT_ASSERT_MSG(p != NULL && ends_with(p, "/Top.map"), "top path=%s", p ? p : "(null)");
    p = localMapIndexFind(idx, md5Deep);
    UT_ASSERT_MSG(p != NULL && ends_with(p, "/Sub/Deeper/Deep.MAP"),
                  "deep path=%s", p ? p : "(null)");

    UT_ASSERT(localMapIndexLoad(idx, md5Deep, &bytes, &len));
    UT_ASSERT_MSG(len == 12 && memcmp(bytes, "BMAPBOLOdeep", 12) == 0,
                  "deep bytes len=%zu", len);
    SDL_free(bytes);

    /* The md5 arrives as lowercase hex, but an uppercase one is the same
     * map. */
    {
        char upper[33];
        size_t i;
        for (i = 0; i < 32; i++) upper[i] = (char)SDL_toupper(md5Top[i]);
        upper[32] = 0;
        UT_ASSERT(localMapIndexFind(idx, upper) != NULL);
    }

    localMapIndexDestroy(idx);
    return 0;
}

/* An md5 no file has, a malformed md5 and a NULL all miss without a
 * crash: this is every game on a map the player does not have. */
static int unknown_md5_misses(void) {
    char root[1024];
    const char *roots[1];
    uint8_t *bytes = (uint8_t *)1;
    size_t len = 99;

    UT_ASSERT(make_dir("b", root, sizeof(root)));
    UT_ASSERT(write_map("b/One.map", "one", NULL));
    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = root;
    localMapIndexScan(idx, roots, 1);

    UT_ASSERT(localMapIndexFind(idx, "00000000000000000000000000000000") == NULL);
    UT_ASSERT(localMapIndexFind(idx, "") == NULL);
    UT_ASSERT(localMapIndexFind(idx, "abc") == NULL);
    UT_ASSERT(localMapIndexFind(idx, NULL) == NULL);
    UT_ASSERT(localMapIndexFind(NULL, "00000000000000000000000000000000") == NULL);
    UT_ASSERT(!localMapIndexLoad(idx, "00000000000000000000000000000000",
                                 &bytes, &len));
    UT_ASSERT_MSG(bytes == NULL && len == 0, "a miss clears the outputs");

    localMapIndexDestroy(idx);
    return 0;
}

/* Only a .map file that starts with BMAPBOLO is offered. The host applies
 * the same header test before it advertises an md5, so anything else can
 * only match by accident. */
static int ignores_files_that_are_not_maps(void) {
    char root[1024];
    char md5Txt[33], md5Bad[33], md5Good[33];
    const char *roots[1];

    UT_ASSERT(make_dir("c", root, sizeof(root)));
    UT_ASSERT(write_map("c/Notes.txt", "text", md5Txt));          /* wrong extension */
    UT_ASSERT(write_file("c/Bad.map", "NOTAMAP!junk", 12, md5Bad)); /* wrong header */
    UT_ASSERT(write_file("c/Empty.map", "", 0, NULL));
    UT_ASSERT(write_map("c/Good.map", "good", md5Good));

    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = root;
    localMapIndexScan(idx, roots, 1);

    UT_ASSERT(localMapIndexFind(idx, md5Txt) == NULL);
    UT_ASSERT(localMapIndexFind(idx, md5Bad) == NULL);
    UT_ASSERT(localMapIndexFind(idx, md5Good) != NULL);

    localMapIndexDestroy(idx);
    return 0;
}

/* A second scan reads nothing when nothing changed, picks up a file added
 * since (a map downloaded during the session), and forgets one that was
 * deleted. */
static int rescan_tracks_added_and_removed_files(void) {
    char root[1024], path[1024];
    char md5A[33], md5B[33];
    const char *roots[1];

    UT_ASSERT(make_dir("d", root, sizeof(root)));
    UT_ASSERT(write_map("d/A.map", "aaaa", md5A));
    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = root;

    UT_ASSERT(localMapIndexScan(idx, roots, 1) == 1);
    UT_ASSERT_MSG(localMapIndexScan(idx, roots, 1) == 0,
                  "an unchanged file is not read again");
    UT_ASSERT(localMapIndexFind(idx, md5A) != NULL);

    UT_ASSERT(write_map("d/B.map", "bbbb", md5B));
    UT_ASSERT_MSG(localMapIndexScan(idx, roots, 1) == 1, "only the new file is read");
    UT_ASSERT(localMapIndexFind(idx, md5A) != NULL);
    UT_ASSERT(localMapIndexFind(idx, md5B) != NULL);

    UT_ASSERT(utScratchPath(path, sizeof(path), "d/A.map"));
    UT_ASSERT(SDL_RemovePath(path));
    UT_ASSERT(localMapIndexScan(idx, roots, 1) == 0);
    UT_ASSERT_MSG(localMapIndexFind(idx, md5A) == NULL, "a deleted file is forgotten");
    UT_ASSERT(localMapIndexFind(idx, md5B) != NULL);
    UT_ASSERT(localMapIndexCount(idx) == 1);

    localMapIndexDestroy(idx);
    return 0;
}

/* A file edited after the scan is hashed again on the next one: the old
 * md5 stops matching and the new one starts. The size differs here so the
 * change shows whatever the file system's time resolution is. */
static int rescan_rehashes_a_changed_file(void) {
    char root[1024];
    char md5Old[33], md5New[33];
    const char *roots[1];

    UT_ASSERT(make_dir("e", root, sizeof(root)));
    UT_ASSERT(write_map("e/Edit.map", "before", md5Old));
    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = root;
    UT_ASSERT(localMapIndexScan(idx, roots, 1) == 1);

    UT_ASSERT(write_map("e/Edit.map", "after, and longer", md5New));
    UT_ASSERT(localMapIndexScan(idx, roots, 1) == 1);
    UT_ASSERT(localMapIndexFind(idx, md5Old) == NULL);
    UT_ASSERT(localMapIndexFind(idx, md5New) != NULL);

    localMapIndexDestroy(idx);
    return 0;
}

/* A file replaced after the scan, with no scan since, is refused by the
 * load: the bytes no longer have the md5 the host advertised, and drawing
 * them would show the wrong map. */
static int load_refuses_a_file_changed_since_the_scan(void) {
    char root[1024];
    char md5Old[33];
    const char *roots[1];
    uint8_t *bytes = NULL;
    size_t len = 0;

    UT_ASSERT(make_dir("f", root, sizeof(root)));
    UT_ASSERT(write_map("f/Swap.map", "original", md5Old));
    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = root;
    localMapIndexScan(idx, roots, 1);

    UT_ASSERT(write_map("f/Swap.map", "replaced", NULL));
    UT_ASSERT(localMapIndexFind(idx, md5Old) != NULL);  /* the index is stale */
    UT_ASSERT(!localMapIndexLoad(idx, md5Old, &bytes, &len));
    UT_ASSERT(bytes == NULL && len == 0);

    localMapIndexDestroy(idx);
    return 0;
}

/* Several roots are searched, a root that does not exist is skipped, and
 * the same map in two roots resolves to the earlier root's file. */
static int searches_every_root_in_order(void) {
    char rootA[1024], rootB[1024], missing[1024];
    char md5Same[33], md5OnlyB[33];
    const char *roots[3];

    UT_ASSERT(make_dir("g1", rootA, sizeof(rootA)));
    UT_ASSERT(make_dir("g2", rootB, sizeof(rootB)));
    UT_ASSERT(utScratchPath(missing, sizeof(missing), "g-missing"));
    UT_ASSERT(write_map("g1/Same.map", "same", md5Same));
    UT_ASSERT(write_map("g2/Copy.map", "same", NULL));
    UT_ASSERT(write_map("g2/OnlyB.map", "only b", md5OnlyB));

    LocalMapIndex *idx = localMapIndexCreate();
    UT_ASSERT(idx != NULL);
    roots[0] = missing; roots[1] = rootA; roots[2] = rootB;
    UT_ASSERT(localMapIndexScan(idx, roots, 3) == 3);

    const char *p = localMapIndexFind(idx, md5Same);
    UT_ASSERT_MSG(p != NULL && ends_with(p, "/g1/Same.map"), "path=%s", p ? p : "(null)");
    UT_ASSERT(localMapIndexFind(idx, md5OnlyB) != NULL);

    /* No roots at all empties the index rather than keeping stale rows. */
    UT_ASSERT(localMapIndexScan(idx, NULL, 0) == 0);
    UT_ASSERT(localMapIndexCount(idx) == 0);

    localMapIndexDestroy(idx);
    localMapIndexDestroy(NULL);  /* allowed */
    return 0;
}

int run_local_map_index(void) {
    int rc;
    rc = finds_maps_in_root_and_subfolders();          if (rc) return rc;
    rc = unknown_md5_misses();                         if (rc) return rc;
    rc = ignores_files_that_are_not_maps();            if (rc) return rc;
    rc = rescan_tracks_added_and_removed_files();      if (rc) return rc;
    rc = rescan_rehashes_a_changed_file();             if (rc) return rc;
    rc = load_refuses_a_file_changed_since_the_scan(); if (rc) return rc;
    rc = searches_every_root_in_order();               if (rc) return rc;
    return 0;
}
