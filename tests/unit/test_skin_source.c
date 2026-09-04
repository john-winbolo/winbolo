/*
 * SkinSource directory/zip parity (src/gui/sdl3/skin_source.c).
 *
 * A skin is a directory or a zip, and every asset reader resolves names the
 * same way whichever it is. This test opens the committed directory fixture,
 * zips it twice with skinSourceZipDirectory — once flat, once under a
 * wrapping top-level folder, the shape a 1.x .wsf has — and asserts all three
 * sources answer the same relative names with the same bytes: skin.ini parses
 * to the same SkinInfo, the 2.x sounds/ wav and the 1.x flat wav both
 * resolve, and a sprite reads back byte-for-byte. The wrapped zip passing
 * proves the single common top-level folder is stripped from the index.
 *
 * No .wsf or .zip is committed: the archives are built in the working
 * directory and removed at the end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "skin_source.h"
#include "test_harness.h"

#ifndef WB_SKINS_FIXTURE_DIR
#define WB_SKINS_FIXTURE_DIR "tests/fixtures/skins"
#endif

#define FLAT_ZIP    "skin_flat_test.zip"
#define WRAPPED_ZIP "skin_wrapped_test.zip"

/* Scratch skin the WorkshopId round trip builds and then removes, as a
 * directory and as the archive made from it. The two ids differ so the
 * second write cannot pass on the first one's value. */
#define WORKSHOP_DIR     "skin_workshop_test_dir"
#define WORKSHOP_ZIP     "skin_workshop_test.wsf"
#define WORKSHOP_ID_DIR  1234567890ULL
#define WORKSHOP_ID_ZIP  9876543210ULL

/* An id no skin on disk can carry: skinSetActive scans the machine's real
 * prefpath and base-path skins folders, which are not assumed to be empty. */
#define MISSING_ID  "user:__wb_missing_skin__"

/* Every source must agree with the directory fixture's skin.ini. */
static int checkIni(const SkinInfo *info, const char *label) {
    UT_ASSERT_MSG(strcmp(info->name, "Basic Test Skin") == 0,
                  "%s: Name is '%s'", label, info->name);
    UT_ASSERT_MSG(strcmp(info->author, "WinBolo Tests") == 0,
                  "%s: Author is '%s'", label, info->author);
    UT_ASSERT_MSG(strcmp(info->notes, "Fixture for test_skin_source") == 0,
                  "%s: Notes is '%s'", label, info->notes);
    UT_ASSERT_MSG(info->maxPixelDensity == 2,
                  "%s: MaxPixelDensity is %d", label, info->maxPixelDensity);
    UT_ASSERT_MSG(info->inGameRotate == 0,
                  "%s: InGameRotate is %d", label, info->inGameRotate);
    UT_ASSERT_MSG(info->workshopId == 0,
                  "%s: WorkshopId is not zero", label);
    return 0;
}

/* A head read has to hand back a prefix of the full read: the first bytes
   when the file is longer than asked for, and every byte when it is not.
   The density scan reads sheet headers this way, so a zip source must not
   need the whole entry inflated to answer. */
static int checkHead(SkinSource *src, const char *label,
                     const unsigned char *refPng, size_t refLen) {
    unsigned char head[16];
    unsigned char *whole;
    size_t got = 0;

    UT_ASSERT_MSG(refLen > sizeof(head),
                  "%s: fixture PNG is %d bytes, too short for the head check",
                  label, (int)refLen);

    if (!skinSourceReadHead(src, "tank_self_00.png", head, sizeof(head), &got)) {
        UT_FAIL("%s: head read of tank_self_00.png failed", label);
    }
    UT_ASSERT_MSG(got == sizeof(head),
                  "%s: head read returned %d bytes, asked for %d",
                  label, (int)got, (int)sizeof(head));
    UT_ASSERT_MSG(memcmp(head, refPng, sizeof(head)) == 0,
                  "%s: head read differs from the start of the full read",
                  label);

    /* Asking for more than the file holds gets the whole file and no more. */
    whole = (unsigned char *)SDL_malloc(refLen + 64);
    if (whole == NULL) {
        UT_FAIL("%s: out of memory", label);
    }
    if (!skinSourceReadHead(src, "tank_self_00.png", whole, refLen + 64, &got)) {
        SDL_free(whole);
        UT_FAIL("%s: oversized head read of tank_self_00.png failed", label);
    }
    if (got != refLen || memcmp(whole, refPng, refLen) != 0) {
        SDL_free(whole);
        UT_FAIL("%s: oversized head read gave %d bytes, full read gave %d, "
                "or the bytes differ", label, (int)got, (int)refLen);
    }
    SDL_free(whole);

    if (skinSourceReadHead(src, "not_in_this_skin.png", head, sizeof(head), &got)) {
        UT_FAIL("%s: head read of a name that isn't there succeeded", label);
    }
    return 0;
}

