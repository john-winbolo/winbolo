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
 *Filename:      sight.c
 *Purpose:
 *  Works out which squares of a block can be seen from where
 *  the player is standing - see sight.h for the rules it
 *  works to.
 *
 *  Buildings and pillboxes are done with angles. Every opaque
 *  square fills a wedge of the view as seen from the eye, and
 *  that wedge is the shadow it casts. The squares are taken
 *  nearest first and each one is asked its question against
 *  the pile of shadows built so far: if any part of its own wedge is
 *  still bare, some part of the square can be seen and it is
 *  seen. Then, if it is opaque itself, its wedge goes on the
 *  pile for the squares behind it.
 *
 *  Nearest is the distance to the nearest point of the square
 *  rather than to its middle, because that is the order that
 *  makes the pile right: anything that can stand between the
 *  eye and a square has a point nearer the eye than any point
 *  of that square, so it is always already on the pile by the
 *  time the square is asked. Two squares the same distance off
 *  never shadow one another, which is what leaves a row of
 *  walls all in plain view.
 *
 *  No angle is ever worked out as a number of degrees. What is
 *  kept is the diamond angle - a whole-number ratio that rises
 *  once round the circle the way an angle does - so every
 *  comparison is an exact integer multiply, and two machines
 *  building the same mask from the same map always agree to
 *  the last square.
 *
 *  Trees are done with the line walk, which is unchanged: one
 *  Bresenham line from the middle of the eye's square to the
 *  middle of the square being asked about, carrying the run of
 *  forest squares it has come through back to back. A square
 *  has to get past both rules to be seen.
 *********************************************************/

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "sight.h"
#include "bolo_map.h"
#include "pillbox.h"

/* World units across one map square, which is what the eye's offset inside its
 * square is measured in - the same units the tank's own position is kept in,
 * so the main view hands its position straight over. */
#define SIGHT_SUB_SIDE (1 << TANK_SHIFT_MAPSIZE)

/* The ground the shadow pass looks over is the block with the eye's square
 * taken in, because a straight line from the eye to any square of the block
 * stays inside that rectangle and nothing outside it can be in the way. This
 * is the widest that rectangle may be before the pass hands the block to the
 * centre-line rule instead. The blocks anything really asks about are the
 * scroll envelope with the tank inside it, which is 29 squares, so the fall
 * back is for a caller that has wandered a long way off rather than for
 * anything a player sees. */
#define SIGHT_REGION_MAX_SIDE 48
#define SIGHT_REGION_MAX      (SIGHT_REGION_MAX_SIDE * SIGHT_REGION_MAX_SIDE)

/* What a square in the rectangle is wanted for. A square can be both: a wall
 * inside the block is asked whether it is seen and then casts its shadow. */
#define SIGHT_E_TARGET 1u
#define SIGHT_E_WALL   2u

/* One square of the rectangle, packed into a single number so the sort has one
 * key to move and no two entries ever compare equal: the distance leads, and
 * the square's own place on the map settles the order inside a distance. That
 * makes the order the same everywhere whatever the sort does with ties. */
#define SIGHT_KEY_SHIFT 18
#define SIGHT_KEY(d2, pos, flags) \
  (((uint64_t)(d2) << SIGHT_KEY_SHIFT) | ((uint64_t)(pos) << 2) | (uint64_t)(flags))
#define SIGHT_KEY_D2(k)    ((uint32_t)((k) >> SIGHT_KEY_SHIFT))
#define SIGHT_KEY_X(k)     ((int)(((k) >> 2) & 0xFFu))
#define SIGHT_KEY_Y(k)     ((int)(((k) >> 10) & 0xFFu))
#define SIGHT_KEY_FLAGS(k) ((unsigned)((k) & 3u))

/* A direction, held as its diamond angle. The whole part is which quarter of
 * the circle the ray points into and the fraction is how far along that
 * quarter's side it crosses, so the value runs 0 to 4 once round and rises the
 * same way all the way, which is everything the pile below asks of it.
 *
 * It is kept as the ratio it comes out as rather than divided: num over den,
 * compared by multiplying out. Nothing here rounds, so there is no size of gap
 * too small for the arithmetic to see and no machine that sees it differently. */
