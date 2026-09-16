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
 * It then writes a second, one-file skin whose ini names no
 * RecommendedFilter, because the value that means "the author did not say"
 * is not the zero a SkinInfo starts life as.
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

/* The hostile-archive check writes entry names skinSourceZipDirectory never
 * would, so it drives minizip's writer itself. */
#include "zip.h"

#ifndef WB_SKINS_FIXTURE_DIR
#define WB_SKINS_FIXTURE_DIR "tests/fixtures/skins"
#endif

#define FLAT_ZIP    "skin_flat_test.zip"
#define WRAPPED_ZIP "skin_wrapped_test.zip"

/* Scratch skin the WorkshopId round trip builds and then removes, as a
 * directory and as the archive made from it. The two ids differ so the
 * second write cannot pass on the first one's value, and so do the two
 * publisher ids written beside them. The third id is written with no
 * publisher, which has to leave the publisher already recorded alone. */
#define WORKSHOP_DIR         "skin_workshop_test_dir"
#define WORKSHOP_ZIP         "skin_workshop_test.wsf"
#define WORKSHOP_ID_DIR      1234567890ULL
#define WORKSHOP_ID_ZIP      9876543210ULL
#define WORKSHOP_ID_REPUB    1357924680ULL
#define WORKSHOP_AUTHOR_DIR  76561197960287930ULL
#define WORKSHOP_AUTHOR_ZIP  76561198012345678ULL

/* An id no skin on disk can carry: skinSetActive scans the machine's real
 * prefpath and base-path skins folders, which are not assumed to be empty. */
#define MISSING_ID  "user:__wb_missing_skin__"

/* Scratch skin the absent-key check writes and then removes: a skin.ini that
 * names no RecommendedFilter, which the committed fixture does. */
#define NOKEY_DIR   "skin_nokey_test_dir"

/* The hostile archive, the directory it is extracted into, and the file its
 * escaping entries try to write one level above that directory. The scratch
 * directory sits inside a parent of its own so "../<name>" from inside it
 * lands somewhere this test owns and can check, not in the working
 * directory itself. */
#define HOSTILE_ZIP          "skin_hostile_test.wsf"
#define HOSTILE_PARENT       "skin_hostile_test_parent"
#define HOSTILE_DIR          HOSTILE_PARENT "/scratch"
#define HOSTILE_ESCAPE_NAME  "skin_hostile_escaped.txt"
#define HOSTILE_ESCAPE       HOSTILE_PARENT "/" HOSTILE_ESCAPE_NAME

static void removeSkinDir(const char *dir);

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
    UT_ASSERT_MSG(info->recommendedFilter == SKIN_FILTER_PIXELART,
                  "%s: RecommendedFilter is %d", label,
                  info->recommendedFilter);
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

/* SKIN_FILTER_NEAREST is 0, so a SkinInfo that has only been zeroed reads as
 * a recommendation of Nearest — a filter the author never asked for. A skin
 * whose ini does not mention the key, and a read with no source at all, both
 * have to come back SKIN_FILTER_NONE instead. */
static int checkNoRecommendedFilter(void) {
    static const char ini[] = "[Skin]\nName=No Filter Key\nMaxPixelDensity=1\n";
    char path[512];
    SkinSource *src;
    SkinInfo info;

    skinSourceReadIni(NULL, &info);
    UT_ASSERT_MSG(info.recommendedFilter == SKIN_FILTER_NONE,
                  "no source: RecommendedFilter is %d, want %d",
                  info.recommendedFilter, SKIN_FILTER_NONE);

    if (!SDL_CreateDirectory(NOKEY_DIR)) {
        UT_FAIL("SDL_CreateDirectory('%s') failed", NOKEY_DIR);
    }
    snprintf(path, sizeof(path), "%s/skin.ini", NOKEY_DIR);
    if (!SDL_SaveFile(path, ini, sizeof(ini) - 1)) {
        UT_FAIL("writing '%s' failed", path);
    }

    src = skinSourceOpen(NOKEY_DIR);
    if (src == NULL) {
        UT_FAIL("skinSourceOpen('%s') failed", NOKEY_DIR);
    }
    skinSourceReadIni(src, &info);
    /* And once more: the second answer comes out of the source's own cached
       SkinInfo, which is cleared the same way the caller's is. */
    skinSourceReadIni(src, &info);
    skinSourceClose(src);

    UT_ASSERT_MSG(strcmp(info.name, "No Filter Key") == 0,
                  "no key: Name is '%s', so that ini did not parse", info.name);
    UT_ASSERT_MSG(info.recommendedFilter == SKIN_FILTER_NONE,
                  "no key: RecommendedFilter is %d, want %d",
                  info.recommendedFilter, SKIN_FILTER_NONE);
    return 0;
}