/* Open an archive and match it against the directory source's answers. */
static int checkArchive(const char *zipPath, const char *label,
                        const unsigned char *refPng, size_t refLen) {
    SkinSource *src;
    SkinInfo info;
    void *png = NULL;
    size_t pngLen = 0;
    int rc;

    src = skinSourceOpen(zipPath);
    UT_ASSERT_MSG(src != NULL, "%s: skinSourceOpen('%s') failed",
                  label, zipPath);

    skinSourceReadIni(src, &info);
    rc = checkIni(&info, label);
    if (rc != 0) {
        skinSourceClose(src);
        return rc;
    }

    if (!skinSourceExists(src, "tank_self_00.png") ||
        !skinSourceExists(src, "grass.png") ||
        !skinSourceExists(src, "sounds/hit_tank_far.wav") ||
        !skinSourceExists(src, "bubbles.wav")) {
        skinSourceClose(src);
        UT_FAIL("%s: an expected name is missing from the index", label);
    }

    if (!skinSourceRead(src, "tank_self_00.png", &png, &pngLen)) {
        skinSourceClose(src);
        UT_FAIL("%s: reading tank_self_00.png failed", label);
    }
    if (pngLen != refLen ||
        memcmp(png, refPng, refLen) != 0) {
        SDL_free(png);
        skinSourceClose(src);
        UT_FAIL("%s: tank_self_00.png differs from the directory read "
                "(%d vs %d bytes)", label, (int)pngLen, (int)refLen);
    }
    if (((const unsigned char *)png)[pngLen] != '\0') {
        SDL_free(png);
        skinSourceClose(src);
        UT_FAIL("%s: read buffer is not NUL-terminated at [len]", label);
    }
    SDL_free(png);

    rc = checkHead(src, label, refPng, refLen);
    skinSourceClose(src);
    return rc;
}

static int checkSkinSources(const char *fixture) {
    SkinSource *dirSrc;
    SkinInfo info;
    void *png = NULL;
    size_t pngLen = 0;
    int rc;

    dirSrc = skinSourceOpen(fixture);
    if (dirSrc == NULL) {
        UT_FAIL("fixture missing or unreadable: %s", fixture);
    }

    skinSourceReadIni(dirSrc, &info);
    rc = checkIni(&info, "directory");
    if (rc != 0) {
        skinSourceClose(dirSrc);
        return rc;
    }

    if (!skinSourceExists(dirSrc, "tank_self_00.png") ||
        !skinSourceExists(dirSrc, "sounds/hit_tank_far.wav") ||
        !skinSourceExists(dirSrc, "bubbles.wav")) {
        skinSourceClose(dirSrc);
        UT_FAIL("directory: an expected name is missing from the index");
    }
    if (skinSourceExists(dirSrc, "not_in_this_skin.png")) {
        skinSourceClose(dirSrc);
        UT_FAIL("directory: a name that isn't there resolved");
    }

    if (!skinSourceRead(dirSrc, "tank_self_00.png", &png, &pngLen)) {
        skinSourceClose(dirSrc);
        UT_FAIL("directory: reading tank_self_00.png failed");
    }
    if (pngLen == 0) {
        SDL_free(png);
        skinSourceClose(dirSrc);
        UT_FAIL("directory: tank_self_00.png read as empty");
    }
    /* SDL_LoadFile's contract, which the zip arm has to match. */
    if (((const unsigned char *)png)[pngLen] != '\0') {
        SDL_free(png);
        skinSourceClose(dirSrc);
        UT_FAIL("directory: read buffer is not NUL-terminated at [len]");
    }

    rc = checkHead(dirSrc, "directory", (const unsigned char *)png, pngLen);
    if (rc != 0) {
        SDL_free(png);
        skinSourceClose(dirSrc);
        return rc;
    }

    /* The ini is parsed once and cached, so a second read is the same
       answer out of the cache rather than a fresh parse. */
    {
        SkinInfo again;
        skinSourceReadIni(dirSrc, &again);
        if (memcmp(&again, &info, sizeof(info)) != 0) {
            SDL_free(png);
            skinSourceClose(dirSrc);
            UT_FAIL("directory: a second skinSourceReadIni disagrees with the first");
        }
    }
    skinSourceClose(dirSrc);

    if (!skinSourceZipDirectory(fixture, FLAT_ZIP, NULL)) {
        SDL_free(png);
        UT_FAIL("skinSourceZipDirectory('%s', '%s', NULL) failed",
                fixture, FLAT_ZIP);
    }
    if (!skinSourceZipDirectory(fixture, WRAPPED_ZIP, "basic")) {
        SDL_free(png);
        UT_FAIL("skinSourceZipDirectory('%s', '%s', \"basic\") failed",
                fixture, WRAPPED_ZIP);
    }

    rc = checkArchive(FLAT_ZIP, "flat zip",
                      (const unsigned char *)png, pngLen);
    if (rc == 0) {
        rc = checkArchive(WRAPPED_ZIP, "wrapped zip",
                          (const unsigned char *)png, pngLen);
    }

    SDL_free(png);
    return rc;
}

