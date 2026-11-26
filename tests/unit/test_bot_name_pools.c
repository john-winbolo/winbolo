/*
 * Coverage for the runtime-loadable bot naming pools
 * (src/bolo/lobby_bot_pools.c + src/common/lobby_bot_pools_json.c).
 *
 * The themed pools can be replaced at runtime via lobbyBotPoolsInstall
 * (copy + safety-clamp) or lobbyBotPoolsLoadFromFile (JSON wrapper).
 * The generated "Numbered Bots" pool is always appended as the last
 * index, and the built-in tables are the fallback when nothing custom
 * is installed.
 *
 * Pool state is process-global, so every test resets to built-ins at
 * entry and leaves built-ins active on exit — this keeps results
 * deterministic whether tests run one-per-process (CTest) or all in
 * sequence.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include <SDL3/SDL.h>

#include "lobby_bot_pools.h"
#include "bolo_rand.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "netpacks.h"               /* PACKET_HEADER_SIZE, PACKET_LOBBY_BOT_POOL_CHUNK */
#include "test_harness.h"

#define BUILTIN_THEMED 8   /* themed built-in pools (excludes numbered) */

/* Index of the always-appended generated "Numbered Bots" pool. */
static int numericIdx(void) { return lobbyBotPoolCount() - 1; }

