/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * The recap clip GIF rules (clip_gif_core.c): the length field and the seek
 * bar's end marker give the same length and are clamped the same way, the
 * gallery's metadata file reads back what was written, bad or missing
 * metadata is handled, and the gallery lists newest first.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "clip_gif_core.h"
#include "test_harness.h"

#define EXPECT_U32(what, got, want)                                          \
    do {                                                                     \
        uint32_t g_ = (got), w_ = (want);                                    \
        if (g_ != w_) {                                                      \
            fprintf(stderr, "%s: %s: got %u, want %u\n", utCurrentTestName(), \
                    (what), (unsigned)g_, (unsigned)w_);                     \
            fails++;                                                         \
        }                                                                    \
    } while (0)

#define EXPECT_TRUE(what, cond)                                              \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s: %s\n", utCurrentTestName(), (what));        \
            fails++;                                                         \
        }                                                                    \
    } while (0)

#define EXPECT_STR(what, got, want)                                          \
    do {                                                                     \
        if (strcmp((got), (want)) != 0) {                                    \
            fprintf(stderr, "%s: %s: got \"%s\", want \"%s\"\n",             \
                    utCurrentTestName(), (what), (got), (want));             \
            fails++;                                                         \
        }                                                                    \
    } while (0)

static int nearF(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

int run_clip_gif_len_sync(void) {
    int fails = 0;

    /* Clamping with no replay length known: whole frames, floor and cap. */
    EXPECT_U32("zero to floor", clipGifLenClampMs(0, 0, 0), 1000);
    EXPECT_U32("round down", clipGifLenClampMs(5049, 0, 0), 5000);
    EXPECT_U32("round up", clipGifLenClampMs(5050, 0, 0), 5100);
    EXPECT_U32("cap", clipGifLenClampMs(99999, 0, 0), 15000);
    EXPECT_U32("cap at max u32", clipGifLenClampMs(0xFFFFFFFFu, 0, 0), 15000);
    EXPECT_U32("default kept", clipGifLenClampMs(CLIP_GIF_LEN_DEFAULT_MS, 0, 0),
               5000);

    /* Held to the time left in the replay, never under the floor. */
    EXPECT_U32("room", clipGifLenClampMs(5000, 10000, 60000), 5000);
    EXPECT_U32("near end", clipGifLenClampMs(5000, 58000, 60000), 2000);
    EXPECT_U32("near end whole frames", clipGifLenClampMs(5000, 58050, 60000),
               1900);
    EXPECT_U32("under floor at end", clipGifLenClampMs(5000, 59500, 60000),
               1000);
    EXPECT_U32("past end", clipGifLenClampMs(5000, 70000, 60000), 1000);

    /* The length field's seconds. */
    EXPECT_U32("2.5 s", clipGifLenFromSeconds(2.5f, 0, 0), 2500);
    EXPECT_U32("2.54 s", clipGifLenFromSeconds(2.54f, 0, 0), 2500);
    EXPECT_U32("2.56 s", clipGifLenFromSeconds(2.56f, 0, 0), 2600);
    EXPECT_U32("negative", clipGifLenFromSeconds(-3.0f, 0, 0), 1000);
    EXPECT_U32("nan", clipGifLenFromSeconds(NAN, 0, 0), 1000);
    EXPECT_U32("huge", clipGifLenFromSeconds(1.0e9f, 0, 0), 15000);
    EXPECT_U32("seconds near end", clipGifLenFromSeconds(10.0f, 57000, 60000),
               3000);
    EXPECT_TRUE("to seconds", nearF(clipGifLenToSeconds(2500), 2.5f));

    /* Timeline x for a time. */
    EXPECT_TRUE("x at 0", nearF(clipGifTimelineX(0, 60000, 100.0f, 300.0f), 100.0f));
    EXPECT_TRUE("x at half",
                nearF(clipGifTimelineX(30000, 60000, 100.0f, 300.0f), 200.0f));
    EXPECT_TRUE("x past end",
                nearF(clipGifTimelineX(90000, 60000, 100.0f, 300.0f), 300.0f));
    EXPECT_TRUE("x no total", nearF(clipGifTimelineX(500, 0, 100.0f, 300.0f), 100.0f));
    EXPECT_TRUE("x empty bar",
                nearF(clipGifTimelineX(500, 60000, 300.0f, 300.0f), 300.0f));

    /* The marker and the field stay in step: every length the field can hold
     * puts the marker at an x that reads back as the same length. */
    {
        const float x0 = 50.0f, x1 = 1050.0f;
        const uint32_t total = 120000, cur = 30000;
        for (uint32_t len = CLIP_GIF_LEN_MIN_MS; len <= CLIP_GIF_LEN_MAX_MS;
             len += CLIP_GIF_LEN_STEP_MS) {
            float secs = clipGifLenToSeconds(len);
            uint32_t fromField = clipGifLenFromSeconds(secs, cur, total);
            float x = clipGifTimelineX(cur + fromField, total, x0, x1);
            uint32_t fromMarker = clipGifLenFromTimelineX(x, cur, total, x0, x1);
            if (fromField != len || fromMarker != len) {
                fprintf(stderr,
                        "%s: len %u: field gave %u, marker gave %u\n",
                        utCurrentTestName(), (unsigned)len,
                        (unsigned)fromField, (unsigned)fromMarker);
                fails++;
            }
        }
    }

    /* Dragging the marker. */
    EXPECT_U32("drag before playhead",
               clipGifLenFromTimelineX(0.0f, 30000, 60000, 100.0f, 700.0f), 1000);
    EXPECT_U32("drag past bar end",
               clipGifLenFromTimelineX(5000.0f, 50000, 60000, 100.0f, 700.0f),
               10000);
    EXPECT_U32("drag to end near end",
               clipGifLenFromTimelineX(700.0f, 57000, 60000, 100.0f, 700.0f),
               3000);
    EXPECT_U32("drag past cap",
               clipGifLenFromTimelineX(700.0f, 0, 60000, 100.0f, 700.0f), 15000);
    /* 10 ms of replay a pixel here: x 400 is 30000 ms, 6000 after the playhead. */
    EXPECT_U32("drag mid",
               clipGifLenFromTimelineX(400.0f, 24000, 60000, 100.0f, 700.0f),
               6000);
    EXPECT_U32("drag nan",
               clipGifLenFromTimelineX(NAN, 24000, 60000, 100.0f, 700.0f), 1000);
    EXPECT_U32("drag no total",
               clipGifLenFromTimelineX(400.0f, 0, 0, 100.0f, 700.0f), 1000);

    return fails ? 1 : 0;
}

static void fillMeta(ClipGifMeta *m) {
    memset(m, 0, sizeof(*m));
    m->createdUnix = 1760976123;
    strcpy(m->map, "Everard Island");
    m->playerCount = 3;
    strcpy(m->players[0], "Alpha");
    strcpy(m->players[1], "J\xc3\xb6rg \"the tank\"");  /* UTF-8 and quotes */
    strcpy(m->players[2], "Bot 3");
    m->durationMs = 4800;
    m->width = 480;
    m->height = 270;
}

static int sameMeta(const ClipGifMeta *a, const ClipGifMeta *b) {
    if (a->createdUnix != b->createdUnix || strcmp(a->map, b->map) != 0 ||
        a->playerCount != b->playerCount || a->durationMs != b->durationMs ||
        a->width != b->width || a->height != b->height) {
        return 0;
    }
    for (int i = 0; i < a->playerCount; i++) {
        if (strcmp(a->players[i], b->players[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

int run_clip_gif_meta_roundtrip(void) {
    int fails = 0;
    ClipGifMeta in, out;
    fillMeta(&in);

    char *json = clipGifMetaToJson(&in);
    EXPECT_TRUE("to json", json != NULL);
    if (json != NULL) {
        EXPECT_TRUE("from json", clipGifMetaFromJson(json, strlen(json), &out));
        EXPECT_TRUE("same after text", sameMeta(&in, &out));
        EXPECT_TRUE("hasMeta set", out.hasMeta);
        EXPECT_STR("gifFile not from text", out.gifFile, "");
        clipGifMetaFreeJson(json);
    }

    /* No players and no map is a valid file too. */
    ClipGifMeta bare;
    memset(&bare, 0, sizeof(bare));
    bare.createdUnix = 5;
    json = clipGifMetaToJson(&bare);
    EXPECT_TRUE("bare to json", json != NULL);
    if (json != NULL) {
        EXPECT_TRUE("bare from json",
                    clipGifMetaFromJson(json, strlen(json), &out));
        EXPECT_TRUE("bare same", sameMeta(&bare, &out));
        clipGifMetaFreeJson(json);
    }

    /* Through a file. */
    char path[1024];
    EXPECT_TRUE("scratch", utScratchPath(path, sizeof(path), "meta.json"));
    EXPECT_TRUE("write file", clipGifMetaWriteFile(path, &in));
    EXPECT_TRUE("read file", clipGifMetaReadFile(path, &out));
    EXPECT_TRUE("same after file", sameMeta(&in, &out));

    /* The metadata path for a GIF. */
    char mp[64];
    EXPECT_TRUE("path .gif", clipGifMetaPathFor("dir/a_0102.gif", mp, sizeof(mp)));
    EXPECT_STR("path .gif", mp, "dir/a_0102.json");
    EXPECT_TRUE("path .GIF", clipGifMetaPathFor("dir/b.GIF", mp, sizeof(mp)));
    EXPECT_STR("path .GIF", mp, "dir/b.json");
    EXPECT_TRUE("path bare", clipGifMetaPathFor("c", mp, sizeof(mp)));
    EXPECT_STR("path bare", mp, "c.json");
    EXPECT_TRUE("path too long", !clipGifMetaPathFor("abc.gif", mp, 8));
    EXPECT_TRUE("path just fits", clipGifMetaPathFor("abc.gif", mp, 9));
    EXPECT_STR("path just fits", mp, "abc.json");

    /* The file stamp is fixed-width digits, so names sort by time. */
    char stamp[32];
    clipGifFormatFileStamp(1760976123, stamp, sizeof(stamp));
    EXPECT_TRUE("stamp length", strlen(stamp) == 15 && stamp[8] == '-');

    return fails ? 1 : 0;
}

static int readsAs(const char *text, ClipGifMeta *out) {
    return clipGifMetaFromJson(text, text ? strlen(text) : 0, out) ? 1 : 0;
}

int run_clip_gif_meta_bad(void) {
    int fails = 0;
    ClipGifMeta m;

    /* Not usable at all. */
    EXPECT_TRUE("null text", !clipGifMetaFromJson(NULL, 10, &m));
    EXPECT_TRUE("null out", !clipGifMetaFromJson("{}", 2, NULL));
    EXPECT_TRUE("empty", !readsAs("", &m));
    EXPECT_TRUE("not json", !readsAs("GIF89a this is not json", &m));
    EXPECT_TRUE("array", !readsAs("[1,2,3]", &m));
    EXPECT_TRUE("no created", !readsAs("{\"map\":\"x\"}", &m));
    EXPECT_TRUE("created text", !readsAs("{\"created\":\"yesterday\"}", &m));
    EXPECT_TRUE("created negative", !readsAs("{\"created\":-5}", &m));
    EXPECT_TRUE("created huge", !readsAs("{\"created\":1e300}", &m));
    EXPECT_TRUE("cut short", !readsAs("{\"created\":100,\"map\":\"Ev", &m));
    EXPECT_TRUE("cleared on failure", m.createdUnix == 0 && !m.hasMeta);
    {
        /* The length given is the end of the text, NUL or not. */
        const char *t = "{\"created\":100}";
        EXPECT_TRUE("short length", !clipGifMetaFromJson(t, 5, &m));
    }

    /* Wrong types are left empty; the file still reads. */
    EXPECT_TRUE("wrong types",
                readsAs("{\"created\":100,\"map\":5,\"players\":\"a\","
                        "\"width\":\"w\",\"height\":-3,\"durationMs\":true}",
                        &m));
    EXPECT_TRUE("wrong types empty",
                m.createdUnix == 100 && m.map[0] == '\0' && m.playerCount == 0 &&
                    m.width == 0 && m.height == 0 && m.durationMs == 0 &&
                    m.hasMeta);

    /* Players that are not names are skipped; past the limit are dropped. */
    {
        char text[2048];
        size_t n = (size_t)snprintf(text, sizeof(text),
                                    "{\"created\":7,\"players\":[1,\"\",null,");
        for (int i = 0; i < CLIP_GIF_META_MAX_PLAYERS + 4; i++) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s\"p%d\"",
                                  i ? "," : "", i);
        }
        snprintf(text + n, sizeof(text) - n, "]}");
        EXPECT_TRUE("many players", readsAs(text, &m));
        EXPECT_TRUE("player limit", m.playerCount == CLIP_GIF_META_MAX_PLAYERS);
        EXPECT_STR("first named player", m.players[0], "p0");
        EXPECT_STR("last kept player", m.players[CLIP_GIF_META_MAX_PLAYERS - 1],
                   "p15");
    }

    /* Long text is cut to fit and stays terminated. */
    {
        char text[512];
        char longName[200];
        memset(longName, 'm', sizeof(longName) - 1);
        longName[sizeof(longName) - 1] = '\0';
        snprintf(text, sizeof(text),
                 "{\"created\":7,\"map\":\"%s\",\"players\":[\"%s\"]}",
                 longName, longName);
        EXPECT_TRUE("long strings", readsAs(text, &m));
        EXPECT_TRUE("map cut", strlen(m.map) == CLIP_GIF_META_MAP_LEN - 1);
        EXPECT_TRUE("player cut",
                    strlen(m.players[0]) == CLIP_GIF_META_NAME_LEN - 1);
    }

    /* Files that are missing or bad. */
    char path[1024];
    EXPECT_TRUE("scratch", utScratchPath(path, sizeof(path), "missing.json"));
    EXPECT_TRUE("missing file", !clipGifMetaReadFile(path, &m));
    EXPECT_TRUE("null path", !clipGifMetaReadFile(NULL, &m));
    EXPECT_TRUE("scratch", utScratchPath(path, sizeof(path), "bad.json"));
    {
        SDL_IOStream *io = SDL_IOFromFile(path, "wb");
        EXPECT_TRUE("open bad", io != NULL);
        if (io != NULL) {
            SDL_WriteIO(io, "\xff\xfe{nope", 7);
            SDL_CloseIO(io);
        }
    }
    EXPECT_TRUE("bad file", !clipGifMetaReadFile(path, &m));

    return fails ? 1 : 0;
}

static int writeBytes(const char *dir, const char *name, const char *text) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (io == NULL) {
        return 0;
    }
    size_t n = strlen(text);
    int ok = SDL_WriteIO(io, text, n) == n;
    SDL_CloseIO(io);
    return ok;
}

int run_clip_gif_gallery_scan(void) {
    int fails = 0;

    /* The sort on its own: newest first, the same time by later name. */
    {
        ClipGifMeta items[4];
        memset(items, 0, sizeof(items));
        items[0].createdUnix = 10; strcpy(items[0].gifFile, "a.gif");
        items[1].createdUnix = 30; strcpy(items[1].gifFile, "b.gif");
        items[2].createdUnix = 20; strcpy(items[2].gifFile, "c.gif");
        items[3].createdUnix = 30; strcpy(items[3].gifFile, "d.gif");
        clipGifMetaSortNewestFirst(items, 4);
        EXPECT_STR("sort 0", items[0].gifFile, "d.gif");
        EXPECT_STR("sort 1", items[1].gifFile, "b.gif");
        EXPECT_STR("sort 2", items[2].gifFile, "c.gif");
        EXPECT_STR("sort 3", items[3].gifFile, "a.gif");
        clipGifMetaSortNewestFirst(NULL, 3);  /* must not crash */
    }

    char dir[1024];
    EXPECT_TRUE("scratch dir", utScratchPath(dir, sizeof(dir), "clips"));
    SDL_CreateDirectory(dir);

    /* Far-future times, so the two with metadata sort ahead of the two that
     * fall back to the files' own (current) times. */
    ClipGifMeta m;
    char path[1024];
    fillMeta(&m);
    m.createdUnix = 4000000000LL;
    EXPECT_TRUE("a.gif", writeBytes(dir, "a.gif", "GIF89a"));
    snprintf(path, sizeof(path), "%s/a.json", dir);
    EXPECT_TRUE("a.json", clipGifMetaWriteFile(path, &m));
    m.createdUnix = 4000000100LL;
    strcpy(m.map, "Second");
    EXPECT_TRUE("b.gif", writeBytes(dir, "b.gif", "GIF89a"));
    snprintf(path, sizeof(path), "%s/b.json", dir);
    EXPECT_TRUE("b.json", clipGifMetaWriteFile(path, &m));
    /* No metadata at all. */
    EXPECT_TRUE("c.gif", writeBytes(dir, "c.gif", "GIF89a"));
    /* Metadata that does not read, written after c. */
    EXPECT_TRUE("d.gif", writeBytes(dir, "d.gif", "GIF89a"));
    EXPECT_TRUE("d.json", writeBytes(dir, "d.json", "{\"created\":"));
    /* Metadata with no GIF, and a file that is not a GIF. */
    EXPECT_TRUE("e.json", writeBytes(dir, "e.json", "{\"created\":4000000200}"));
    EXPECT_TRUE("f.txt", writeBytes(dir, "f.txt", "x"));

    ClipGifMeta *items = NULL;
    int count = clipGifGalleryScan(dir, &items);
    EXPECT_TRUE("count 4", count == 4);
    if (count == 4 && items != NULL) {
        EXPECT_STR("first", items[0].gifFile, "b.gif");
        EXPECT_STR("first map", items[0].map, "Second");
        EXPECT_TRUE("first has meta", items[0].hasMeta);
        EXPECT_TRUE("first players", items[0].playerCount == 3);
        EXPECT_STR("second", items[1].gifFile, "a.gif");
        EXPECT_STR("second map", items[1].map, "Everard Island");
        EXPECT_STR("third", items[2].gifFile, "d.gif");
        EXPECT_TRUE("third no meta", !items[2].hasMeta);
        EXPECT_TRUE("third has a time", items[2].createdUnix > 0);
        EXPECT_TRUE("third no map", items[2].map[0] == '\0' &&
                                        items[2].playerCount == 0);
        EXPECT_STR("fourth", items[3].gifFile, "c.gif");
        EXPECT_TRUE("fourth no meta", !items[3].hasMeta);
    }
    SDL_free(items);

    /* A folder that is not there. */
    char missing[1024];
    snprintf(missing, sizeof(missing), "%s/not-here", dir);
    items = (ClipGifMeta *)&m;  /* must be set back to NULL */
    EXPECT_TRUE("missing dir", clipGifGalleryScan(missing, &items) == 0);
    EXPECT_TRUE("missing dir out", items == NULL);
    EXPECT_TRUE("null dir", clipGifGalleryScan(NULL, &items) == 0);

    return fails ? 1 : 0;
}
