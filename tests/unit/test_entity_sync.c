/*
 * Which items are on the map, told to a client that has just taken a copy
 * of it (test_entity_sync.c).
 *
 * The compressed map carries every pillbox, base and start the map has and
 * not whether any of them has since been taken off it, so installing one
 * puts them all back on the map. That is right for a map as it loads and
 * wrong for a client that joins or resyncs after a removal, and
 * CTRL_ENTITY_SYNC is what corrects it: three 16-bit masks, one per list,
 * bit i standing for index i.
 *
 * What the cases pin:
 *   - codec_roundtrip: all on the map, all off it and a mixed mask survive
 *     the body encoder and decoder, the body is one fixed length, and a
 *     body of any other length is refused.
 *   - client_clears_the_holes: a client holding an all-live list ends up
 *     matching a mask with holes, with the counts untouched.
 *   - client_restores_the_live: a client already holding holes ends up all
 *     live on an all-live mask, with the counts and the records untouched.
 *   - mask_above_the_count_ignored: bits past the end of a list name no
 *     item, so they neither raise the count nor reach anything.
 *   - loopback_join: a pillbox removed on the server before a second client
 *     joins is off the map on that joiner too — the case the whole event
 *     exists for, driven over real sockets rather than asserted on the
 *     event the server sent.
 *   - wire_corpus_fixture: the committed golden bytes in
 *     tests/fixtures/wire/entity_sync.hex decode and re-encode unchanged,
 *     and each mask is read against the bytes on disk, so the byte order is
 *     pinned against something outside this file. A round trip on its own
 *     cannot catch a format change made to both halves at once.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "client_sim.h"
#include "client_net.h"    /* clientSimGetConnectState */
#include "client_sim_control.h"
#include "client_connect_state.h"
#include "game_sim.h"
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim.pb — the unittests profile */
#include "server_sim_scenario.h"
#include "test_harness.h"
#include "loopback_harness.h"

#ifndef WB_WIRE_FIXTURE_DIR
#define WB_WIRE_FIXTURE_DIR "tests/fixtures/wire"
#endif

/* [pills 2][bases 2][starts 2] — see transport_control_codec.c. */
#define ES_BODY_LEN 6
#define ES_FIXTURE_MAX 32

/* ── Codec ───────────────────────────────────────────────────────── */

/* Encode evt, decode the bytes back into out, and say whether both halves
 * agreed the body was the expected length. */
static bool esRoundTrip(const ControlEvent *evt, ControlEvent *out) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_ENTITY_SYNC);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_ENTITY_SYNC);
    uint8_t buf[MAX_CONTROL_PACKET];
    size_t outLen = 0;

    if (enc == NULL || dec == NULL) return false;
    if (enc(evt, NULL, buf, sizeof(buf), &outLen) != ENCODE_OK) return false;
    if (outLen != ES_BODY_LEN) return false;
    memset(out, 0, sizeof(*out));
    if (!dec(buf, outLen, out)) return false;
    return (out->type == CTRL_ENTITY_SYNC);
}

static void esMasks(ControlEvent *evt, uint16_t pills, uint16_t bases,
                    uint16_t starts) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_ENTITY_SYNC;
    evt->u.entitySync.pills  = pills;
    evt->u.entitySync.bases  = bases;
    evt->u.entitySync.starts = starts;
}