int run_bot_pool_builtin_defaults(void) {
    lobbyBotPoolsReset();

    /* 8 themed built-ins + 1 generated numbered pool. */
    UT_ASSERT_MSG(lobbyBotPoolCount() == BUILTIN_THEMED + 1,
                  "expected %d pools, got %d",
                  BUILTIN_THEMED + 1, lobbyBotPoolCount());

    /* First themed pool is "Classic AI" and its first name is stable. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolName(0, 0), "HAL-9000") == 0);

    /* Numbered pool sits last, has 16 generated entries. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(numericIdx()), "Numbered Bots") == 0);
    UT_ASSERT(lobbyBotPoolNameCount(numericIdx()) == 16);
    UT_ASSERT(strcmp(lobbyBotPoolName(numericIdx(), 0), "Bot-01") == 0);

    /* Out-of-range guards. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(-1), "Unknown") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(9999), "Unknown") == 0);
    UT_ASSERT(lobbyBotPoolName(0, -1) == NULL);
    UT_ASSERT(lobbyBotPoolName(0, 99999) == NULL);

    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_install_and_clamp(void) {
    LobbyBotPoolLoadStats st;
    const char *aNames[] = { "Alpha", "Bravo", "Charlie" };
    const char *bNames[] = { "One" };
    LobbyBotPoolDef defs[2];

    lobbyBotPoolsReset();

    defs[0].label = "Greek Letters";
    defs[0].names = aNames;
    defs[0].nameCount = 3;
    /* No label on the second pool -> defaults to "Pool 2". */
    defs[1].label = NULL;
    defs[1].names = bNames;
    defs[1].nameCount = 1;

    UT_ASSERT(lobbyBotPoolsInstall(defs, 2, &st) == 2);
    UT_ASSERT(st.poolsIn == 2 && st.poolsKept == 2 && st.poolsDropped == 0);

    UT_ASSERT(lobbyBotPoolCount() == 2 + 1);   /* + numbered */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Greek Letters") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Pool 2") == 0);
    UT_ASSERT(lobbyBotPoolNameCount(0) == 3);
    UT_ASSERT(strcmp(lobbyBotPoolName(0, 2), "Charlie") == 0);
    /* Numbered pool still appended after the custom set. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(numericIdx()), "Numbered Bots") == 0);

    /* Overlong-name clamp: a name longer than the byte cap is dropped. */
    {
        char longName[LOBBY_BOT_POOL_MAX_NAME_BYTES + 10];
        const char *names2[2];
        LobbyBotPoolDef d;
        memset(longName, 'x', sizeof(longName) - 1);
        longName[sizeof(longName) - 1] = '\0';
        names2[0] = "Keep";
        names2[1] = longName;
        d.label = "Lengths";
        d.names = names2;
        d.nameCount = 2;
        UT_ASSERT(lobbyBotPoolsInstall(&d, 1, &st) == 1);
        UT_ASSERT(lobbyBotPoolNameCount(0) == 1);
        UT_ASSERT(st.namesKept == 1 && st.namesDropped == 1);
    }

    /* Names-per-pool cap. Build > MAX_NAMES unique names. */
    {
        int total = LOBBY_BOT_POOL_MAX_NAMES + 20;
        char *storage = (char *)malloc((size_t)total * 8);
        const char **names = (const char **)malloc((size_t)total * sizeof(char *));
        LobbyBotPoolDef d;
        int i;
        for (i = 0; i < total; i++) {
            char *slot = storage + i * 8;
            snprintf(slot, 8, "n%05d", i);
            names[i] = slot;
        }
        d.label = "Many";
        d.names = names;
        d.nameCount = total;
        UT_ASSERT(lobbyBotPoolsInstall(&d, 1, &st) == 1);
        UT_ASSERT_MSG(lobbyBotPoolNameCount(0) == LOBBY_BOT_POOL_MAX_NAMES,
                      "expected %d names after cap, got %d",
                      LOBBY_BOT_POOL_MAX_NAMES, lobbyBotPoolNameCount(0));
        UT_ASSERT(st.namesDropped == 20);
        free(storage);
        free((void *)names);
    }

    /* Pools-per-set cap. Offer > MAX_POOLS pools (one name each). */
    {
        int total = LOBBY_BOT_POOL_MAX_POOLS + 5;
        const char *oneName[] = { "x" };
        LobbyBotPoolDef *many =
            (LobbyBotPoolDef *)malloc((size_t)total * sizeof(LobbyBotPoolDef));
        int i;
        for (i = 0; i < total; i++) {
            many[i].label = "P";
            many[i].names = oneName;
            many[i].nameCount = 1;
        }
        UT_ASSERT(lobbyBotPoolsInstall(many, total, &st) ==
                  LOBBY_BOT_POOL_MAX_POOLS);
        UT_ASSERT(st.poolsKept == LOBBY_BOT_POOL_MAX_POOLS);
        UT_ASSERT(st.poolsDropped == 5);
        free(many);
    }

    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_install_dedup_and_drop(void) {
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();

    /* Case-insensitive dedupe within a pool, plus empty-name skip. */
    {
        const char *names[] = { "Echo", "echo", "ECHO", "", "Foxtrot" };
        LobbyBotPoolDef d;
        d.label = "Dups";
        d.names = names;
        d.nameCount = 5;
        UT_ASSERT(lobbyBotPoolsInstall(&d, 1, &st) == 1);
        UT_ASSERT_MSG(lobbyBotPoolNameCount(0) == 2,
                      "expected 2 unique names, got %d",
                      lobbyBotPoolNameCount(0));
        UT_ASSERT(strcmp(lobbyBotPoolName(0, 0), "Echo") == 0);
        UT_ASSERT(strcmp(lobbyBotPoolName(0, 1), "Foxtrot") == 0);
        UT_ASSERT(st.namesDropped == 3);   /* echo, ECHO, "" */
    }

    /* A pool with no usable names is dropped entirely; if that leaves
     * nothing usable, the previously-active set is retained. */
    {
        const char *empties[] = { "", "" };
        LobbyBotPoolDef d;
        int before = lobbyBotPoolCount();
        d.label = "AllEmpty";
        d.names = empties;
        d.nameCount = 2;
        UT_ASSERT(lobbyBotPoolsInstall(&d, 1, &st) == 0);
        UT_ASSERT(st.poolsKept == 0 && st.poolsDropped == 1);
        /* Active pools unchanged (still the "Dups" set from above). */
        UT_ASSERT(lobbyBotPoolCount() == before);
        UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Dups") == 0);
    }

    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_reset_restores_builtin(void) {
    const char *names[] = { "Solo" };
    LobbyBotPoolDef d;

    lobbyBotPoolsReset();

    d.label = "Custom";
    d.names = names;
    d.nameCount = 1;
    UT_ASSERT(lobbyBotPoolsInstall(&d, 1, NULL) == 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Custom") == 0);

    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolCount() == BUILTIN_THEMED + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    return 0;
}

int run_bot_pool_pick_unique_and_overflow(void) {
    const char *names[] = { "Ann", "Bob" };
    LobbyBotPoolDef d;
    const char *used[8];
    int usedCount = 0;
    char buf[64];

    lobbyBotPoolsReset();

    d.label = "Pair";
    d.names = names;
    d.nameCount = 2;
    UT_ASSERT(lobbyBotPoolsInstall(&d, 1, NULL) == 1);

    /* First two picks exhaust the pool with distinct names. */
    lobbyBotPoolPick(0, used, usedCount, buf, sizeof(buf));
    UT_ASSERT(strcmp(buf, "Ann") == 0 || strcmp(buf, "Bob") == 0);
    used[usedCount++] = strdup(buf);

    lobbyBotPoolPick(0, used, usedCount, buf, sizeof(buf));
    UT_ASSERT(strcmp(buf, used[0]) != 0);
    UT_ASSERT(strcmp(buf, "Ann") == 0 || strcmp(buf, "Bob") == 0);
    used[usedCount++] = strdup(buf);

    /* Pool exhausted -> overflow into "<label> N". */
    lobbyBotPoolPick(0, used, usedCount, buf, sizeof(buf));
    UT_ASSERT_MSG(strcmp(buf, "Pair 1") == 0,
                  "expected overflow 'Pair 1', got '%s'", buf);

    free((void *)used[0]);
    free((void *)used[1]);
    lobbyBotPoolsReset();
    return 0;
}

/* ── JSON loader ────────────────────────────────────────────────── */

static int writeFile(const char *path, const char *contents) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(contents, f);
    fclose(f);
    return 1;
}

int run_bot_pool_json_load_roundtrip(void) {
    const char *path = "test_bot_names_tmp.json";
    const char *json =
        "{ \"pools\": [\n"
        "  { \"label\": \"Colors\", \"names\": [\"Red\", \"Green\", \"red\"] },\n"
        "  { \"label\": \"Numbers\", \"names\": [\"One\", \"\", \"Two\"] }\n"
        "] }\n";
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();
    UT_ASSERT(writeFile(path, json));

    UT_ASSERT(lobbyBotPoolsLoadFromFile(path, &st) == true);
    UT_ASSERT(st.poolsKept == 2);
    UT_ASSERT(lobbyBotPoolCount() == 2 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Colors") == 0);
    /* "red" deduped against "Red". */
    UT_ASSERT(lobbyBotPoolNameCount(0) == 2);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Numbers") == 0);
    /* empty string skipped. */
    UT_ASSERT(lobbyBotPoolNameCount(1) == 2);

    remove(path);
    lobbyBotPoolsReset();
    return 0;
}

/* ── Wire catalog (serialize → compress → deserialize) ──────────── */

int run_bot_pool_wire_roundtrip(void) {
    static unsigned char buf[70000];
    const char *a[] = { "Red", "Green", "Blue" };
    const char *b[] = { "One", "Two" };
    LobbyBotPoolDef defs[2];
    LobbyBotPoolLoadStats st;
    int n;

    lobbyBotPoolsReset();
    defs[0].label = "Colors"; defs[0].names = a; defs[0].nameCount = 3;
    defs[1].label = "Nums";   defs[1].names = b; defs[1].nameCount = 2;
    UT_ASSERT(lobbyBotPoolsInstall(defs, 2, NULL) == 2);

    n = lobbyBotPoolsSerialize(buf, (int)sizeof(buf));
    UT_ASSERT_MSG(n > 4, "serialize returned %d", n);

    /* Wipe back to built-ins so the deserialize is observable. */
    lobbyBotPoolsReset();
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);

    UT_ASSERT(lobbyBotPoolsDeserializeInstall(buf, n, &st) == 2);
    UT_ASSERT(lobbyBotPoolCount() == 2 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Colors") == 0);
    UT_ASSERT(lobbyBotPoolNameCount(0) == 3);
    UT_ASSERT(strcmp(lobbyBotPoolName(0, 2), "Blue") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Nums") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolName(1, 1), "Two") == 0);
    /* Numbered pool is NOT serialized but reappears as the last index. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(numericIdx()), "Numbered Bots") == 0);

    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_wire_builtin_roundtrip(void) {
    static unsigned char buf[70000];
    int n, before;

    lobbyBotPoolsReset();
    before = lobbyBotPoolCount();
    n = lobbyBotPoolsSerialize(buf, (int)sizeof(buf));
    UT_ASSERT(n > 4);
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(buf, n, NULL) == BUILTIN_THEMED);
    UT_ASSERT(lobbyBotPoolCount() == before);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolName(0, 0), "HAL-9000") == 0);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_wire_rejects_garbage(void) {
    unsigned char tooShort[3] = { 0, 0, 0 };
    unsigned char zeroRaw[8]  = { 0, 0, 0, 0, 1, 2, 3, 4 };       /* rawLen 0 */
    unsigned char badZlib[16] = { 0, 0, 0, 10, 0xFF, 0xFF, 0xFF, 0xFF,
                                  0, 0, 0, 0, 0, 0, 0, 0 };        /* bad deflate */
    unsigned char hugeRaw[8]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };

    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(tooShort, 3, NULL) == -1);
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(zeroRaw, 8, NULL) == -1);
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(badZlib, 16, NULL) == -1);
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(hugeRaw, 8, NULL) == -1);

    /* Built-ins untouched after every rejection. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    lobbyBotPoolsReset();
    return 0;
}

/* The retired chunk path for the catalog: serialize → split into
 * CTRL_LOBBY_BOT_POOL_CHUNK events → encode each through the real codec →
 * decode → reassemble in order → deserialize+install. Nothing sends the
 * chunks now (the catalogue goes on CHANNEL_BULK), but the codec stays so
 * recordings that hold them decode, and this still holds it to that. */
