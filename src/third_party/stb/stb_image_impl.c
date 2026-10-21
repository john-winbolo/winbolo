/* First, while STB_IMAGE_IMPLEMENTATION is not defined: stb_image.h has no
 * guard round its implementation, so a second include after the define
 * would give every stb function twice. */
#include "stb_image_gif_capped.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ── WinBolo: a GIF decode with a memory limit (stb_image_gif_capped.h) ──
 * In this file because it calls stb's static frame reader. The loop is
 * stbi__load_gif_main's, with these changes: the block size is size_t and is
 * checked against a limit (which also counts stb's working buffers) before
 * each frame is added; the file is checked to be a GIF, and its logical
 * screen's size against the limit, before the first frame is read, so a
 * huge screen is not allocated at all; a stop function is asked before each
 * frame; and the frame two back that dispose mode 3 restores is
 * the real one (stbi__load_gif_main points before the start of its block). */

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

stbi_uc *stbiLoadGifCapped(const stbi_uc *buffer, int len, size_t maxBytes,
                           int **delays, int *x, int *y, int *frames,
                           int *tooLarge, StbiGifStopFn stop,
                           void *stopUser) {
   stbi__context s;
   stbi__gif *g;
   stbi_uc *out = NULL;
   int *dl = NULL;
   size_t layers = 0;
   size_t stride = 0;
   int comp = 0;
   int w = 0, h = 0;

   if (delays) *delays = NULL;
   *x = 0;
   *y = 0;
   *frames = 0;
   *tooLarge = 0;
   if (buffer == NULL || len <= 0) {
      return NULL;
   }
   /* The GIF test first, so a file of another kind is "could not be opened"
    * and never "too large". stbi__gif_info does not rewind when it reads the
    * header, so the read starts again after it. */
   stbi__start_mem(&s, buffer, len);
   if (!stbi__gif_test(&s)) {
      return NULL;
   }
   if (!stbi__gif_info(&s, &w, &h, &comp)) {
      return NULL;
   }
   stbi__rewind(&s);
   if (!stbiGifFramesFit(w, h, 1 + STBI_GIF_WORK_FRAMES, maxBytes)) {
      *tooLarge = 1;
      return NULL;
   }

   /* About 1.5 KB, so on the heap rather than a worker thread's stack. */
   g = (stbi__gif *)STBI_MALLOC(sizeof(stbi__gif));
   if (g == NULL) {
      return NULL;
   }
   memset(g, 0, sizeof(*g));

   for (;;) {
      stbi_uc *two_back = (layers >= 2) ? out + (layers - 2) * stride : NULL;
      stbi_uc *u;
      stbi_uc *grown;
      int *grownDelays;
      if (stop != NULL && stop(stopUser)) {
         goto fail;
      }
      u = stbi__gif_load_next(&s, g, &comp, 4, two_back);
      if (u == (stbi_uc *)&s) {
         u = NULL;  /* the end of the GIF */
      }
      if (u == NULL) {
         break;
      }
      if (!stbiGifFramesFit(g->w, g->h, layers + 1 + STBI_GIF_WORK_FRAMES,
                            maxBytes)) {
         *tooLarge = 1;
         goto fail;
      }
      stride = (size_t)g->w * (size_t)g->h * 4u;
      grown = (stbi_uc *)STBI_REALLOC(out, (layers + 1) * stride);
      if (grown == NULL) {
         goto fail;
      }
      out = grown;
      grownDelays = (int *)STBI_REALLOC(dl, (layers + 1) * sizeof(int));
      if (grownDelays == NULL) {
         goto fail;
      }
      dl = grownDelays;
      memcpy(out + layers * stride, u, stride);
      dl[layers] = g->delay;
      layers++;
   }

   STBI_FREE(g->out);
   STBI_FREE(g->history);
   STBI_FREE(g->background);
   if (layers == 0) {
      STBI_FREE(g);
      STBI_FREE(out);
      STBI_FREE(dl);
      return NULL;
   }
   *x = g->w;
   *y = g->h;
   *frames = (int)layers;
   STBI_FREE(g);
   if (delays) {
      *delays = dl;
   } else {
      STBI_FREE(dl);
   }
   return out;

fail:
   STBI_FREE(g->out);
   STBI_FREE(g->history);
   STBI_FREE(g->background);
   STBI_FREE(g);
   STBI_FREE(out);
   STBI_FREE(dl);
   return NULL;
}