int run_entity_sync_codec_roundtrip(void) {
    ControlEvent in, out;

    /* Every index of every list on the map. */
    esMasks(&in, 0xFFFFu, 0xFFFFu, 0xFFFFu);
    UT_ASSERT(esRoundTrip(&in, &out));
    UT_ASSERT_MSG(out.u.entitySync.pills == 0xFFFFu,
                  "the pill mask became 0x%04X",
                  (unsigned)out.u.entitySync.pills);
    UT_ASSERT(out.u.entitySync.bases  == 0xFFFFu);
    UT_ASSERT(out.u.entitySync.starts == 0xFFFFu);

    /* Every index off it. */
    esMasks(&in, 0, 0, 0);
    UT_ASSERT(esRoundTrip(&in, &out));
    UT_ASSERT(out.u.entitySync.pills  == 0);
    UT_ASSERT(out.u.entitySync.bases  == 0);
    UT_ASSERT(out.u.entitySync.starts == 0);

    /* A different value in each mask, none of them a palindrome, so a
       swapped pair or a byte order that flipped shows up. */
    esMasks(&in, 0x8001u, 0x1234u, 0x00F0u);
    UT_ASSERT(esRoundTrip(&in, &out));
    UT_ASSERT_MSG(out.u.entitySync.pills == 0x8001u,
                  "the pill mask became 0x%04X",
                  (unsigned)out.u.entitySync.pills);
    UT_ASSERT_MSG(out.u.entitySync.bases == 0x1234u,
                  "the base mask became 0x%04X",
                  (unsigned)out.u.entitySync.bases);
    UT_ASSERT_MSG(out.u.entitySync.starts == 0x00F0u,
                  "the start mask became 0x%04X",
                  (unsigned)out.u.entitySync.starts);

    /* The body is one length, so anything else is refused rather than read
       out of a buffer that does not hold it. */
    {
        ControlDecodeBodyFn dec =
            transportControlCodecBodyDecoder(CTRL_ENTITY_SYNC);
        uint8_t buf[ES_BODY_LEN + 1];
        memset(buf, 0, sizeof(buf));
        UT_ASSERT(dec != NULL);
        UT_ASSERT_MSG(!dec(buf, ES_BODY_LEN - 1, &out),
                      "a short body decoded");
        UT_ASSERT_MSG(!dec(buf, ES_BODY_LEN + 1, &out),
                      "a long body decoded");
    }

    /* The encoder refuses a buffer the body does not fit rather than
       writing past it. */
    {
        ControlEncodeBodyFn enc =
            transportControlCodecBodyEncoder(CTRL_ENTITY_SYNC);
        uint8_t small[ES_BODY_LEN - 1];
        size_t outLen = 0;
        UT_ASSERT(enc != NULL);
        esMasks(&in, 0x0F0Fu, 0, 0);
        UT_ASSERT(enc(&in, NULL, small, sizeof(small), &outLen)
                  == ENCODE_OVERFLOW);
    }
    return 0;
}

/* ── Client apply ────────────────────────────────────────────────── */

/* A ClientSim with `count` pillboxes, bases and starts on its lists, each
 * on its own square, all on the map — the state a map install leaves. */
static ClientSim *esClientWithLists(BYTE count) {
    ClientSim *cs = clientSimAlloc();
    GameSim *gs;
    BYTE i;

    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    gs = clientSimGetGameSim(cs);

    pillsSetNumPills(&gs->pb, count);
    basesSetNumBases(&gs->bs, count);
    startsSetNumStarts(&gs->ss, count);
    for (i = 1; i <= count; i++) {
        pillbox pill;
        base bse;
        start st;

        memset(&pill, 0, sizeof(pill));
        pill.x = (BYTE)(40 + i);
        pill.y = 40;
        pill.owner = NEUTRAL;
        pill.armour = PILLS_MAX_ARMOUR;
        pill.speed = PILLBOX_ATTACK_NORMAL;
        pillsSetPill(gs, &gs->pb, &pill, i);

        memset(&bse, 0, sizeof(bse));
        bse.x = (BYTE)(60 + i);
        bse.y = 60;
        bse.owner = NEUTRAL;
        bse.armour = BASE_FULL_ARMOUR;
        bse.shells = BASE_FULL_SHELLS;
        bse.mines = BASE_FULL_MINES;
        basesSetBase(&gs->bs, &bse, i);

        memset(&st, 0, sizeof(st));
        st.x = (BYTE)(80 + i);
        st.y = 80;
        st.dir = 0;
        startsSetStart(&gs->ss, &st, i);
    }
    return cs;
}

