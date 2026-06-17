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
 * `unpackGameEvent` is the variable-length residue codegen left hand-written;
 * it now takes an `avail` bound (a truncated trailing event used to over-read
 * the datagram — caught by fuzz_client_snapshot, fixed at the leaf), so it is
 * fuzzed here directly alongside the generated codecs.
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
    ClientCommand          cmd;
    GameEvent              ev;

    (void)unpackTankSnapshot(data, size, &tank);
    (void)unpackShellSnapshot(data, size, &shell);
    (void)unpackTkExplosionSnapshot(data, size, &expl);
    (void)unpackBaseSnapshot(data, size, &base);
    (void)unpackPillSnapshot(data, size, &pill);
    (void)unpackGameEvent(data, size, &ev);
    (void)commandCodecDecode(data, size, &cmd);

    return 0;
}
