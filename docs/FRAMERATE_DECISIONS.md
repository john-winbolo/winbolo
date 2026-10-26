# Frame rate and Hide Main View: decisions (#312)

This is a proof of concept. Each item says what was decided, the options
that were weighed, and why this one was picked. All of it is open for
review.

## What was there before

- The Frame Rate list had 10, 12, 15, 20, 30, 50 and 60. The numbers on
  the labels were not rates. Each one was an old timer value
  (`FRAME_RATE_30` was 55), and the game loop slept for
  `1000 / value` ms after each frame, with vsync also on.
- The real rates were about: "10" = 19, "12" = 23, "15" = 28, "20" = 38,
  "30" = 58, "50" and "60" = the display refresh (vsync was the real
  limit). The default was "30", so most players drew at about 58.
- A separate SDL timer (`windowFrameRateTimer`) set a `needsRedraw` flag
  that nothing read. The loop drew every pass.
- Hide Main View was already removed in #179. Only its language string
  (`STR_MENU_HIDE_MAIN`) was left.
- The menu's background game had a hook (`bgFrameEveryN`) that waited
  for this issue. It always drew every refresh.

## Decisions

1. **The choices are: Match display (vsync), 30, 60, 120, 144, 240 and
   Unlimited (vsync off).**
   - Options weighed: keep the old seven numbers but make them real;
     only "vsync on / off"; a free number field; a list of common limits.
   - Why: the old reason to draw less (give CPU to brains in other copies
     of the game) is gone, so values below 30 have no use now. 30 is
     kept to save power on a laptop. 60, 120, 144 and 240 match common
     screens. Unlimited is for players who want the lowest delay and do
     not mind tearing. A free number field is more to test and gives
     little more.

2. **The default is Match display (vsync).**
   - Options weighed: 60; Match display.
   - Why: it is what most players had before (about 58 on a 60 Hz
     screen), it never tears, and on a 120 Hz or 144 Hz screen it uses
     the full rate without a change by the player.

3. **Fixed limits keep vsync on. Only Unlimited turns it off.**
   - Options weighed: a limit turns vsync off (frames can come at any
     time and tear); a limit keeps vsync on (frames wait for a refresh).
   - Why: with one list and no separate vsync switch, the player can
     only see tearing when they pick "Unlimited (vsync off)", which says
     so. The cost: a limit at or above the refresh rate gives the same
     result as Match display, and a limit that does not divide the
     refresh rate (120 on a 144 Hz screen) has uneven frame times. This
     is normal for a frame limit with vsync on.

4. **The limit waits on a fixed grid of due times and wakes half a
   refresh early.**
   - Options weighed: sleep for the frame period after each frame (what
     the old code did); `SDL_SetRenderVSync(renderer, n)` to present on
     every n-th refresh; a grid of due times.
   - Why: a sleep after each frame adds the draw time to the period, so
     30 on a 60 Hz screen can fall to 20. Vsync intervals above 1 are not
     there on every SDL renderer (Metal), and they only give whole
     fractions of the refresh rate. The grid keeps the average rate at
     the limit. Waking half a refresh early lets vsync put the frame on
     the refresh nearest the due time. When a frame is more than one
     period late (a stall), the grid starts again, so it does not rush to
     catch up. The wait uses `SDL_DelayPrecise` (ns), not `SDL_Delay`
     (whole ms).

5. **Only drawing changes. The game tick does not.**
   - The game tick has its own 10 ms SDL timer. The main loop runs every
     tick that is due by the wall clock on each pass (the existing
     catch-up loop in `windowRunGameTick`), so a low limit does not slow
     the game. At 30 the ticks run in groups of about three, as they did
     at the old low settings. Input is read once per pass, so a low limit
     adds some input delay. That is the same as before.

6. **The choice applies in the game window only. Menus and dialogs
   always use vsync.**
   - Options weighed: apply it everywhere; game window only.
   - Why: the menus and dialogs have their own draw loops with no wait.
     With vsync off there, Unlimited would draw the menus thousands of
     times a second and use a full CPU core for nothing. When the game
     loop ends, vsync goes back on.
   - The menu's background game does follow a fixed limit: it redraws
     its scene on every n-th refresh, n the nearest whole number to
     refresh / limit (the hook `bgFrameEveryN` was there for this). The
     menu itself still draws every refresh.