/* Writes one stored entry with the exact name given, which is how a hostile
 * or archiver-made zip gets names skinSourceZipDirectory would never write. */
static int zipPutRaw(zipFile zf, const char *name, const char *body) {
    zip_fileinfo zi;
    memset(&zi, 0, sizeof(zi));
    if (zipOpenNewFileInZip(zf, name, &zi, NULL, 0, NULL, 0, NULL,
                            Z_DEFLATED, Z_DEFAULT_COMPRESSION) != ZIP_OK) {
        return -1;
    }
    if (zipWriteInFileInZip(zf, body, (unsigned int)strlen(body)) != ZIP_OK) {
        zipCloseFileInZip(zf);
        return -1;
    }
    return zipCloseFileInZip(zf) == ZIP_OK ? 0 : -1;
}

/* A .wsf is handed around, and Publish unpacks whatever it holds, so an
 * entry name that climbs out of the skin must never become a path. This
 * archive carries one such entry, plus the __MACOSX/ tree Finder's Compress
 * adds beside a skin folder; the skin's own files sit under one folder the
 * way that archiver leaves them. The reader has to drop both kinds, still
 * strip the folder, and extract nothing outside the scratch directory. */
static int checkHostileArchive(void) {
    SkinSource *src;
    SkinInfo    info;
    zipFile     zf;
    void       *buf = NULL;
    size_t      len = 0;
    SDL_PathInfo pi;
    int         rc = -1;

    remove(HOSTILE_ZIP);
    remove(HOSTILE_ESCAPE);
    removeSkinDir(HOSTILE_DIR);
    SDL_CreateDirectory(HOSTILE_PARENT);

    zf = zipOpen(HOSTILE_ZIP, APPEND_STATUS_CREATE);
    if (zf == NULL) {
        UT_FAIL("could not create %s", HOSTILE_ZIP);
    }
    if (zipPutRaw(zf, "mytheme/skin.ini",
                  "[Skin]\nName=Hostile\n") != 0 ||
        zipPutRaw(zf, "mytheme/grass.png", "not really a png") != 0 ||
        zipPutRaw(zf, "mytheme/sounds/bubbles.wav", "not a wav") != 0 ||
        zipPutRaw(zf, "__MACOSX/mytheme/._grass.png", "resource fork") != 0 ||
        zipPutRaw(zf, "mytheme/.DS_Store", "finder") != 0 ||
        zipPutRaw(zf, "../" HOSTILE_ESCAPE_NAME, "escaped") != 0 ||
        zipPutRaw(zf, "mytheme/../../" HOSTILE_ESCAPE_NAME, "escaped") != 0) {
        zipClose(zf, NULL);
        UT_FAIL("could not write the hostile entries");
    }
    zipClose(zf, NULL);

    src = skinSourceOpen(HOSTILE_ZIP);
    UT_ASSERT_MSG(src != NULL, "hostile archive did not open at all");

    /* The junk was dropped before the common folder was worked out, so the
     * skin's own names resolve at the top level. */
    skinSourceReadIni(src, &info);
    if (strcmp(info.name, "Hostile") != 0) {
        skinSourceClose(src);
        UT_FAIL("skin.ini under the wrapping folder was not found; Name is "
                "'%s'", info.name);
    }
    if (!skinSourceExists(src, "grass.png") ||
        !skinSourceExists(src, "sounds/bubbles.wav")) {
        skinSourceClose(src);
        UT_FAIL("the skin's own files did not resolve once the junk was "
                "dropped");
    }
    if (skinSourceExists(src, "../" HOSTILE_ESCAPE_NAME) ||
        skinSourceExists(src, "../../" HOSTILE_ESCAPE_NAME) ||
        skinSourceExists(src, HOSTILE_ESCAPE_NAME)) {
        skinSourceClose(src);
        UT_FAIL("an entry that climbs out of the skin was indexed");
    }
    if (skinSourceExists(src, "__macosx/mytheme/._grass.png") ||
        skinSourceExists(src, "._grass.png") ||
        skinSourceExists(src, ".ds_store")) {
        skinSourceClose(src);
        UT_FAIL("an archiver housekeeping entry was indexed");
    }

    /* Extraction writes the real files under the scratch directory and
     * nothing beside it. */
    if (!skinSourceExtractTo(src, HOSTILE_DIR)) {
        skinSourceClose(src);
        UT_FAIL("extraction of the hostile archive failed outright");
    }
    skinSourceClose(src);

    if (SDL_GetPathInfo(HOSTILE_ESCAPE, &pi)) {
        UT_FAIL("extraction wrote %s outside the scratch directory",
                HOSTILE_ESCAPE);
    }
    if (!SDL_LoadFile(HOSTILE_DIR "/grass.png", &len)) {
        UT_FAIL("extraction did not write grass.png into the scratch "
                "directory");
    }
    buf = SDL_LoadFile(HOSTILE_DIR "/sounds/bubbles.wav", &len);
    if (buf == NULL || len != strlen("not a wav")) {
        SDL_free(buf);
        UT_FAIL("extraction did not write sounds/bubbles.wav");
    }
    SDL_free(buf);
    rc = 0;
    return rc;
}

