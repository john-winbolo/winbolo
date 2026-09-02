/*
 * Game-binding claim predicate (key_claims.h). keyIsClaimedByGame() decides
 * which keys a second window that also drives the game — the Map Overview
 * pop-out — must leave alone, so the player's binding always wins over the
 * window's own shortcut.
 */
#include "key_claims.h"
#include "test_harness.h"

/* One distinct scancode per keyItems field, in the order the struct declares
 * them.  Every one must come back claimed: that is what catches a field added
 * to keyItems later and not added to the predicate, which would be a key the
 * game owns and the overview silently steals. */
#define KC_FIRST_SENTINEL 101
#define KC_FIELD_COUNT    23

/* An unbound scancode — outside the sentinel block above. */
#define KC_UNBOUND_SCANCODE 200

int run_key_claims(void) {
    int i;

    /* Every field is checked. */
    {
        keyItems k;
        k.kiForward      = KC_FIRST_SENTINEL + 0;
        k.kiBackward     = KC_FIRST_SENTINEL + 1;
        k.kiLeft         = KC_FIRST_SENTINEL + 2;
        k.kiRight        = KC_FIRST_SENTINEL + 3;
        k.kiShoot        = KC_FIRST_SENTINEL + 4;
        k.kiLayMine      = KC_FIRST_SENTINEL + 5;
        k.kiGunIncrease  = KC_FIRST_SENTINEL + 6;
        k.kiGunDecrease  = KC_FIRST_SENTINEL + 7;
        k.kiTankView     = KC_FIRST_SENTINEL + 8;
        k.kiPillView     = KC_FIRST_SENTINEL + 9;
        k.kiOverviewZoom = KC_FIRST_SENTINEL + 10;
        k.kiScrollUp     = KC_FIRST_SENTINEL + 11;
        k.kiScrollDown   = KC_FIRST_SENTINEL + 12;
        k.kiScrollLeft   = KC_FIRST_SENTINEL + 13;
        k.kiScrollRight  = KC_FIRST_SENTINEL + 14;
        k.kiAllyView     = KC_FIRST_SENTINEL + 15;
        k.kiLGMView      = KC_FIRST_SENTINEL + 16;
        k.kiBaseView     = KC_FIRST_SENTINEL + 17;
        k.kiQuickTree    = KC_FIRST_SENTINEL + 18;
        k.kiQuickRoad    = KC_FIRST_SENTINEL + 19;
        k.kiQuickWall    = KC_FIRST_SENTINEL + 20;
        k.kiQuickPillbox = KC_FIRST_SENTINEL + 21;
        k.kiQuickMine    = KC_FIRST_SENTINEL + 22;

        for (i = 0; i < KC_FIELD_COUNT; i++) {
            UT_ASSERT_MSG(keyIsClaimedByGame(&k, KC_FIRST_SENTINEL + i),
                          "scancode %d is bound to an action and must be "
                          "claimed — a keyItems field is missing from the "
                          "predicate", KC_FIRST_SENTINEL + i);
        }

        /* A scancode bound to nothing is free. */
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, KC_UNBOUND_SCANCODE),
                      "an unbound scancode must stay free");

        /* Scancode 0 is never claimed, even against a binding that is 0. */
        k.kiQuickMine = 0;
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, 0),
                      "scancode 0 means unbound and must never be claimed");
    }

    /* NULL bindings claim nothing. */
    UT_ASSERT_MSG(!keyIsClaimedByGame(NULL, KC_FIRST_SENTINEL),
                  "no bindings means no claim");

    /* The collision table under the shipped defaults: which of the overview's
     * own keys the game already owns.  The scancodes are copied from the
     * DEFAULT_* block in gamefront.h rather than included from it, so this
     * case documents today's collisions and does not track a later change to
     * the defaults. */
    {
        keyItems k;
        k.kiForward      = 8;    /* SDL_SCANCODE_E      */
        k.kiBackward     = 7;    /* SDL_SCANCODE_D      */
        k.kiLeft         = 22;   /* SDL_SCANCODE_S      */
        k.kiRight        = 9;    /* SDL_SCANCODE_F      */
        k.kiShoot        = 44;   /* SDL_SCANCODE_SPACE  */
        k.kiLayMine      = 225;  /* SDL_SCANCODE_LSHIFT */
        k.kiGunIncrease  = 87;   /* SDL_SCANCODE_KP_PLUS */
        k.kiGunDecrease  = 40;   /* SDL_SCANCODE_RETURN  */
        k.kiTankView     = 23;   /* SDL_SCANCODE_T      */
        k.kiPillView     = 10;   /* SDL_SCANCODE_G      */
        k.kiOverviewZoom = 224;  /* SDL_SCANCODE_LCTRL  */
        k.kiScrollUp     = 82;   /* SDL_SCANCODE_UP     */
        k.kiScrollDown   = 81;   /* SDL_SCANCODE_DOWN   */
        k.kiScrollLeft   = 80;   /* SDL_SCANCODE_LEFT   */
        k.kiScrollRight  = 79;   /* SDL_SCANCODE_RIGHT  */
        k.kiAllyView     = 28;   /* SDL_SCANCODE_Y      */
        k.kiLGMView      = 11;   /* SDL_SCANCODE_H      */
        k.kiBaseView     = 16;   /* SDL_SCANCODE_M      */
        k.kiQuickTree    = 30;   /* SDL_SCANCODE_1      */
        k.kiQuickRoad    = 31;   /* SDL_SCANCODE_2      */
        k.kiQuickWall    = 32;   /* SDL_SCANCODE_3      */
        k.kiQuickPillbox = 33;   /* SDL_SCANCODE_4      */
        k.kiQuickMine    = 34;   /* SDL_SCANCODE_5      */

        /* The overview's pan and zoom-in keys are the game's scroll and
         * increase-range bindings, so the overview does without them. */
        UT_ASSERT_MSG(keyIsClaimedByGame(&k, 80),  /* SDL_SCANCODE_LEFT  */
                      "left arrow scrolls the main view by default");
        UT_ASSERT_MSG(keyIsClaimedByGame(&k, 79),  /* SDL_SCANCODE_RIGHT */
                      "right arrow scrolls the main view by default");
        UT_ASSERT_MSG(keyIsClaimedByGame(&k, 82),  /* SDL_SCANCODE_UP    */
                      "up arrow scrolls the main view by default");
        UT_ASSERT_MSG(keyIsClaimedByGame(&k, 81),  /* SDL_SCANCODE_DOWN  */
                      "down arrow scrolls the main view by default");
        UT_ASSERT_MSG(keyIsClaimedByGame(&k, 87),  /* SDL_SCANCODE_KP_PLUS */
                      "keypad plus increases gun range by default");

        /* The wheel's zoom key is a binding of its own, so the overview must
         * not reach for it as a shortcut as well. */
        UT_ASSERT_MSG(keyIsClaimedByGame(&k, 224), /* SDL_SCANCODE_LCTRL */
                      "left control holds the overview's wheel on zoom");

        /* The rest of the overview's keys are unbound by default and stay
         * the overview's. */
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, 46),  /* SDL_SCANCODE_EQUALS   */
                      "equals is unbound and still zooms the overview in");
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, 45),  /* SDL_SCANCODE_MINUS    */
                      "minus is unbound and still zooms the overview out");
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, 86),  /* SDL_SCANCODE_KP_MINUS */
                      "keypad minus is unbound and still zooms out");
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, 74),  /* SDL_SCANCODE_HOME     */
                      "home is unbound and still centres the overview");
        UT_ASSERT_MSG(!keyIsClaimedByGame(&k, 6),   /* SDL_SCANCODE_C        */
                      "c is unbound and still centres the overview");
    }

    return 0;
}
