/*
 * Differential coverage for the generated flat-leaf-snapshot codecs.
 *
 * For each flat leaf message (Shell / TkExplosion / Base / Pill) this
 * instantiates a generated codec under a *_gen name from the same field list
 * the worked example uses, then checks it against the hand-rolled packer in
 * transport_udp_common.c on two sources of vectors:
 *
 *   - committed golden fixtures in tests/fixtures/wire/<label>.hex (captured
 *     from a real loopback session by test_wire_corpus_capture), and
 *   - programmatic boundary vectors (every field 0, then every field at its
 *     type maximum), so coverage does not depend on which messages a session
 *     happened to emit.
 *
 * Per vector it asserts: (a) the fixture round-trips through the hand-rolled
 * codec unchanged; (b) the generated pack reproduces the bytes exactly; (c)
 * hand-rolled and generated unpack yield struct-identical results; and once
 * per message (d) WIRE_SIZE_OF(fields) equals the message's *_WIRE_SIZE.
 *
 * No sockets and no file writes — it only reads committed fixtures.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_packet.h"            /* snapshot structs + *_WIRE_SIZE */
#include "transport_udp_internal.h"  /* hand-rolled pack*/unpack* */
#include "wire_codec.h"              /* DEFINE_WIRE_CODEC_NAMED, WIRE_SIZE_OF */
#include "test_harness.h"

#ifndef WB_WIRE_FIXTURE_DIR
#define WB_WIRE_FIXTURE_DIR "tests/fixtures/wire"
#endif

#define WC_BYTES        64
#define WC_MAX_VECTORS  64

/* Field lists — order and types must match the hand-rolled packers exactly. */
#define SHELL_SNAPSHOT_FIELDS(F) \
    F(U16, worldX) F(U16, worldY) F(U8, angle) F(U8, owner) F(U8, length)
#define TK_EXPLOSION_SNAPSHOT_FIELDS(F) \
    F(U16, worldX) F(U16, worldY) F(U8, angle) F(U8, length) \
    F(U8, explodeType) F(U8, creator)
#define BASE_SNAPSHOT_FIELDS(F) \
    F(U8, owner) F(U8, armour) F(U8, shells) F(U8, mines)
#define PILL_SNAPSHOT_FIELDS(F) \
    F(U8, x) F(U8, y) F(U8, owner) F(U8, armour) F(U8, speed) F(U8, inTank)