int run_bot_pool_wire_chunk_transport(void) {
    static unsigned char blob[70000];
    static unsigned char assembled[70000];
    const char *a[] = { "Red", "Green", "Blue", "Cyan" };
    const char *b[] = { "One", "Two", "Three" };
    LobbyBotPoolDef defs[2];
    ControlEncodeFn enc;
    ControlDecodeFn dec;
    int blen, frag, nChunks, off, ci;
    uint32_t asmLen = 0;

    lobbyBotPoolsReset();
    defs[0].label = "Colors"; defs[0].names = a; defs[0].nameCount = 4;
    defs[1].label = "Nums";   defs[1].names = b; defs[1].nameCount = 3;
    UT_ASSERT(lobbyBotPoolsInstall(defs, 2, NULL) == 2);

    blen = lobbyBotPoolsSerialize(blob, (int)sizeof(blob));
    UT_ASSERT(blen > 4);

    enc = transportControlCodecEncoder(CTRL_LOBBY_BOT_POOL_CHUNK);
    dec = transportControlCodecDecoder(PACKET_LOBBY_BOT_POOL_CHUNK);
    UT_ASSERT(enc != NULL && dec != NULL);

    /* Wipe to built-ins so a successful reassembly is observable. */
    lobbyBotPoolsReset();
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);

    frag = LOBBY_BOT_POOL_CHUNK_FRAG_MAX;
    nChunks = (blen + frag - 1) / frag;
    off = 0;
    for (ci = 0; ci < nChunks; ci++) {
        ControlEvent src, dst;
        uint8_t pkt[PACKET_HEADER_SIZE + 4 + LOBBY_BOT_POOL_CHUNK_FRAG_MAX];
        size_t outLen = 0;
        int fl = blen - off;
        if (fl > frag) fl = frag;

        memset(&src, 0, sizeof(src));
        src.type = CTRL_LOBBY_BOT_POOL_CHUNK;
        src.u.lobbyBotPoolChunk.seq = (uint8_t)ci;
        src.u.lobbyBotPoolChunk.count = (uint8_t)nChunks;
        src.u.lobbyBotPoolChunk.fragLen = (uint16_t)fl;
        memcpy(src.u.lobbyBotPoolChunk.frag, blob + off, (size_t)fl);

        UT_ASSERT(enc(&src, NULL, pkt, sizeof(pkt), &outLen) == ENCODE_OK);
        UT_ASSERT(outLen > PACKET_HEADER_SIZE);
        UT_ASSERT(dec(pkt + PACKET_HEADER_SIZE,
                      outLen - PACKET_HEADER_SIZE, &dst));
        UT_ASSERT(dst.type == CTRL_LOBBY_BOT_POOL_CHUNK);
        UT_ASSERT(dst.u.lobbyBotPoolChunk.seq == (uint8_t)ci);
        UT_ASSERT(dst.u.lobbyBotPoolChunk.count == (uint8_t)nChunks);
        UT_ASSERT(dst.u.lobbyBotPoolChunk.fragLen == (uint16_t)fl);

        memcpy(assembled + asmLen, dst.u.lobbyBotPoolChunk.frag, (size_t)fl);
        asmLen += (uint32_t)fl;
        off += fl;
    }

    UT_ASSERT((int)asmLen == blen);
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(assembled, (int)asmLen, NULL) == 2);
    UT_ASSERT(lobbyBotPoolCount() == 2 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Colors") == 0);
    UT_ASSERT(lobbyBotPoolNameCount(0) == 4);
    UT_ASSERT(strcmp(lobbyBotPoolName(0, 3), "Cyan") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Nums") == 0);

    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_json_missing_file_keeps_active(void) {
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();

    /* Missing file -> false, built-ins untouched. */
    UT_ASSERT(lobbyBotPoolsLoadFromFile("definitely-not-here-12345.json",
                                        &st) == false);
    UT_ASSERT(lobbyBotPoolCount() == BUILTIN_THEMED + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);

    /* Malformed JSON -> false, built-ins untouched. */
    {
        const char *path = "test_bot_names_bad_tmp.json";
        UT_ASSERT(writeFile(path, "{ this is not json "));
        UT_ASSERT(lobbyBotPoolsLoadFromFile(path, &st) == false);
        UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
        remove(path);
    }

    return 0;
}

/* THE CATALOGUE ID NAMES THE POOLS, NOT THE BYTES THAT CARRIED THEM.
 *
 * A joiner compares the id the server named with the id of its own pools and
 * fetches the catalogue only when they differ. So the id has to be the same
 * for the same pools whichever peer computes it — including a client that got
 * them by installing the server's blob — and different for different pools.
 * And SerializeWithId must hand out the id that CatalogId would. */
int run_bot_pool_catalog_id(void) {
    static unsigned char blob[70000];
    const char *a[] = { "Red", "Green", "Blue" };
    LobbyBotPoolDef def;
    uint32_t builtinId, withId = 0, otherId;
    int n;

    lobbyBotPoolsReset();
    builtinId = lobbyBotPoolsCatalogId();
    UT_ASSERT_MSG(builtinId != 0, "the built-in pools have no id");
    UT_ASSERT(lobbyBotPoolsCatalogId() == builtinId);

    n = lobbyBotPoolsSerializeWithId(blob, (int)sizeof(blob), &withId);
    UT_ASSERT(n > 4);
    UT_ASSERT_MSG(withId == builtinId,
                  "SerializeWithId said %08x, CatalogId %08x",
                  (unsigned)withId, (unsigned)builtinId);

    def.label = "Colors"; def.names = a; def.nameCount = 3;
    UT_ASSERT(lobbyBotPoolsInstall(&def, 1, NULL) == 1);
    otherId = lobbyBotPoolsCatalogId();
    UT_ASSERT_MSG(otherId != 0 && otherId != builtinId,
                  "other pools have id %08x, the built-in ones %08x",
                  (unsigned)otherId, (unsigned)builtinId);

    /* Installing the built-in blob over them gives the built-in id back:
       what a client that fetched the server's catalogue then compares. */
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(blob, n, NULL) > 0);
    UT_ASSERT_MSG(lobbyBotPoolsCatalogId() == builtinId,
                  "the pools installed from the blob have id %08x, the pools "
                  "it was made from %08x",
                  (unsigned)lobbyBotPoolsCatalogId(), (unsigned)builtinId);

    lobbyBotPoolsReset();
    return 0;
}

/* ── -botnames pool labels ──────────────────────────────────────────
 *
 * WinBoloDS reads -botnames as a pool file when the value is an existing
 * file or looks like a path, and otherwise as comma-separated labels of
 * pools in the shipped file. lobbyBotPoolsLoadFromArg holds those rules;
 * these cases hand it their own "shipped" file so they do not depend on
 * data/bot_names.json. */

#define LABEL_TEST_FILE  "test_bot_names_labels_tmp.json"
#define LABEL_OTHER_FILE "test_bot_names_other_tmp.json"
#define LABEL_WARN_FILE  "test_bot_names_warn_tmp.txt"

static int writeLabelFile(void) {
    return writeFile(LABEL_TEST_FILE,
        "{ \"pools\": [\n"
        "  { \"label\": \"Classic AI\", \"names\": [\"HAL\", \"Skynet\", \"WOPR\"] },\n"
        "  { \"label\": \"Famous Painters\", \"names\": [\"Monet\", \"Dali\"] },\n"
        "  { \"label\": \"Planes\", \"names\": [\"Spitfire\", \"Mustang\"] }\n"
        "] }\n");
}

/* Run lobbyBotPoolsLoadFromArg with warnings into a file, and read them
 * back into out (NUL-terminated). */
static bool loadArgCapture(const char *arg, char *out, size_t cap,
                           LobbyBotPoolLoadStats *st) {
    FILE *w = fopen(LABEL_WARN_FILE, "w+b");
    bool ok;
    size_t n = 0;
    out[0] = '\0';
    ok = lobbyBotPoolsLoadFromArg(arg, LABEL_TEST_FILE, w, st);
    if (w) {
        fflush(w);
        fseek(w, 0, SEEK_SET);
        n = fread(out, 1, cap - 1, w);
        out[n] = '\0';
        fclose(w);
        remove(LABEL_WARN_FILE);
    }
    return ok;
}

int run_bot_pool_label_one(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    UT_ASSERT(loadArgCapture("Classic AI", warn, sizeof(warn), &st));
    UT_ASSERT_MSG(lobbyBotPoolCount() == 1 + 1,
                  "one label kept %d pools (with numbered)",
                  lobbyBotPoolCount());
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT(lobbyBotPoolNameCount(0) == 3);
    UT_ASSERT(strcmp(lobbyBotPoolName(0, 1), "Skynet") == 0);
    UT_ASSERT_MSG(lobbyBotPoolDrawCount() == 1,
                  "draws may pick %d pools, want only the chosen one",
                  lobbyBotPoolDrawCount());
    UT_ASSERT(st.poolsKept == 1);
    UT_ASSERT_MSG(strstr(warn, "Warning") == NULL, "unexpected: %s", warn);
    UT_ASSERT(strstr(warn, "Bot name pools chosen: Classic AI") != NULL);

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());
    return 0;
}

