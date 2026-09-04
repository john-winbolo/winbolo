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
`.wsf` files). The preferences folder wins, so a skin you install yourself
shadows a shipped one of the same name. It is also the only location writable on
every platform — the others sit inside the application, which is read-only on
macOS, Steam and mobile installs.

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
WorkshopId=
WorkshopAuthor=
```

- **`MaxPixelDensity`** — the finest multiple your SVG art is meant to be drawn
  at. `0` or absent means unlimited, which is right for genuine vector art. Set
  it to `1` if your SVGs are pixel-art rectangle grids that should stay chunky.
  It only caps SVGs; `@Nx` PNG files are never capped, because shipping the file
  is the statement.
- **`InGameRotate`** — see [Rotating skins](#rotating-skins).
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

Individual files win over the sheet, so you can ship a sheet and override a few
sprites with separate PNGs.

### The background

`background.bmp`, or `screen.bmp` (the 1.x name). This is the panel artwork
around the play area. The stock one is 515 × 325; larger is fine and looks
sharper, since it is scaled to fit.

### Sounds

`sounds/<name>.wav`, or `<name>.wav` at the top level (the 1.x layout). Both are
searched, `sounds/` first. See [Sound names](#sound-names).

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

An image of the wrong size is scaled to fit and a warning is logged.

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

The setting is greyed out when the active skin has nothing above its base size,
since all three would then build the same thing.

For a chosen density the game looks for, in order: the exact `@Nx` file, the
nearest larger `@Mx` scaled down, your SVG rasterised at that size, the nearest
smaller `@Mx` scaled up, then the base-size chain. Everything is point-sampled —
the artwork is colour-keyed pixel art, and averaging neighbouring pixels both
softens edges and drags the key colour into them.

A whole sheet supplied at 2× or more counts as covering **every** sprite at that
density, so a single `skin32.bmp` at 992 × 352 gives you a complete 2× skin.

## SVG

A `.svg` is rasterised at whatever size is needed, so one file covers every
density. `MaxPixelDensity` caps how fine it will be drawn.

SVG is tried before PNG for the same sprite name, so if you ship both, the SVG
wins.

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
lobby_unready.wav
```

`near` and `far` are the same event heard close by or at a distance.

## Packaging a .wsf

A `.wsf` is a zip with a different extension. From the folder containing your
skin directory:

```
zip -r mytheme.wsf mytheme
```

Copy the result into the skins folder. Either a folder or a `.wsf` works, so
packaging is only for sharing.

## Which sprites move smoothly

The **Animation smoothness** setting can position sprites more finely than whole
game pixels, but only where the game has a finer position to use:

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
tileLoaderBuildSheet: scale=2, sheet=992x352, tile detail=classic,
  loaded 0 SVG, 305 PNG, 2 BMP fallback sprites;
  skin=user:mytheme: 0 SVG, 12 PNG, 4 @Nx, 0 sheet sprites
  from a density 1 sheet, 0 slots filled by rotation
```

Reading it:

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

A skin that fails to load leaves the previous one selected and shows an error in
the settings panel rather than falling back silently.
