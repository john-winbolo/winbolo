#!/usr/bin/env python3
"""
Generate the bot-order-bids arenas (tests/scenario/order_*.map).

One field, written once per arena name, because the gate
runner pairs each <name>.scenario.lua with maps/<name>.map.

    x=96..156, y=96..156   grass.  Both axes have the integer midpoint 126,
                           so mapRead's recenter is a no-op and the squares
                           in this file are the squares in the game.
    x=129..135, y=99..105  a forest block.  The go-there test sends a bot to
                           its middle square, (132,102).
    pill #1 (154,98)       neutral and dead.  It is only here so the two pills
                           the arenas order are not brain pill 0 (scenario
                           pill n is brain pill n-1).
    pill #2 (106,106)      neutral, full: "attack 1" in the arenas.
    pill #3 (106,124)      neutral here; the arenas give it to the bots' team,
                           and "defend 2" sends a bot to it.
    base #1 (150,96)       the arenas give it to the bots' team: the refuel
                           stop the bid prices.
    starts                 four one-square deep-sea ponds (a start square has
                           to be deep sea).  The arenas teleport the tanks to
                           where each test needs them and fill the ponds.

The chained-handoff arena (order_handoff_chain) has its own pill row on the
same field, three pills in a line so each bot can stand next to one job:

    pill #1 (154,98)       neutral and dead, the same filler.
    pill #2 (100,110)      Z, "defend 1": the new order.
    pill #3 (114,110)      X, "defend 2": A's job, then B's.
    pill #4 (128,110)      Y, "defend 3": B's job, then C's.
    The arena gives #2..#4 to the bots' team.

The enemy-base field (order_capture_enemy_base, order_capture_live_pill,
order_warmup_start) is the first with a second base:

    base #2 (112,128)      the arena gives it to an enemy seat, full armour:
                           "capture base 1".

The human-decoy field (order_human_decoy) is the main field with a wall:

    x=109..110, y=104..105 a 2x2 block of buildings between pill #2
                           (106,106) and the stand-in human the arena parks
                           at (112,104).  A shell from the pill to him stops
                           in the wall, so decoy_getaway.block() says "wall".
                           The block is small so it does not move the bot's
                           own standoff choice: without the feature the bot
                           parks at (113,105), beside the human.

Usage:
    python tests/generate_order_bids_maps.py
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import generate_test_map as gtm  # noqa: E402
import generate_take_cover_map as gtc  # noqa: E402
from generate_test_map import GRASS, FOREST, DEEP_SEA, BUILDING, MAP_SIZE, write_bmap  # noqa: E402

# generate_test_map's encoder writes one run per row and cannot hold a
# deep-sea square inside it; take_cover's splits a row at each one.
gtm.encode_map_runs = gtc.encode_map_runs

ARENAS = [
    "order_handoff_relief",
    "order_handoff_busy",
    "order_goto_forest_exact",
    "order_bid_refuel_stop",
]

CHAIN_ARENA = "order_handoff_chain"

# The enemy-base field: base #2 is the target, base #1 the same filler the
# other arenas give the bots' team.  The live-pill capture arena plays on it
# too (it only needs pill #2).
BASE_ARENAS = [
    "order_capture_enemy_base",
    "order_capture_live_pill",
    "order_warmup_start",
]

DECOY_ARENA = "order_human_decoy"

PONDS = [(98, 98), (154, 154), (98, 154), (154, 126)]


def make_map():
    terrain = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    for y in range(96, 157):
        for x in range(96, 157):
            terrain[y][x] = GRASS
    for y in range(99, 106):
        for x in range(129, 136):
            terrain[y][x] = FOREST
    for x, y in PONDS:
        terrain[y][x] = DEEP_SEA
    return terrain


def main():
    terrain = make_map()
    # (x, y, owner, armour, speed)
    pills = [
        (154, 98, 0xFF, 0, 50),
        (106, 106, 0xFF, 15, 50),
        (106, 124, 0xFF, 15, 50),
    ]
    # (x, y, owner, armour, shells, mines)
    bases = [(150, 96, 0xFF, 90, 90, 90)]
    starts = [(x, y, 0) for x, y in PONDS]
    out = HERE / "scenario" / "maps"
    for name in ARENAS:
        write_bmap(str(out / (name + ".map")), terrain, pills, bases, starts)
    chain_pills = [
        (154, 98, 0xFF, 0, 50),
        (100, 110, 0xFF, 15, 50),
        (114, 110, 0xFF, 15, 50),
        (128, 110, 0xFF, 15, 50),
    ]
    write_bmap(str(out / (CHAIN_ARENA + ".map")), terrain, chain_pills, bases, starts)
    base_bases = bases + [(112, 128, 0xFF, 90, 90, 90)]
    for name in BASE_ARENAS:
        write_bmap(str(out / (name + ".map")), terrain, pills, base_bases, starts)
    decoy_terrain = make_map()
    for y in range(104, 106):
        for x in range(109, 111):
            decoy_terrain[y][x] = BUILDING
    write_bmap(str(out / (DECOY_ARENA + ".map")), decoy_terrain, pills, bases, starts)


if __name__ == "__main__":
    main()
