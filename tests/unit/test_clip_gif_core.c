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
    EXPECT_U32("cap", clipGifLenClampMs(99999, 0, 0), 30000);
    EXPECT_U32("cap at max u32", clipGifLenClampMs(0xFFFFFFFFu, 0, 0), 30000);
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
    EXPECT_U32("huge", clipGifLenFromSeconds(1.0e9f, 0, 0), 30000);
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
               clipGifLenFromTimelineX(700.0f, 0, 60000, 100.0f, 700.0f), 30000);
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
    EXPECT_TRUE("30 s at 480 px fits",
                stbiGifFramesFit(480, 270, 300, CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("30 s of a 4K crop does not",
                !stbiGifFramesFit(3840, 2160, 300, CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("one 4K frame fits",
                stbiGifFramesFit(3840, 2160, 1, CLIP_GIF_PREVIEW_MAX_BYTES));
    EXPECT_TRUE("int max sides",
                !stbiGifFramesFit(0x7FFFFFFF, 0x7FFFFFFF, 300,
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

/* The frame count of a GIF: one frame every 100 ms, at least 10 and at most
 * 300 frames, as the game's replay and the Log Viewer both make them. */
int run_clip_gif_frame_count(void) {
    int fails = 0;
    EXPECT_U32("0 ms", clipGifFrameCount(0), CLIP_GIF_MIN_FRAMES);
    EXPECT_U32("999 ms", clipGifFrameCount(999), CLIP_GIF_MIN_FRAMES);
    EXPECT_U32("1 s", clipGifFrameCount(1000), 10);
    EXPECT_U32("1.5 s", clipGifFrameCount(1500), 15);
    EXPECT_U32("1.599 s rounds down", clipGifFrameCount(1599), 15);
    EXPECT_U32("5 s", clipGifFrameCount(5000), 50);
    EXPECT_U32("15 s", clipGifFrameCount(15000), 150);
    EXPECT_U32("30 s", clipGifFrameCount(30000), CLIP_GIF_MAX_FRAMES);
    EXPECT_U32("40 s", clipGifFrameCount(40000), CLIP_GIF_MAX_FRAMES);
    EXPECT_U32("most ms", clipGifFrameCount(0xFFFFFFFFu), CLIP_GIF_MAX_FRAMES);
    /* The longest length the length field takes is the cap, and the
     * shortest is the floor. */
    EXPECT_U32("longest length", clipGifFrameCount(CLIP_GIF_LEN_MAX_MS),
               CLIP_GIF_MAX_FRAMES);
    EXPECT_U32("shortest length", clipGifFrameCount(CLIP_GIF_LEN_MIN_MS),
               CLIP_GIF_MIN_FRAMES);
    return fails ? 1 : 0;
}

#define EXPECT_RECT(what, r, ex, ey, ew, eh)                                 \
    do {                                                                     \
        if ((r).x != (ex) || (r).y != (ey) || (r).w != (ew) ||               \
            (r).h != (eh)) {                                                 \
            fprintf(stderr, "%s: %s: got %d,%d %dx%d, want %d,%d %dx%d\n",   \
                    utCurrentTestName(), (what), (r).x, (r).y, (r).w, (r).h, \
                    (ex), (ey), (ew), (eh));                                 \
            fails++;                                                         \
        }                                                                    \
    } while (0)

/* The part of the view a GIF takes when no frame was drawn: all of a narrow
 * view, the middle of a wide one with the same shape. */
int run_clip_gif_centre_crop(void) {
    int fails = 0;
    ClipGifRect r;

    EXPECT_TRUE("narrow view", clipGifCentreCrop(16, 8, 320, 240, 480, &r));
    EXPECT_RECT("narrow view", r, 16, 8, 320, 240);

    EXPECT_TRUE("max width view", clipGifCentreCrop(3, 4, 480, 200, 480, &r));
    EXPECT_RECT("max width view", r, 3, 4, 480, 200);

    /* 960x640 at the 480 cap: half of each side, in the middle. */
    EXPECT_TRUE("wide view", clipGifCentreCrop(5, 7, 960, 640, 480, &r));
    EXPECT_RECT("wide view", r, 5 + 240, 7 + 160, 480, 320);

    /* A very flat view keeps at least one row. */
    EXPECT_TRUE("flat view", clipGifCentreCrop(0, 0, 1000, 3, 480, &r));
    EXPECT_RECT("flat view", r, 260, 1, 480, 1);

    /* No cap: all of it. */
    EXPECT_TRUE("no cap", clipGifCentreCrop(0, 0, 1000, 500, 0, &r));
    EXPECT_RECT("no cap", r, 0, 0, 1000, 500);

    /* An empty view gives nothing, and the rectangle is cleared. */
    r.x = r.y = r.w = r.h = 9;
    EXPECT_TRUE("no width", !clipGifCentreCrop(0, 0, 0, 100, 480, &r));
    EXPECT_RECT("no width", r, 0, 0, 0, 0);
    r.x = r.y = r.w = r.h = 9;
    EXPECT_TRUE("no height", !clipGifCentreCrop(0, 0, 100, -1, 480, &r));
    EXPECT_RECT("no height", r, 0, 0, 0, 0);
    EXPECT_TRUE("no out", !clipGifCentreCrop(0, 0, 100, 100, 480, NULL));
    return fails ? 1 : 0;
}

/* The names a GIF gets: the base name from the map and the time, the file
 * name in the gallery folder, and the map in its metadata. */
int run_clip_gif_names(void) {
    int fails = 0;
    char buf[160];

    clipGifBaseName(buf, sizeof(buf), "Everard Island.map", 1, 5);
    EXPECT_STR("base name", buf, "Everard_Island_0105");
    clipGifBaseName(buf, sizeof(buf), "Rock-n_Roll", 12, 34);
    EXPECT_STR("kept characters", buf, "Rock-n_Roll_1234");
    clipGifBaseName(buf, sizeof(buf), NULL, 0, 0);
    EXPECT_STR("no map", buf, "clip_0000");
    clipGifBaseName(buf, sizeof(buf), "", 2, 3);
    EXPECT_STR("empty map", buf, "clip_0203");
    /* Only a ".map" in small letters comes off, as before. */
    clipGifBaseName(buf, sizeof(buf), "a.MAP", 2, 3);
    EXPECT_STR("upper case .MAP", buf, "a_MAP_0203");
    clipGifBaseName(buf, sizeof(buf), "x/y:z.map", 0, 9);
    EXPECT_STR("path characters", buf, "x_y_z_0009");

    EXPECT_TRUE("first name",
                clipGifFileName(buf, sizeof(buf), "map_0105", "20261026-101500", 1));
    EXPECT_STR("first name", buf, "map_0105_20261026-101500.gif");
    EXPECT_TRUE("second name",
                clipGifFileName(buf, sizeof(buf), "map_0105", "20261026-101500", 2));
    EXPECT_STR("second name", buf, "map_0105_20261026-101500_2.gif");
    EXPECT_TRUE("last name",
                clipGifFileName(buf, sizeof(buf), "m", "s", CLIP_GIF_FILE_NAME_MAX_N));
    EXPECT_STR("last name", buf, "m_s_99.gif");
    EXPECT_TRUE("no base", clipGifFileName(buf, sizeof(buf), NULL, "s", 1));
    EXPECT_STR("no base", buf, "clip_s.gif");
    EXPECT_TRUE("empty base", clipGifFileName(buf, sizeof(buf), "", "s", 3));
    EXPECT_STR("empty base", buf, "clip_s_3.gif");
    EXPECT_TRUE("n 0", !clipGifFileName(buf, sizeof(buf), "m", "s", 0));
    EXPECT_STR("n 0 empty", buf, "");
    EXPECT_TRUE("n past the last",
                !clipGifFileName(buf, sizeof(buf), "m", "s",
                                 CLIP_GIF_FILE_NAME_MAX_N + 1));
    {
        char small[8];
        EXPECT_TRUE("does not fit",
                    !clipGifFileName(small, sizeof(small), "map_0105", "s", 1));
        EXPECT_STR("does not fit empty", small, "");
        EXPECT_TRUE("just fits", clipGifFileName(small, sizeof(small), "m", "s", 1));
        EXPECT_STR("just fits", small, "m_s.gif");
    }

    ClipGifMeta m;
    memset(&m, 0, sizeof(m));
    clipGifMetaSetMap(&m, "Everard Island.MAP");
    EXPECT_STR("meta map any case", m.map, "Everard Island");
    clipGifMetaSetMap(&m, "Bridge.map");
    EXPECT_STR("meta map", m.map, "Bridge");
    clipGifMetaSetMap(&m, ".map");
    EXPECT_STR("only .map", m.map, ".map");
    clipGifMetaSetMap(&m, NULL);
    EXPECT_STR("meta no map", m.map, "");
    {
        /* A long name is cut on a whole UTF-8 character. */
        char longName[200];
        size_t n = 0;
        for (int i = 0; i < 60; i++) {
            longName[n++] = (char)0xC3;
            longName[n++] = (char)0xA9; /* e with an acute accent */
        }
        longName[n] = '\0';
        clipGifMetaSetMap(&m, longName);
        size_t len = strlen(m.map);
        EXPECT_TRUE("long name fits", len < sizeof(m.map));
        EXPECT_TRUE("long name whole characters", len % 2 == 0 && len > 0);
    }
    clipGifMetaSetMap(NULL, "x"); /* does nothing */
    return fails ? 1 : 0;
}

#define EXPECT_BOX(what, b, ex0, ey0, ex1, ey1)                              \
    do {                                                                     \
        if (!nearF((b).x0, (ex0)) || !nearF((b).y0, (ey0)) ||                \
            !nearF((b).x1, (ex1)) || !nearF((b).y1, (ey1))) {                \
            fprintf(stderr, "%s: %s: got %g,%g - %g,%g, want %g,%g - %g,%g\n", \
                    utCurrentTestName(), (what), (double)(b).x0,             \
                    (double)(b).y0, (double)(b).x1, (double)(b).y1,          \
                    (double)(ex0), (double)(ey0), (double)(ex1),             \
                    (double)(ey1));                                          \
            fails++;                                                         \
        }                                                                    \
    } while (0)

/* The crop frame on the screen: its fractions to a box on the view and back,
 * and the part of it that a press at a point takes hold of. */
int run_clip_gif_frame_box(void) {
    int fails = 0;
    const ClipGifBox image = { 100.0f, 50.0f, 300.0f, 250.0f };
    const ClipGifFrame init = { 0.25f, 0.25f, 0.75f, 0.75f };

    ClipGifBox b = clipGifFrameToBox(&init, &image);
    EXPECT_BOX("start frame", b, 150.0f, 100.0f, 250.0f, 200.0f);

    ClipGifFrame f = { 0.0f, 0.0f, 0.0f, 0.0f };
    const ClipGifBox moved = { 120.0f, 70.0f, 300.0f, 150.0f };
    clipGifFrameFromBox(&f, &moved, &image);
    EXPECT_BOX("from box", f, 0.1f, 0.1f, 1.0f, 0.5f);

    /* A view with no width keeps the frame as it was. */
    const ClipGifBox flat = { 10.0f, 10.0f, 10.0f, 90.0f };
    clipGifFrameFromBox(&f, &moved, &flat);
    EXPECT_BOX("empty view keeps frame", f, 0.1f, 0.1f, 1.0f, 0.5f);

    /* The hit test with the game's 6 px grab, on 150,100 - 250,200. */
    const float g = 6.0f;
    EXPECT_U32("left edge", (uint32_t)clipGifFrameHit(&b, 150.0f, 150.0f, g),
               (uint32_t)CLIP_GIF_GRAB_LEFT);
    EXPECT_U32("left edge, outside in grab",
               (uint32_t)clipGifFrameHit(&b, 145.0f, 150.0f, g),
               (uint32_t)CLIP_GIF_GRAB_LEFT);
    EXPECT_U32("past the grab",
               (uint32_t)clipGifFrameHit(&b, 140.0f, 150.0f, g), 0u);
    EXPECT_U32("top right corner",
               (uint32_t)clipGifFrameHit(&b, 250.0f, 100.0f, g),
               (uint32_t)(CLIP_GIF_GRAB_RIGHT | CLIP_GIF_GRAB_TOP));
    EXPECT_U32("bottom edge", (uint32_t)clipGifFrameHit(&b, 200.0f, 204.0f, g),
               (uint32_t)CLIP_GIF_GRAB_BOTTOM);
    EXPECT_U32("inside", (uint32_t)clipGifFrameHit(&b, 200.0f, 150.0f, g),
               (uint32_t)CLIP_GIF_GRAB_MOVE);
    EXPECT_U32("inside, near an edge",
               (uint32_t)clipGifFrameHit(&b, 156.5f, 150.0f, g),
               (uint32_t)CLIP_GIF_GRAB_MOVE);
    EXPECT_U32("no box", (uint32_t)clipGifFrameHit(NULL, 0.0f, 0.0f, g), 0u);
    return fails ? 1 : 0;
}

/* A drag of the crop frame. A move keeps the frame's size and stops at the
 * view's edge. An edge stops at the view's edge, and stays the minimum size
 * away from the edge across from it. */
int run_clip_gif_frame_drag(void) {
    int fails = 0;
    const ClipGifBox image = { 0.0f, 0.0f, 100.0f, 100.0f };
    const ClipGifBox start = { 20.0f, 20.0f, 60.0f, 60.0f };
    const float minSize = 16.0f;
    ClipGifBox b;

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_MOVE, 10.0f, 5.0f, &image, minSize);
    EXPECT_BOX("move", b, 30.0f, 25.0f, 70.0f, 65.0f);

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_MOVE, -50.0f, 10.0f, &image, minSize);
    EXPECT_BOX("move stops at the left", b, 0.0f, 30.0f, 40.0f, 70.0f);

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_MOVE, 100.0f, 100.0f, &image, minSize);
    EXPECT_BOX("move stops at the bottom right", b, 60.0f, 60.0f, 100.0f,
               100.0f);

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_RIGHT, 100.0f, 0.0f, &image, minSize);
    EXPECT_BOX("right edge stops at the view", b, 20.0f, 20.0f, 100.0f, 60.0f);

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_LEFT, 100.0f, 0.0f, &image, minSize);
    EXPECT_BOX("left edge stops short of the right", b, 44.0f, 20.0f, 60.0f,
               60.0f);

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_RIGHT | CLIP_GIF_GRAB_BOTTOM, -100.0f,
                     -100.0f, &image, minSize);
    EXPECT_BOX("corner stops at the minimum", b, 20.0f, 20.0f, 36.0f, 36.0f);

    b = start;
    clipGifFrameDrag(&b, CLIP_GIF_GRAB_LEFT | CLIP_GIF_GRAB_TOP, -5.0f, -30.0f,
                     &image, minSize);
    EXPECT_BOX("corner out", b, 15.0f, 0.0f, 60.0f, 60.0f);

    b = start;
    clipGifFrameDrag(&b, 0, 10.0f, 10.0f, &image, minSize);
    EXPECT_BOX("nothing held", b, 20.0f, 20.0f, 60.0f, 60.0f);
    return fails ? 1 : 0;
}