/* Every index of every list against the bit that names it. */
static int esListsMatch(GameSim *gs, uint16_t pills, uint16_t bases,
                        uint16_t starts, const char *when) {
    BYTE i;
    for (i = 1; i <= pillsGetNumPills(&gs->pb); i++) {
        bool want = (pills & (1u << (i - 1))) != 0;
        UT_ASSERT_MSG(pillsIsActive(&gs->pb, i) == (want ? TRUE : FALSE),
                      "%s: pillbox %u is %s and the mask says %s",
                      when, (unsigned)i,
                      pillsIsActive(&gs->pb, i) ? "on the map" : "removed",
                      want ? "on the map" : "removed");
    }
    for (i = 1; i <= basesGetNumBases(&gs->bs); i++) {
        bool want = (bases & (1u << (i - 1))) != 0;
        UT_ASSERT_MSG(basesIsActive(&gs->bs, i) == (want ? TRUE : FALSE),
                      "%s: base %u is %s and the mask says %s",
                      when, (unsigned)i,
                      basesIsActive(&gs->bs, i) ? "on the map" : "removed",
                      want ? "on the map" : "removed");
    }
    for (i = 1; i <= startsGetNumStarts(&gs->ss); i++) {
        bool want = (starts & (1u << (i - 1))) != 0;
        UT_ASSERT_MSG(startsIsActive(&gs->ss, i) == (want ? TRUE : FALSE),
                      "%s: start %u is %s and the mask says %s",
                      when, (unsigned)i,
                      startsIsActive(&gs->ss, i) ? "on the map" : "removed",
                      want ? "on the map" : "removed");
    }
    return 0;
}

/* A client holding a freshly installed list — everything on the map — takes
 * a mask with holes in it and ends up naming the same items the mask does.
 * The counts do not move: the blob decided how long the lists are. */
int run_entity_sync_client_clears_the_holes(void) {
    ClientSim *cs = esClientWithLists(4);
    GameSim *gs;
    ControlEvent evt;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);
    UT_ASSERT(pillsGetNumPills(&gs->pb) == 4);
    UT_ASSERT(basesGetNumBases(&gs->bs) == 4);
    UT_ASSERT(startsGetNumStarts(&gs->ss) == 4);

    /* Pills: index 1 off. Bases: indices 0 and 3 off. Starts: all but 0
       off, so one list keeps a single item and another keeps three. */
    esMasks(&evt, 0x000Du, 0x0006u, 0x0001u);
    clientSimApplyControl(cs, &evt);

    if (esListsMatch(gs, 0x000Du, 0x0006u, 0x0001u, "after the mask") != 0) {
        clientSimDestroy(cs);
        return 1;
    }
    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 4,
                  "the pill count moved to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == 4,
                  "the base count moved to %u",
                  (unsigned)basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(startsGetNumStarts(&gs->ss) == 4,
                  "the start count moved to %u",
                  (unsigned)startsGetNumStarts(&gs->ss));

    clientSimDestroy(cs);
    return 0;
}

/* The other direction: a client already holding holes takes an all-live
 * mask and puts every index back on the map, with the record each slot
 * already held still in it. */
int run_entity_sync_client_restores_the_live(void) {
    ClientSim *cs = esClientWithLists(3);
    GameSim *gs;
    ControlEvent evt;
    pillbox pill;
    base bse;
    start st;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);

    /* Put the lists into the state a run of entity changes would leave. */
    UT_ASSERT(pillsRemoveItem(&gs->pb, 2) == TRUE);
    UT_ASSERT(basesRemoveItem(&gs->bs, 1) == TRUE);
    UT_ASSERT(basesRemoveItem(&gs->bs, 3) == TRUE);
    UT_ASSERT(startsRemoveItem(&gs->ss, 3) == TRUE);

    esMasks(&evt, 0x0007u, 0x0007u, 0x0007u);
    clientSimApplyControl(cs, &evt);

    if (esListsMatch(gs, 0x0007u, 0x0007u, 0x0007u, "after the mask") != 0) {
        clientSimDestroy(cs);
        return 1;
    }
    UT_ASSERT(pillsGetNumPills(&gs->pb) == 3);
    UT_ASSERT(basesGetNumBases(&gs->bs) == 3);
    UT_ASSERT(startsGetNumStarts(&gs->ss) == 3);

    /* The records are the ones the slots held before the removals: the mask
       moves the flag and nothing else. */
    memset(&pill, 0, sizeof(pill));
    pillsGetPill(&gs->pb, &pill, 2);
    UT_ASSERT_MSG(pill.x == 42 && pill.y == 40,
                  "pillbox 2 came back at %u,%u",
                  (unsigned)pill.x, (unsigned)pill.y);
    memset(&bse, 0, sizeof(bse));
    basesGetBase(&gs->bs, &bse, 1);
    UT_ASSERT_MSG(bse.x == 61 && bse.y == 60,
                  "base 1 came back at %u,%u",
                  (unsigned)bse.x, (unsigned)bse.y);
    memset(&st, 0, sizeof(st));
    startsGetStartStruct(&gs->ss, &st, 3);
    UT_ASSERT_MSG(st.x == 83 && st.y == 80,
                  "start 3 came back at %u,%u",
                  (unsigned)st.x, (unsigned)st.y);

    clientSimDestroy(cs);
    return 0;
}

