# Making a WinBolo skin

A skin replaces some or all of the game's artwork and sounds. You can replace
one tile or every one of them — anything you leave out falls back to the
built-in asset, so a skin that only recolours grass is a perfectly good skin.

## Where skins live

Put skins in the `skins` folder under WinBolo's preferences directory:

| Platform | Path |
|---|---|
| Windows | `%APPDATA%\WinBolo\WinBolo\skins\` |
| macOS | `~/Library/Application Support/WinBolo/WinBolo/skins/` |
| Linux | `~/.local/share/WinBolo/WinBolo/skins/` |

The **Open skins folder** button in Settings → Display & Sound → Skin opens it
and creates it the first time.

Three other locations are also searched, in this order after the one above:
Steam Workshop installs, `data/skins/` beside the executable (for skins shipped
with the game), and `skins/` beside the executable (where WinBolo 1.x users kept
`.wsf` files). The preferences folder is the only location writable on every
platform — the others sit inside the application, which is read-only on macOS,
Steam and mobile installs.

The picker tags each skin with where it came from, and a skin you install is a
separate entry from a shipped one of the same name: both are listed. Within the
two user locations, the preferences folder wins over `skins/` beside the
executable for the same name, and within one folder a `mytheme/` directory wins
over a `mytheme.wsf` beside it.

## A skin is a folder or a zip

Both of these work, and the game reads them identically:

```
skins/mytheme/              skins/mytheme.wsf   (a zip; .zip works too)
    skin.ini                    skin.ini
    grass.png                   grass.png
    forest.png                  forest.png
    sounds/                     sounds/
        bubbles.wav                 bubbles.wav
```

If everything inside the zip sits under a single top-level folder, that folder is
stripped — so zipping the `mytheme` directory itself gives a working `.wsf`
without repacking.

Only the top level and the `sounds/` folder are read. Files in any other
subfolder are ignored, so `sprites/grass.png` never matches `grass.png`.

## The smallest possible skin

```
skins/swampy/
    skin.ini
    grass.png
```

`skin.ini`:

```ini
[Skin]
Name=Swampy
Author=Your Name
Notes=Grass reads as swamp. That's the whole skin.
```

Pick it in Settings → Display & Sound → Skin. It applies immediately.

## skin.ini

All keys are optional. Without the file the skin is listed under its folder or
file name.

```ini
[Skin]
Name=Display name in the skin picker
Author=Your name
Notes=Free text, shown under the picker
MaxPixelDensity=0
InGameRotate=0
RecommendedFilter=
WorkshopId=
WorkshopAuthor=
```

A second section, `[MapPalette]`, is described under
[The map palette](#the-map-palette). Any other section is ignored, so a skin
may carry its own without the game reading it.

- **`MaxPixelDensity`** — the finest multiple your SVG art is meant to be drawn
  at. `0` or absent means unlimited, which is right for genuine vector art. Set
  it to `1` if your SVGs are pixel-art rectangle grids that should stay chunky.
  It only caps SVGs; `@Nx` PNG files are never capped, because shipping the file
  is the statement.
- **`InGameRotate`** — see [Rotating skins](#rotating-skins).
- **`RecommendedFilter`** — which Texture filter setting suits your art:
  `nearest`, `linear` or `pixelart` (`pixel art` and `pixel-art` are accepted
  too). The game only shows it. It appears under
  your notes in the skin picker, and the entry you named is marked
  "(recommended)" in the Texture filter dropdown. Nothing is applied or
  preselected: the player's own Texture filter setting always wins, whatever
  you put here. Leave it out if you have no preference.
- **`WorkshopId`** — the Workshop item this skin was published as. Written by
  the game when you publish to Steam Workshop. Do not set it by hand.
- **`WorkshopAuthor`** — the SteamID of whoever published that item. Written by
  the game alongside `WorkshopId`. When a skin is passed on, it lets the game
  see that a later publish is from a different account and has to make its own
  item. Do not set it by hand.

## What you can replace

### Individual sprites

Drop in `<name>.png` or `<name>.svg` using the sprite names below. PNGs use their
own alpha channel for transparency. Draw each sprite at its native size — see
[Sprite names and sizes](#sprite-names-and-sizes).

### A whole sheet

Instead of individual files you can supply the entire sprite sheet as one image,
the way WinBolo 1.x skins did. Name it `tiles.bmp`, `skin.bmp` or `skin32.bmp` —
all three are accepted, tried in that order.

The sheet must be an exact multiple of **496 × 176** on both axes: 496×176 for
1×, 992×352 for 2×, 1488×528 for 3×, up to 8×. A sheet at any other size is used
as if it were 1× and a warning goes in the log. Transparency in a sheet is the
green colour key `#00FF00`, not alpha.

