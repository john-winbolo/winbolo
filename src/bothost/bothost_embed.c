/*
 * The Bot Host inside a WinBolo client: the WinBoloBotHost program's code,
 * built a second time with WINBOLO_BOTHOST_EMBED. That leaves out the
 * program's main and its argument parsing and adds botHostEmbedStart and
 * botHostEmbedStop (see bothost_embed.h). Every name in bothost_main.c is
 * static, so this copy shares nothing with any other file.
 */
#define WINBOLO_BOTHOST_EMBED 1
#include "bothost_main.c"
