/*
 * Blocks-level append-fed stream source (test_blocks_stream.c). Pins the
 * no-zip path: lv_blocksBeginStream resets the reader, lv_blocksAppendBytes
 * feeds plaintext bytes in chunks, and the existing lv_blocksReadBytes /
 * lv_logSetPosition machinery reads and seeks over them with no zip handle.
 * Plaintext throughout (blockKey == 0, identity de-XOR).
 */
#include <stdint.h>
#include <string.h>

#include "lv_global.h"
#include "blocks.h"
#include "test_harness.h"

int run_blocks_stream(void) {
  /* A known pattern with distinct bytes, fed in two separate appends so the
   * buffer must grow across calls. */
  static const uint8_t pattern[20] = {
      0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87, 0x98, 0xA9,
      0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F, 0x11, 0x22, 0x33, 0x44};
  BYTE buf[20];
  int n;

  lv_blocksDestroy();      /* clean slate */
  lv_blocksBeginStream();
  lv_blocksSetKey(0);      /* plaintext: reads de-XOR with key 0 */

  /* Append in two chunks; each must succeed. */
  UT_ASSERT(lv_blocksAppendBytes(pattern, 8) == TRUE);
  UT_ASSERT(lv_blocksAppendBytes(pattern + 8, 12) == TRUE);
  UT_ASSERT(lv_logGetTotalSize() == 20);

  /* Read it back across the chunk boundary; bytes match exactly (no XOR) and
   * the read cursor advances by the number of bytes read. */
  n = logReadBytes(buf, 8);
  UT_ASSERT(n == 8);
  UT_ASSERT(memcmp(buf, pattern, 8) == 0);
  UT_ASSERT(lv_logGetCurrentPosition() == 8);

  n = logReadBytes(buf, 12);
  UT_ASSERT(n == 12);
  UT_ASSERT(memcmp(buf, pattern + 8, 12) == 0);
  UT_ASSERT(lv_logGetCurrentPosition() == 20);

  /* Seek back to an earlier offset and re-read. */
  lv_logSetPosition(5);
  UT_ASSERT(lv_logGetCurrentPosition() == 5);
  n = logReadBytes(buf, 4);
  UT_ASSERT(n == 4);
  UT_ASSERT(memcmp(buf, pattern + 5, 4) == 0);
  UT_ASSERT(lv_logGetCurrentPosition() == 9);

  /* Seek forward and re-read the tail. */
  lv_logSetPosition(15);
  UT_ASSERT(lv_logGetCurrentPosition() == 15);
  n = logReadBytes(buf, 5);
  UT_ASSERT(n == 5);
  UT_ASSERT(memcmp(buf, pattern + 15, 5) == 0);
  UT_ASSERT(lv_logGetCurrentPosition() == 20);

  /* Mark the stream complete; reading past the appended end yields no data
   * and EOF reports true once the cursor is at/after the end. */
  lv_blocksSetStreamEOF();
  n = logReadBytes(buf, 4);
  UT_ASSERT(n == 0);
  UT_ASSERT(lv_blocksIsEOF() == TRUE);

  lv_blocksDestroy();      /* free the buffer */
  return 0;
}