The key carries a small tolerance: any pixel within 8 of `#00FF00` on every
channel counts as transparent, so an anti-aliased sheet does not keep a green
fringe around every sprite. Green you want to keep should sit further from pure
green than that.

The ten status icon slots under [Interface art](#interface-art) are the
exception — the key is never applied to them. Both the built-in sheet and 1.x
skins draw those icons *in* green on a black background, so green there is
treated as artwork and drawn as you left it. You therefore cannot use green as
a see-through background inside a status slot: draw those icons against the
black they sit on.

Individual files win over the sheet, so you can ship a sheet and override a few
sprites with separate PNGs.

### The background

`background.bmp`, or `screen.bmp` (the 1.x name). This is the panel artwork
around the play area. The stock one is 515 × 325 and it is drawn at that size
times the zoom level. A larger image is scaled to fit without smoothing, so it
only looks sharper at zoom 2 and above; at zoom 1 it is point-sampled down. The
Texture filter setting does not apply to the background.

### The map palette

Once a map is drawn too small for its sprites, the game stops drawing them: each
square becomes one flat colour, and tanks, pillboxes and bases become marker
shapes — a triangle pointing the way a tank faces, a disc for a pillbox, a square
for a base. That happens in the Map Overview and the full screen map below 1x
(the player can turn it off under Settings → Display → Map view), and always in
the map choosers, which draw a whole map in a thumbnail.

Your art is not involved at those sizes, so a skin sets the colours instead, in
a `[MapPalette]` section of `skin.ini`:

```ini
[MapPalette]
; the ground, one key per terrain
Grass=#002806
Swamp=#003933
Rubble=#303819
Crater=#292911
Forest=#045311
Road=#000000
River=#008c9c
DeepSea=#008a9e
Boat=#61848b
Building=#785e41
HalfBuilding=#56422c

; the shapes standing on it, one colour per side
MarkerGood=#58d858
MarkerEvil=#ff5d5d
MarkerNeutral=#f0b429
```

Those are the built-in values, so that block changes nothing — copy it and edit
the lines you care about.

- Every key is optional and stands on its own. Name three and you get those
  three with the built-in rest; leave the section out and you get the built-in
  set.
- `#rrggbb`, `rrggbb` and `0xrrggbb` are all accepted, upper or lower case. Key
  names are matched without regard to case.
- Exactly six hex digits. Anything else — three digits, eight, a colour name,
  an empty value — leaves that one key at its built-in colour and does not
  disturb the others. Nothing is reported, so check your spelling.
- `#000000` is black, not "unset". Setting a key to black is a thing you can
  do.
- One colour covers a whole terrain family, however the square is shaped: a
  river bend and a river straight are both `River`.
- `MarkerGood` and `MarkerEvil` colour a tank, a pillbox and a base alike, so a
  friendly tank parked on a friendly base is one colour. `MarkerNeutral` is
  bases only — a pillbox nobody owns takes `MarkerEvil`, because to everyone
  who can see it that is what it is.
- A pillbox draws the same whatever its health, and a square carrying a mine
  draws as the ground under it.

These colours are the map at a glance rather than the map in detail, so the
built-in set keeps the ground dark and low contrast and lets the markers carry
the eye. A set with loud ground will read as busy once pings, fog and markers
are drawn over it.

### Sounds

`sounds/<name>.wav`, or `<name>.wav` at the top level (the 1.x layout). Both are
searched, `sounds/` first.

A sound can have up to eleven files: `<name>_0.wav` through `<name>_9.wav`
beside `<name>.wav`, one of which is picked each time the sound plays. Gaps in
the numbering are fine. Whichever of those names your skin holds is the whole
pool for that sound, so the game's own file is not mixed in with yours, and a
file that will not decode is skipped while the rest of the pool still plays.
See [Sound names](#sound-names).

The six smart-ping sounds are the one exception to a sound needing a file: a
ping kind with no file of its own plays `ping_default.wav`, so a skin can
replace all six with one file or just one of them. See
[Ping sounds](#ping-sounds).

## Sprite names and sizes

There are **307** sprites. All are **16 × 16** except the 33 listed here:

| Sprite | Size |
|---|---|
| `indent_on`, `indent_off` | 54 × 54 |
| `indent_dot_on`, `indent_dot_off` | 6 × 6 |
| `status_*` (10 sprites) | 12 × 12 |
| `lgm_frame0`, `lgm_frame1`, `lgm_frame2` | 3 × 4 |
| `shell_00`, `_01`, `_07`, `_08`, `_09`, `_10`, `_15` | 3 × 4 |
| `shell_03`, `_04`, `_05`, `_11`, `_12`, `_13` | 4 × 3 |
| `shell_02`, `_06`, `_14` | 4 × 4 |

An image of the wrong size is scaled to fit without smoothing, and nothing is
logged about it, so check your sizes against this table yourself.

### Terrain

`grass`, `swamp`, `rubble`, `crater`, `crater_single`, `mine`, `shot_building`

`forest`, `forest_above`, `forest_aboveleft`, `forest_aboveright`,
`forest_below`, `forest_bottomleft`, `forest_bottomright`, `forest_left`,
`forest_right`, `forest_single`

`deep_sea`, `deep_sea_corner1`–`4`, `deep_sea_side1`–`4`

`river_corner1`–`4`, `river_end1`–`4`, `river_oneside1`–`4`, `river_side1`–`2`,
`river_solid`, `river_surround`

`road_corner1`–`4`, `road_corner5_solid`–`road_corner8_solid`,
`road_crossroads`, `road_horizontal`, `road_vertical`, `road_side1`–`4`,
`road_solid`, `road_t1`–`4`, `road_water1`–`4`,
`road_water5_corner`–`road_water8_corner`, `road_water_horizontal`,
`road_water_vertical`, `road_water_lone`

`building_corner1`–`4`, `building_cross`, `building_horizontal`,
`building_vertical`, `building_horzend1`–`2`, `building_vertend1`–`2`,
`building_l1`–`4`, `building_most1`–`4`, `building_side1`–`4`,
`building_sidecorn1`–`16`, `building_single`, `building_solid`,
`building_t1`–`4`, `building_twist1`–`2`

`boat0`–`boat7`

### Tanks

Six groups of sixteen facings each, numbered `_00` (north) through `_15`,
turning clockwise:

`tank_self_00`–`15`, `tank_good_00`–`15`, `tank_evil_00`–`15`,
`tank_selfboat_00`–`15`, `tank_goodboat_00`–`15`, `tank_evilboat_00`–`15`

### Bases and pillboxes

`base_good`, `base_evil`, `base_neutral`

`pillbox_good_00`–`15` and `pillbox_evil_00`–`15` — these sixteen are **armour
levels, not facings**, so they are never rotated. (`pillbox_evil15`, without the
second underscore, is a legacy duplicate that also exists.)

### Shells, explosions and the little green man

`shell_00`–`15` (sixteen directions, clockwise from north)

`explosion1`–`explosion8`

`lgm_frame0`, `lgm_frame1`, `lgm_frame2`, `lgm_helicopter`

### Interface art

These are drawn on the status panel and the build cursor rather than on the map:

`status_base_good`, `status_base_evil`, `status_base_neutral`,
`status_base_alliegood`, `status_dead`, `status_pill_neutral`,
`status_pill_evil`, `status_pill_tankgood`, `status_pill_tankallie`,
`status_pill_tankevil`

`indent_on`, `indent_off`, `indent_dot_on`, `indent_dot_off`, `gunsight`,
`mouse_square`, `tank_icon`, `tank_transparent`, `static`

Interface art is excluded from the coverage calculation described below, so a
skin that redraws every map tile but no interface art still counts as complete.

## Higher resolution art

Add `@Nx` to a filename to supply that sprite at N times its native size:

```
grass.png          16 x 16
grass@2x.png       32 x 32
grass@4x.png       64 x 64
tank_self_00@2x.png    32 x 32
shell_00@2x.png         6 x 8     (native is 3 x 4)
indent_on@2x.png      108 x 108   (native is 54 x 54)
```

`N` runs from 2 to 8. Note that density is relative to **each sprite's own**
native size, not to a fixed 16 pixels — the stock set is not uniformly 16 × 16.

You do not have to supply every sprite at every density. Mixing is fine, and the
**Tile detail** setting decides what the player gets:

- **Classic** — always the base size, upscaled without smoothing. Pixel art stays
  pixel art. This is the default.
- **Match to zoom** — one density for the whole sheet: the highest that every map
  sprite has, capped at the current zoom. Never mixes a crisp tank with a blocky
  road.
- **High detail** — the best density each sprite individually has. Partial
  coverage means mixed crispness, which is what shipping partial art asks for.

All three are greyed out when the active skin has nothing above its base size,
since they would then build the same thing. When only *some* of your sprites
have finer art, **Match to zoom** alone is greyed out: it needs a density every
sprite has, so it would build exactly Classic's tiles. Classic and High detail
stay available, and High detail is the one that uses the art you did supply.

For a chosen density the game looks for, in order: the exact `@Nx` file, the
nearest larger `@Mx` scaled down, your SVG rasterised at that size, the nearest
smaller `@Mx` scaled up, then the base-size chain. All of that is point-sampled,
as is a sheet coarser than the size being built — the artwork is pixel art and
stays crisp that way, which is what 1.x did too.

A whole sheet finer than the size being built is the one thing averaged rather
than point-sampled, weighted by each pixel's own alpha so the key colour cannot
bleed into an edge. That is the case when you ship a 2× sheet and the player is
at zoom 1, and it is what lets dithered art average to the tone you drew it to
make instead of collapsing to whichever pixels the sampling happened to land
on.

A whole sheet supplied at 2× or more counts as covering **every** sprite at that
density, so a single `skin32.bmp` at 992 × 352 gives you a complete 2× skin.

## SVG

A `.svg` is rasterised at whatever size is needed, so one file covers every
density. `MaxPixelDensity` caps how fine it will be drawn.

At base size SVG is tried before PNG for the same sprite name, so if you ship
both, the SVG wins. Above base size the order in the previous section applies:
an exact `@Nx` file, or the nearest larger `@Mx`, wins over the SVG.

## Rotating skins

If you set `InGameRotate=1`, you can ship only the north-facing `_00` sprite of a
tank group and the game will produce the other fifteen by rotating it when it
builds the sheet.

```ini
[Skin]
Name=One Tank
InGameRotate=1
```

```
skins/onetank/
    skin.ini
    tank_self_00.png
    tank_good_00.png
```

Rules:

- It applies to the six tank groups only. **Shells are not rotated** — their
  sprites change shape by direction (3 × 4 north, 4 × 3 east, 4 × 4 diagonal), so
  a rotated copy would not fit.
- A group is only rotated if **your skin** supplied its `_00`. If you leave a
  group out entirely it uses the built-in art untouched.
- **A facing you draw yourself always wins.** Ship `tank_self_04.png` alongside
  `tank_self_00.png` and that facing uses your file; the other fourteen are
  rotated.
- A skin that supplies a whole sheet is not rotated, because a sheet already
  contains all sixteen facings.
- Tanks turn in sixteen steps, the same as any other skin. Rotation saves you
  drawing fifteen sprites; it does not make the turn smoother.
- Rotated art benefits from **Texture filter** set to Linear or Pixel art —
  Nearest makes rotated edges shimmer.

## Sound names

Any of these can be replaced, at `sounds/<name>` or at the skin's top level:

```
big_explosion_far.wav      big_explosion_near.wav     bubbles.wav
farming_tree_far.wav       farming_tree_near.wav      hit_tank_far.wav
hit_tank_near.wav          hit_tank_self.wav          man_building_far.wav
man_building_near.wav      man_dying_far.wav          man_dying_near.wav
man_lay_mine_near.wav      mine_explosion_far.wav     mine_explosion_near.wav
shooting_far.wav           shooting_near.wav          shooting_self.wav
shot_building_far.wav      shot_building_near.wav     shot_tree_far.wav
shot_tree_near.wav         tank_sinking_far.wav       tank_sinking_near.wav
lobby_chat.wav             lobby_countdown.wav        lobby_game_start.wav
lobby_player_join.wav      lobby_player_leave.wav     lobby_ready.wav
lobby_unready.wav          ping_default.wav           ping_standard.wav
ping_caution.wav           ping_assist.wav            ping_attack.wav
ping_onmyway.wav           ping_botcommand.wav
```

`near` and `far` are the same event heard close by or at a distance.

### Ping sounds

A received smart ping plays the sound for its own kind — `ping_attack.wav` for
an Attack ping — and `ping_default.wav` when there is no file for that kind, in
your skin or in the game. The game itself ships only `ping_default.wav` and
`ping_caution.wav`, so out of the box four of the six kinds play the default
one.

That makes both of the obvious skins easy. Replace `ping_default.wav` alone and
every kind without its own file plays yours. Ship `ping_attack.wav` alone and
only the Attack ping changes, with the rest still on the game's default. Each
of the seven takes `_0` through `_9` variants like any other sound.

Mix ping files to the level you want them heard at: the game plays them as
authored, with no ping-specific gain, so a skin's ping is exactly as loud as its
file. The two the game ships peak at about half scale, which keeps a ping under
the shells and explosions it is heard against; a file mastered to full scale
will shout over the fight.

A file that will not decode, or an empty one, counts as no file: that kind falls
back rather than pinging silently. The log line below says how many kinds ended
up on the default.

### Variants

A sound can have more than one file, and one of them is picked each time that
sound plays. Beside `<name>.wav`, ship any of `<name>_0.wav` through
`<name>_9.wav`:

```
skins/mytheme/sounds/
    farming_tree_near_0.wav
    farming_tree_near_1.wav
    farming_tree_near_2.wav
```

- The pool for a sound is whichever of those eleven names your skin holds: the
  ten numbered ones plus the plain `<name>.wav`, which counts as a member like
  any other. Eleven is the most a sound can have.
- Gaps are fine — `_0`, `_1` and `_5` is a pool of three. `_10` and above are
  not part of the scheme and are ignored.
- Each file is looked for at `sounds/<file>` and then at the skin's top level,
  the same two places a single sound file is looked for.
- **One source wins the whole sound.** If your skin holds any file for a sound
  that decodes, your files are the pool and the game's own is not mixed in.
  Three chops of your own do not end up rotating with the stock one.
- A file that will not decode is dropped and the rest of the pool still plays.
  If none of your files for a sound decode, the game's own sound is used rather
  than silence.
- Names match case-insensitively, and nothing goes in `skin.ini`. A variant is
  found by its filename alone.
- Variants are a skin feature. The game's own set is one file per effect, so a
  sound you do not replace has a pool of one.
- The file that played last is never the one picked next. Two files therefore
  alternate; a larger pool is random, but never the same file twice running.

**No sound name ends in `_<digit>`**, and none will, so a numbered file is
always a variant and never a sound in its own right. Nothing in the list above
is shaped that way.

Give the files in a pool matching loudness and length. One that is louder or
longer than its neighbours is heard as a wobble on every shot rather than as
variety.

Two players hear different variants of the same event. The choice is made on
each machine as the sound plays rather than sent with the event, so both hear
the same shot but not the same recording of it.

## Packaging a .wsf

A `.wsf` is a zip with a different extension. From the folder containing your
skin directory:

```
zip -r mytheme.wsf mytheme
```

Copy the result into the skins folder. Either a folder or a `.wsf` works, so
packaging is only for sharing.

Do not use Finder's **Compress** on macOS: it adds a `__MACOSX` folder beside
yours, so the archive no longer has a single top-level folder to strip and none
of your filenames match. Use the `zip` command above instead.

## Publishing to the Steam Workshop

Under Steam, a **Publish to Workshop...** button appears on the Skin row for a
skin in your own skins folder that loaded. The built-in art, Workshop skins and
skins that failed to load cannot be published.

The dialog's title defaults to `Name` and its description to `Notes` from
`skin.ini`. The preview image is rendered from the skin's own art; you cannot
supply your own. What is uploaded is a `.wsf`: a folder skin is zipped for you,
and an existing `.wsf` is rebuilt with the `skin.ini` change below. In either
case the archive entries are flattened to the top level (plus `sounds/`) and
lowercased.

After a successful publish the game writes `WorkshopId` and `WorkshopAuthor`
into your `skin.ini`, so publishing again offers **Update the item this skin
came from**. That option is preselected only when `WorkshopAuthor` matches the
account you are signed in with; a copy of someone else's skin defaults to
**Publish as a new item**.

Subscribed Workshop skins appear in the same picker tagged Workshop. One that
Steam has not finished downloading is listed greyed and cannot be picked until
it arrives.

## Which sprites move smoothly

The **Animation smoothness** setting can position sprites more finely than whole
game pixels. It has three modes:

- **Classic** — whole game pixels, as the game has always drawn. This is the
  default.
- **Match pixelation** — motion snaps to the size of one sheet texel on screen,
  a step between Classic and Smooth.
- **Smooth** — continuous motion, as fine as the screen allows.

A separate **Smooth shells** checkbox, shown in the Classic and Match
pixelation modes, moves shells smoothly on their own, since they are small and
fast and stepping shows on them most. Both settings are the player's; a skin
cannot set them.

A **Texture filter** setting chooses Nearest, Linear or Pixel art for the tile
sheet only. Nearest is the default. See `RecommendedFilter` above for how a
skin can suggest one.

Finer positioning only applies where the game has a finer position to use:

| Moves smoothly | Stays on whole pixels |
|---|---|
| Your own tank | Other players' tanks |
| Shells | Other players' little green men |
| Tank-explosion debris | Your own little green man |
| | Explosions (they do not move) |

Other players' positions arrive over the network already rounded to game pixels,
so there is nothing finer to draw with. This is a property of the game, not of
your skin, and no artwork changes it.

## What a skin cannot change

- Fonts, menu and dialog colours, and the lobby and settings interface.
- The map overview's own layers — its fog, grid and labels.
- Anything about how the game plays. A skin is artwork and sound only.

## Troubleshooting

Skins are loaded when you pick one and when the game starts. If something looks
wrong, the log records what was used. It is at `winbolo.log` in the same
preferences folder as the `skins` directory.

```
tileLoaderBuildSheet: scale=2, sheet=992x352, tile detail=classic, loaded 0 SVG, 289 PNG, 2 BMP fallback sprites; skin=user:mytheme: 0 SVG, 12 PNG, 4 @Nx, 0 sheet sprites from a density 1 sheet, 0 slots filled by rotation
```

It is one line in the log. Every sprite is counted exactly once, so the
built-in counts and the skin counts always add up to 307. Reading it:

- **`skin=none`** — your skin was not found or failed to open. Check it is in the
  right folder and that a `.wsf` is a valid zip.
- **`skin=` names your skin but every count is 0** — it opened, but no filename
  matched. Check spelling against the sprite list above; names are
  case-insensitive but must otherwise match exactly.
- **`from a density 1 sheet`** when you shipped a 2× or larger sheet — the
  dimensions are not an exact multiple of 496 × 176, so it was treated as 1×.
  A warning line just above names the file and its actual size. The same
  words appear when the skin has no sheet at all, so check the sheet sprite
  count beside it: 0 means no sheet was used.
- **`0 slots filled by rotation`** with `InGameRotate=1` — the skin did not supply
  a group's `_00`, or it supplies a whole sheet.

Sounds get a line of their own:

```
soundSetup: 34 effects, 12 from skin=user:mytheme, 7 with variants (max 4), 2 members unreadable, 1 fell back to the built-in, 4 ping kinds on the default sound
```

Reading it:

- **`34 effects`** — sounds with at least one file that plays. There are 38 in
  all, but six of those are the per-kind ping sounds, which are allowed to be
  missing (see [Ping sounds](#ping-sounds)); short of that, a smaller number
  means some sound has nothing to play at all, and a warning line just above
  names the file.
- **`12 from skin=`** — sounds whose pool came from your skin rather than from
  the game's own set. **`skin=none`** means no skin is loaded: either you have
  not picked one, or yours was not found or failed to open.
- **`7 with variants (max 4)`** — how many sounds have more than one file, and
  the largest pool of any sound. With no `_N` files anywhere this reads
  `0 with variants (max 1)`.
- **`2 members unreadable`** — individual files that would not decode, each with
  its own warning line naming it just above. This is how you find out that
  `farming_tree_near_5.wav` was never picked up.
- **`1 fell back to the built-in`** — sounds where your skin held files but none
  of them decoded, so the game's own sound is what plays.
- **`4 ping kinds on the default sound`** — ping kinds with no file of their
  own anywhere, which play `ping_default.wav`. A line just above names each
  one. Five is what the game on its own gives you, since it ships
  `ping_caution.wav` and nothing else per-kind; each sound your skin adds
  takes one off that count. A kind your skin meant to cover but which is
  still listed here has its file in the wrong place or under the wrong name.

The last two count different things. One bad file among several moves
`members unreadable` only, and the sound still works with one variant fewer. A
sound whose whole pool is bad moves both, and what you hear there is the game's
own audio rather than yours.

A skin that fails to load when you pick it leaves the previous one selected and
shows an error in the settings panel. At startup it is different: if the saved
skin cannot be opened (a `.wsf` on an unmounted drive, or a Workshop item still
downloading) the game uses the built-in art and says so only in the log. Your
choice is kept, and it applies again as soon as the files are there.