int run_bot_pool_label_several_spaces_case(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;
    LobbyBotPoolFilterResult fr;

    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    /* Odd case, spaces and tabs around labels, an empty item, a repeat. */
    UT_ASSERT(loadArgCapture("  planes ,\tCLASSIC ai,, classic AI ", warn,
                             sizeof(warn), &st));
    UT_ASSERT_MSG(lobbyBotPoolCount() == 2 + 1, "kept %d pools",
                  lobbyBotPoolCount() - 1);
    /* File order, not list order. */
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Planes") == 0);
    UT_ASSERT(lobbyBotPoolDrawCount() == 2);
    UT_ASSERT_MSG(strstr(warn, "Warning") == NULL, "unexpected: %s", warn);

    /* The pure filter on its own: counts labels as typed. */
    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolsLoadFromFile(LABEL_TEST_FILE, NULL));
    UT_ASSERT(lobbyBotPoolsFilterByLabels(" Famous Painters,planes", &fr) == 2);
    UT_ASSERT(fr.labelsGiven == 2 && fr.labelsMatched == 2);
    UT_ASSERT(fr.unknownCount == 0 && fr.poolsKept == 2);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Famous Painters") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Planes") == 0);

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_unknown_falls_back(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    UT_ASSERT(loadArgCapture("Zebras", warn, sizeof(warn), &st));
    UT_ASSERT_MSG(strstr(warn,
                  "Warning: -botnames: no pool named 'Zebras'; pools are: "
                  "Classic AI, Famous Painters, Planes") != NULL,
                  "warning was: %s", warn);
    UT_ASSERT_MSG(strstr(warn, "using all pools") != NULL,
                  "warning was: %s", warn);
    /* All pools, and draws as with no -botnames. */
    UT_ASSERT(lobbyBotPoolCount() == 3 + 1);
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());
    UT_ASSERT(st.poolsKept == 3);

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_mixed_known_unknown(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    UT_ASSERT(loadArgCapture("Planes, Zebras ,famous painters", warn,
                             sizeof(warn), &st));
    UT_ASSERT_MSG(strstr(warn, "no pool named 'Zebras'; pools are: "
                  "Classic AI, Famous Painters, Planes") != NULL,
                  "warning was: %s", warn);
    UT_ASSERT(strstr(warn, "using all pools") == NULL);
    UT_ASSERT(lobbyBotPoolCount() == 2 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Famous Painters") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(1), "Planes") == 0);
    UT_ASSERT(lobbyBotPoolDrawCount() == 2);
    UT_ASSERT(st.poolsKept == 2);

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_file_path_still_loads(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;

    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    UT_ASSERT(writeFile(LABEL_OTHER_FILE,
        "{ \"pools\": [ { \"label\": \"Own\", \"names\": [\"A\", \"B\"] } ] }"));
    /* An existing file is loaded as a pool file, not as labels. */
    UT_ASSERT(loadArgCapture(LABEL_OTHER_FILE, warn, sizeof(warn), &st));
    UT_ASSERT(lobbyBotPoolCount() == 1 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Own") == 0);
    /* A file keeps today's draws, the numbered pool included. */
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());
    UT_ASSERT_MSG(warn[0] == '\0', "unexpected: %s", warn);

    remove(LABEL_OTHER_FILE);
    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_path_missing_not_label(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;

    UT_ASSERT(lobbyBotNamesArgLooksLikePath("pools/mine.json"));
    UT_ASSERT(lobbyBotNamesArgLooksLikePath("C:\\names\\x"));
    UT_ASSERT(lobbyBotNamesArgLooksLikePath("Classic AI.JSON"));
    UT_ASSERT(!lobbyBotNamesArgLooksLikePath("Classic AI"));
    UT_ASSERT(!lobbyBotNamesArgLooksLikePath("Classic AI,Planes"));
    UT_ASSERT(!lobbyBotNamesArgLooksLikePath(".js"));

    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    /* A missing path is never tried as a label, even when its stem is
     * one: today's warning, and the built-in names stay. */
    UT_ASSERT(!loadArgCapture("Classic AI.json", warn, sizeof(warn), &st));
    UT_ASSERT_MSG(strstr(warn, "Warning: -botnames 'Classic AI.json' could "
                  "not be loaded; using built-in bot names") != NULL,
                  "warning was: %s", warn);
    UT_ASSERT(lobbyBotPoolCount() == BUILTIN_THEMED + 1);
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());

    UT_ASSERT(!loadArgCapture("no-such-dir/names", warn, sizeof(warn), &st));
    UT_ASSERT(strstr(warn, "could not be loaded") != NULL);
    UT_ASSERT(lobbyBotPoolCount() == BUILTIN_THEMED + 1);

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_none_same_draws(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;
    BoloRandState rs;
    uint32_t before[64];
    int i, n;

    /* The reference: the shipped file loaded directly, as WinBoloDS did
     * before -botnames took labels, and the draws over the pool count. */
    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    UT_ASSERT(lobbyBotPoolsLoadFromFile(LABEL_TEST_FILE, NULL));
    n = lobbyBotPoolCount();
    bolo_rand_save(&rs);
    for (i = 0; i < 64; i++) before[i] = bolo_rand_below((uint32_t)n);

    /* No -botnames: same pools, same draw count, same draws. */
    lobbyBotPoolsReset();
    UT_ASSERT(loadArgCapture(NULL, warn, sizeof(warn), &st));
    UT_ASSERT_MSG(warn[0] == '\0', "unexpected: %s", warn);
    UT_ASSERT(lobbyBotPoolCount() == n);
    UT_ASSERT(lobbyBotPoolDrawCount() == n);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(2), "Planes") == 0);
    bolo_rand_restore(&rs);
    for (i = 0; i < 64; i++) {
        UT_ASSERT(bolo_rand_below((uint32_t)lobbyBotPoolDrawCount()) == before[i]);
    }

    /* "list" prints the labels and keeps every pool. */
    UT_ASSERT(loadArgCapture("list", warn, sizeof(warn), &st));
    UT_ASSERT_MSG(strstr(warn, "Bot name pools: Classic AI, Famous Painters, "
                  "Planes") != NULL, "output was: %s", warn);
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());

    /* A later install (a file, or a client taking a catalogue from a
     * server with no labels) puts the draws back over every pool. */
    UT_ASSERT(lobbyBotPoolsFilterByLabels("Planes", NULL) == 1);
    UT_ASSERT(lobbyBotPoolDrawCount() == 1);
    UT_ASSERT(lobbyBotPoolsLoadFromFile(LABEL_TEST_FILE, NULL));
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_dir_not_file(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;
    SDL_PathInfo info;
    bool made = false;

    /* A directory with a label's name is not a pool file: the value is
     * still read as that label. (fopen opens a directory on Linux.) */
    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    if (!SDL_GetPathInfo("Planes", &info)) {
        UT_ASSERT(SDL_CreateDirectory("Planes"));
        made = true;
    }
    UT_ASSERT(SDL_GetPathInfo("Planes", &info));
    UT_ASSERT(info.type == SDL_PATHTYPE_DIRECTORY);

    UT_ASSERT(loadArgCapture("Planes", warn, sizeof(warn), &st));
    if (made) SDL_RemovePath("Planes");
    UT_ASSERT_MSG(strstr(warn, "could not be loaded") == NULL,
                  "read the directory as a file: %s", warn);
    UT_ASSERT(lobbyBotPoolCount() == 1 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Planes") == 0);
    UT_ASSERT(lobbyBotPoolDrawCount() == 1);
    UT_ASSERT(st.poolsKept == 1);

    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_shipped_missing(void) {
    char warn[2048];
    LobbyBotPoolLoadStats st;
    FILE *w;
    size_t n;
    bool ok;

    /* The shipped file is missing: a warning, and the labels then choose
     * from the built-in pools. */
    lobbyBotPoolsReset();
    remove("test_bot_names_no_such_file.json");
    w = fopen(LABEL_WARN_FILE, "w+b");
    UT_ASSERT(w != NULL);
    ok = lobbyBotPoolsLoadFromArg("Classic AI",
                                  "test_bot_names_no_such_file.json", w, &st);
    fflush(w);
    fseek(w, 0, SEEK_SET);
    n = fread(warn, 1, sizeof(warn) - 1, w);
    warn[n] = '\0';
    fclose(w);
    remove(LABEL_WARN_FILE);

    UT_ASSERT(!ok);
    UT_ASSERT_MSG(strstr(warn, "Warning: -botnames: the shipped bot names "
                  "file could not be loaded; choosing from the built-in "
                  "pools") != NULL, "warning was: %s", warn);
    UT_ASSERT_MSG(strstr(warn, "Bot name pools chosen: Classic AI") != NULL,
                  "output was: %s", warn);
    UT_ASSERT_MSG(lobbyBotPoolCount() == 1 + 1, "kept %d pools",
                  lobbyBotPoolCount() - 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT(lobbyBotPoolNameCount(0) > 0);
    UT_ASSERT(lobbyBotPoolDrawCount() == 1);

    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolCount() == BUILTIN_THEMED + 1);
    return 0;
}

static unsigned long blobRawLen(const unsigned char *b) {
    return ((unsigned long)b[0] << 24) | ((unsigned long)b[1] << 16) |
           ((unsigned long)b[2] << 8) | (unsigned long)b[3];
}

int run_bot_pool_label_catalog_carries_filter(void) {
    static unsigned char filtered[LOBBY_BOT_CATALOG_WIRE_MAX];
    static unsigned char plain[LOBBY_BOT_CATALOG_WIRE_MAX];
    int filteredLen, plainLen;
    uint32_t filteredId, plainId;

    /* A server that chose pools by label: its catalogue says so, and a
     * client that installs it draws only from those pools too (the first
     * bot a player adds to a team in the lobby). */
    lobbyBotPoolsReset();
    UT_ASSERT(writeLabelFile());
    UT_ASSERT(lobbyBotPoolsLoadFromFile(LABEL_TEST_FILE, NULL));
    UT_ASSERT(lobbyBotPoolsFilterByLabels("Classic AI", NULL) == 1);
    filteredLen = lobbyBotPoolsSerializeWithId(filtered, sizeof(filtered),
                                               &filteredId);
    UT_ASSERT(filteredLen > 4);
    UT_ASSERT(filteredId == lobbyBotPoolsCatalogId());

    /* The same pool from a file, with no labels: no flags byte, so the
     * payload is one byte shorter and the id is another. */
    UT_ASSERT(writeFile(LABEL_OTHER_FILE,
        "{ \"pools\": [ { \"label\": \"Classic AI\", "
        "\"names\": [\"HAL\", \"Skynet\", \"WOPR\"] } ] }"));
    UT_ASSERT(lobbyBotPoolsLoadFromFile(LABEL_OTHER_FILE, NULL));
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());
    plainLen = lobbyBotPoolsSerializeWithId(plain, sizeof(plain), &plainId);
    UT_ASSERT(plainLen > 4);
    UT_ASSERT_MSG(blobRawLen(filtered) == blobRawLen(plain) + 1,
                  "payloads %lu and %lu bytes", blobRawLen(filtered),
                  blobRawLen(plain));
    UT_ASSERT(filteredId != plainId);

    /* The client side: the filtered catalogue keeps the draws off
     * "Numbered Bots"; the plain one draws over every pool. */
    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(filtered, filteredLen, NULL) == 1);
    UT_ASSERT(lobbyBotPoolCount() == 1 + 1);
    UT_ASSERT(strcmp(lobbyBotPoolLabel(0), "Classic AI") == 0);
    UT_ASSERT_MSG(lobbyBotPoolDrawCount() == 1,
                  "client draws over %d pools, want 1",
                  lobbyBotPoolDrawCount());
    UT_ASSERT(lobbyBotPoolsCatalogId() == filteredId);

    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolsDeserializeInstall(plain, plainLen, NULL) == 1);
    UT_ASSERT(lobbyBotPoolDrawCount() == lobbyBotPoolCount());
    UT_ASSERT(lobbyBotPoolsCatalogId() == plainId);

    remove(LABEL_OTHER_FILE);
    remove(LABEL_TEST_FILE);
    lobbyBotPoolsReset();
    return 0;
}

int run_bot_pool_label_unknown_cut_utf8(void) {
    char label[64];
    LobbyBotPoolFilterResult fr;
    int i;

    /* 47 ASCII bytes, then a 2-byte character across the 48-byte cut. */
    for (i = 0; i < 47; i++) label[i] = 'a';
    label[47] = (char)0xC3;
    label[48] = (char)0xA9;
    label[49] = 'b';
    label[50] = '\0';

    lobbyBotPoolsReset();
    UT_ASSERT(lobbyBotPoolsFilterByLabels(label, &fr) == 0);
    UT_ASSERT(fr.unknownCount == 1);
    UT_ASSERT_MSG(strlen(fr.unknown[0]) == 47,
                  "cut at %d bytes, want 47", (int)strlen(fr.unknown[0]));

    /* A cut that falls between characters keeps all 48 bytes. */
    for (i = 0; i < 50; i++) label[i] = 'a';
    label[50] = '\0';
    UT_ASSERT(lobbyBotPoolsFilterByLabels(label, &fr) == 0);
    UT_ASSERT(strlen(fr.unknown[0]) == LOBBY_BOT_POOL_MAX_LABEL_BYTES);

    lobbyBotPoolsReset();
    return 0;
}