DEFINE_WIRE_CODEC_NAMED(ShellSnapshot_gen, ShellSnapshot, SHELL_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC_NAMED(TkExplosionSnapshot_gen, TkExplosionSnapshot,
                        TK_EXPLOSION_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC_NAMED(BaseSnapshot_gen, BaseSnapshot, BASE_SNAPSHOT_FIELDS)
DEFINE_WIRE_CODEC_NAMED(PillSnapshot_gen, PillSnapshot, PILL_SNAPSHOT_FIELDS)

/* Set each field to its type maximum, applied to a field list. */
#define WIRE_MAX_U8  0xFFu
#define WIRE_MAX_U16 0xFFFFu
#define WIRE_MAX_U32 0xFFFFFFFFu
#define FIELD_SET_MAX(type, name) s.name = (WIRE_MAX_##type);

/* Parse one hex char; -1 if not a hex digit. */
static int hexVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one lowercase-hex line into out (<= WC_BYTES). Returns byte count, or
 * -1 on a malformed line. */
static int decodeHexLine(const char *line, uint8_t *out) {
    int n = 0;
    while (line[0] && line[0] != '\n' && line[0] != '\r') {
        int hi, lo;
        if (n >= WC_BYTES) return -1;
        hi = hexVal((unsigned char)line[0]);
        lo = hexVal((unsigned char)line[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        line += 2;
    }
    return n;
}

/* Load tests/fixtures/wire/<label>.hex into vecs/lens. Returns the number of
 * vectors, or 0 if the file is absent (a missing fixture is not a failure;
 * boundary vectors still run). */
static int loadFixture(const char *label, uint8_t vecs[][WC_BYTES],
                       size_t *lens, int maxVecs) {
    char path[512];
    char line[256];
    FILE *f;
    int n = 0;
    snprintf(path, sizeof(path), "%s/%s.hex", WB_WIRE_FIXTURE_DIR, label);
    f = fopen(path, "r");
    if (!f) return 0;
    while (n < maxVecs && fgets(line, sizeof(line), f)) {
        int len;
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
        len = decodeHexLine(line, vecs[n]);
        if (len <= 0) continue;
        lens[n] = (size_t)len;
        n++;
    }
    fclose(f);
    return n;
}

/* Emit a per-message check function. HandPack/HandUnpack are the hand-rolled
 * symbols; pack##Gen / unpack##Gen are the generated pair. */
#define MSG_CHECK_FN(FnName, Struct, HandPack, HandUnpack, Gen, LABEL, WSIZE, \
                     FIELDS)                                                  \
static int FnName(void) {                                                     \
    uint8_t vecs[WC_MAX_VECTORS][WC_BYTES];                                   \
    size_t  lens[WC_MAX_VECTORS];                                             \
    int n, k;                                                                 \
    UT_ASSERT_MSG(WIRE_SIZE_OF(FIELDS) == (WSIZE),                            \
                  LABEL " (d) WIRE_SIZE_OF mismatch");                        \
    n = loadFixture(LABEL, vecs, lens, WC_MAX_VECTORS);                       \
    for (k = 0; k < n; k++) {                                                 \
        Struct sh, sg;                                                        \
        uint8_t rt[WC_BYTES], gen[WC_BYTES];                                  \
        UT_ASSERT_MSG(lens[k] == (WSIZE), LABEL " fixture wrong length");     \
        memset(&sh, 0, sizeof(sh));                                           \
        HandUnpack(vecs[k], &sh);                                             \
        memset(rt, 0, sizeof(rt));                                            \
        HandPack(rt, &sh);                                                    \
        UT_ASSERT_MSG(memcmp(rt, vecs[k], (WSIZE)) == 0,                      \
                      LABEL " (a) fixture not self-consistent");              \
        memset(gen, 0, sizeof(gen));                                          \
        pack##Gen(gen, &sh);                                                  \
        UT_ASSERT_MSG(memcmp(gen, vecs[k], (WSIZE)) == 0,                     \
                      LABEL " (b) generated pack != golden");                 \
        memset(&sg, 0, sizeof(sg));                                           \
        UT_ASSERT_MSG(unpack##Gen(vecs[k], lens[k], &sg) == (int)(WSIZE),     \
                      LABEL " (c) generated unpack short read");              \
        UT_ASSERT_MSG(memcmp(&sh, &sg, sizeof(sh)) == 0,                      \
                      LABEL " (c) hand vs generated struct differ");          \
    }                                                                         \
    {                                                                         \
        Struct s, su, sgu;                                                    \
        uint8_t h[WC_BYTES], g[WC_BYTES];                                     \
        int pass;                                                             \
        for (pass = 0; pass < 2; pass++) {                                    \
            memset(&s, 0, sizeof(s));                                         \
            if (pass == 1) { FIELDS(FIELD_SET_MAX) }                          \
            memset(h, 0, sizeof(h));                                          \
            memset(g, 0, sizeof(g));                                          \
            HandPack(h, &s);                                                  \
            pack##Gen(g, &s);                                                 \
            UT_ASSERT_MSG(memcmp(h, g, (WSIZE)) == 0,                         \
                          LABEL " (b) boundary generated pack != hand");      \
            memset(&su, 0, sizeof(su));                                       \
            memset(&sgu, 0, sizeof(sgu));                                     \
            HandUnpack(h, &su);                                               \
            unpack##Gen(h, (WSIZE), &sgu);                                    \
            UT_ASSERT_MSG(memcmp(&su, &sgu, sizeof(su)) == 0,                 \
                          LABEL " (c) boundary struct differ");              \
        }                                                                     \
    }                                                                         \
    return 0;                                                                 \
}

MSG_CHECK_FN(check_shell, ShellSnapshot, packShellSnapshot, unpackShellSnapshot,
             ShellSnapshot_gen, "shell_snapshot", SHELL_SNAPSHOT_WIRE_SIZE,
             SHELL_SNAPSHOT_FIELDS)
MSG_CHECK_FN(check_tk, TkExplosionSnapshot, packTkExplosionSnapshot,
             unpackTkExplosionSnapshot, TkExplosionSnapshot_gen,
             "tk_explosion_snapshot", TK_EXPLOSION_SNAPSHOT_WIRE_SIZE,
             TK_EXPLOSION_SNAPSHOT_FIELDS)
MSG_CHECK_FN(check_base, BaseSnapshot, packBaseSnapshot, unpackBaseSnapshot,
             BaseSnapshot_gen, "base_snapshot", BASE_SNAPSHOT_WIRE_SIZE,
             BASE_SNAPSHOT_FIELDS)
MSG_CHECK_FN(check_pill, PillSnapshot, packPillSnapshot, unpackPillSnapshot,
             PillSnapshot_gen, "pill_snapshot", PILL_SNAPSHOT_WIRE_SIZE,
             PILL_SNAPSHOT_FIELDS)

int run_wire_corpus(void) {
    if (check_shell() != 0) return 1;
    if (check_tk() != 0) return 1;
    if (check_base() != 0) return 1;
    if (check_pill() != 0) return 1;
    return 0;
}
