/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Sight
 *Filename:      sight.h
 *Purpose:
 *  Which squares of a block can be seen from where the
 *  player is standing. A square is seen when any part of it
 *  can be seen - one corner, one sliver down the side of a
 *  wall - and not only when its middle can.
 *
 *  Terrain and geometry, nothing else: no regions, no fog,
 *  and nothing kept between calls, so everything that wants
 *  this answer asks here rather than working it out again.
 *
 *  Buildings and pillboxes are answered with angles. Every
 *  opaque square fills a wedge of the view as seen from the
 *  eye, and that wedge is the shadow it casts. The shadows are piled up
 *  nearest square first, and a square is seen when some part
 *  of its own wedge is still bare once the shadows of every
 *  square nearer than it have been taken out of it. A square
 *  at the same distance as another never shadows it - two
 *  walls side by side are both in plain view - and the eye's
 *  own square shadows nothing.
 *
 *  So the sliver counts. A pillbox with a wall run up beside
 *  it is seen as long as a hair of it shows past the wall,
 *  which is what a player looking at it sees, and what an
 *  attacker needs of the pillbox they are shooting at. The
 *  gaps that are not seen through are the ones with no width
 *  at all: two opaque squares that touch, along an edge or
 *  only at a corner, cast one shadow with no crack in it, so
 *  sight still does not slip between two buildings meeting
 *  corner to corner.
 *
 *  A live pillbox standing on the map is one of those
 *  squares. It is a building for this purpose: it shadows
 *  what is behind it, its own square is seen the way a wall
 *  square is, and where it touches a wall or another pill
 *  their shadows close the corner between them. A pill being
 *  carried in a tank is not on the map and stops nothing.
 *  Bases are not walls and never have been.
 *
 *  A dead pillbox on the ground stops nothing either. It is a
 *  structure standing on the square and it is drawn there,
 *  which is the argument for saying it should, but the
 *  engine's own movement has already answered the question
 *  the other way: mapGetSpeed lets a tank drive straight over
 *  a dead pill to pick it up, where a live one is impassable.
 *  A thing the world lets you drive through is not a thing
 *  that hides what is behind it, so sight agrees with the
 *  movement rather than with the drawing. Armour is the whole
 *  of the difference, and whoever wants the other answer
 *  changes sightPillBlocks in sight.c and nothing else.
 *
 *  Trees are not answered that way and have not changed.
 *  They stop a line by depth, counted as a run of consecutive
 *  forest squares along the one line from the middle of the
 *  eye's square to the middle of the square asked about: a
 *  single row of trees is seen through and a wood two or more
 *  deep is not. Forest, grass, forest is two one-deep
 *  runs and stops nothing; any square that is not forest puts
 *  the count back to zero. The other reading - every tree on
 *  the line added up however far apart they are - would blind
 *  a player who is looking across scattered ground, so the run
 *  is what is counted.
 *
 *  A wood is stopped at rather than seen into: the square that
 *  takes the run to its limit is itself unseen, so the line
 *  ends on the last tree before it and the boundary sits on the
 *  near edge of the wood. That is what a player can really tell
 *  about a wood at a distance - a tank inside one is withheld
 *  from them anyway - so showing ground a little way in would
 *  only say they can see something they cannot.
 *
 *  Close to, they can: a square within SIGHT_TREE_NEAR of the
 *  origin is never hidden by trees, which is the same corner
 *  the tank hide gives up (MIN_TREEHIDE_DIST, three squares of
 *  world distance, which is a separation of two whole squares
 *  or less). The two rules then agree about a tank in the trees
 *  beside you: the server sends it and the fog does not cover
 *  it. Buildings have no such exemption - a wall is a wall at
 *  any range.
 *
 *  A square has to get past both rules to be seen: the wood
 *  hides what is behind it whatever the walls say, and the
 *  walls hide what is behind them whatever the trees say.
 *********************************************************/

#ifndef SIGHT_H
#define SIGHT_H

#include "global.h"
#include "types.h"
#include "overview_types.h" /* OverviewRect, OVERVIEW_TANK_HALF */

/* Which terrain stops a line. This is the only place terrain opacity is
 * decided - every test in the module goes through it - so a terrain is added or
 * taken away by changing this one line. A pillbox is not terrain and is decided
 * in sightPillBlocks, the module's other say on the subject. */
#define SIGHT_OPAQUE(t) ((t) == BUILDING || (t) == HALFBUILDING)

/* Which terrain counts towards the tree depth, how deep a run of it stops a
 * line, and how close a square has to be to be seen whatever the trees say.
 * Each is the only place its question is decided, so what counts as a tree,
 * how far into a wood a player sees and where that stops applying are one line
 * each. A mined tree is still a tree: the square reads back as MINE_FOREST
 * rather than FOREST, and without the second term laying a mine in a wood
 * would open a one-square hole in the depth count that nobody can see on the
 * map.
 *
 * At a run of 2 the second tree of a wood is the first square not seen, so one
 * row of trees is seen through and a wood is stopped at its near edge.
 *
 * SIGHT_TREE_NEAR is a separation in whole squares, tested on each axis the
 * way MIN_TREEHIDE_DIST is, so the exempt ground is a box round the origin
 * rather than a circle. */
