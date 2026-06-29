/*
 * bolo_rand save/restore contract.
 *
 * A snapshot taken with bolo_rand_save and reapplied with bolo_rand_restore
 * must rewind the generator exactly, so any draws made between the save and the
 * restore do not shift the post-restore sequence. This is the property the
 * dedicated server relies on to keep cosmetic bot naming (which draws from the
 * shared PRNG) from perturbing the deterministic game stream for a given seed.
 */

#include <stdint.h>

#include "bolo_rand.h"
#include "test_harness.h"

int run_rand_save_restore(void) {
  const uint64_t seed = 0x1234ABCDu;
  uint32_t expected[4];
  uint32_t got[4];
  BoloRandState snap;
  int i;

  /* Reference continuation from the seed with nothing drawn in between. */
  bolo_srand(seed);
  for (i = 0; i < 4; i++) expected[i] = bolo_rand();

  /* Same seed: snapshot, burn a block of draws (as cosmetic naming would),
   * restore, and the continuation must match the reference exactly. */
  bolo_srand(seed);
  bolo_rand_save(&snap);
  for (i = 0; i < 17; i++) (void)bolo_rand();   /* intervening full draws */
  (void)bolo_rand_below(7);                     /* and a bounded draw */
  bolo_rand_restore(&snap);
  for (i = 0; i < 4; i++) got[i] = bolo_rand();

  for (i = 0; i < 4; i++) {
    UT_ASSERT_MSG(got[i] == expected[i],
                  "restore did not rewind draw %d: got %u, expected %u",
                  i, (unsigned)got[i], (unsigned)expected[i]);
  }

  /* Restore is repeatable: a second restore of the same snapshot reproduces
   * the same continuation again. */
  bolo_rand_restore(&snap);
  for (i = 0; i < 4; i++) {
    UT_ASSERT_MSG(bolo_rand() == expected[i],
                  "second restore did not reproduce draw %d", i);
  }

  /* NULL-safe: the NULL forms must neither crash nor advance the generator. */
  {
    BoloRandState before, after;
    bolo_rand_save(&before);
    bolo_rand_save(NULL);
    bolo_rand_restore(NULL);
    bolo_rand_save(&after);
    for (i = 0; i < 4; i++) {
      UT_ASSERT_MSG(before.s[i] == after.s[i],
                    "NULL save/restore must not advance the generator");
    }
  }

  return 0;
}
