/*
 * pool_render_host: a raw pool block (the brain's raw capture, recorded by
 * -brain-debug with -bd-pool-format raw) renders to the JSON the
 * brain's old get_pool_breakdown_json built on the same tick (the same JSON
 * value; see the key order note below).
 *
 * The fixtures in tests/fixtures/pool_render/ come from real games: the
 * equality run recorded the raw block and the old JSON of the same bot on
 * the same tick (the old JSON was built by the original breakdown code). The
 * raw blocks were first packed with string.buffer ("WBPR1:"); they were
 * converted to the brain_value_pack form ("WBPR2:") with the same capture tables.
 * The
 * render runs in pool_render_host's plain lua_State, the way BrainTest and
 * brainrec_split use it, with brains/GoalHunter as the brain directory.
 * Run from the repository root.
 */
#include "test_harness.h"

#include <string.h>

#include "pool_render_host.h"
#include "cJSON.h"

#define PR_FIXTURE_DIR "tests/fixtures/pool_render"
#define PR_BRAIN_DIR   "brains/GoalHunter"

static char *prReadFile(const char *path, size_t *lenOut) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (lenOut) *lenOut = got;
    return buf;
}

static const char *kFixtures[] = {
    "fixture_a",
    "fixture_b",
};

int run_pool_render_raw_matches_old_json(void) {
    int checked = 0;
    for (size_t i = 0; i < sizeof(kFixtures) / sizeof(kFixtures[0]); i++) {
        char rawPath[256], jsonPath[256];
        snprintf(rawPath, sizeof rawPath, "%s/%s.raw", PR_FIXTURE_DIR, kFixtures[i]);
        snprintf(jsonPath, sizeof jsonPath, "%s/%s.json", PR_FIXTURE_DIR, kFixtures[i]);
        char *raw = prReadFile(rawPath, NULL);
        char *want = prReadFile(jsonPath, NULL);
        if (!raw || !want) {
            free(raw);
            free(want);
            UT_FAIL("missing fixture %s (run from the repository root)", rawPath);
        }
        if (!poolRenderIsRaw(raw)) {
            free(raw);
            free(want);
            UT_FAIL("%s is not a raw pool block", rawPath);
        }
        char err[512];
        char *got = poolRenderToJson(PR_BRAIN_DIR, raw, err, sizeof err);
        /* Same JSON value, compared parsed: json.lua writes object keys in
         * pairs() order, which follows each Lua state's string hashes, so a
         * render in another Lua state may order the keys of an object
         * differently. In the brain's own Lua state the text is byte-equal
         * (the equality run checks that). Numbers, strings, arrays and
         * which keys each object holds must all match. */
        cJSON *jg = got ? cJSON_Parse(got) : NULL;
        cJSON *jw = cJSON_Parse(want);
        bool same = jg && jw && err[0] == '\0' && cJSON_Compare(jg, jw, true);
        if (!same) {
            fprintf(stderr, "  %s: err='%s' parsed got=%s want=%s\n",
                    kFixtures[i], err, jg ? "yes" : "no", jw ? "yes" : "no");
        }
        cJSON_Delete(jg);
        cJSON_Delete(jw);
        free(got);
        free(raw);
        free(want);
        UT_ASSERT_MSG(same, "render of %s differs from the old JSON", kFixtures[i]);
        checked++;
    }
    poolRenderHostShutdown();
    fprintf(stderr, "  %d raw pool blocks render to the old JSON\n", checked);
    return 0;
}

int run_pool_render_json_and_bad_blocks(void) {
    /* An old JSON block comes back unchanged. */
    const char *old = "{\"phase\":\"x\",\"sections\":[]}";
    char err[512];
    char *got = poolRenderToJson(PR_BRAIN_DIR, old, err, sizeof err);
    UT_ASSERT(got != NULL);
    UT_ASSERT_MSG(strcmp(got, old) == 0, "a JSON block changed: %s", got);
    UT_ASSERT(err[0] == '\0');
    free(got);
    UT_ASSERT(!poolRenderIsRaw(old));

    /* A broken raw block gives an error panel, not a crash or NULL. */
    got = poolRenderToJson(PR_BRAIN_DIR, POOL_RENDER_RAW_TAG "not a buffer", err, sizeof err);
    UT_ASSERT(got != NULL);
    UT_ASSERT_MSG(err[0] != '\0', "a broken block rendered without an error");
    UT_ASSERT_MSG(strstr(got, "Pool render error") != NULL, "no error panel: %s", got);
    free(got);

    /* A block of the first raw form (string.buffer, which the game no
     * longer has) gives an error panel that says so. */
    UT_ASSERT(poolRenderIsRaw(POOL_RENDER_RAW_TAG_V1 "\x07\x02"));
    got = poolRenderToJson(PR_BRAIN_DIR, POOL_RENDER_RAW_TAG_V1 "\x07\x02", err, sizeof err);
    UT_ASSERT(got != NULL);
    UT_ASSERT_MSG(strstr(err, "WBPR1") != NULL, "no WBPR1 error: '%s'", err);
    UT_ASSERT_MSG(strstr(got, "Pool render error") != NULL, "no error panel: %s", got);
    free(got);

    /* A brain dir with no pool_render.lua gives an error panel too. */
    got = poolRenderToJson("tests/fixtures/pool_render/no_such_brain",
                           POOL_RENDER_RAW_TAG "x", err, sizeof err);
    UT_ASSERT(got != NULL);
    UT_ASSERT(err[0] != '\0');
    free(got);
    poolRenderHostShutdown();
    return 0;
}