typedef struct {
  int32_t num; /* Top of the ratio */
  int32_t den; /* Bottom of it, which is the two offsets added up */
} sightAngle;

/* A wedge of the view, from one ray round to another. lo past hi means the
 * wedge runs over the seam at the diamond angle's zero, which is the ray
 * straight along +x; the pile keeps such a wedge as its two pieces. */
typedef struct {
  sightAngle lo; /* The ray the wedge starts at */
  sightAngle hi; /* The ray it ends at */
} sightSpan;

/* The shadows cast so far, in order and with no two of them touching. */
typedef struct {
  sightSpan span[SIGHT_SHADOW_MAX]; /* The shadows themselves */
  int       count;                  /* How many of them there are */
} sightShadows;

/* The two ends of the seam. Zero is a real ray - straight along +x - and four
 * is one past the whole circle, which no ray ever reaches, so a wedge cut at
 * the seam keeps every direction it had. */
static const sightAngle kSightAngleZero = {0, 1};
static const sightAngle kSightAngleFull = {4, 1};

/* Whether a square is on the map at all. The block is a rect of ints, so it
 * can name a square off the top or left edge, and a coordinate cast into a
 * BYTE there would come back somewhere else entirely. */
static bool sightOnMap(int x, int y) {
  return (x >= 0 && x < MAP_ARRAY_SIZE && y >= 0 && y < MAP_ARRAY_SIZE);
}

/* Whether a pillbox that stops a line is standing on the square. This is the
 * module's whole opinion of pillboxes, so it is the one place to change to give
 * them another.
 *
 * pillsExistPos answers for the map rather than for the list: a pill being
 * carried in a tank is on no square and is not counted, which is what leaves a
 * carried pill blocking nothing, and a pill whose square the client has not
 * been told is current is not counted either.
 *
 * pillsDeadPos then takes the dead ones back out. A dead pill is a structure
 * standing there and it is drawn there, which is the argument for letting it
 * block, but the engine's own movement has already answered the question the
 * other way: mapGetSpeed lets a tank drive straight over a dead pill to pick it
 * up, where a live one is impassable. A thing the world lets you drive through
 * is not a thing that hides what is behind it. Armour is the whole of the
 * difference, and dropping this second test is all it costs to put dead pills
 * back to blocking.
 *
 * A caller with no pill list passes NULL and no pill blocks anything. */
static bool sightPillBlocks(pillboxes *pb, int x, int y) {
  if (pb == NULL || *pb == NULL || sightOnMap(x, y) == FALSE) {
    return FALSE;
  }
  if (pillsExistPos(pb, (BYTE)x, (BYTE)y) == FALSE) {
    return FALSE;
  }
  return (pillsDeadPos(pb, (BYTE)x, (BYTE)y) == TRUE) ? FALSE : TRUE;
}

/* Whether a square stops a line passing through it. Off the map counts: there
 * is nothing out there to see past. */
static bool sightBlocks(map *mp, pillboxes *pb, int x, int y) {
  BYTE terrain; /* What is on the square */

  if (sightOnMap(x, y) == FALSE) {
    return TRUE;
  }
  terrain = mapGetPos(mp, (BYTE)x, (BYTE)y);
  if (SIGHT_OPAQUE(terrain)) {
    return TRUE;
  }
  return sightPillBlocks(pb, x, y);
}

/* Whether a square is one of the trees the depth count is kept over. Off the
 * map is not: there is nothing out there, and a line to a square on the map
 * never leaves the map to get there. */
static bool sightIsTree(map *mp, int x, int y) {
  BYTE terrain; /* What is on the square */

  if (sightOnMap(x, y) == FALSE) {
    return FALSE;
  }
  terrain = mapGetPos(mp, (BYTE)x, (BYTE)y);
  return (SIGHT_TREE(terrain) ? TRUE : FALSE);
}

/* Whether a square is close enough to the origin that trees never hide it.
 * Measured on each axis rather than as a distance, so the ground it covers is
 * a box - the shape the tank hide uses, for the same reason. */