/* The render-target pixels that the crop frame takes on the visible view:
 * the fractions times the view's size, to the nearest pixel, inside the
 * view and at least one pixel. */
int run_clip_gif_crop_from_frame(void) {
    int fails = 0;
    ClipGifRect r;
    const ClipGifFrame init = { 0.25f, 0.25f, 0.75f, 0.75f };
    EXPECT_TRUE("start frame",
                clipGifCropFromFrame(&init, 10, 20, 200, 100, &r));
    EXPECT_RECT("start frame", r, 60, 45, 100, 50);

    const ClipGifFrame all = { 0.0f, 0.0f, 1.0f, 1.0f };
    EXPECT_TRUE("all", clipGifCropFromFrame(&all, 0, 0, 7, 3, &r));
    EXPECT_RECT("all", r, 0, 0, 7, 3);

    const ClipGifFrame past = { -0.2f, -0.1f, 1.3f, 1.2f };
    EXPECT_TRUE("past the edges",
                clipGifCropFromFrame(&past, 4, 4, 10, 10, &r));
    EXPECT_RECT("past the edges", r, 4, 4, 10, 10);

    const ClipGifFrame dot = { 0.5f, 0.5f, 0.5f, 0.5f };
    EXPECT_TRUE("no size", clipGifCropFromFrame(&dot, 0, 0, 10, 10, &r));
    EXPECT_RECT("no size", r, 5, 5, 1, 1);

    const ClipGifFrame corner = { 1.0f, 1.0f, 1.0f, 1.0f };
    EXPECT_TRUE("at the corner",
                clipGifCropFromFrame(&corner, 0, 0, 10, 10, &r));
    EXPECT_RECT("at the corner", r, 9, 9, 1, 1);

    r.x = r.y = r.w = r.h = 9;
    EXPECT_TRUE("empty view", !clipGifCropFromFrame(&init, 0, 0, 0, 10, &r));
    EXPECT_RECT("empty view", r, 0, 0, 0, 0);
    EXPECT_TRUE("no frame", !clipGifCropFromFrame(NULL, 0, 0, 10, 10, &r));
    EXPECT_TRUE("no out", !clipGifCropFromFrame(&init, 0, 0, 10, 10, NULL));
    return fails ? 1 : 0;
}