7. **The choice lives in Settings > Display, and also stays in the Edit
   menu as Edit > Frame Rate.**
   - Options weighed: Settings only; Edit menu only (like the original
     game); both.
   - Why: the issue asked which. Settings is where a new player looks,
     and it has a help tooltip. The Edit menu is where players of the
     original game look, and it can be changed during a game in one
     click. Both lists come from one function (`imguiFrameLimitLabel`),
     and the macOS menu bar uses it too, so the three lists cannot
     differ. If the owner wants one place only, removing the Edit menu
     entry is a small change.

8. **The choice is shown on Windows, Linux and macOS only.**
   - Options weighed: show it everywhere; desktop only.
   - Why: the web page is drawn when the browser asks for a frame
     (`requestAnimationFrame`), and Android and iOS pace their own
     screens. There the old list did nothing. The `frameRate` global
     stays on those builds so the shared code still links.
     `IMGUI_FRAME_LIMIT_UI` in `imgui_settings.h` holds the test.

9. **A change is saved to the preferences file at once.**
   - Before, `windowSetFrameRate` did not save. The value was written
     only when something else saved the preferences. Now `windowSetFrameRate` calls
     `gameFrontSaveCurrentPrefs`, the same as the letterbox toggle.

10. **New preferences key, with the old value moved across once.**
    - The new key is `[MENU] Frame Limit`, with a word for each choice:
      `Match display`, `30`, `60`, `120`, `144`, `240`, `Unlimited`.
    - Options weighed: keep `[MENU] Frame Rate` with new numbers; a new
      key.
    - Why: an older build (for example the 2.1 Steam build) reads
      `Frame Rate` as an old number. A new value in that key would give
      it a number it does not know. With a new key, both builds can read
      the same file. The old line is not written again and not deleted.
    - When there is no `Frame Limit` value, the old `Frame Rate` value is
      moved to the nearest new choice by what it really did:
      - 111 ("60"), 82 ("50"), 55 ("30", the old default) become
        Match display. Most players have 55 only because it was the
        default, and it drew at about the refresh rate.
      - 37 ("20"), 28 ("15"), 23 ("12"), 19 ("10") become 30. These
        players picked a low rate on purpose; 30 is the nearest new
        choice.
      - Anything else becomes the default.
    - The rules are in `src/gui/frame_limit.h` and the unit tests
      `frame_limit_*` cover them.

11. **The unused frame-rate timer is removed.**
    - `windowFrameRateTimer`, its timer ID and the `needsRedraw` flag
      did nothing (the loop draws every pass and only cleared the flag).
      The timer woke a thread up to 58 times a second for no use.

12. **The Hide Main View string is removed.**
    - The option itself went in #179, which left `STR_MENU_HIDE_MAIN`
      for a later language cleanup. It is now removed from `lang.h`,
      `lang.c`, `en.txt`, `lang_names.inc` and the 16 translation files.
      An unknown key in a translation file logs a warning at load, so
      leaving it in those files was not an option.

13. **The old `FRAME_RATE_*` macros are removed from `winbolo.h`.**
    - The copies in `sdl3imgui.cpp` and `imgui_settings.cpp` went too.
      The old numbers are kept only as `FRAME_LIMIT_OLD_*` in
      `frame_limit.h`, to read an old preferences file.
    - The global keeps its name `frameRate` (it now holds a
      `FrameLimit`). A rename would touch the web, Android, iOS and macOS
      files more, and those builds could not be compiled for this
      proof of concept.

## Not done

- The web, Android, iOS and macOS builds were not compiled. Their edits
  are small (the global's first value, the web setter, the macOS menu
  loop).
- A message box opened during a game (for example "lost connection")
  runs its own draw loop and keeps the game's vsync state. With
  Unlimited it draws with no limit until it closes. Those boxes are
  short-lived, so this was left as it is.
- No separate vsync switch, and no adaptive vsync
  (`SDL_RENDERER_VSYNC_ADAPTIVE`). Either is easy to add later.
- The new strings are in English only. The other languages fall back to
  English until they are translated.
