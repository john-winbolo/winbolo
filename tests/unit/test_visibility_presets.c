/*
 * The visibility preset table and the match over it.
 *
 * The lobby puts no preset on the wire. Every client reads the same seven
 * settings and works out for itself which named set they add up to, so
 * the whole agreement between clients rests on this match being exact and
 * on every preset being reachable from it. Four things are pinned:
 *
 *   1. Every preset round-trips: the values the table hands out read
 *      back as the preset they came from, and no two presets hold the
 *      same values (which would make one of them unreachable).
 *
 *   2. A set that is none of them reads as Custom.
 *
 *   3. The decay seconds are outside the match. No preset uses the Decay
 *      policy, so a host's seconds must not decide which row is ticked.
 *
 *   4. The INI words round-trip, and a word that is none of them falls
 *      back rather than picking a preset at random.
 */

#include <stdio.h>
#include <string.h>

#include "visibility_presets.h"
#include "test_harness.h"

/* The classic set, written out rather than taken from the table, so a
 * change to the table has to be made here too. */
static void fill_classic(VisibilitySettings *v) {
    memset(v, 0, sizeof(*v));
    v->policy[viewCategoryPill] = (uint8_t)viewPolicyKey;
    v->policy[viewCategoryBase] = (uint8_t)viewPolicyOff;
    v->policy[viewCategoryAlly] = (uint8_t)viewPolicyOff;
    v->decaySecs[viewCategoryPill] = (uint16_t)VIEW_DECAY_DEFAULT_SECS;
    v->decaySecs[viewCategoryBase] = (uint16_t)VIEW_DECAY_DEFAULT_SECS;
    v->decaySecs[viewCategoryAlly] = (uint16_t)VIEW_DECAY_DEFAULT_SECS;
    v->classicMode    = true;
    v->overviewWindow = (uint8_t)overviewWindowNone;
    v->lineOfSight    = (uint8_t)lineOfSightOff;
    v->alliesInTrees  = false;
}

int run_visibility_preset_round_trip(void) {
    VisibilitySettings sets[VISIBILITY_PRESET_COUNT];
    int p;
    int q;

    for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
        UT_ASSERT_MSG(visibilityPresetSettings((VisibilityPreset)p, &sets[p]),
                      "preset %d has no values", p);
        UT_ASSERT_MSG(visibilityPresetMatch(&sets[p]) == (VisibilityPreset)p,
                      "preset %d read back as %d", p,
                      (int)visibilityPresetMatch(&sets[p]));
    }

    /* Two presets holding the same values would make the second one
     * unreachable — the match always answers with the first. */
    for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
        for (q = p + 1; q < (int)VISIBILITY_PRESET_COUNT; q++) {
            UT_ASSERT_MSG(!visibilitySettingsEqual(&sets[p], &sets[q]),
                          "presets %d and %d hold the same values", p, q);
        }
    }

    /* Custom is not a row of the table and has nothing to hand out. */
    {
        VisibilitySettings untouched;
        fill_classic(&untouched);
        UT_ASSERT(!visibilityPresetSettings(visibilityPresetCustom, &untouched));
        UT_ASSERT_MSG(untouched.classicMode,
                      "a refused fill must leave the caller's set alone");
        UT_ASSERT(!visibilityPresetSettings((VisibilityPreset)-1, &untouched));
        UT_ASSERT(!visibilityPresetSettings(visibilityPresetClassic, NULL));
    }

    /* The Classic row is the set classic mode itself forces, spelled out
     * here so a change to one has to be a change to the other. */
    {
        VisibilitySettings classic;
        fill_classic(&classic);
        UT_ASSERT_MSG(visibilityPresetMatch(&classic) == visibilityPresetClassic,
                      "the hand-written classic set read back as %d",
                      (int)visibilityPresetMatch(&classic));
    }
    return 0;
}