/* The Log Viewer's map. From its blit and the part of the window that is
 * map: the whole render-target pixels that show, and the box in window
 * points that they are drawn in. */
int run_clip_gif_blit_view(void) {
    int fails = 0;
    ClipGifRect slice;
    ClipGifBox view;

    /* All of the blit shows, at 1x on a 1x display. */
    {
        const ClipGifBox src  = { 0.0f, 0.0f, 100.0f, 50.0f };
        const ClipGifBox dst  = { 0.0f, 0.0f, 100.0f, 50.0f };
        const ClipGifBox area = { 0.0f, 0.0f, 200.0f, 200.0f };
        EXPECT_TRUE("all shows", clipGifBlitView(&src, &dst, &area, 1.0f,
                                                 &slice, &view));
        EXPECT_RECT("all shows", slice, 0, 0, 100, 50);
        EXPECT_BOX("all shows", view, 0.0f, 0.0f, 100.0f, 50.0f);
        /* A scale of 0 is taken as 1. */
        EXPECT_TRUE("no scale", clipGifBlitView(&src, &dst, &area, 0.0f,
                                                &slice, &view));
        EXPECT_RECT("no scale", slice, 0, 0, 100, 50);
    }
    /* At 2x, cut by the area on the left and the right. Only whole pixels:
     * from the first that starts in the area to the last that ends in it. */
    {
        const ClipGifBox src  = { 8.0f, 0.0f, 108.0f, 50.0f };
        const ClipGifBox dst  = { 10.0f, 0.0f, 210.0f, 100.0f };
        const ClipGifBox area = { 15.0f, 0.0f, 101.0f, 1000.0f };
        EXPECT_TRUE("cut at 2x", clipGifBlitView(&src, &dst, &area, 1.0f,
                                                 &slice, &view));
        EXPECT_RECT("cut at 2x", slice, 11, 0, 42, 50);
        EXPECT_BOX("cut at 2x", view, 16.0f, 0.0f, 100.0f, 100.0f);
    }
    /* A display with 2 frame buffer pixels to each point. The area is in
     * points, the blit in pixels, and the view comes back in points. */
    {
        const ClipGifBox src  = { 0.0f, 0.0f, 100.0f, 60.0f };
        const ClipGifBox dst  = { 0.0f, 40.0f, 200.0f, 160.0f };
        const ClipGifBox area = { 0.0f, 10.0f, 50.0f, 1000.0f };
        EXPECT_TRUE("dense display", clipGifBlitView(&src, &dst, &area, 2.0f,
                                                     &slice, &view));
        EXPECT_RECT("dense display", slice, 0, 0, 50, 60);
        EXPECT_BOX("dense display", view, 0.0f, 20.0f, 50.0f, 80.0f);
    }
    /* No part of the blit in the area: false, and both are cleared. */
    {
        const ClipGifBox src  = { 0.0f, 0.0f, 100.0f, 50.0f };
        const ClipGifBox dst  = { 0.0f, 0.0f, 100.0f, 50.0f };
        const ClipGifBox area = { 300.0f, 0.0f, 400.0f, 50.0f };
        EXPECT_TRUE("no map shows", !clipGifBlitView(&src, &dst, &area, 1.0f,
                                                     &slice, &view));
        EXPECT_RECT("no map shows", slice, 0, 0, 0, 0);
        EXPECT_BOX("no map shows", view, 0.0f, 0.0f, 0.0f, 0.0f);
        EXPECT_TRUE("no out", !clipGifBlitView(&src, &dst, &area, 1.0f,
                                               NULL, &view));
    }
    return fails ? 1 : 0;
}

