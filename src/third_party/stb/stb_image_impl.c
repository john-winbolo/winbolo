/* First, while STB_IMAGE_IMPLEMENTATION is not defined: stb_image.h has no
 * guard round its implementation, so a second include after the define
 * would give every stb function twice. */
#include "stb_image_gif_capped.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ── WinBolo: a GIF read a frame at a time (stb_image_gif_capped.h) ──
 * In this file because it calls stb's static frame reader. The reading is
 * stbi__load_gif_main's, one frame for each call instead of all at once,
 * with these changes: the file is checked to be a GIF, and its logical
 * screen's size against a limit, before anything is allocated, so a huge
 * screen is not allocated at all; the size arithmetic is in size_t; and the
 * frame two back that dispose mode 3 restores is the real one, kept by the
 * stream (stbi__load_gif_main points before the start of its block). */

int stbiGifFramesFit(int w, int h, size_t frames, size_t maxBytes) {
   size_t frameBytes;
   if (w <= 0 || h <= 0 || frames == 0) {
      return 0;
   }
   /* w and h are ints, so (size_t)w * h is exact on a 64-bit size_t; on a
    * 32-bit one, check it in steps. */
   if ((size_t)w > maxBytes / 4u) {
      return 0;
   }
   frameBytes = (size_t)w * 4u;
   if ((size_t)h > maxBytes / frameBytes) {
      return 0;
   }
   frameBytes *= (size_t)h;
   return frames <= maxBytes / frameBytes;
}

struct StbiGifStream {
   stbi__context s;
   /* About 35 KB, with the stream on the heap rather than a stack. */
   stbi__gif g;
   const stbi_uc *buffer;
   int len;
   int w, h;
   /* The two frames before the current one, frame i in keep[i % 2], for a
    * frame whose dispose mode restores the frame two back. */
   stbi_uc *keep[2];
   int read;   /* frames given since the open or the last rewind */
   int ended;  /* the end of the GIF, or a fault, was reached */
};

/* Frees what stb's reader allocated for the GIF, so the reader starts the
 * GIF again (it allocates when g->out is NULL). */
static void stbiGifStreamFreeReader(StbiGifStream *st) {
   STBI_FREE(st->g.out);
   STBI_FREE(st->g.history);
   STBI_FREE(st->g.background);
   memset(&st->g, 0, sizeof(st->g));
}

StbiGifStream *stbiGifStreamOpen(const stbi_uc *buffer, int len,
                                 size_t maxBytes, int *w, int *h,
                                 int *tooLarge) {
   stbi__context s;
   StbiGifStream *st;
   size_t full;
   int comp = 0;
   int sw = 0, sh = 0;

   if (w) *w = 0;
   if (h) *h = 0;
   if (tooLarge) *tooLarge = 0;
   if (buffer == NULL || len <= 0) {
      return NULL;
   }
   /* The GIF test first, so a file of another kind is "could not be opened"
    * and never "too large". */
   stbi__start_mem(&s, buffer, len);
   if (!stbi__gif_test(&s)) {
      return NULL;
   }
   if (!stbi__gif_info(&s, &sw, &sh, &comp)) {
      return NULL;
   }
   if (!stbiGifFramesFit(sw, sh, STBI_GIF_STREAM_FRAMES, maxBytes)) {
      if (tooLarge) *tooLarge = 1;
      return NULL;
   }
   st = (StbiGifStream *)STBI_MALLOC(sizeof(StbiGifStream));
   if (st == NULL) {
      return NULL;
   }
   memset(st, 0, sizeof(*st));
   st->buffer = buffer;
   st->len = len;
   st->w = sw;
   st->h = sh;
   full = (size_t)sw * (size_t)sh * 4u;
   st->keep[0] = (stbi_uc *)STBI_MALLOC(full);
   st->keep[1] = (stbi_uc *)STBI_MALLOC(full);
   if (st->keep[0] == NULL || st->keep[1] == NULL) {
      stbiGifStreamClose(st);
      return NULL;
   }
   stbi__start_mem(&st->s, buffer, len);
   if (w) *w = sw;
   if (h) *h = sh;
   return st;
}

int stbiGifStreamNext(StbiGifStream *st, const stbi_uc **out, int *delayMs) {
   stbi_uc *two_back;
   stbi_uc *u;
   int comp = 0;

   if (out) *out = NULL;
   if (delayMs) *delayMs = 0;
   if (st == NULL || st->ended) {
      return 0;
   }
   two_back = (st->read >= 2) ? st->keep[st->read % 2] : NULL;
   u = stbi__gif_load_next(&st->s, &st->g, &comp, 4, two_back);
   if (u == (stbi_uc *)&st->s) {
      u = NULL;  /* the end of the GIF */
   }
   /* Every frame is the logical screen's size, which the header gave; a
    * frame of another size is a fault, as a cut-off frame is. */
   if (u == NULL || st->g.w != st->w || st->g.h != st->h) {
      st->ended = 1;
      return 0;
   }
   memcpy(st->keep[st->read % 2], u, (size_t)st->w * (size_t)st->h * 4u);
   st->read++;
   if (out) *out = u;
   if (delayMs) *delayMs = st->g.delay;
   return 1;
}

int stbiGifStreamFramesRead(const StbiGifStream *st) {
   return (st != NULL) ? st->read : 0;
}

void stbiGifStreamRewind(StbiGifStream *st) {
   if (st == NULL) {
      return;
   }
   stbiGifStreamFreeReader(st);
   stbi__start_mem(&st->s, st->buffer, st->len);
   st->read = 0;
   st->ended = 0;
}

void stbiGifStreamClose(StbiGifStream *st) {
   if (st == NULL) {
      return;
   }
   stbiGifStreamFreeReader(st);
   STBI_FREE(st->keep[0]);
   STBI_FREE(st->keep[1]);
   STBI_FREE(st);
}
