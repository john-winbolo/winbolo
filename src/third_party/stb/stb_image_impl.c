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

/* The smallest whole factor the frames of a w x h GIF are shrunk by so that
 * planFrames of them fit in maxBytes beside stb's working buffers and, once
 * they are shrunk, the two full frames kept for dispose mode 3. 1 when they
 * fit as they are, when planFrames is 0, or when the full frames alone do
 * not fit (shrinking the kept frames cannot help then, and the decode goes
 * on to find the GIF too large as it would unshrunk). The caller has
 * checked that one full frame and the working buffers fit. */
static int stbiGifShrinkFor(int w, int h, size_t planFrames, size_t maxBytes) {
   const size_t full = (size_t)w * (size_t)h * 4u;
   const size_t held = full * (STBI_GIF_WORK_FRAMES + STBI_GIF_SHRINK_FRAMES);
   int k;
   if (planFrames == 0 ||
       stbiGifFramesFit(w, h, planFrames + STBI_GIF_WORK_FRAMES, maxBytes) ||
       held > maxBytes) {
      return 1;
   }
   for (k = 2;; k++) {
      const int rw = (w + k - 1) / k;
      const int rh = (h + k - 1) / k;
      if (stbiGifFramesFit(rw, rh, planFrames, maxBytes - held) ||
          (rw == 1 && rh == 1)) {
         return k;
      }
   }
}

/* Shrinks the w x h RGBA frame src by k into dst, rw x rh: each pixel of
 * dst is the mean of the k x k block of src it stands for (a smaller block
 * at the right and bottom edges when k does not divide w or h). */
static void stbiGifShrinkFrame(const stbi_uc *src, int w, int h, int k,
                               stbi_uc *dst, int rw, int rh) {
   int rx, ry;
   for (ry = 0; ry < rh; ry++) {
      const int y0 = ry * k;
      const int y1 = (y0 + k < h) ? y0 + k : h;
      for (rx = 0; rx < rw; rx++) {
         const int x0 = rx * k;
         const int x1 = (x0 + k < w) ? x0 + k : w;
         /* size_t: a block can be most of a frame, and a frame's pixels
          * times 255 is near the top of 32 bits at the preview's limit. */
         size_t sum[4] = { 0, 0, 0, 0 };
         size_t n = 0;
         stbi_uc *d = dst + ((size_t)ry * (size_t)rw + (size_t)rx) * 4u;
         int x, y, c;
         for (y = y0; y < y1; y++) {
            const stbi_uc *p = src + ((size_t)y * (size_t)w + (size_t)x0) * 4u;
            for (x = x0; x < x1; x++, p += 4) {
               sum[0] += p[0];
               sum[1] += p[1];
               sum[2] += p[2];
               sum[3] += p[3];
               n++;
            }
         }
         for (c = 0; c < 4; c++) {
            d[c] = (stbi_uc)((sum[c] + n / 2u) / n);
         }
      }
   }
}

stbi_uc *stbiLoadGifCappedShrink(const stbi_uc *buffer, int len,
                                 size_t maxBytes, size_t planFrames,
                                 int **delays, int *x, int *y, int *frames,
                                 int *tooLarge, StbiGifStopFn stop,
                                 void *stopUser) {
   stbi__context s;
   stbi__gif *g;
   stbi_uc *out = NULL;
   int *dl = NULL;
   /* The last two full frames, kept while the frames out holds are shrunk:
    * frame i in keep[i % 2]. */
   stbi_uc *keep[2] = { NULL, NULL };
   size_t layers = 0;
   size_t stride = 0;
   size_t full = 0;
   size_t held = 0;
   int comp = 0;
   int w = 0, h = 0;
   int shrink = 1;
   int rw = 0, rh = 0;

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
   shrink = stbiGifShrinkFor(w, h, planFrames, maxBytes);
   rw = (w + shrink - 1) / shrink;
   rh = (h + shrink - 1) / shrink;
   stride = (size_t)rw * (size_t)rh * 4u;
   /* What is held beside the frames kept: stb's buffers, and the two full
    * frames when the kept ones are shrunk. Within maxBytes: the header check
    * above for the one, stbiGifShrinkFor for the other. */
   full = (size_t)w * (size_t)h * 4u;
   held = full * (STBI_GIF_WORK_FRAMES +
                  ((shrink > 1) ? STBI_GIF_SHRINK_FRAMES : 0u));

   /* About 35 KB, so on the heap rather than a worker thread's stack. */
   g = (stbi__gif *)STBI_MALLOC(sizeof(stbi__gif));
   if (g == NULL) {
      return NULL;
   }
   memset(g, 0, sizeof(*g));
   if (shrink > 1) {
      keep[0] = (stbi_uc *)STBI_MALLOC(full);
      keep[1] = (stbi_uc *)STBI_MALLOC(full);
      if (keep[0] == NULL || keep[1] == NULL) {
         goto fail;
      }
   }

   for (;;) {
      stbi_uc *two_back = NULL;
      stbi_uc *u;
      stbi_uc *grown;
      int *grownDelays;
      if (layers >= 2) {
         two_back = (shrink > 1) ? keep[layers % 2]
                                 : out + (layers - 2) * stride;
      }
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
      /* Every frame is the logical screen's size, which the header gave. */
      if (g->w != w || g->h != h ||
          !stbiGifFramesFit(rw, rh, layers + 1, maxBytes - held)) {
         *tooLarge = 1;
         goto fail;
      }
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
      if (shrink > 1) {
         memcpy(keep[layers % 2], u, full);
         stbiGifShrinkFrame(u, w, h, shrink, out + layers * stride, rw, rh);
      } else {
         memcpy(out + layers * stride, u, stride);
      }
      dl[layers] = g->delay;
      layers++;
   }

   STBI_FREE(g->out);
   STBI_FREE(g->history);
   STBI_FREE(g->background);
   STBI_FREE(keep[0]);
   STBI_FREE(keep[1]);
   if (layers == 0) {
      STBI_FREE(g);
      STBI_FREE(out);
      STBI_FREE(dl);
      return NULL;
   }
   *x = rw;
   *y = rh;
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
   STBI_FREE(keep[0]);
   STBI_FREE(keep[1]);
   STBI_FREE(out);
   STBI_FREE(dl);
   return NULL;
}

stbi_uc *stbiLoadGifCapped(const stbi_uc *buffer, int len, size_t maxBytes,
                           int **delays, int *x, int *y, int *frames,
                           int *tooLarge, StbiGifStopFn stop,
                           void *stopUser) {
   return stbiLoadGifCappedShrink(buffer, len, maxBytes, 0u, delays, x, y,
                                  frames, tooLarge, stop, stopUser);
}
