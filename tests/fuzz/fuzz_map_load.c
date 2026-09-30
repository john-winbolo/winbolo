/* Fuzz target — the compressed-map loader.
 *
 * mapLoadCompressedMap is the parser for every map that arrives over the
 * network: the bulk transfer reassembles the download and installCompressedMap
 * hands it straight here, so every byte is chosen by whatever server the
 * player joined. One zlib stream inflates into the bases, pillboxes and
 * starts structs and then the 256x256 terrain array.
 *
 * The reason it earns a target: the struct region is copied wholesale, so the
 * per-field clamps the file-load setters apply never run on this route. The
 * loader now calls basesValidate/pillsValidate/startsValidate to close that,
 * and this target is what checks the clamps actually hold against arbitrary
 * input rather than only against the blob the unit test hands them.
 *
 * The input is the whole compressed blob. It is copied onto an exact-size
 * heap buffer first so ASan guards the tail: an over-read past inputLen is a
 * hard abort with the triggering bytes, which is the failure this is hunting.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "types.h"
#include "bolo_map.h"
#include "bases.h"
#include "pillbox.h"
#include "starts.h"

/* Reused across inputs: the loader overwrites them wholesale each call, and
 * creating a fresh set per input would spend the whole budget in malloc. */
static map       g_mp;
static pillboxes g_pb;
static bases     g_bs;
static starts    g_ss;

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    mapCreate(&g_mp);
    pillsCreate(&g_pb);
    basesCreate(&g_bs);
    startsCreate(&g_ss);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    BYTE *buf;

    /* mapLoadCompressedMap takes an int length; keep inputs well inside it.
     * A real map is a few KB — the download arm caps far below this. */
    if (size == 0 || size > (1u << 20)) {
        return 0;
    }
    buf = (BYTE *)malloc(size);   /* exact size -> ASan-guarded tail */
    if (buf == NULL) {
        return 0;
    }
    memcpy(buf, data, size);
    (void)mapLoadCompressedMap(&g_mp, &g_pb, &g_bs, &g_ss, buf, (int)size);
    free(buf);
    return 0;
}
