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
#include "stb_image_gif_capped.h"
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

/* Recording mode: the GIF controls show only with it on, and stay while a
 * capture runs even with it off, so a GIF being made keeps its controls. */
int run_clip_gif_record_mode(void) {
    int fails = 0;
    EXPECT_TRUE("off, no capture: hidden", !clipGifToolsShown(false, false));
    EXPECT_TRUE("on, no capture: shown", clipGifToolsShown(true, false));
    EXPECT_TRUE("on, capture: shown", clipGifToolsShown(true, true));
    EXPECT_TRUE("off, capture: shown", clipGifToolsShown(false, true));
    return fails ? 1 : 0;
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
    strcpy(m->game, "0123456789abcdef");
}

static int sameMeta(const ClipGifMeta *a, const ClipGifMeta *b) {
    if (a->createdUnix != b->createdUnix || strcmp(a->map, b->map) != 0 ||
        a->playerCount != b->playerCount || a->durationMs != b->durationMs ||
        a->width != b->width || a->height != b->height ||
        strcmp(a->game, b->game) != 0) {
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

/* ── Text ──────────────────────────────────────────────────────────── */

int run_clip_gif_text_fill(void) {
    int fails = 0;
    char out[64];

    /* The value goes where the token is, wherever that is in the text. */
    clipGifFillString1("Players: {string1}", "Alpha, Bravo", out, sizeof(out));
    EXPECT_STR("end", out, "Players: Alpha, Bravo");
    clipGifFillString1("{string1} could not be saved.", "a.gif", out,
                       sizeof(out));
    EXPECT_STR("start", out, "a.gif could not be saved.");
    clipGifFillString1("No token.", "x", out, sizeof(out));
    EXPECT_STR("no token", out, "No token.");
    clipGifFillString1("{string1}/{string1}", "ab", out, sizeof(out));
    EXPECT_STR("twice", out, "ab/ab");
    clipGifFillString1("{string1}!", NULL, out, sizeof(out));
    EXPECT_STR("null value", out, "!");
    clipGifFillString1("{string2} {string1", "x", out, sizeof(out));
    EXPECT_STR("other tokens kept", out, "{string2} {string1");

    /* A value longer than the lang module's 63-byte slot is kept whole. */
    char longList[400];
    char big[512];
    longList[0] = '\0';
    for (int i = 0; i < 16; i++) {
        char name[32];
        snprintf(name, sizeof(name), "%sPlayer number %02d", i ? ", " : "", i);
        SDL_strlcat(longList, name, sizeof(longList));
    }
    clipGifFillString1("Players: {string1}", longList, big, sizeof(big));
    EXPECT_TRUE("long value whole",
                strlen(big) == strlen("Players: ") + strlen(longList));

    /* Cut to fit, on a whole character. "\xC3\xA9" is e with an acute. */
    clipGifFillString1("ab{string1}", "\xC3\xA9\xC3\xA9", out, 5);
    EXPECT_STR("cut before a 2-byte char", out, "ab\xC3\xA9");
    clipGifFillString1("ab{string1}", "\xC3\xA9\xC3\xA9", out, 6);
    EXPECT_STR("cut inside a 2-byte char", out, "ab\xC3\xA9");
    /* "\xE2\x82\xAC" is the euro sign, "\xF0\x9F\x98\x80" an emoji. */
    clipGifFillString1("{string1}", "a\xE2\x82\xAC", out, 4);
    EXPECT_STR("cut inside a 3-byte char", out, "a");
    clipGifFillString1("{string1}", "a\xE2\x82\xAC", out, 5);
    EXPECT_STR("3-byte char fits", out, "a\xE2\x82\xAC");
    clipGifFillString1("{string1}", "\xF0\x9F\x98\x80z", out, 4);
    EXPECT_STR("cut inside a 4-byte char", out, "");
    clipGifFillString1("x{string1}", "yz", out, 2);
    EXPECT_STR("ASCII cut", out, "x");
    clipGifFillString1("x", "", out, 1);
    EXPECT_STR("no room", out, "");

    /* clipGifUtf8CutEnd on its own. */
    char s[16];
    SDL_strlcpy(s, "ab\xE2\x82", sizeof(s));
    clipGifUtf8CutEnd(s, strlen(s));
    EXPECT_STR("cut tail dropped", s, "ab");
    SDL_strlcpy(s, "ab\xE2\x82\xAC", sizeof(s));
    clipGifUtf8CutEnd(s, strlen(s));
    EXPECT_STR("whole tail kept", s, "ab\xE2\x82\xAC");
    SDL_strlcpy(s, "abc", sizeof(s));
    clipGifUtf8CutEnd(s, 2);
    EXPECT_STR("ended at len", s, "ab");

    /* A name in the metadata that is longer than the field is cut on a whole
     * character too: 63 bytes of room, and "a" then 31 two-byte characters
     * fill 63, so the 32nd must be dropped, not cut in two. */
    char json[512];
    char name[128];
    SDL_strlcpy(name, "a", sizeof(name));
    for (int i = 0; i < 32; i++) {
        SDL_strlcat(name, "\xC3\xA9", sizeof(name));
    }
    snprintf(json, sizeof(json),
             "{\"created\": 5, \"map\": \"%s\", \"players\": [\"%s\"]}", name,
             name);
    ClipGifMeta m;
    EXPECT_TRUE("long names read", clipGifMetaFromJson(json, strlen(json), &m));
    EXPECT_TRUE("map cut whole", strlen(m.map) == 63 &&
                                     (unsigned char)m.map[62] == 0xA9);
    EXPECT_TRUE("player cut whole",
                m.playerCount == 1 && strlen(m.players[0]) == 63 &&
                    (unsigned char)m.players[0][62] == 0xA9);

    return fails ? 1 : 0;
}

/* ── Preview size limit ───────────────────────────────────────────── */

/* A GIF of `frames` 1 x 1 frames on a w x h logical screen. stb decodes every
 * frame at the screen's size, so a few bytes stand for any amount of decoded
 * pixels. Every frame after the first uses dispose mode 3 (restore the frame
 * before), which reads the frame two back. Returns the byte count, or 0 when
 * it does not fit in cap. */
static size_t makeGif(unsigned char *buf, size_t cap, int w, int h, int frames,
                      int trailer) {
    static const unsigned char head[] = { 'G', 'I', 'F', '8', '9', 'a' };
    static const unsigned char table[] = { 0, 0, 0, 255, 255, 255 };
    static const unsigned char image[] = {
        0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0,  /* 1 x 1 at 0,0 */
        0x02, 0x02, 0x44, 0x01, 0x00 };   /* LZW: clear, colour 0, end */
    size_t n = 0;
    if (cap < 32 + (size_t)frames * 24) {
        return 0;
    }
    memcpy(buf, head, sizeof(head));
    n = sizeof(head);
    buf[n++] = (unsigned char)(w & 0xFF);
    buf[n++] = (unsigned char)((w >> 8) & 0xFF);
    buf[n++] = (unsigned char)(h & 0xFF);
    buf[n++] = (unsigned char)((h >> 8) & 0xFF);
    buf[n++] = 0x80;  /* a 2-colour global table */
    buf[n++] = 0;
    buf[n++] = 0;
    memcpy(buf + n, table, sizeof(table));
    n += sizeof(table);
    for (int i = 0; i < frames; i++) {
        /* Graphic control: 10 cs, dispose 3 after the first frame. */
        buf[n++] = 0x21;
        buf[n++] = 0xF9;
        buf[n++] = 4;
        buf[n++] = (unsigned char)(i > 0 ? 0x0C : 0x00);
        buf[n++] = 10;
        buf[n++] = 0;
        buf[n++] = 0;
        buf[n++] = 0;
        memcpy(buf + n, image, sizeof(image));
        n += sizeof(image);
    }
    if (trailer) {
        buf[n++] = 0x3B;
    }
    return n;
}

/* A 4 x 4 GIF of four 1 x 1 frames on a 4-colour table (black, white, red,
 * blue) that proves dispose mode 3 restores the frame two back:
 *   frame 0: black at 0,0 (dispose 0)
 *   frame 1: white at 0,0 (dispose 2: put back the background, black)
 *   frame 2: red at 0,0   (dispose 3: put back the frame before, frame 1)
 *   frame 3: blue at 1,1
 * Before frame 3, pixel 0,0 is restored from frame 1, so it is white. Mode
 * 2 would make it black (the background), no disposal would leave it red,
 * and a read from outside the block would give whatever is there. */
static size_t makeDisposeGif(unsigned char *buf, size_t cap) {
    static const unsigned char head[] = {
        'G', 'I', 'F', '8', '9', 'a', 4, 0, 4, 0,
        0x81, 0, 0,  /* a 4-colour global table, background 0 */
        0, 0, 0,  255, 255, 255,  255, 0, 0,  0, 0, 255 };
    /* Per frame: dispose flags, x, y, and the LZW byte for the colour. With
     * a code size of 2 the codes are 3 bits, packed low bit first: clear (4),
     * the colour c, end (5) is 4 | c << 3 | 5 << 6, which is 0x144 + 8 * c,
     * so the bytes are 0x44 + 8 * c and 0x01. */
    static const unsigned char frame[4][4] = {
        { 0x00, 0, 0, 0x44 },   /* black */
        { 0x08, 0, 0, 0x4C },   /* white, dispose 2 */
        { 0x0C, 0, 0, 0x54 },   /* red, dispose 3 */
        { 0x00, 1, 1, 0x5C } }; /* blue at 1,1 */
    size_t n = 0;
    if (cap < sizeof(head) + 4 * 23 + 1) {
        return 0;
    }
    memcpy(buf, head, sizeof(head));
    n = sizeof(head);
    for (int i = 0; i < 4; i++) {
        const unsigned char gce[] = { 0x21, 0xF9, 4, frame[i][0], 10, 0, 0, 0 };
        const unsigned char image[] = {
            0x2C, frame[i][1], 0, frame[i][2], 0, 1, 0, 1, 0, 0,
            0x02, 0x02, frame[i][3], 0x01, 0x00 };
        memcpy(buf + n, gce, sizeof(gce));
        n += sizeof(gce);
        memcpy(buf + n, image, sizeof(image));
        n += sizeof(image);
    }
    buf[n++] = 0x3B;
    return n;
}

/* A stop function that says stop once it has been asked limit times. */
typedef struct StopCount {
    int calls;
    int limit;
} StopCount;

static int stopAfter(void *user) {
    StopCount *c = (StopCount *)user;
    c->calls++;
    return c->calls > c->limit;
}

static int pixelIs(const stbi_uc *p, int r, int g, int b, int a) {
    return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

int run_clip_gif_preview_cap(void) {
    int fails = 0;

    /* The size check. */
    EXPECT_TRUE("fit 1 px", stbiGifFramesFit(1, 1, 1, 4));
    EXPECT_TRUE("over by 1", !stbiGifFramesFit(1, 1, 1, 3));
    EXPECT_TRUE("zero w", !stbiGifFramesFit(0, 10, 1, 1000));
    EXPECT_TRUE("negative h", !stbiGifFramesFit(10, -1, 1, 1000));
    EXPECT_TRUE("zero frames", !stbiGifFramesFit(10, 10, 0, 1000));
    EXPECT_TRUE("15 s at 480 px fits",
                stbiGifFramesFit(480, 270, 150, CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("15 s of a 4K crop does not",
                !stbiGifFramesFit(3840, 2160, 150, CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("one 4K frame fits",
                stbiGifFramesFit(3840, 2160, 1, CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("int max sides",
                !stbiGifFramesFit(0x7FFFFFFF, 0x7FFFFFFF, 150,
                                  CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("huge frame count",
                !stbiGifFramesFit(1, 1, (size_t)-1, (size_t)-1));
    EXPECT_TRUE("whole size_t",
                stbiGifFramesFit(1, 1, ((size_t)-1) / 4u, (size_t)-1));

    unsigned char gif[1024];
    int *delays = NULL;
    int x = 0, y = 0, frames = 0, tooLarge = 0;
    stbi_uc *px;

    /* 4 x 4, 3 frames: 64 bytes a frame, and 3 more frames for stb's
     * working buffers, 384 in all. */
    size_t len = makeGif(gif, sizeof(gif), 4, 4, 3, 1);
    EXPECT_TRUE("built", len > 0);
    px = stbiLoadGifCapped(gif, (int)len, 384, &delays, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("decodes at the limit", px != NULL && !tooLarge);
    EXPECT_TRUE("size", x == 4 && y == 4 && frames == 3);
    EXPECT_TRUE("delays", delays != NULL && delays[0] == 100 &&
                              delays[2] == 100);
    if (px != NULL) {
        /* The top left pixel of frame 1 is colour 0: black, opaque. */
        EXPECT_TRUE("pixel", px[0] == 0 && px[3] == 255);
        stbi_image_free(px);
    }
    if (delays != NULL) {
        stbi_image_free(delays);
    }

    delays = NULL;
    px = stbiLoadGifCapped(gif, (int)len, 383, &delays, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("one byte over", px == NULL && tooLarge && delays == NULL &&
                                     frames == 0);
    px = stbiLoadGifCapped(gif, (int)len, 255, NULL, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("first frame over", px == NULL && tooLarge);

    /* One frame: the frame (64 bytes) fits in 255, but stb's buffers do
     * not. */
    len = makeGif(gif, sizeof(gif), 4, 4, 1, 1);
    px = stbiLoadGifCapped(gif, (int)len, 255, NULL, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("working buffers counted", px == NULL && tooLarge);
    px = stbiLoadGifCapped(gif, (int)len, 256, NULL, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("one frame with buffers fits",
                px != NULL && frames == 1 && !tooLarge);
    if (px != NULL) {
        stbi_image_free(px);
    }

    /* A 65535 x 65535 screen is 17 GB a frame: refused before stb is asked
     * for any of it. */
    len = makeGif(gif, sizeof(gif), 65535, 65535, 2, 1);
    px = stbiLoadGifCapped(gif, (int)len, CLIP_GIF_PREVIEW_MAX_BYTES, &delays,
                           &x, &y, &frames, &tooLarge, NULL, NULL);
    EXPECT_TRUE("huge screen", px == NULL && tooLarge);

    /* Cut off after 2 frames, with no end code: the 2 frames are kept. */
    len = makeGif(gif, sizeof(gif), 4, 4, 2, 0);
    px = stbiLoadGifCapped(gif, (int)len, 1024, NULL, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("cut off", px != NULL && frames == 2 && !tooLarge);
    if (px != NULL) {
        stbi_image_free(px);
    }

    /* Not a GIF, and no data. */
    static const unsigned char notGif[] = "This is not a picture at all.";
    px = stbiLoadGifCapped(notGif, (int)sizeof(notGif), 1024, &delays, &x, &y,
                           &frames, &tooLarge, NULL, NULL);
    EXPECT_TRUE("not a GIF", px == NULL && !tooLarge);
    px = stbiLoadGifCapped(NULL, 0, 1024, &delays, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("no data", px == NULL && !tooLarge);

    /* A PNG header of 7000 x 7000 (stb does not check the CRC when it reads
     * the size): it is not a GIF, so it is not reported as too large. */
    static const unsigned char bigPng[] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
        0, 0, 0, 13, 'I', 'H', 'D', 'R',
        0, 0, 0x1B, 0x58, 0, 0, 0x1B, 0x58,  /* 7000 x 7000 */
        8, 6, 0, 0, 0,  0, 0, 0, 0,          /* RGBA, 8 bits; the CRC */
        0, 0, 0, 0, 'I', 'D', 'A', 'T' };   /* stb's size read ends here */
    int comp = 0;
    EXPECT_TRUE("PNG size is read",
                stbi_info_from_memory(bigPng, (int)sizeof(bigPng), &x, &y,
                                      &comp) && x == 7000 && y == 7000);
    px = stbiLoadGifCapped(bigPng, (int)sizeof(bigPng), 1024, &delays, &x,
                           &y, &frames, &tooLarge, NULL, NULL);
    EXPECT_TRUE("large PNG not too large", px == NULL && !tooLarge);

    /* The stop function: asked before each frame, and a stop ends the decode
     * with no result and no "too large". */
    len = makeGif(gif, sizeof(gif), 4, 4, 3, 1);
    StopCount never = { 0, 1000 };
    px = stbiLoadGifCapped(gif, (int)len, 1024, NULL, &x, &y, &frames,
                           &tooLarge, stopAfter, &never);
    /* 3 frames, and one more ask before the read that finds the end. */
    EXPECT_TRUE("not stopped", px != NULL && frames == 3 && never.calls == 4);
    if (px != NULL) {
        stbi_image_free(px);
    }
    StopCount atOnce = { 0, 0 };
    delays = NULL;
    px = stbiLoadGifCapped(gif, (int)len, 1024, &delays, &x, &y, &frames,
                           &tooLarge, stopAfter, &atOnce);
    EXPECT_TRUE("stopped before the first frame",
                px == NULL && !tooLarge && frames == 0 && delays == NULL &&
                    atOnce.calls == 1);
    StopCount afterTwo = { 0, 2 };
    px = stbiLoadGifCapped(gif, (int)len, 1024, &delays, &x, &y, &frames,
                           &tooLarge, stopAfter, &afterTwo);
    EXPECT_TRUE("stopped after two frames",
                px == NULL && !tooLarge && frames == 0 && delays == NULL &&
                    afterTwo.calls == 3);

    /* Dispose mode 3 restores the real frame two back. */
    len = makeDisposeGif(gif, sizeof(gif));
    EXPECT_TRUE("dispose GIF built", len > 0);
    px = stbiLoadGifCapped(gif, (int)len, 1024, NULL, &x, &y, &frames,
                           &tooLarge, NULL, NULL);
    EXPECT_TRUE("dispose GIF decodes",
                px != NULL && x == 4 && y == 4 && frames == 4);
    if (px != NULL && frames == 4) {
        /* 64 bytes a frame; pixel 1,0 is 4 bytes in and pixel 1,1 is 20. */
        EXPECT_TRUE("frame 0 black", pixelIs(px + 0, 0, 0, 0, 255));
        EXPECT_TRUE("frame 1 white", pixelIs(px + 64, 255, 255, 255, 255));
        EXPECT_TRUE("frame 2 red", pixelIs(px + 128, 255, 0, 0, 255));
        EXPECT_TRUE("frame 3 restored from frame 1",
                    pixelIs(px + 192, 255, 255, 255, 255));
        EXPECT_TRUE("frame 3 blue", pixelIs(px + 192 + 20, 0, 0, 255, 255));
        EXPECT_TRUE("frame 3 not drawn", pixelIs(px + 192 + 4, 0, 0, 0, 0));
    }
    if (px != NULL) {
        stbi_image_free(px);
    }

    return fails ? 1 : 0;
}

int run_clip_gif_current_game(void) {
    int fails = 0;
    char a[CLIP_GIF_META_GAME_LEN];
    char b[CLIP_GIF_META_GAME_LEN];

    /* The id: 16 hex digits, the same for the same bytes, different for
     * different bytes, and empty for no bytes. */
    const char bytes1[] = "PK\x03\x04 round one";
    const char bytes2[] = "PK\x03\x04 round two";
    clipGifGameIdFromBytes(bytes1, sizeof(bytes1), a, sizeof(a));
    EXPECT_TRUE("id length", strlen(a) == 16);
    {
        int hex = 1;
        for (size_t i = 0; a[i] != '\0'; i++) {
            if (!((a[i] >= '0' && a[i] <= '9') || (a[i] >= 'a' && a[i] <= 'f'))) {
                hex = 0;
            }
        }
        EXPECT_TRUE("id hex", hex);
    }
    clipGifGameIdFromBytes(bytes1, sizeof(bytes1), b, sizeof(b));
    EXPECT_STR("same bytes same id", b, a);
    clipGifGameIdFromBytes(bytes2, sizeof(bytes2), b, sizeof(b));
    EXPECT_TRUE("other bytes other id", strcmp(a, b) != 0);
    /* FNV-1a 64 of "a", the published test value. */
    clipGifGameIdFromBytes("a", 1, b, sizeof(b));
    EXPECT_STR("fnv-1a of a", b, "af63dc4c8601ec8c");
    clipGifGameIdFromBytes(NULL, 10, b, sizeof(b));
    EXPECT_STR("null bytes", b, "");
    clipGifGameIdFromBytes(bytes1, 0, b, sizeof(b));
    EXPECT_STR("no bytes", b, "");
    {
        /* A short buffer is cut and stays ended. */
        char small[5];
        clipGifGameIdFromBytes(bytes1, sizeof(bytes1), small, sizeof(small));
        EXPECT_TRUE("short buffer", strlen(small) == 4 &&
                                        strncmp(small, a, 4) == 0);
    }

    /* Which GIFs are from the current game. */
    ClipGifMeta m;
    memset(&m, 0, sizeof(m));
    strcpy(m.game, a);
    EXPECT_TRUE("same game", clipGifMetaIsCurrentGame(&m, a));
    EXPECT_TRUE("other game", !clipGifMetaIsCurrentGame(&m, b));
    EXPECT_TRUE("no current game", !clipGifMetaIsCurrentGame(&m, ""));
    EXPECT_TRUE("null current game", !clipGifMetaIsCurrentGame(&m, NULL));
    m.game[0] = '\0';
    EXPECT_TRUE("old file, no id", !clipGifMetaIsCurrentGame(&m, a));
    EXPECT_TRUE("both empty", !clipGifMetaIsCurrentGame(&m, ""));

    /* The current game first, each group still newest first. */
    {
        ClipGifMeta items[6];
        memset(items, 0, sizeof(items));
        const char *files[6] = { "1.gif", "2.gif", "3.gif",
                                 "4.gif", "5.gif", "6.gif" };
        const char *games[6] = { "old", "", "cur", "old", "cur", "cur" };
        for (int i = 0; i < 6; i++) {
            strcpy(items[i].gifFile, files[i]);
            strcpy(items[i].game, games[i]);
        }
        int n = clipGifMetaCurrentGameFirst(items, 6, "cur");
        EXPECT_TRUE("current count", n == 3);
        EXPECT_STR("order 0", items[0].gifFile, "3.gif");
        EXPECT_STR("order 1", items[1].gifFile, "5.gif");
        EXPECT_STR("order 2", items[2].gifFile, "6.gif");
        EXPECT_STR("order 3", items[3].gifFile, "1.gif");
        EXPECT_STR("order 4", items[4].gifFile, "2.gif");
        EXPECT_STR("order 5", items[5].gifFile, "4.gif");
        EXPECT_STR("whole item moves", items[0].game, "cur");

        /* No current game leaves the list as it was. */
        n = clipGifMetaCurrentGameFirst(items, 6, "");
        EXPECT_TRUE("no current count", n == 0);
        EXPECT_STR("unchanged 0", items[0].gifFile, "3.gif");
        EXPECT_STR("unchanged 5", items[5].gifFile, "4.gif");
        EXPECT_TRUE("null list", clipGifMetaCurrentGameFirst(NULL, 6, "cur") == 0);
        EXPECT_TRUE("all current",
                    clipGifMetaCurrentGameFirst(items, 1, "cur") == 1);
    }

    /* The id is kept in the metadata file, and a file from before the field
     * reads with no id. */
    {
        ClipGifMeta in, out;
        memset(&in, 0, sizeof(in));
        in.createdUnix = 100;
        strcpy(in.game, a);
        char *json = clipGifMetaToJson(&in);
        EXPECT_TRUE("game to json", json != NULL);
        if (json != NULL) {
            EXPECT_TRUE("game in text", strstr(json, "\"game\"") != NULL);
            EXPECT_TRUE("game from json",
                        clipGifMetaFromJson(json, strlen(json), &out));
            EXPECT_STR("game kept", out.game, a);
            clipGifMetaFreeJson(json);
        }
        in.game[0] = '\0';
        json = clipGifMetaToJson(&in);
        EXPECT_TRUE("no game to json", json != NULL);
        if (json != NULL) {
            EXPECT_TRUE("no game key", strstr(json, "\"game\"") == NULL);
            clipGifMetaFreeJson(json);
        }
        EXPECT_TRUE("old file", readsAs("{\"created\":100,\"map\":\"x\"}", &out));
        EXPECT_STR("old file no game", out.game, "");
        EXPECT_TRUE("game wrong type",
                    readsAs("{\"created\":100,\"game\":7}", &out));
        EXPECT_STR("game wrong type empty", out.game, "");
        char text[256];
        snprintf(text, sizeof(text),
                 "{\"created\":100,\"game\":\"%s%s\"}", a, a);
        EXPECT_TRUE("game long", readsAs(text, &out));
        EXPECT_TRUE("game cut", strlen(out.game) == CLIP_GIF_META_GAME_LEN - 1);
    }

    return fails ? 1 : 0;
}

static int pathExists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) ? 1 : 0;
}

int run_clip_gif_delete(void) {
    int fails = 0;

    /* The row selected after a removal. */
    EXPECT_TRUE("only row", clipGifSelectAfterRemove(1, 0) == -1);
    EXPECT_TRUE("empty list", clipGifSelectAfterRemove(0, 0) == -1);
    EXPECT_TRUE("first of 3", clipGifSelectAfterRemove(3, 0) == 0);
    EXPECT_TRUE("middle of 3", clipGifSelectAfterRemove(3, 1) == 1);
    EXPECT_TRUE("last of 3", clipGifSelectAfterRemove(3, 2) == 1);
    EXPECT_TRUE("last of 2", clipGifSelectAfterRemove(2, 1) == 0);
    EXPECT_TRUE("out of range", clipGifSelectAfterRemove(3, 7) == 0);
    EXPECT_TRUE("negative", clipGifSelectAfterRemove(3, -1) == 0);

    char dir[1024];
    EXPECT_TRUE("scratch dir", utScratchPath(dir, sizeof(dir), "delclips"));
    SDL_CreateDirectory(dir);
    char gif[1024], json[1024];

    /* A GIF with its metadata: both go. */
    snprintf(gif, sizeof(gif), "%s/a.gif", dir);
    snprintf(json, sizeof(json), "%s/a.json", dir);
    EXPECT_TRUE("a.gif", writeBytes(dir, "a.gif", "GIF89a"));
    EXPECT_TRUE("a.json", writeBytes(dir, "a.json", "{\"created\":5}"));
    EXPECT_TRUE("delete a", clipGifDeleteFiles(gif));
    EXPECT_TRUE("a.gif gone", !pathExists(gif));
    EXPECT_TRUE("a.json gone", !pathExists(json));

    /* A GIF with no metadata. */
    snprintf(gif, sizeof(gif), "%s/b.GIF", dir);
    EXPECT_TRUE("b.GIF", writeBytes(dir, "b.GIF", "GIF89a"));
    EXPECT_TRUE("delete b", clipGifDeleteFiles(gif));
    EXPECT_TRUE("b.GIF gone", !pathExists(gif));

    /* Already gone counts as done; the metadata left beside it goes too. */
    snprintf(gif, sizeof(gif), "%s/c.gif", dir);
    snprintf(json, sizeof(json), "%s/c.json", dir);
    EXPECT_TRUE("c.json", writeBytes(dir, "c.json", "{\"created\":5}"));
    EXPECT_TRUE("delete missing", clipGifDeleteFiles(gif));
    EXPECT_TRUE("c.json gone", !pathExists(json));

    /* Other files in the folder are not touched. */
    snprintf(gif, sizeof(gif), "%s/d.gif", dir);
    EXPECT_TRUE("d.gif", writeBytes(dir, "d.gif", "GIF89a"));
    EXPECT_TRUE("e.gif", writeBytes(dir, "e.gif", "GIF89a"));
    EXPECT_TRUE("delete d", clipGifDeleteFiles(gif));
    snprintf(gif, sizeof(gif), "%s/e.gif", dir);
    EXPECT_TRUE("e.gif kept", pathExists(gif));

    EXPECT_TRUE("null path", !clipGifDeleteFiles(NULL));
    EXPECT_TRUE("empty path", !clipGifDeleteFiles(""));

    return fails ? 1 : 0;
}