/* A bit past the end of a list names no item. It must not extend the list:
 * how long the lists are is the blob's to say, and a mask only ever says
 * which of the slots the blob filled still hold an item. */
int run_entity_sync_mask_above_the_count_ignored(void) {
    ClientSim *cs = esClientWithLists(2);
    GameSim *gs;
    ControlEvent evt;

    UT_ASSERT(cs != NULL);
    gs = clientSimGetGameSim(cs);

    /* Every bit of every mask set, against lists two items long. */
    esMasks(&evt, 0xFFFFu, 0xFFFFu, 0xFFFFu);
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) == 2,
                  "the pill count rose to %u",
                  (unsigned)pillsGetNumPills(&gs->pb));
    UT_ASSERT_MSG(basesGetNumBases(&gs->bs) == 2,
                  "the base count rose to %u",
                  (unsigned)basesGetNumBases(&gs->bs));
    UT_ASSERT_MSG(startsGetNumStarts(&gs->ss) == 2,
                  "the start count rose to %u",
                  (unsigned)startsGetNumStarts(&gs->ss));
    UT_ASSERT(pillsIsActive(&gs->pb, 1) == TRUE);
    UT_ASSERT(pillsIsActive(&gs->pb, 2) == TRUE);
    /* Out of range either way, so the reader says removed. */
    UT_ASSERT(pillsIsActive(&gs->pb, 3) == FALSE);
    UT_ASSERT(basesIsActive(&gs->bs, 3) == FALSE);
    UT_ASSERT(startsIsActive(&gs->ss, 3) == FALSE);

    clientSimDestroy(cs);
    return 0;
}

/* ── Loopback ────────────────────────────────────────────────────── */

#define ES_CONNECT_MAX 2000   /* join + map download                   */
#define ES_SETTLE_MAX   200   /* a few running ticks after the download */
#define ES_SYNC_MAX     600   /* the mask crossing to the joiner        */

/* The pillbox the case takes off the map before the second client joins. */
#define ES_PILL_INDEX 5

static bool es_pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool es_pred_joiner_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return h->cs2 != NULL &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

/* True once the joiner agrees the pillbox is off the map. */
static bool es_pred_joiner_sees_removal(LoopbackHarness *h, void *user) {
    GameSim *gs = h->cs2 != NULL ? clientSimGetGameSim(h->cs2) : NULL;
    (void)user;
    return gs != NULL && gs->pb != NULL &&
           pillsIsActive(&gs->pb, ES_PILL_INDEX + 1) == FALSE;
}

/* Both lists name the same pillboxes, on the map and off it. */
static int esJoinerAgrees(ServerSim *sim, ClientSim *cs) {
    GameSim *sv = &sim->sim;
    GameSim *cl = clientSimGetGameSim(cs);
    BYTE n;
    BYTE i;

    UT_ASSERT(cl != NULL && cl->pb != NULL);
    n = pillsGetNumPills(&sv->pb);
    UT_ASSERT_MSG(pillsGetNumPills(&cl->pb) == n,
                  "the joiner holds %u pillboxes and the server %u",
                  (unsigned)pillsGetNumPills(&cl->pb), (unsigned)n);
    for (i = 1; i <= n; i++) {
        UT_ASSERT_MSG(pillsIsActive(&cl->pb, i) == pillsIsActive(&sv->pb, i),
                      "pillbox %u is %s on the server and %s on the joiner",
                      (unsigned)i,
                      pillsIsActive(&sv->pb, i) ? "on the map" : "removed",
                      pillsIsActive(&cl->pb, i) ? "on the map" : "removed");
    }
    return 0;
}

/* A pillbox taken off the map, and only then a second client joining. The
 * joiner's whole picture of the map is the compressed blob, which says
 * nothing about a removal, so without the mask it shows a pillbox that is
 * not there. */