/* Recording mode's state. It starts off, with the frame not set by the
 * player, on the whole view until it sees one, and at the default length.
 * The toggle changes nothing while a GIF is made. A reset puts the start
 * values back. */
int run_clip_gif_mode_state(void) {
    int fails = 0;
    ClipGifMode m = CLIP_GIF_MODE_INIT;
    EXPECT_TRUE("starts off", !m.on && !m.frameSet);
    EXPECT_BOX("starts on the whole view", m.frame, 0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_U32("starts at the default length", m.lenMs,
               CLIP_GIF_LEN_DEFAULT_MS);

    EXPECT_TRUE("toggle on", clipGifModeToggle(&m, false) && m.on);
    EXPECT_TRUE("held while a GIF is made",
                clipGifModeToggle(&m, true) && m.on);
    EXPECT_TRUE("toggle off", !clipGifModeToggle(&m, false) && !m.on);
    EXPECT_TRUE("off is held too", !clipGifModeToggle(&m, true) && !m.on);

    m.on = true;
    m.frameSet = true;
    m.frame.x0 = 0.1f;
    m.lenMs = 12000u;
    clipGifModeReset(&m);
    EXPECT_TRUE("reset: off", !m.on && !m.frameSet);
    EXPECT_BOX("reset: frame", m.frame, 0.0f, 0.0f, 1.0f, 1.0f);
    EXPECT_U32("reset: length", m.lenMs, CLIP_GIF_LEN_DEFAULT_MS);

    /* The tools show with the mode on, or while a GIF is made. */
    EXPECT_TRUE("tools off", !clipGifToolsShown(m.on, false));
    clipGifModeToggle(&m, false);
    EXPECT_TRUE("tools on", clipGifToolsShown(m.on, false));

    EXPECT_TRUE("no mode", !clipGifModeToggle(NULL, false));
    clipGifModeReset(NULL); /* does nothing */
    return fails ? 1 : 0;
}

/* Preview clip: where the clip starts and ends, the clamp to the length's
 * floor and cap and to the end of the replay, and when the preview ends (at
 * the end of the clip) or lets go (a pause or a seek). */
int run_clip_gif_preview_play(void) {
    int fails = 0;
    ClipGifPlay p = CLIP_GIF_PLAY_INIT;
    EXPECT_TRUE("starts off", !p.active);
    EXPECT_U32("band start off: the time now", clipGifPlayBandStart(&p, 4321u),
               4321u);
    EXPECT_U32("band start, no preview", clipGifPlayBandStart(NULL, 77u), 77u);

    /* A clip of 5 s from 10 s in a replay of 60 s. */
    EXPECT_TRUE("start", clipGifPlayStart(&p, 10000u, 5000u, 60000u));
    EXPECT_TRUE("on", p.active);
    EXPECT_U32("start ms", p.startMs, 10000u);
    EXPECT_U32("end ms", p.endMs, 15000u);
    EXPECT_U32("band start: the clip's start", clipGifPlayBandStart(&p, 12000u),
               10000u);

    EXPECT_TRUE("plays on", clipGifPlayUpdate(&p, 10000u, true) ==
                            CLIP_GIF_PLAY_NONE && p.active);
    EXPECT_TRUE("plays on, inside", clipGifPlayUpdate(&p, 14999u, true) ==
                                    CLIP_GIF_PLAY_NONE && p.active);
    EXPECT_TRUE("the end", clipGifPlayUpdate(&p, 15000u, true) ==
                           CLIP_GIF_PLAY_END && !p.active);
    EXPECT_TRUE("off: nothing more", clipGifPlayUpdate(&p, 15100u, true) ==
                                     CLIP_GIF_PLAY_NONE);

    /* A frame can go a little past the end before it is seen. */
    clipGifPlayStart(&p, 10000u, 5000u, 60000u);
    EXPECT_TRUE("past the end in one frame",
                clipGifPlayUpdate(&p, 15000u + CLIP_GIF_PLAY_OVERRUN_MS,
                                  true) == CLIP_GIF_PLAY_END);
    /* Far past the end is a seek by the player. */
    clipGifPlayStart(&p, 10000u, 5000u, 60000u);
    EXPECT_TRUE("seek forward lets go",
                clipGifPlayUpdate(&p, 15001u + CLIP_GIF_PLAY_OVERRUN_MS,
                                  true) == CLIP_GIF_PLAY_LET_GO && !p.active);
    clipGifPlayStart(&p, 10000u, 5000u, 60000u);
    EXPECT_TRUE("seek back lets go", clipGifPlayUpdate(&p, 9000u, true) ==
                                     CLIP_GIF_PLAY_LET_GO && !p.active);
    clipGifPlayStart(&p, 10000u, 5000u, 60000u);
    EXPECT_TRUE("pause lets go", clipGifPlayUpdate(&p, 12000u, false) ==
                                 CLIP_GIF_PLAY_LET_GO && !p.active);
    /* Stopped at the end of the clip (the replay ended there) is the end, not
     * a pause. */
    clipGifPlayStart(&p, 55000u, 5000u, 60000u);
    EXPECT_TRUE("the replay ends with the clip",
                clipGifPlayUpdate(&p, 60000u, false) == CLIP_GIF_PLAY_END);

    /* The length's floor and cap. */
    clipGifPlayStart(&p, 0u, 10u, 60000u);
    EXPECT_U32("floor", p.endMs, CLIP_GIF_LEN_MIN_MS);
    clipGifPlayStart(&p, 0u, 99000u, 60000u);
    EXPECT_U32("cap", p.endMs, CLIP_GIF_LEN_MAX_MS);
    /* Near the end, the clip ends with the replay. */
    clipGifPlayStart(&p, 59500u, 5000u, 60000u);
    EXPECT_U32("end of the replay", p.endMs, 60000u);
    EXPECT_TRUE("still on", p.active);

    /* Nothing to play. */
    EXPECT_TRUE("at the end", !clipGifPlayStart(&p, 60000u, 5000u, 60000u) &&
                              !p.active);
    EXPECT_TRUE("no replay", !clipGifPlayStart(&p, 0u, 5000u, 0u) && !p.active);
    EXPECT_TRUE("no state", !clipGifPlayStart(NULL, 0u, 5000u, 60000u));

    clipGifPlayStart(&p, 1000u, 5000u, 60000u);
    clipGifPlayStop(&p);
    EXPECT_TRUE("stop", !p.active);
    EXPECT_U32("band start after stop", clipGifPlayBandStart(&p, 2500u), 2500u);
    clipGifPlayStop(NULL); /* does nothing */
    EXPECT_TRUE("no state, update", clipGifPlayUpdate(NULL, 0u, true) ==
                                    CLIP_GIF_PLAY_NONE);
    return fails ? 1 : 0;
}

/* The crop both hosts take: always the part in the mode's frame. Before
 * the player moves the frame that is the default frame, which takes the
 * same pixels clipGifCentreCrop does (the centre of the view at most
 * CLIP_GIF_MAX_WIDTH wide), on any view size. A frame the player set is
 * kept, with no width cap. */
int run_clip_gif_choose_crop(void) {
    int fails = 0;
    ClipGifRect a, b;

    /* Not set by the player: the same pixels as the centre crop, on views
     * wider and narrower than the cap, and on odd sizes. */
    static const int views[][4] = {
        { 10, 20, 1000, 500 }, { 0, 0, 480, 270 },   { 3, 4, 320, 240 },
        { 7, 9, 1921, 1081 },  { 0, 0, 3840, 2160 }, { 0, 0, 481, 1 },
        { 5, 5, 1, 1 },
    };
    for (size_t i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
        ClipGifMode m = CLIP_GIF_MODE_INIT;
        const int *v = views[i];
        const bool gotA = clipGifChooseCrop(&m, v[0], v[1], v[2], v[3], &a);
        const bool gotB = clipGifCentreCrop(v[0], v[1], v[2], v[3],
                                            CLIP_GIF_MAX_WIDTH, &b);
        if (!gotA || !gotB || a.x != b.x || a.y != b.y || a.w != b.w ||
            a.h != b.h) {
            fprintf(stderr,
                    "%s: default on %dx%d: got %d,%d %dx%d, want %d,%d %dx%d\n",
                    utCurrentTestName(), v[2], v[3], a.x, a.y, a.w, a.h, b.x,
                    b.y, b.w, b.h);
            fails++;
        }
        EXPECT_TRUE("default: not set by the choice", !m.frameSet);
    }

    /* The default follows the view while the player has not set it: the
     * view shrinks under the cap and the whole view is taken. */
    ClipGifMode m = CLIP_GIF_MODE_INIT;
    EXPECT_TRUE("wide view", clipGifChooseCrop(&m, 0, 0, 1000, 500, &a));
    EXPECT_RECT("wide view", a, 260, 130, 480, 240);
    EXPECT_TRUE("small view", clipGifChooseCrop(&m, 0, 0, 300, 200, &a));
    EXPECT_RECT("small view", a, 0, 0, 300, 200);

    /* Set by the player: the frame is kept, with no width cap, and stays
     * inside a view of any size. */
    m.frameSet = true;
    m.frame.x0 = 0.0f;
    m.frame.y0 = 0.0f;
    m.frame.x1 = 1.0f;
    m.frame.y1 = 1.0f;
    EXPECT_TRUE("set: whole", clipGifChooseCrop(&m, 0, 0, 1000, 500, &a));
    EXPECT_RECT("set: whole, no cap", a, 0, 0, 1000, 500);
    m.frame.x0 = 0.25f;
    m.frame.y0 = 0.25f;
    m.frame.x1 = 0.75f;
    m.frame.y1 = 0.75f;
    EXPECT_TRUE("set", clipGifChooseCrop(&m, 10, 20, 800, 600, &a));
    EXPECT_TRUE("set: as clipGifCropFromFrame",
                clipGifCropFromFrame(&m.frame, 10, 20, 800, 600, &b) &&
                    a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h);
    EXPECT_BOX("set: kept", m.frame, 0.25f, 0.25f, 0.75f, 0.75f);
    EXPECT_TRUE("set: tiny view", clipGifChooseCrop(&m, 0, 0, 2, 1, &a));
    EXPECT_RECT("set: tiny view", a, 1, 0, 1, 1);

    EXPECT_TRUE("empty view", !clipGifChooseCrop(&m, 0, 0, 0, 0, &a));
    EXPECT_TRUE("no mode", !clipGifChooseCrop(NULL, 0, 0, 100, 100, &a) &&
                               a.w == 0 && a.h == 0);
    return fails ? 1 : 0;
}

/* The default crop frame, as fractions of the view: the whole view up to
 * CLIP_GIF_MAX_WIDTH wide, else the middle CLIP_GIF_MAX_WIDTH pixels with
 * the view's shape. clipGifModeFitFrame puts it in a mode only while the
 * player has not set the frame. */
int run_clip_gif_default_frame(void) {
    int fails = 0;
    ClipGifFrame f = clipGifFrameDefault(320, 240);
    EXPECT_BOX("narrow view: whole", f, 0.0f, 0.0f, 1.0f, 1.0f);
    f = clipGifFrameDefault(CLIP_GIF_MAX_WIDTH, 100);
    EXPECT_BOX("at the cap: whole", f, 0.0f, 0.0f, 1.0f, 1.0f);
    f = clipGifFrameDefault(960, 640);
    EXPECT_BOX("twice the cap: middle half", f, 0.25f, 0.25f, 0.75f, 0.75f);
    f = clipGifFrameDefault(0, 100);
    EXPECT_BOX("empty view: whole", f, 0.0f, 0.0f, 1.0f, 1.0f);

    ClipGifMode m = CLIP_GIF_MODE_INIT;
    clipGifModeFitFrame(&m, 960, 640);
    EXPECT_BOX("fit", m.frame, 0.25f, 0.25f, 0.75f, 0.75f);
    clipGifModeFitFrame(&m, 0, 0);
    EXPECT_BOX("fit, empty view: kept", m.frame, 0.25f, 0.25f, 0.75f, 0.75f);
    clipGifModeFitFrame(&m, 300, 300);
    EXPECT_BOX("fit follows the view", m.frame, 0.0f, 0.0f, 1.0f, 1.0f);
    m.frameSet = true;
    m.frame.x0 = 0.1f;
    clipGifModeFitFrame(&m, 960, 640);
    EXPECT_BOX("set by the player: kept", m.frame, 0.1f, 0.0f, 1.0f, 1.0f);
    clipGifModeFitFrame(NULL, 960, 640); /* does nothing */
    return fails ? 1 : 0;
}

/* The gallery's metadata as both hosts begin it: the time, the game, the map
 * without its .map, and then the players, with no empty names and no more
 * than the list holds. */
int run_clip_gif_meta_begin(void) {
    int fails = 0;
    ClipGifMeta m;
    memset(&m, 0x5a, sizeof(m));
    clipGifMetaBegin(&m, 1700000000, "abcdef0123456789", "Everard Island.map");
    EXPECT_TRUE("time", m.createdUnix == 1700000000);
    EXPECT_STR("game", m.game, "abcdef0123456789");
    EXPECT_STR("map", m.map, "Everard Island");
    EXPECT_TRUE("no players", m.playerCount == 0);
    EXPECT_U32("no length yet", m.durationMs, 0u);
    EXPECT_TRUE("no size yet", m.width == 0 && m.height == 0);

    clipGifMetaBegin(&m, 0, NULL, NULL);
    EXPECT_STR("no game", m.game, "");
    EXPECT_STR("no map", m.map, "");

    EXPECT_TRUE("add", clipGifMetaAddPlayer(&m, "Canuck"));
    EXPECT_TRUE("empty name", !clipGifMetaAddPlayer(&m, ""));
    EXPECT_TRUE("no name", !clipGifMetaAddPlayer(&m, NULL));
    EXPECT_TRUE("add 2", clipGifMetaAddPlayer(&m, "Elvis"));
    EXPECT_TRUE("two players", m.playerCount == 2);
    EXPECT_STR("player 1", m.players[0], "Canuck");
    EXPECT_STR("player 2", m.players[1], "Elvis");
    for (int i = 2; i < CLIP_GIF_META_MAX_PLAYERS; i++) {
        clipGifMetaAddPlayer(&m, "Bot");
    }
    EXPECT_TRUE("full", m.playerCount == CLIP_GIF_META_MAX_PLAYERS);
    EXPECT_TRUE("no more", !clipGifMetaAddPlayer(&m, "One more"));
    EXPECT_TRUE("still full", m.playerCount == CLIP_GIF_META_MAX_PLAYERS);
    EXPECT_TRUE("no meta", !clipGifMetaAddPlayer(NULL, "x"));
    clipGifMetaBegin(NULL, 0, NULL, NULL); /* does nothing */
    return fails ? 1 : 0;
}

/* The GIF's span: where it is put the first time the zoomed timeline is drawn
 * (at the playhead), where a press on the main timeline puts it (round the
 * press), how it keeps its length at the start and the end of the replay,
 * and the zoomed timeline that fits it with 10 s before and after. */
int run_clip_gif_span_place(void) {
    int fails = 0;
    ClipGifMode m = CLIP_GIF_MODE_INIT;
    EXPECT_TRUE("no span at first", !m.spanSet);
    clipGifModeToggle(&m, false);
    EXPECT_TRUE("on: no span yet", m.on && !m.spanSet);

    /* No length known yet: nothing. */
    clipGifSpanRefit(&m, 12000u, 0u);
    EXPECT_TRUE("no length: no span", !m.spanSet);

    /* The first draw: at the playhead, the default 5 s. */
    clipGifSpanRefit(&m, 12000u, 60000u);
    EXPECT_TRUE("span set", m.spanSet);
    ClipGifRange s = clipGifSpanRange(&m, 60000u);
    EXPECT_U32("first: start", s.startMs, 12000u);
    EXPECT_U32("first: end", s.endMs, 17000u);
    EXPECT_U32("first: zoom start", m.zoom.startMs, 2000u);
    EXPECT_U32("first: zoom end", m.zoom.endMs, 27000u);

    /* The playhead moves on: the span stays. A typed length: the zoomed
     * timeline follows it. */
    m.lenMs = 8000u;
    clipGifSpanRefit(&m, 40000u, 60000u);
    s = clipGifSpanRange(&m, 60000u);
    EXPECT_U32("stays: start", s.startMs, 12000u);
    EXPECT_U32("typed: end", s.endMs, 20000u);
    EXPECT_U32("typed: zoom end", m.zoom.endMs, 30000u);

    /* Near the end it starts earlier and keeps its length. */
    clipGifSpanPlace(&m, 58000u, 60000u);
    s = clipGifSpanRange(&m, 60000u);
    EXPECT_U32("end: start", s.startMs, 52000u);
    EXPECT_U32("end: end", s.endMs, 60000u);
    EXPECT_U32("end: zoom start", m.zoom.startMs, 42000u);
    EXPECT_U32("end: zoom end, the end of the replay", m.zoom.endMs, 60000u);

    /* Near the start the zoomed timeline stops at 0. */
    clipGifSpanPlace(&m, 3000u, 60000u);
    EXPECT_U32("start: zoom start", m.zoom.startMs, 0u);
    EXPECT_U32("start: zoom end", m.zoom.endMs, 21000u);

    /* A press on the main timeline: the span round it. */
    clipGifSpanCentre(&m, 30000u, 60000u);
    s = clipGifSpanRange(&m, 60000u);
    EXPECT_U32("centre: start", s.startMs, 26000u);
    EXPECT_U32("centre: end", s.endMs, 34000u);
    EXPECT_U32("centre: zoom start", m.zoom.startMs, 16000u);
    EXPECT_U32("centre: zoom end", m.zoom.endMs, 44000u);
    clipGifSpanCentre(&m, 1000u, 60000u);
    EXPECT_U32("centre near the start", m.spanStartMs, 0u);
    clipGifSpanCentre(&m, 59000u, 60000u);
    EXPECT_U32("centre near the end", m.spanStartMs, 52000u);

    /* A replay shorter than the span: all of it. */
    clipGifSpanPlace(&m, 2000u, 3000u);
    s = clipGifSpanRange(&m, 3000u);
    EXPECT_U32("short: start", s.startMs, 0u);
    EXPECT_U32("short: end", s.endMs, 3000u);
    EXPECT_U32("short: zoom", m.zoom.endMs - m.zoom.startMs, 3000u);

    /* A start past the replay (a shorter log opened) is held to its end. */
    m.spanStartMs = 70000u;
    s = clipGifSpanRange(&m, 60000u);
    EXPECT_U32("past: start", s.startMs, 60000u);
    EXPECT_U32("past: end", s.endMs, 60000u);

    /* Turned off and on again: the span goes back to the playhead. */
    clipGifModeToggle(&m, false);
    clipGifModeToggle(&m, false);
    EXPECT_TRUE("on again: no span", m.on && !m.spanSet);
    clipGifSpanRefit(&m, 5000u, 60000u);
    EXPECT_U32("on again: at the playhead", m.spanStartMs, 5000u);
    clipGifModeReset(&m);
    EXPECT_TRUE("reset: no span", !m.spanSet && m.spanStartMs == 0u);

    /* What Preview clip and Save clip take: the span, or from the playhead
     * before the span has a place. */
    uint32_t st = 1u, len = 1u;
    clipGifSpanClip(&m, 58000u, 60000u, &st, &len);
    EXPECT_U32("no span: from the playhead", st, 58000u);
    EXPECT_U32("no span: the time left", len, 2000u);
    clipGifSpanPlace(&m, 20000u, 60000u);
    clipGifSpanClip(&m, 58000u, 60000u, &st, &len);
    EXPECT_U32("span: its start", st, 20000u);
    EXPECT_U32("span: its length", len, 5000u);
    clipGifSpanClip(NULL, 1u, 2u, &st, &len);
    EXPECT_TRUE("no mode", st == 0u && len == 0u);

    /* The fit on its own, and nothing to do with no mode. */
    ClipGifRange span = { 15000u, 20000u };
    ClipGifRange z = clipGifZoomFit(span, 0u);
    EXPECT_U32("fit: no length, start", z.startMs, 5000u);
    EXPECT_U32("fit: no length, end", z.endMs, 30000u);
    s = clipGifSpanRange(NULL, 60000u);
    EXPECT_TRUE("no mode: empty", s.startMs == 0u && s.endMs == 0u);
    clipGifSpanPlace(NULL, 0u, 0u);   /* does nothing */
    clipGifSpanCentre(NULL, 0u, 0u);  /* does nothing */
    clipGifSpanRefit(NULL, 0u, 1u);   /* does nothing */
    return fails ? 1 : 0;
}

/* The zoomed timeline's own scale: a time to a screen x and back, held to
 * the part it shows. */
int run_clip_gif_zoom_axis(void) {
    int fails = 0;
    const ClipGifRange r = { 10000u, 30000u };
    EXPECT_TRUE("x of the middle",
                fabsf(clipGifRangeX(20000u, r, 100.0f, 300.0f) - 200.0f) < 0.01f);
    EXPECT_TRUE("x before: the left end",
                clipGifRangeX(5000u, r, 100.0f, 300.0f) == 100.0f);
    EXPECT_TRUE("x after: the right end",
                clipGifRangeX(40000u, r, 100.0f, 300.0f) == 300.0f);
    EXPECT_U32("time of the middle", clipGifRangeTime(200.0f, r, 100.0f, 300.0f),
               20000u);
    EXPECT_U32("time left of it", clipGifRangeTime(50.0f, r, 100.0f, 300.0f),
               10000u);
    EXPECT_U32("time right of it", clipGifRangeTime(1000.0f, r, 100.0f, 300.0f),
               30000u);
    EXPECT_U32("time of NaN", clipGifRangeTime(NAN, r, 100.0f, 300.0f), 10000u);
    for (uint32_t t = 10000u; t <= 30000u; t += 2000u) {
        const float x = clipGifRangeX(t, r, 100.0f, 300.0f);
        EXPECT_U32("there and back", clipGifRangeTime(x, r, 100.0f, 300.0f), t);
    }
    const ClipGifRange e = { 5000u, 5000u };
    EXPECT_TRUE("empty range: x0", clipGifRangeX(5000u, e, 1.0f, 9.0f) == 1.0f);
    EXPECT_U32("empty range: its start", clipGifRangeTime(5.0f, e, 1.0f, 9.0f),
               5000u);
    EXPECT_TRUE("no width: x0", clipGifRangeX(20000u, r, 4.0f, 4.0f) == 4.0f);
    EXPECT_U32("no width: the start", clipGifRangeTime(4.0f, r, 4.0f, 4.0f),
               10000u);
    return fails ? 1 : 0;
}

/* A drag of the span's start or end handle: the other end stays, and the
 * length keeps to whole frames, the floor, the cap and the replay. */
int run_clip_gif_span_drag(void) {
    int fails = 0;
    ClipGifMode m = CLIP_GIF_MODE_INIT;
    m.spanSet = true;
    m.spanStartMs = 20000u;
    m.lenMs = 5000u;

    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_END, 27000u, 60000u);
    EXPECT_U32("end out", m.lenMs, 7000u);
    EXPECT_U32("end out: start stays", m.spanStartMs, 20000u);
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_END, 27049u, 60000u);
    EXPECT_U32("end: whole frames", m.lenMs, 7000u);
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_END, 19000u, 60000u);
    EXPECT_U32("end before the start: the floor", m.lenMs, CLIP_GIF_LEN_MIN_MS);
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_END, 60000u, 60000u);
    EXPECT_U32("end far out: the cap", m.lenMs, CLIP_GIF_LEN_MAX_MS);
    m.spanStartMs = 55000u;
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_END, 70000u, 60000u);
    EXPECT_U32("end past the replay: the time left", m.lenMs, 5000u);

    m.spanStartMs = 20000u;
    m.lenMs = 5000u;
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_START, 18000u, 60000u);
    EXPECT_U32("start out: start", m.spanStartMs, 18000u);
    EXPECT_U32("start out: length", m.lenMs, 7000u);
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_START, 24500u, 60000u);
    EXPECT_U32("start to the end: the floor", m.lenMs, CLIP_GIF_LEN_MIN_MS);
    EXPECT_U32("start to the end: end stays", m.spanStartMs + m.lenMs, 25000u);
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_START, 0u, 60000u);
    EXPECT_U32("start to 0: start", m.spanStartMs, 0u);
    EXPECT_U32("start to 0: length", m.lenMs, 25000u);
    m.spanStartMs = 30000u;
    m.lenMs = 10000u;
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_START, 0u, 60000u);
    EXPECT_U32("start far out: the cap", m.lenMs, CLIP_GIF_LEN_MAX_MS);
    EXPECT_U32("start far out: end stays", m.spanStartMs, 10000u);

    /* A replay shorter than the floor: from its start. */
    m.spanStartMs = 0u;
    m.lenMs = 5000u;
    clipGifSpanDragEdge(&m, CLIP_GIF_EDGE_START, 0u, 600u);
    EXPECT_U32("short: start", m.spanStartMs, 0u);
    EXPECT_U32("short: the floor", m.lenMs, CLIP_GIF_LEN_MIN_MS);

    /* Not an edge: nothing. */
    m.spanStartMs = 20000u;
    m.lenMs = 5000u;
    clipGifSpanDragEdge(&m, 0, 1000u, 60000u);
    EXPECT_TRUE("no edge", m.spanStartMs == 20000u && m.lenMs == 5000u);
    clipGifSpanDragEdge(NULL, CLIP_GIF_EDGE_END, 1000u, 60000u); /* nothing */
    return fails ? 1 : 0;
}