static bool sightNearOrigin(int x0, int y0, int x, int y) {
  int dx; /* Squares across, either way round */
  int dy; /* Squares down */

  dx = (x > x0) ? (x - x0) : (x0 - x);
  dy = (y > y0) ? (y - y0) : (y0 - y);
  return (dx <= SIGHT_TREE_NEAR && dy <= SIGHT_TREE_NEAR);
}

/* Walks the line from one square to another and says whether it arrives. The
 * walk carries the run of forest squares it has come through back to back,
 * which is what makes the depth a thickness of wood rather than a tally of
 * every tree on the line: one square that is not forest puts it back to zero.
 * The far end is counted as a tree, so a wood is stopped at its near edge.
 *
 * The run hides nothing inside SIGHT_TREE_NEAR of the origin, so a wood the
 * player is standing beside is seen into as far as that and stopped at beyond
 * it. Passing over an exempt square leaves the run standing rather than
 * clearing it: the wood is still that deep, and a square further out behind it
 * is still behind it.
 *
 * wallsStop says whether buildings are the walk's business too. The shadow
 * pass answers buildings itself and asks this only about the trees, so it
 * passes FALSE and a wall on the line neither stops the walk nor counts
 * towards the run - it is a square that is not forest, and puts the run back
 * to zero like any other. The centre-line rule passes TRUE and gets the whole
 * of the rule as it was: neither end tested for a building, every square in
 * between tested, and the diagonal between two buildings that touch closed so
 * sight does not slip between them. */
static bool sightLineReaches(map *mp, pillboxes *pb, int x0, int y0, int x1,
                             int y1, bool wallsStop) {
  int trees;     /* Forest squares passed through in a row */
  bool atTarget; /* Is the walk standing on the square being asked about */
  int dx;    /* Squares across, counted up */
  int dy;    /* Squares down, counted down, so one error term serves both */
  int sx;    /* Which way x moves */
  int sy;    /* Which way y moves */
  int err;   /* The running error the step is chosen from */
  int e2;    /* Twice it, which is what the two tests compare against */
  int stepX; /* What this step adds to x */
  int stepY; /* What this step adds to y */
  int x;     /* Where the walk has got to */
  int y;

  x = x0;
  y = y0;
  trees = 0;
  dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
  sx = (x0 < x1) ? 1 : -1;
  sy = (y0 < y1) ? 1 : -1;
  err = dx + dy;

  while (x != x1 || y != y1) {
    e2 = 2 * err;
    stepX = 0;
    stepY = 0;
    if (e2 >= dy) {
      err += dy;
      stepX = sx;
    }
    if (e2 <= dx) {
      err += dx;
      stepY = sy;
    }

    /* A step that moves on both axes cuts the corner where the two squares
     * beside it meet. Two buildings touching along that edge close it, so
     * sight does not slip between them. Trees are left out of this: the depth
     * rule is about how far into a wood a player sees, and a pair of trees is
     * not a wall. */
    if (wallsStop == TRUE && stepX != 0 && stepY != 0 &&
        sightBlocks(mp, pb, x + stepX, y) == TRUE &&
        sightBlocks(mp, pb, x, y + stepY) == TRUE) {
      return FALSE;
    }

    x += stepX;
    y += stepY;
    atTarget = (x == x1 && y == y1);

    if (wallsStop == TRUE && atTarget == FALSE &&
        sightBlocks(mp, pb, x, y) == TRUE) {
      return FALSE;
    }
    if (sightIsTree(mp, x, y) == TRUE) {
      trees++;
      if (trees >= SIGHT_TREE_BLOCK_RUN &&
          sightNearOrigin(x0, y0, x, y) == FALSE) {
        return FALSE;
      }
    } else {
      trees = 0;
    }
    if (atTarget == TRUE) {
      return TRUE;
    }
  }
  return TRUE;
}

/* Which of two rays comes first round the circle. The two ratios are compared
 * by multiplying out, so the answer is exact and equal means the same ray. */