int run_entity_sync_loopback_join(void) {
    LoopbackHarness h;
    ScenarioOp op;
    BYTE serverCount;
    int at;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "EntityFirst", false, NULL, 90211),
                  "loopback start failed");

    at = loopbackHarnessPumpUntil(&h, ES_CONNECT_MAX, es_pred_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the first client never connected");
    loopbackHarnessPumpUntil(&h, ES_SETTLE_MAX, NULL, NULL);

    serverCount = pillsGetNumPills(&h.sim->sim.pb);
    UT_ASSERT_MSG(serverCount > ES_PILL_INDEX,
                  "the map carries %u pillboxes", (unsigned)serverCount);

    /* Take it off the map before anyone else is here to be told about it. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_ENTITY_REMOVE_PILL;
    op.u.entityRemovePill.pill = ES_PILL_INDEX;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(h.sim, &op, NULL) == SCN_OP_OK,
                  "the removal was refused on the server");
    loopbackHarnessPumpUntil(&h, ES_SETTLE_MAX, NULL, NULL);
    UT_ASSERT(pillsIsActive(&h.sim->sim.pb, ES_PILL_INDEX + 1) == FALSE);

    UT_ASSERT_MSG(loopbackHarnessAddClient(&h, "EntityJoiner"),
                  "the second client failed to connect");
    at = loopbackHarnessPumpUntil(&h, ES_CONNECT_MAX,
                                  es_pred_joiner_connected, NULL);
    UT_ASSERT_MSG(at > 0, "the joiner never connected");

    at = loopbackHarnessPumpUntil(&h, ES_SYNC_MAX,
                                  es_pred_joiner_sees_removal, NULL);
    UT_ASSERT_MSG(at > 0,
                  "the joiner still has pillbox %u on the map after %d pumps",
                  (unsigned)(ES_PILL_INDEX + 1), ES_SYNC_MAX);

    if (esJoinerAgrees(h.sim, h.cs2) != 0) {
        loopbackHarnessStop(&h);
        return 1;
    }

    /* The client that was already here has not been disturbed: it heard the
       removal as a change, and the mask the joiner got is unicast. */
    if (esJoinerAgrees(h.sim, h.cs) != 0) {
        loopbackHarnessStop(&h);
        return 1;
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ── Wire corpus ─────────────────────────────────────────────────── */

/* Parse one hex char; -1 if not a hex digit. */
static int esHexVal(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one lowercase-hex line into out. Returns the byte count, or -1 on a
 * malformed line. */
static int esDecodeHexLine(const char *line, uint8_t *out, int outCap) {
    int n = 0;
    while (line[0] && line[0] != '\n' && line[0] != '\r') {
        int hi, lo;
        if (n >= outCap) return -1;
        hi = esHexVal((unsigned char)line[0]);
        lo = esHexVal((unsigned char)line[1]);
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        line += 2;
    }
    return n;
}

/* One mask composed out of the file's own bytes, high byte first. Spelled
 * out here rather than taken from unpackU16 so the fixture is checked
 * against the layout it pins and not against the same helper the encoder
 * used to write it. */
static uint16_t esMaskFromBytes(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* The committed golden bytes: every vector decodes, re-encodes to exactly
 * the bytes on disk, and each of its three masks is read against those
 * bytes. The byte check matters on its own — an encoder and a decoder that
 * both swapped to little-endian would still round-trip against each other,
 * and only the high-bit vector tells the two apart.
 *
 * The fixture must carry four shapes: every mask zero, every index on the
 * map, holes in a different place per list, and a mask with the top bit
 * set. */
int run_entity_sync_wire_corpus_fixture(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_ENTITY_SYNC);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_ENTITY_SYNC);
    uint8_t vecs[ES_FIXTURE_MAX][ES_BODY_LEN];
    int lens[ES_FIXTURE_MAX];
    char path[512];
    char line[256];
    FILE *f;
    int n = 0;
    int k;
    int seenZero = 0, seenFull = 0, seenMixed = 0, seenHighBit = 0;

    UT_ASSERT(enc != NULL);
    UT_ASSERT(dec != NULL);

    snprintf(path, sizeof(path), "%s/entity_sync.hex", WB_WIRE_FIXTURE_DIR);
    f = fopen(path, "r");
    UT_ASSERT_MSG(f != NULL, "no fixture at %s", path);
    while (n < ES_FIXTURE_MAX && fgets(line, sizeof(line), f)) {
        int len;
        if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
        len = esDecodeHexLine(line, vecs[n], ES_BODY_LEN);
        if (len <= 0) {
            fclose(f);
            UT_ASSERT_MSG(0, "malformed hex on line %d of %s", n + 1, path);
        }
        lens[n] = len;
        n++;
    }
    fclose(f);
    UT_ASSERT_MSG(n > 0, "%s holds no vectors", path);

    for (k = 0; k < n; k++) {
        ControlEvent evt;
        uint8_t gen[MAX_CONTROL_PACKET];
        size_t outLen = 0;
        uint16_t pills, bases, starts;

        UT_ASSERT_MSG(lens[k] == ES_BODY_LEN,
                      "fixture vector %d is %d bytes", k, lens[k]);
        memset(&evt, 0, sizeof(evt));
        UT_ASSERT_MSG(dec(vecs[k], (size_t)lens[k], &evt),
                      "fixture vector %d did not decode", k);
        memset(gen, 0, sizeof(gen));
        UT_ASSERT_MSG(enc(&evt, NULL, gen, sizeof(gen), &outLen) == ENCODE_OK,
                      "fixture vector %d did not re-encode", k);
        UT_ASSERT_MSG(outLen == (size_t)lens[k],
                      "fixture vector %d re-encoded to %u bytes",
                      k, (unsigned)outLen);
        UT_ASSERT_MSG(memcmp(gen, vecs[k], (size_t)lens[k]) == 0,
                      "fixture vector %d re-encoded to different bytes", k);

        /* Field by field against the file: the three masks in order, each
           high byte first. */
        pills  = esMaskFromBytes(vecs[k]);
        bases  = esMaskFromBytes(vecs[k] + 2);
        starts = esMaskFromBytes(vecs[k] + 4);
        UT_ASSERT_MSG(evt.u.entitySync.pills == pills,
                      "vector %d: the pill mask decoded 0x%04X and bytes "
                      "%02X %02X read 0x%04X", k,
                      (unsigned)evt.u.entitySync.pills,
                      (unsigned)vecs[k][0], (unsigned)vecs[k][1],
                      (unsigned)pills);
        UT_ASSERT_MSG(evt.u.entitySync.bases == bases,
                      "vector %d: the base mask decoded 0x%04X and bytes "
                      "%02X %02X read 0x%04X", k,
                      (unsigned)evt.u.entitySync.bases,
                      (unsigned)vecs[k][2], (unsigned)vecs[k][3],
                      (unsigned)bases);
        UT_ASSERT_MSG(evt.u.entitySync.starts == starts,
                      "vector %d: the start mask decoded 0x%04X and bytes "
                      "%02X %02X read 0x%04X", k,
                      (unsigned)evt.u.entitySync.starts,
                      (unsigned)vecs[k][4], (unsigned)vecs[k][5],
                      (unsigned)starts);

        if (pills == 0 && bases == 0 && starts == 0) {
            seenZero = 1;
        }
        if (pills == 0xFFFFu && bases == 0xFFFFu && starts == 0xFFFFu) {
            seenFull = 1;
        }
        /* Three masks that are neither empty nor full and all differ: the
           holes fall somewhere different in each list. */
        if (pills != 0 && bases != 0 && starts != 0 &&
            pills != 0xFFFFu && bases != 0xFFFFu && starts != 0xFFFFu &&
            pills != bases && bases != starts && pills != starts) {
            seenMixed = 1;
        }
        /* Bit 15 set in a mask that is not simply all ones — the one shape
           that reads differently at each end of the field. */
        if ((pills  & 0x8000u) != 0 && pills  != 0xFFFFu) seenHighBit = 1;
        if ((bases  & 0x8000u) != 0 && bases  != 0xFFFFu) seenHighBit = 1;
        if ((starts & 0x8000u) != 0 && starts != 0xFFFFu) seenHighBit = 1;
    }

    UT_ASSERT_MSG(seenZero,
                  "%s carries no vector with every index off the map", path);
    UT_ASSERT_MSG(seenFull,
                  "%s carries no vector with every index on the map", path);
    UT_ASSERT_MSG(seenMixed,
                  "%s carries no vector whose three lists differ", path);
    UT_ASSERT_MSG(seenHighBit,
                  "%s carries no vector with a mask's top bit set, so the "
                  "byte order is not pinned", path);
    return 0;
}