int run_skin_source_dir_and_zip(void) {
    const char *dir = getenv("WB_SKINS_FIXTURE_DIR");
    char fixture[512];
    int rc;

    if (dir == NULL || dir[0] == '\0') dir = WB_SKINS_FIXTURE_DIR;
    snprintf(fixture, sizeof(fixture), "%s/basic", dir);

    rc = checkSkinSources(fixture);

    remove(FLAT_ZIP);
    remove(WRAPPED_ZIP);
    return rc;
}

/* skinGetActive names the skin whose assets are loaded; skinGetRequested
 * names the one the player picked. They part company on an id that cannot be
 * resolved right now — a Workshop item still downloading, a .wsf on a drive
 * that is not mounted — and it is the requested id that is written to
 * WinBolo.json, so a skin that is momentarily absent is not erased from the
 * preferences by the next save. */
static int checkActiveVsRequested(void) {
    /* Built-in assets: nothing loaded and nothing chosen. */
    if (!skinSetActive("")) {
        UT_FAIL("skinSetActive(\"\") returned false");
    }
    if (skinGetActive()[0] != '\0') {
        UT_FAIL("after \"\": skinGetActive() is '%s', want empty",
                skinGetActive());
    }
    if (skinGetRequested()[0] != '\0') {
        UT_FAIL("after \"\": skinGetRequested() is '%s', want empty",
                skinGetRequested());
    }
    if (skinGetActiveSource() != NULL) {
        UT_FAIL("after \"\": skinGetActiveSource() is not NULL");
    }

    /* A skin that is not there: the load fails and the built-in assets stay
     * loaded, but the id survives as the choice. */
    if (skinSetActive(MISSING_ID)) {
        UT_FAIL("skinSetActive(\"%s\") returned true", MISSING_ID);
    }
    if (skinGetActive()[0] != '\0') {
        UT_FAIL("after a failed load: skinGetActive() is '%s', want empty",
                skinGetActive());
    }
    if (skinGetActiveSource() != NULL) {
        UT_FAIL("after a failed load: skinGetActiveSource() is not NULL");
    }
    if (strcmp(skinGetRequested(), MISSING_ID) != 0) {
        UT_FAIL("after a failed load: skinGetRequested() is '%s', want '%s'",
                skinGetRequested(), MISSING_ID);
    }

    /* "default" is the built-in assets by another name, and clears both. */
    if (!skinSetActive("default")) {
        UT_FAIL("skinSetActive(\"default\") returned false");
    }
    if (skinGetActive()[0] != '\0' || skinGetRequested()[0] != '\0') {
        UT_FAIL("after \"default\": active '%s', requested '%s', want both "
                "empty", skinGetActive(), skinGetRequested());
    }

    /* NULL clears both the same way. Requested is put back first so the
     * clear is what the assertion is reading. */
    skinSetActive(MISSING_ID);
    if (!skinSetActive(NULL)) {
        UT_FAIL("skinSetActive(NULL) returned false");
    }
    if (skinGetActive()[0] != '\0' || skinGetRequested()[0] != '\0') {
        UT_FAIL("after NULL: active '%s', requested '%s', want both empty",
                skinGetActive(), skinGetRequested());
    }
    return 0;
}

int run_skin_active_vs_requested(void) {
    int rc = checkActiveVsRequested();

    /* The registry is global: leave it on the built-in assets for whatever
     * test runs next. */
    skinSetActive("");
    return rc;
}

/* Publishing to the Workshop writes the item's id back into the skin, so a
 * later publish updates that item instead of making a second one. The skin on
 * disk is a directory or an archive, and either way every other key in
 * skin.ini and every other file in the skin has to come through untouched —
 * an archive is unpacked, edited and zipped up again to get there.
 *
 * The directory under test is the fixture run through skinSourceExtractTo,
 * so that walk is covered too, and the archive is that directory zipped. */

/* Everything the fixture's skin.ini carries besides WorkshopId: the rewrite
 * has to hand all of it back exactly as it found it. */
static int checkOtherIniKeys(const SkinInfo *info, const char *label) {
    UT_ASSERT_MSG(strcmp(info->name, "Basic Test Skin") == 0,
                  "%s: Name is '%s'", label, info->name);
    UT_ASSERT_MSG(strcmp(info->author, "WinBolo Tests") == 0,
                  "%s: Author is '%s'", label, info->author);
    UT_ASSERT_MSG(strcmp(info->notes, "Fixture for test_skin_source") == 0,
                  "%s: Notes is '%s'", label, info->notes);
    UT_ASSERT_MSG(info->maxPixelDensity == 2,
                  "%s: MaxPixelDensity is %d", label, info->maxPixelDensity);
    return 0;
}

