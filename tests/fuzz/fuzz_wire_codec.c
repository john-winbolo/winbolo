/* Tier-1 fuzz target — the bounded wire-record parsers.
 *
 * Feeds attacker-controlled bytes straight into every generated `unpack*`
 * codec (the single bounds-checked path that Cluster A's codegen produced)
 * plus `commandCodecDecode`. Each of these takes an explicit `avail`/`len`
 * bound, so they are safe to call with an arbitrary buffer: AddressSanitizer
 * turns any read past `size` into a hard abort with the triggering bytes.
 *
 * This is the lowest-friction half of hardening plan §1.2 and doubles as the
 * architecture plan's A.5 differential guard — it exercises the generated
 * unpack output rather than the retired hand-rolled offset arithmetic.
 *
 * NOT exercised here: `unpackGameEvent`, which takes no `avail` and trusts its
 * caller to have guaranteed the bytes (the variable-length residue codegen
 * left hand-written). Its real over-read surface lives in the reliable-event
 * loop inside the snapshot handler, reached through the tier-2 dispatcher
 * target, not by calling the leaf in isolation.
 */
#include <stddef.h>
#include <stdint.h>

#include "input_packet.h"            /* snapshot structs */
#include "client_command.h"          /* ClientCommand */
#include "transport_udp_internal.h"  /* generated unpack* prototypes */
#include "transport_command_codec.h" /* commandCodecDecode */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    TankSnapshot        tank;
    ShellSnapshot       shell;
    TkExplosionSnapshot expl;
    BaseSnapshot        base;
    PillSnapshot        pill;
    MapDownloadChunkHeader dl;
    MapUploadChunkHeader   ul;
    MapPreviewChunkHeader  pv;
    ClientCommand          cmd;

    (void)unpackTankSnapshot(data, size, &tank);
    (void)unpackShellSnapshot(data, size, &shell);
    (void)unpackTkExplosionSnapshot(data, size, &expl);
    (void)unpackBaseSnapshot(data, size, &base);
    (void)unpackPillSnapshot(data, size, &pill);
    (void)unpackMapDownloadChunkHeader(data, size, &dl);
    (void)unpackMapUploadChunkHeader(data, size, &ul);
    (void)unpackMapPreviewChunkHeader(data, size, &pv);
    (void)commandCodecDecode(data, size, &cmd);

    return 0;
}