/* A handle held at an end of the zoomed timeline: that end moves out by up
 * to a step, to no more than 10 s past the span and not past the replay,
 * and never moves in. */
int run_clip_gif_zoom_grow(void) {
    int fails = 0;
    ClipGifRange z = { 10000u, 35000u };
    ClipGifRange span = { 20000u, 30000u };
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_END, 1000u, 60000u);
    EXPECT_U32("end: one step", z.endMs, 36000u);
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_END, 10000u, 60000u);
    EXPECT_U32("end: 10 s past the span", z.endMs, 40000u);
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_END, 10000u, 60000u);
    EXPECT_U32("end: no more", z.endMs, 40000u);
    EXPECT_U32("end: the start stays", z.startMs, 10000u);
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_START, 10000u, 60000u);
    EXPECT_U32("start: 10 s before already", z.startMs, 10000u);

    span.startMs = 15000u;
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_START, 2000u, 60000u);
    EXPECT_U32("start: one step", z.startMs, 8000u);
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_START, 10000u, 60000u);
    EXPECT_U32("start: 10 s before the span", z.startMs, 5000u);
    span.startMs = 3000u;
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_START, 10000u, 60000u);
    EXPECT_U32("start: not before 0", z.startMs, 0u);

    span.endMs = 55000u;
    z.endMs = 50000u;
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_END, 20000u, 60000u);
    EXPECT_U32("end: not past the replay", z.endMs, 60000u);

    /* Wider than the span needs: it stays. */
    z.startMs = 0u;
    z.endMs = 50000u;
    span.startMs = 20000u;
    span.endMs = 25000u;
    clipGifZoomGrow(&z, span, CLIP_GIF_EDGE_END, 5000u, 60000u);
    EXPECT_U32("never in", z.endMs, 50000u);
    clipGifZoomGrow(&z, span, 0, 5000u, 60000u);
    EXPECT_TRUE("no edge", z.startMs == 0u && z.endMs == 50000u);
    clipGifZoomGrow(NULL, span, CLIP_GIF_EDGE_END, 5000u, 60000u); /* nothing */
    return fails ? 1 : 0;
}