int run_visibility_preset_custom(void) {
    VisibilitySettings v;

    /* Max view with the bases turned back off is nobody's preset. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetMaxView, &v));
    v.policy[viewCategoryBase] = (uint8_t)viewPolicyOff;
    UT_ASSERT_MSG(visibilityPresetMatch(&v) == visibilityPresetCustom,
                  "a set no preset holds read back as %d",
                  (int)visibilityPresetMatch(&v));

    /* One category on Decay is enough on its own: no preset uses it. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetExpanded, &v));
    v.policy[viewCategoryPill] = (uint8_t)viewPolicyDecay;
    UT_ASSERT(visibilityPresetMatch(&v) == visibilityPresetCustom);

    /* So is the overview window on its own. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetMaxView, &v));
    v.overviewWindow = (uint8_t)overviewWindowClassic;
    UT_ASSERT(visibilityPresetMatch(&v) == visibilityPresetCustom);

    /* And so is allies in trees, which is why it is part of a preset at
     * all — leaving it out would make Max view match with it either way. */
    UT_ASSERT(visibilityPresetSettings(visibilityPresetMaxView, &v));
    v.alliesInTrees = false;
    UT_ASSERT(visibilityPresetMatch(&v) == visibilityPresetCustom);

    UT_ASSERT(visibilityPresetMatch(NULL) == visibilityPresetCustom);
    return 0;
}

int run_visibility_preset_ignores_decay(void) {
    VisibilitySettings v;
    int p;
    int c;

    for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
        UT_ASSERT(visibilityPresetSettings((VisibilityPreset)p, &v));
        for (c = 0; c < VIEW_CATEGORY_COUNT; c++) {
            v.decaySecs[c] = (uint16_t)(VIEW_DECAY_MIN_SECS + c);
        }
        UT_ASSERT_MSG(visibilityPresetMatch(&v) == (VisibilityPreset)p,
                      "preset %d stopped matching once the seconds moved", p);
    }

    /* The seconds are not outside every comparison, though: the "has the
     * host's own set changed" test counts them, or a change to a decay
     * value would never be remembered. */
    {
        VisibilitySettings a;
        VisibilitySettings b;
        UT_ASSERT(visibilityPresetSettings(visibilityPresetExpanded, &a));
        b = a;
        UT_ASSERT(visibilitySettingsEqual(&a, &b));
        b.decaySecs[viewCategoryAlly] =
            (uint16_t)(a.decaySecs[viewCategoryAlly] + 1);
        UT_ASSERT(!visibilitySettingsEqual(&a, &b));
        UT_ASSERT(!visibilitySettingsEqual(&a, NULL));
        UT_ASSERT(visibilitySettingsEqual(NULL, NULL));
    }
    return 0;
}

int run_visibility_preset_pref_words(void) {
    int p;

    for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
        const char *word = visibilityPresetPrefWord((VisibilityPreset)p);
        UT_ASSERT_MSG(word != NULL && word[0] != '\0',
                      "preset %d has no INI word", p);
        UT_ASSERT_MSG(visibilityPresetFromPrefWord(word, visibilityPresetCustom)
                          == (VisibilityPreset)p,
                      "INI word '%s' read back as %d", word,
                      (int)visibilityPresetFromPrefWord(word,
                                                        visibilityPresetCustom));
    }

    UT_ASSERT(visibilityPresetFromPrefWord("Custom", visibilityPresetClassic)
              == visibilityPresetCustom);
    /* A word nobody wrote leaves the caller on whatever they already had,
     * so a mistyped INI cannot open a view up. */
    UT_ASSERT(visibilityPresetFromPrefWord("Banana", visibilityPresetClassic)
              == visibilityPresetClassic);
    UT_ASSERT(visibilityPresetFromPrefWord(NULL, visibilityPresetMaxView)
              == visibilityPresetMaxView);

    /* Every preset has a name and a description to draw with, and an
     * index that is not a preset falls back to the Custom row's rather
     * than to zero, which is not a string id at all. */
    for (p = 0; p < (int)VISIBILITY_PRESET_COUNT; p++) {
        UT_ASSERT(visibilityPresetNameId((VisibilityPreset)p) > 0);
        UT_ASSERT(visibilityPresetDescId((VisibilityPreset)p) > 0);
    }
    UT_ASSERT(visibilityPresetNameId(visibilityPresetCustom) > 0);
    UT_ASSERT(visibilityPresetDescId(visibilityPresetCustom) > 0);
    return 0;
}