int run_skin_source_dir_and_zip(void) {
    const char *dir = getenv("WB_SKINS_FIXTURE_DIR");
    char fixture[512];
    int rc;

    if (dir == NULL || dir[0] == '\0') dir = WB_SKINS_FIXTURE_DIR;
    snprintf(fixture, sizeof(fixture), "%s/basic", dir);

    rc = checkSkinSources(fixture);
    if (rc == 0) rc = checkNoRecommendedFilter();
    if (rc == 0) rc = checkHostileArchive();

    remove(FLAT_ZIP);
    remove(WRAPPED_ZIP);
    remove(HOSTILE_ZIP);
    remove(HOSTILE_ESCAPE);
    removeSkinDir(HOSTILE_DIR);
    SDL_RemovePath(HOSTILE_PARENT);
    /* One file went in, so the directory empties without a walk. */
    remove(NOKEY_DIR "/skin.ini");
    SDL_RemovePath(NOKEY_DIR);
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
 * later publish updates that item instead of making a second one, and the
 * publisher's SteamID beside it, so a later publish can tell that author's
 * own item from one that arrived inside somebody else's skin. The skin on
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
    UT_ASSERT_MSG(info->recommendedFilter == SKIN_FILTER_PIXELART,
                  "%s: RecommendedFilter is %d", label,
                  info->recommendedFilter);
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
    if (!skinSetWorkshopId(WORKSHOP_DIR, WORKSHOP_ID_DIR,
                           WORKSHOP_AUTHOR_DIR)) {
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
    if (info.workshopAuthor != WORKSHOP_AUTHOR_DIR) {
        UT_FAIL("directory: WorkshopAuthor is %llu, want %llu",
                (unsigned long long)info.workshopAuthor,
                (unsigned long long)WORKSHOP_AUTHOR_DIR);
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
    if (!skinSetWorkshopId(WORKSHOP_ZIP, WORKSHOP_ID_ZIP,
                           WORKSHOP_AUTHOR_ZIP)) {
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
    if (info.workshopAuthor != WORKSHOP_AUTHOR_ZIP) {
        UT_FAIL("archive: WorkshopAuthor is %llu, want %llu",
                (unsigned long long)info.workshopAuthor,
                (unsigned long long)WORKSHOP_AUTHOR_ZIP);
    }
    rc = checkOtherIniKeys(&info, "archive");
    if (rc != 0) return rc;

    /* A write with no publisher: the id changes and the publisher already in
     * the ini stays, rather than being replaced by a line saying nobody
     * published it. The directory still carries the first write's pair. */
    if (!skinSetWorkshopId(WORKSHOP_DIR, WORKSHOP_ID_REPUB, 0)) {
        UT_FAIL("skinSetWorkshopId('%s', %llu, 0) failed", WORKSHOP_DIR,
                (unsigned long long)WORKSHOP_ID_REPUB);
    }
    src = skinSourceOpen(WORKSHOP_DIR);
    if (src == NULL) {
        UT_FAIL("reopening the directory '%s' failed", WORKSHOP_DIR);
    }
    skinSourceReadIni(src, &info);
    skinSourceClose(src);
    if (info.workshopId != WORKSHOP_ID_REPUB) {
        UT_FAIL("unknown publisher: WorkshopId is %llu, want %llu",
                (unsigned long long)info.workshopId,
                (unsigned long long)WORKSHOP_ID_REPUB);
    }
    if (info.workshopAuthor != WORKSHOP_AUTHOR_DIR) {
        UT_FAIL("unknown publisher: WorkshopAuthor is %llu, want the %llu the "
                "earlier write left", (unsigned long long)info.workshopAuthor,
                (unsigned long long)WORKSHOP_AUTHOR_DIR);
    }
    return checkOtherIniKeys(&info, "unknown publisher");
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

/* The [MapPalette] section. A skin may name any subset of the colours a
 * zoomed-out map is drawn in; the ones it does not name have to come back
 * SKIN_COLOUR_NONE so map_colours keeps its built-in for those. 0 is a real
 * colour here (black road), so "absent" cannot be 0 the way a zeroed struct
 * would give. */
#define PALETTE_DIR "skin_map_palette_test_dir"

int run_skin_map_palette(void) {
    static const char ini[] =
        "[Skin]\n"
        "Name=Palette Skin\n"
        "MaxPixelDensity=2\n"
        "\n"
        "[MapPalette]\n"
        "Grass=#123456\n"          /* the documented spelling */
        "Swamp=abcdef\n"           /* bare, and lower case */
        "Road=0xFF0000\n"          /* 0x, and upper case */
        "deepsea=#000000\n"        /* key case is not significant; 0 is a colour */
        "MarkerGood=#00ff00\n"
        "MarkerSelf=#010203\n"
        "TeamGrey=#111111\n"          /* the first of the seventeen */
        "teampurple=#222222\n"        /* the last, key case ignored */
        "Rubble=#12345\n"          /* five digits: not a colour */
        "Crater=#1234567\n"        /* seven: not a colour either */
        "Forest=green\n"           /* not hex at all */
        "Boat=\n"                   /* empty */
        "\n"
        "[SomethingElse]\n"
        "Grass=#ffffff\n"          /* a section we do not know is skipped */
        "Name=Wrong Name\n";
    char path[512];
    SkinSource *src;
    SkinInfo info;
    int rc = 0;

    /* No source at all: every entry absent, so a palette read before any skin
       is chosen leaves the built-ins alone rather than painting everything
       black. */
    skinSourceReadIni(NULL, &info);
    UT_ASSERT_MSG(info.mapPalette.grass == SKIN_COLOUR_NONE &&
                  info.mapPalette.road == SKIN_COLOUR_NONE &&
                  info.mapPalette.markerGood == SKIN_COLOUR_NONE,
                  "no source: grass %d, road %d, markerGood %d, want %d",
                  info.mapPalette.grass, info.mapPalette.road,
                  info.mapPalette.markerGood, SKIN_COLOUR_NONE);

    if (!SDL_CreateDirectory(PALETTE_DIR)) {
        UT_FAIL("SDL_CreateDirectory('%s') failed", PALETTE_DIR);
    }
    snprintf(path, sizeof(path), "%s/skin.ini", PALETTE_DIR);
    if (!SDL_SaveFile(path, ini, sizeof(ini) - 1)) {
        UT_FAIL("writing '%s' failed", path);
    }

    src = skinSourceOpen(PALETTE_DIR);
    if (src == NULL) {
        remove(path);
        removeSkinDir(PALETTE_DIR);
        UT_FAIL("skinSourceOpen('%s') failed", PALETTE_DIR);
    }
    skinSourceReadIni(src, &info);
    skinSourceClose(src);
    remove(path);
    removeSkinDir(PALETTE_DIR);

    /* [Skin] still parses with another section after it. */
    UT_ASSERT_MSG(strcmp(info.name, "Palette Skin") == 0,
                  "Name is '%s': the [Skin] section did not parse, or a later "
                  "section was read as part of it", info.name);
    UT_ASSERT_MSG(info.maxPixelDensity == 2,
                  "MaxPixelDensity is %d, want 2", info.maxPixelDensity);

    /* The three spellings of a colour. */
    UT_ASSERT_MSG(info.mapPalette.grass == 0x123456,
                  "Grass is %06x, want 123456", info.mapPalette.grass);
    UT_ASSERT_MSG(info.mapPalette.swamp == 0xabcdef,
                  "Swamp is %06x, want abcdef", info.mapPalette.swamp);
    UT_ASSERT_MSG(info.mapPalette.road == 0xff0000,
                  "Road is %06x, want ff0000", info.mapPalette.road);

    /* Black is a colour, not an absence. */
    UT_ASSERT_MSG(info.mapPalette.deepSea == 0,
                  "DeepSea is %d, want 0 — black has to survive as a value",
                  info.mapPalette.deepSea);

    UT_ASSERT_MSG(info.mapPalette.markerGood == 0x00ff00,
                  "MarkerGood is %06x, want 00ff00", info.mapPalette.markerGood);

    /* Four ways to write something that is not a colour. Each leaves its own
       entry absent and none of them disturbs the others. */
    UT_ASSERT_MSG(info.mapPalette.rubble == SKIN_COLOUR_NONE,
                  "Rubble is %d: five digits was taken as a colour",
                  info.mapPalette.rubble);
    UT_ASSERT_MSG(info.mapPalette.crater == SKIN_COLOUR_NONE,
                  "Crater is %d: seven digits was taken as a colour",
                  info.mapPalette.crater);
    UT_ASSERT_MSG(info.mapPalette.forest == SKIN_COLOUR_NONE,
                  "Forest is %d: 'green' was taken as a colour",
                  info.mapPalette.forest);
    UT_ASSERT_MSG(info.mapPalette.boat == SKIN_COLOUR_NONE,
                  "Boat is %d: an empty value was taken as a colour",
                  info.mapPalette.boat);

    /* markerSelf and the seventeen assignable colours are settable, and an
     * unnamed one of the seventeen is left alone rather than dragged along
     * with its neighbours. */
    UT_ASSERT_MSG(info.mapPalette.markerSelf == 0x010203,
                  "MarkerSelf is %d, want 010203", info.mapPalette.markerSelf);
    UT_ASSERT_MSG(info.mapPalette.team[0] == 0x111111,
                  "TeamGrey is %d, want 111111", info.mapPalette.team[0]);
    UT_ASSERT_MSG(info.mapPalette.team[SKIN_TEAM_COLOUR_COUNT - 1] == 0x222222,
                  "teampurple is %d, want 222222",
                  info.mapPalette.team[SKIN_TEAM_COLOUR_COUNT - 1]);
    {
        int i;
        for (i = 1; i < SKIN_TEAM_COLOUR_COUNT - 1; i++) {
            UT_ASSERT_MSG(info.mapPalette.team[i] == SKIN_COLOUR_NONE,
                          "team[%d] is %d, but the ini named neither it nor "
                          "anything next to it", i, info.mapPalette.team[i]);
        }
    }

    /* Keys the ini never named. */
    UT_ASSERT_MSG(info.mapPalette.building == SKIN_COLOUR_NONE &&
                  info.mapPalette.halfBuilding == SKIN_COLOUR_NONE &&
                  info.mapPalette.river == SKIN_COLOUR_NONE &&
                  info.mapPalette.markerEvil == SKIN_COLOUR_NONE &&
                  info.mapPalette.markerNeutral == SKIN_COLOUR_NONE,
                  "an unnamed key came back set");

    return rc;
}
