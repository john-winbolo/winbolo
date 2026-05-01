/*********************************************************
 * BrainTest viz-detail inspector window — ImGui panel
 * inside the main map window. Lists every viz_detail the
 * brain registered this tick, with collapsible bodies +
 * read-only multiline text fields so the user can copy
 * coordinates, scores, etc. Clicking an entry highlights
 * the corresponding primitive on the map; clicking the
 * primitive on the map scrolls + expands the entry here.
 *********************************************************/

#ifndef BRAINTEST_VIZDETAILWINDOW_H
#define BRAINTEST_VIZDETAILWINDOW_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void vizDetailWindowToggle(void);
bool vizDetailWindowIsVisible(void);

/* Render one ImGui frame. Caller must be inside the main ImGui
 * context's BeginFrame/EndFrame. */
void vizDetailWindowRender(void);

/* External "highlight this id" — call from the main-map click
 * handler when the click hit-tests onto a registered detail. The
 * dialog scrolls to that entry on next render and expands it. */
void vizDetailWindowSelectAndScroll(const char *id);

/* Currently-selected id, "" if nothing. Read by the host's per-
 * frame map renderer to draw the highlight overlay on the matching
 * primitive's geometry. */
const char *vizDetailWindowGetSelected(void);

/* Currently-hovered id (mouse over a row in the index dialog).
 * Cleared each frame; set during renderIndexDialog. The renderer
 * uses this to draw a transient secondary highlight in a different
 * color so the user can preview which map primitive a row maps to
 * without committing a click. "" when nothing is hovered. */
const char *vizDetailWindowGetHovered(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_VIZDETAILWINDOW_H */