static int sightAngleCmp(const sightAngle *a, const sightAngle *b) {
  int64_t left;  /* a's top times b's bottom */
  int64_t right; /* b's top times a's bottom */

  left = (int64_t)a->num * (int64_t)b->den;
  right = (int64_t)b->num * (int64_t)a->den;
  if (left < right) {
    return -1;
  }
  if (left > right) {
    return 1;
  }
  return 0;
}

/* The diamond angle of a direction. The quarter of the circle it points into
 * counts whole numbers, and how far along that quarter's side it crosses is
 * the fraction, which comes out over the two offsets added up. The value
 * rises the same way all the way round and is nought exactly along +x. */
static sightAngle sightAngleOf(int32_t dx, int32_t dy) {
  sightAngle a; /* The angle to return */
  int32_t side; /* The two offsets added up, which is the fraction's bottom */
  int32_t quad; /* Which quarter of the circle */
  int32_t along; /* How far along that quarter's side */

  side = (dx >= 0 ? dx : -dx) + (dy >= 0 ? dy : -dy);
  if (side == 0) {
    /* Nothing asks for the angle of no direction at all: the eye is kept
     * inside its own square and that square is never one of the squares a
     * wedge is worked out for. Answering zero keeps the arithmetic safe. */
    return kSightAngleZero;
  }
  if (dx > 0 && dy >= 0) {
    quad = 0;
    along = dy;
  } else if (dy > 0) {
    quad = 1;
    along = -dx;
  } else if (dx < 0) {
    quad = 2;
    along = -dy;
  } else {
    quad = 3;
    along = dx;
  }
  a.den = side;
  a.num = quad * side + along;
  return a;
}

/* The wedge a square fills as seen from the eye, which is its shadow if it is
 * opaque and what has to be left bare for it to be seen.
 *
 * The eye is always outside the square, so the wedge is less than half a turn
 * and its two ends are two of the square's own corners: the first corner is
 * the one every other corner lies the same way round from, and the last is the
 * one they all lie the other way round from. Which pair that is falls out of
 * the sixteen cross products rather than out of a list of cases, so the eye
 * being level with the square, or square on to a face, needs nothing said
 * about it. */
static void sightSquareSpan(int32_t eyeX, int32_t eyeY, int x, int y,
                            sightSpan *span) {
  int32_t cx[4]; /* The four corners, as offsets from the eye */
  int32_t cy[4];
  int32_t x0;    /* The square's near side on each axis */
  int32_t y0;
  int i;         /* Looping variable */
  int j;         /* Looping variable */
  int first;     /* Corner the wedge starts at */
  int last;      /* Corner it ends at */
  bool isFirst;  /* Does every other corner lie one way round from this one */
  bool isLast;   /* Does every other corner lie the other way round */
  int64_t cross; /* Which way round one corner lies from another */

  x0 = (int32_t)x * SIGHT_SUB_SIDE - eyeX;
  y0 = (int32_t)y * SIGHT_SUB_SIDE - eyeY;
  cx[0] = x0;
  cy[0] = y0;
  cx[1] = x0 + SIGHT_SUB_SIDE;
  cy[1] = y0;
  cx[2] = x0 + SIGHT_SUB_SIDE;
  cy[2] = y0 + SIGHT_SUB_SIDE;
  cx[3] = x0;
  cy[3] = y0 + SIGHT_SUB_SIDE;

  first = 0;
  last = 0;
  for (i = 0; i < 4; i++) {
    isFirst = TRUE;
    isLast = TRUE;
    for (j = 0; j < 4; j++) {
      cross = (int64_t)cx[i] * (int64_t)cy[j] - (int64_t)cy[i] * (int64_t)cx[j];
      if (cross < 0) {
        isFirst = FALSE;
      }
      if (cross > 0) {
        isLast = FALSE;
      }
    }
    if (isFirst == TRUE) {
      first = i;
    }
    if (isLast == TRUE) {
      last = i;
    }
  }
  span->lo = sightAngleOf(cx[first], cy[first]);
  span->hi = sightAngleOf(cx[last], cy[last]);
}