static int checkWorkshopIdRoundtrip(const char *fixture) {
    SkinSource *src;
    SkinInfo    info;
    int         rc;

    src = skinSourceOpen(fixture);
    if (src == NULL) {
        UT_FAIL("fixture missing or unreadable: %s", fixture);
    }
    if (!skinSourceExtractTo(src, WORKSHOP_DIR)) {
        skinSourceClose(src);
        UT_FAIL("skinSourceExtractTo('%s') failed", WORKSHOP_DIR);
    }
    skinSourceClose(src);

    /* Directory skin: skin.ini is rewritten where it sits. */
    if (!skinSetWorkshopId(WORKSHOP_DIR, WORKSHOP_ID_DIR)) {
        UT_FAIL("skinSetWorkshopId('%s', %llu) failed", WORKSHOP_DIR,
                (unsigned long long)WORKSHOP_ID_DIR);
    }
    src = skinSourceOpen(WORKSHOP_DIR);
    if (src == NULL) {
        UT_FAIL("reopening the extracted directory '%s' failed", WORKSHOP_DIR);
    }
    skinSourceReadIni(src, &info);
    skinSourceClose(src);
    if (info.workshopId != WORKSHOP_ID_DIR) {
        UT_FAIL("directory: WorkshopId is %llu, want %llu",
                (unsigned long long)info.workshopId,
                (unsigned long long)WORKSHOP_ID_DIR);
    }
    rc = checkOtherIniKeys(&info, "directory");
    if (rc != 0) return rc;

    /* Archive skin, under a second id: the one already in the ini has to be
     * replaced rather than joined by another, and the id read back says
     * which happened — a leftover line sits after the new one, so a parse
     * would come away with the old value. */
    if (!skinSourceZipDirectory(WORKSHOP_DIR, WORKSHOP_ZIP, NULL)) {
        UT_FAIL("skinSourceZipDirectory('%s', '%s', NULL) failed",
                WORKSHOP_DIR, WORKSHOP_ZIP);
    }
    if (!skinSetWorkshopId(WORKSHOP_ZIP, WORKSHOP_ID_ZIP)) {
        UT_FAIL("skinSetWorkshopId('%s', %llu) failed", WORKSHOP_ZIP,
                (unsigned long long)WORKSHOP_ID_ZIP);
    }
    src = skinSourceOpen(WORKSHOP_ZIP);
    if (src == NULL) {
        UT_FAIL("reopening the rebuilt archive '%s' failed", WORKSHOP_ZIP);
    }
    skinSourceReadIni(src, &info);
    if (!skinSourceExists(src, "tank_self_00.png") ||
        !skinSourceExists(src, "sounds/hit_tank_far.wav")) {
        skinSourceClose(src);
        UT_FAIL("archive: a file did not survive the rebuild");
    }
    skinSourceClose(src);
    if (info.workshopId != WORKSHOP_ID_ZIP) {
        UT_FAIL("archive: WorkshopId is %llu, want %llu",
                (unsigned long long)info.workshopId,
                (unsigned long long)WORKSHOP_ID_ZIP);
    }
    return checkOtherIniKeys(&info, "archive");
}

/* SDL_RemovePath will not delete a directory with anything left in it, so
   the files go first, and sounds/ before the folder holding it. */
static void removeDirFiles(const char *dir) {
    char **list;
    int    count = 0;
    int    i;

    list = SDL_GlobDirectory(dir, "*", 0, &count);
    if (list == NULL) return;
    for (i = 0; i < count; i++) {
        char full[512];
        if (list[i] == NULL || list[i][0] == '\0') continue;
        snprintf(full, sizeof(full), "%s/%s", dir, list[i]);
        SDL_RemovePath(full);
    }
    SDL_free(list);
}

static void removeSkinDir(const char *dir) {
    char sounds[512];

    snprintf(sounds, sizeof(sounds), "%s/sounds", dir);
    removeDirFiles(sounds);
    SDL_RemovePath(sounds);
    removeDirFiles(dir);
    SDL_RemovePath(dir);
}

int run_skin_workshop_id_roundtrip(void) {
    const char *dir = getenv("WB_SKINS_FIXTURE_DIR");
    char fixture[512];
    int rc;

    if (dir == NULL || dir[0] == '\0') dir = WB_SKINS_FIXTURE_DIR;
    snprintf(fixture, sizeof(fixture), "%s/basic", dir);

    rc = checkWorkshopIdRoundtrip(fixture);

    removeSkinDir(WORKSHOP_DIR);
    remove(WORKSHOP_ZIP);
    return rc;
}