#define SIGHT_TREE(t) ((t) == FOREST || (t) == MINE_FOREST)
#define SIGHT_TREE_BLOCK_RUN 2
#define SIGHT_TREE_NEAR      2

/* There is no "off" here: a caller that is not working sight out builds no
 * mask at all rather than asking for one that hides nothing.
 *
 * The widest block anything asks about is the whole scroll envelope, so a
 * caller sizes one buffer from this and reuses it for whatever block it hands
 * over. A narrower block simply leaves the tail of the buffer alone. */
#define SIGHT_MAX_HALF   OVERVIEW_TANK_HALF
#define SIGHT_MAX_SIDE   (2 * SIGHT_MAX_HALF + 1)
#define SIGHT_MASK_BYTES (SIGHT_MAX_SIDE * SIGHT_MAX_SIDE)

/* How many separate shadows the pile holds. Touching shadows are folded into
 * one as they go on, so this counts the gaps between them rather than the
 * walls on the map, and a view with five hundred separate cracks of daylight
 * in it is not a view anybody has.
 *
 * A pile that does fill up folds the shadow it cannot hold into the one beside
 * it, over the gap between them, so it hides a little more than the geometry
 * says rather than a little less. Nothing on a real map comes near the cap, so
 * that branch is out of reach of any case built from terrain; the definition is
 * left open for a build to set instead, and the one build that sets it is the
 * overflow case in the tests, which compiles the module a second time with the
 * cap turned right down. Nothing that ships ever passes it. */
#ifndef SIGHT_SHADOW_MAX
#define SIGHT_SHADOW_MAX 512
#endif

/* Where inside its square the eye is, in world units across the square, which
 * is what the tank's own position is kept in. A caller that has only a square
 * to give passes SIGHT_SUB_CENTRE and is answered from the middle of it.
 *
 * Which corner of a wall a player can see round turns on where in the square
 * they are standing, so the main view hands over the tank's real position and
 * the map, which knows only squares, hands over the middle. */
#define SIGHT_SUB_CENTRE (1 << (TANK_SHIFT_MAPSIZE - 1))

/* Writes one byte per square of the block: 1 for a square that can be seen
 * from the eye and 0 for one that cannot. The eye stands on square
 * (originX, originY), (offsetX, offsetY) inside it.
 *
 * pb is the pill list the squares are checked against, and NULL says the
 * caller has none, so no pill blocks anything.
 *
 * vis is indexed by the block's own width, not the buffer's:
 *
 *   vis[(y - block->top) * (block->right - block->left + 1) + (x - block->left)]
 *
 * so one buffer of SIGHT_MASK_BYTES serves every block, and a block narrower
 * than the widest one is written with a shorter stride. A block wider or taller
 * than SIGHT_MAX_SIDE, or one with no squares in it, is refused and the buffer
 * is left as it was.
 *
 * The origin square is always seen, whatever is standing on it, and never
 * casts a shadow, so a tank sitting in a building is not blinded by the square
 * it is on. Every other square is seen when some part of it is left bare by
 * the shadows of the opaque squares nearer to the eye than it is, and when the
 * run of forest on the line to it does not reach SIGHT_TREE_BLOCK_RUN.
 *
 * An opaque square casts its shadow and is seen in it: a wall is drawn and the
 * ground behind it is not. A tree at the far end is counted, so the square
 * that takes the run to its limit is the first one hidden and the wood is
 * stopped at rather than seen into. Within SIGHT_TREE_NEAR squares of the
 * origin on both axes the run hides nothing.
 *
 * Where two opaque squares touch - along an edge or only at a corner - their
 * shadows are one shadow with no crack in it, so sight does not slip through
 * the corner where two buildings meet; trees cast no shadow at all. Squares
 * off the map are never seen. */
void sightBuildMask(map *mp, pillboxes *pb, BYTE originX, BYTE originY,
                    BYTE offsetX, BYTE offsetY, const OverviewRect *block,
                    BYTE *vis);

/* The rule as it stood before the shadows: one line from the middle of the
 * origin square to the middle of every square of the block, and the square is
 * seen when the line arrives. It is kept because the tests pin what the change
 * did - a corner that the centre line misses and the shadows find - and
 * because a block further from the eye than the shadow pass is built to reach
 * is answered with it rather than with nothing. The trees rule is the same one
 * in both, so only the buildings answer differently. */
void sightBuildMaskCentreLine(map *mp, pillboxes *pb, BYTE originX,
                              BYTE originY, const OverviewRect *block,
                              BYTE *vis);

#endif /* SIGHT_H */