/* The first shadow on the pile whose far end has reached this ray, which is
 * where a wedge starting there first meets the pile. The pile is in order and
 * its shadows do not overlap, so the far ends are in order too. */
static int sightShadowFirst(const sightShadows *sh, const sightAngle *a) {
  int lo; /* Bottom of the search */
  int hi; /* Top of it */
  int mid;

  lo = 0;
  hi = sh->count;
  while (lo < hi) {
    mid = lo + (hi - lo) / 2;
    if (sightAngleCmp(&sh->span[mid].hi, a) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* Whether the pile covers a wedge with nothing left over. The sweep runs from
 * the wedge's first ray towards its last, stepping over each shadow it meets:
 * a shadow that starts past where the sweep has got to leaves daylight between
 * them, and daylight of any width at all is enough to see through. A gap of no
 * width - where one shadow ends exactly where the next begins, which is what
 * two touching walls give - is not daylight and is stepped straight over. */
static bool sightShadowCovers(const sightShadows *sh, const sightAngle *lo,
                              const sightAngle *hi) {
  sightAngle cur; /* How far along the wedge the sweep has got */
  int i;          /* Looping variable */

  if (sightAngleCmp(lo, hi) >= 0) {
    return TRUE;
  }
  cur = *lo;
  for (i = sightShadowFirst(sh, lo); i < sh->count; i++) {
    if (sightAngleCmp(&sh->span[i].lo, &cur) > 0) {
      return FALSE;
    }
    if (sightAngleCmp(&sh->span[i].hi, &cur) > 0) {
      cur = sh->span[i].hi;
    }
    if (sightAngleCmp(&cur, hi) >= 0) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Folds a shadow into the pile, joining it to any shadow it overlaps or merely
 * touches. Touching counts, which is what stops two walls that meet - along an
 * edge or only at a corner - from leaving a crack of nothing between them. */
static void sightShadowAdd(sightShadows *sh, const sightAngle *lo,
                           const sightAngle *hi) {
  int i; /* First shadow the new one reaches */
  int j; /* One past the last shadow it reaches */
  int k; /* Shadow being widened when the pile is full */

  if (sightAngleCmp(lo, hi) > 0) {
    return;
  }
  i = sightShadowFirst(sh, lo);
  if (i == sh->count || sightAngleCmp(&sh->span[i].lo, hi) > 0) {
    /* It touches nothing, so it goes in on its own. A full pile takes it by
     * widening the shadow beside it over the gap instead: hiding a little more
     * than the geometry says is the safe way to be wrong, and a pile that full
     * is not a view anybody is looking at. */
    if (sh->count == SIGHT_SHADOW_MAX) {
      k = (i > 0) ? (i - 1) : 0;
      if (i == 0) {
        sh->span[0].lo = *lo;
      }
      if (sightAngleCmp(&sh->span[k].hi, hi) < 0) {
        sh->span[k].hi = *hi;
      }
      while (k + 1 < sh->count &&
             sightAngleCmp(&sh->span[k + 1].lo, &sh->span[k].hi) <= 0) {
        if (sightAngleCmp(&sh->span[k + 1].hi, &sh->span[k].hi) > 0) {
          sh->span[k].hi = sh->span[k + 1].hi;
        }
        memmove(&sh->span[k + 1], &sh->span[k + 2],
                (size_t)(sh->count - k - 2) * sizeof(sightSpan));
        sh->count--;
      }
      return;
    }
    memmove(&sh->span[i + 1], &sh->span[i],
            (size_t)(sh->count - i) * sizeof(sightSpan));
    sh->span[i].lo = *lo;
    sh->span[i].hi = *hi;
    sh->count++;
    return;
  }

  j = i;
  while (j < sh->count && sightAngleCmp(&sh->span[j].lo, hi) <= 0) {
    j++;
  }
  if (sightAngleCmp(lo, &sh->span[i].lo) < 0) {
    sh->span[i].lo = *lo;
  }
  if (sightAngleCmp(hi, &sh->span[j - 1].hi) > 0) {
    sh->span[i].hi = *hi;
  } else {
    sh->span[i].hi = sh->span[j - 1].hi;
  }
  if (j > i + 1) {
    memmove(&sh->span[i + 1], &sh->span[j],
            (size_t)(sh->count - j) * sizeof(sightSpan));
    sh->count -= (j - i - 1);
  }
}

/* A wedge that runs over the seam is two pieces, one each side of it. Cutting
 * it costs nothing: a wedge is covered when both its pieces are, and two
 * shadows that meet exactly on the seam still leave no daylight between them
 * because neither piece has any. */
static void sightShadowAddSpan(sightShadows *sh, const sightSpan *span) {
  if (sightAngleCmp(&span->lo, &span->hi) <= 0) {
    sightShadowAdd(sh, &span->lo, &span->hi);
    return;
  }
  sightShadowAdd(sh, &span->lo, &kSightAngleFull);
  sightShadowAdd(sh, &kSightAngleZero, &span->hi);
}

static bool sightSpanCovered(const sightShadows *sh, const sightSpan *span) {
  if (sightAngleCmp(&span->lo, &span->hi) <= 0) {
    return sightShadowCovers(sh, &span->lo, &span->hi);
  }
  return (sightShadowCovers(sh, &span->lo, &kSightAngleFull) == TRUE &&
          sightShadowCovers(sh, &kSightAngleZero, &span->hi) == TRUE);
}

/* How far the eye is from the nearest point of a square, squared - the square
 * root is never wanted, since all this is for is putting the squares in order.
 * Straight off the side of a square the nearest point is on that side, and
 * past a corner it is the corner. */
static uint32_t sightNearDistSq(int32_t eyeX, int32_t eyeY, int x, int y) {
  int32_t x0; /* The square's sides */
  int32_t x1;
  int32_t y0;
  int32_t y1;
  int32_t dx; /* How far outside the square the eye is on each axis */
  int32_t dy;

  x0 = (int32_t)x * SIGHT_SUB_SIDE;
  x1 = x0 + SIGHT_SUB_SIDE;
  y0 = (int32_t)y * SIGHT_SUB_SIDE;
  y1 = y0 + SIGHT_SUB_SIDE;
  dx = (eyeX < x0) ? (x0 - eyeX) : ((eyeX > x1) ? (eyeX - x1) : 0);
  dy = (eyeY < y0) ? (y0 - eyeY) : ((eyeY > y1) ? (eyeY - y1) : 0);
  return (uint32_t)(dx * dx) + (uint32_t)(dy * dy);
}

/* Puts the squares in order of distance. A plain shell sort: the keys carry
 * the square's own place on the map under the distance, so no two of them are
 * ever equal and the order does not depend on how the sort breaks a tie. */
static void sightSortKeys(uint64_t *keys, int count) {
  static const int kGaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
  int g;        /* Which gap */
  int gap;      /* The gap itself */
  int i;        /* Looping variable */
  int j;        /* Where the key being placed has got back to */
  uint64_t key; /* The key being placed */

  for (g = 0; g < (int)(sizeof(kGaps) / sizeof(kGaps[0])); g++) {
    gap = kGaps[g];
    for (i = gap; i < count; i++) {
      key = keys[i];
      for (j = i; j >= gap && keys[j - gap] > key; j -= gap) {
        keys[j] = keys[j - gap];
      }
      keys[j] = key;
    }
  }
}

void sightBuildMaskCentreLine(map *mp, pillboxes *pb, BYTE originX,
                              BYTE originY, const OverviewRect *block,
                              BYTE *vis) {
  int width;  /* Squares across the block, which is the mask's stride */
  int height; /* Squares down it */
  int ox;     /* The origin, as the walk counts */
  int oy;
  int x;      /* Looping variable */
  int y;      /* Looping variable */
  BYTE *row;  /* The mask row this square belongs to */
  bool seen;  /* Does the line reach this square */

  if (mp == NULL || block == NULL || vis == NULL) {
    return;
  }

  width = block->right - block->left + 1;
  height = block->bottom - block->top + 1;
  if (width <= 0 || height <= 0 || width > SIGHT_MAX_SIDE ||
      height > SIGHT_MAX_SIDE) {
    return;
  }

  ox = (int)originX;
  oy = (int)originY;
  for (y = block->top; y <= block->bottom; y++) {
    row = vis + (y - block->top) * width;
    for (x = block->left; x <= block->right; x++) {
      if (sightOnMap(x, y) == FALSE) {
        seen = FALSE;
      } else if (x == ox && y == oy) {
        seen = TRUE;
      } else {
        seen = sightLineReaches(mp, pb, ox, oy, x, y, TRUE);
      }
      row[x - block->left] = (BYTE)((seen == TRUE) ? 1 : 0);
    }
  }
}

void sightBuildMask(map *mp, pillboxes *pb, BYTE originX, BYTE originY,
                    BYTE offsetX, BYTE offsetY, const OverviewRect *block,
                    BYTE *vis) {
  uint64_t keys[SIGHT_REGION_MAX]; /* The squares in the way, and the block's */
  sightShadows shadows;            /* What has been hidden so far */
  sightSpan span;                  /* The wedge the square in hand fills */
  int width;    /* Squares across the block, which is the mask's stride */
  int height;   /* Squares down it */
  int ox;       /* The square the eye is standing on */
  int oy;
  int32_t eyeX; /* Where the eye is, in world units */
  int32_t eyeY;
  int left;     /* The rectangle the shadow pass looks over */
  int top;
  int right;
  int bottom;
  int count;    /* Squares in it worth looking at */
  int i;        /* Looping variable */
  int j;        /* One past the last square the same distance off */
  int k;        /* Looping variable */
  int x;        /* Looping variable */
  int y;        /* Looping variable */
  uint32_t here; /* The distance the squares in hand are all at */
  unsigned flags; /* What the square in hand is wanted for */
  BYTE *row;    /* The mask row this square belongs to */

  if (mp == NULL || block == NULL || vis == NULL) {
    return;
  }

  width = block->right - block->left + 1;
  height = block->bottom - block->top + 1;
  if (width <= 0 || height <= 0 || width > SIGHT_MAX_SIDE ||
      height > SIGHT_MAX_SIDE) {
    return;
  }

  ox = (int)originX;
  oy = (int)originY;

  /* The rectangle to look over: the block with the eye's square taken in, and
   * no further, because a straight line from the eye to any square of the
   * block stays inside it. Off the map is cut away - a square out there is
   * never seen, and nothing on the map is behind one. */
  left = (block->left < ox) ? block->left : ox;
  right = (block->right > ox) ? block->right : ox;
  top = (block->top < oy) ? block->top : oy;
  bottom = (block->bottom > oy) ? block->bottom : oy;
  if (left < 0) {
    left = 0;
  }
  if (top < 0) {
    top = 0;
  }
  if (right > MAP_ARRAY_SIZE - 1) {
    right = MAP_ARRAY_SIZE - 1;
  }
  if (bottom > MAP_ARRAY_SIZE - 1) {
    bottom = MAP_ARRAY_SIZE - 1;
  }
  if (right - left + 1 > SIGHT_REGION_MAX_SIDE ||
      bottom - top + 1 > SIGHT_REGION_MAX_SIDE) {
    sightBuildMaskCentreLine(mp, pb, originX, originY, block, vis);
    return;
  }

  /* The eye, kept off the edges of its own square at both ends. Standing
   * exactly on an edge would put the eye on the line of another square's face,
   * where the wedge that square fills is half a turn wide and the square is
   * neither in front of nor behind anything; a world unit in from the edge is
   * nearer the truth than that and costs the player nothing they could see.
   *
   * Both ends matter, and for the same reason: everything below is worked out
   * from an eye that is on no grid line. That is what leaves sightSquareSpan
   * one first corner and one last one, rather than a pair lying in a straight
   * line through the eye with nothing to choose between them, and it is what
   * keeps the distance to the nearest point of a square strictly greater for a
   * square further out, which is the order the pile is built in. The offsets
   * are BYTEs and so cannot reach SIGHT_SUB_SIDE on their own, which is the
   * only reason the low clamp has been enough so far; the high one is here so
   * that widening the parameter one day does not quietly take the rule away. */
  eyeX = (int32_t)ox * SIGHT_SUB_SIDE +
         (offsetX < 1 ? 1
                      : (offsetX > SIGHT_SUB_SIDE - 1 ? SIGHT_SUB_SIDE - 1
                                                      : (int32_t)offsetX));
  eyeY = (int32_t)oy * SIGHT_SUB_SIDE +
         (offsetY < 1 ? 1
                      : (offsetY > SIGHT_SUB_SIDE - 1 ? SIGHT_SUB_SIDE - 1
                                                      : (int32_t)offsetY));

  /* Nothing is seen until something says it is, so a square of the block that
   * is off the map, or that the pass never reaches, stays hidden. */
  memset(vis, 0, (size_t)(width * height));

  count = 0;
  for (y = top; y <= bottom; y++) {
    for (x = left; x <= right; x++) {
      bool isTarget; /* Is the square one the block is asking about */
      bool isWall;   /* Does it cast a shadow */

      isTarget = (x >= block->left && x <= block->right && y >= block->top &&
                  y <= block->bottom);
      if (x == ox && y == oy) {
        /* Where the player is standing: always seen, whatever is on it, and it
         * casts nothing, so a tank in a building still sees out of it. */
        if (isTarget == TRUE) {
          vis[(y - block->top) * width + (x - block->left)] = 1;
        }
        continue;
      }
      isWall = (SIGHT_OPAQUE(mapGetPos(mp, (BYTE)x, (BYTE)y)) ||
                        sightPillBlocks(pb, x, y) == TRUE)
                   ? TRUE
                   : FALSE;
      if (isTarget == FALSE && isWall == FALSE) {
        continue;
      }
      flags = (isTarget == TRUE ? SIGHT_E_TARGET : 0u) |
              (isWall == TRUE ? SIGHT_E_WALL : 0u);
      keys[count] = SIGHT_KEY(sightNearDistSq(eyeX, eyeY, x, y),
                              (y << 8) | x, flags);
      count++;
    }
  }
  sightSortKeys(keys, count);

  /* Nearest first, a distance at a time. Every square at this distance is
   * asked its question before any of them casts its shadow, so two walls the
   * same distance off do not hide one another and the ground between a row of
   * them is not hidden by the row. */
  shadows.count = 0;
  i = 0;
  while (i < count) {
    here = SIGHT_KEY_D2(keys[i]);
    j = i;
    while (j < count && SIGHT_KEY_D2(keys[j]) == here) {
      if ((SIGHT_KEY_FLAGS(keys[j]) & SIGHT_E_TARGET) != 0) {
        x = SIGHT_KEY_X(keys[j]);
        y = SIGHT_KEY_Y(keys[j]);
        sightSquareSpan(eyeX, eyeY, x, y, &span);
        if (sightSpanCovered(&shadows, &span) == FALSE) {
          vis[(y - block->top) * width + (x - block->left)] = 1;
        }
      }
      j++;
    }
    for (k = i; k < j; k++) {
      if ((SIGHT_KEY_FLAGS(keys[k]) & SIGHT_E_WALL) != 0) {
        sightSquareSpan(eyeX, eyeY, SIGHT_KEY_X(keys[k]), SIGHT_KEY_Y(keys[k]),
                        &span);
        sightShadowAddSpan(&shadows, &span);
      }
    }
    i = j;
  }

  /* Then the trees, over what the walls left. A wood hides what is behind it
   * whatever the walls say, so this only ever takes squares away, and asking
   * it last keeps the walk off every square the walls have hidden already. */
  for (y = block->top; y <= block->bottom; y++) {
    row = vis + (y - block->top) * width;
    for (x = block->left; x <= block->right; x++) {
      if (row[x - block->left] == 0 || (x == ox && y == oy)) {
        continue;
      }
      if (sightLineReaches(mp, pb, ox, oy, x, y, FALSE) == FALSE) {
        row[x - block->left] = 0;
      }
    }
  }
}
