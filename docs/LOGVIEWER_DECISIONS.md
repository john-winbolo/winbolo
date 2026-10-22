# Log Viewer: decisions

This file lists the choices made for Log Viewer work while the owner was
away. Each choice can be changed; the file says where.

Note: an earlier file with this name was removed from main on Oct 17
(commit 9df60219c, "a record of the review rounds, not a design
document"). This file was made again only because the Chat panel request
asked for its choices here. Remove it before the merge if it is not wanted.

## Chat panel (branch `logviewer-chat-window`)

### What was there before

- The round log has three chat records. `log_MessageAll` is a line to
  everyone. `log_MessagePlayers` is a line to one player or to the
  sender's team; a team line has a team address (0x81 to 0x90) as its
  destination. `log_SpectatorChat` is a line from a spectator, by number.
- The viewer showed chat only as lines of the Events panel (kind "chat"),
  in the form "Message to all from Ann: hello". Map pings are filed with
  the chat lines there.
- A team line in the Events panel names the destination as a player slot,
  so it shows the empty-slot name. This branch does not change the Events
  panel.

### What the branch adds

- A Chat panel (Windows > Chat, Ctrl+9; Cmd+9 in the macOS menu bar). It is
  a normal panel: it floats or docks in any sidebar, it is in the saved
  layout, and both "Reset Window Positions" items handle it.
- It shows the last chat lines at the playback time, oldest at the top.
  Each line has its time, the sender in the colour of the sender's team,
  a note for a team line ("(team)") or a line to one player ("(to Bob)"),
  and the words.

### Choices

- Where the lines come from: the Events panel's list. Each chat line in
  that list now keeps its parts (sender, note, words), set by the decoder
  with `lv_messageTagChat` (screen.c) and kept by `lvEventListSetChat`
  (lv_event_list.c). The panel and the Events panel can then never give
  different lines. Seeks, the whole-log load and live feeds work as they
  do for the Events panel.
- Map pings are not in the Chat panel. They are not typed text, and the
  map and the Events panel show them.
- Spectator lines are named "Spectator N", as the Events panel's line
  does. Their names are drawn in the text colour, because a spectator has
  no team.
- Team colour: the colour every other panel uses for the team
  (`lv_drawTeamPanelColour` of the Team Colours dialog's entry). The team
  is the sender's team when the line was sent.
- How many lines: the last 10 at or before the playhead
  (`LV_CHAT_MAX_LINES` in lv_chat_view.h). There is no age limit on a
  line: when a new line comes, the lines before it show with it, so a
  reply can be read with what it replies to. Each line has its time, so an
  old line is easy to see.
- With "Hide Lobby" on, lines from before the game start are not listed,
  the same rule as the Events panel.
- Fade times (lv_chat_view.h): the lines stay at full opacity for 8 s of
  replay time after the newest line (`LV_CHAT_HOLD_MS`), then fade out
  over 0.25 s (`LV_CHAT_FADE_MS`). A new line shows at full opacity at
  once. 8 s is enough to read two or three short lines at normal speed.
- The fade follows the replay time only (never the clock on the wall).
  So nothing fades while paused, the fade is faster at fast speeds and
  slower at slow speeds, and after a seek (forward or back) the panel shows
  what that replay time shows.
- What fades: docked in a sidebar, the panel keeps its title and
  background, and only the lines fade. Floating, the whole window fades,
  title and background too, so it does not cover the map when there is
  nothing to read.
- Hover: with the mouse over the panel, the lines are at full opacity, so
  they can be read at any time. A floating window that has faded out still
  takes the mouse where it is, and the hover brings it back at once, so
  the user can see what is there. There is no separate option for this.
- With no chat line to list (none yet at the playhead), the panel shows
  "No chat messages yet." at full opacity, so an opened panel is never
  empty and unseen.
- On by default (`Window.Chat.Visible`, default Yes). The default sidebar
  arrangement ("Reset Window Positions into Dock") puts it in the right
  sidebar under the Events panel, where it shares the free height by
  weight (2, the same as Item Information). A user with a saved layout
  gets it as a floating window, low on the left of the map.
- The panel is in the overview's desktop layout only, like the HUD,
  Overview and Tank panels. The game view has the game's own message
  lines, and the compact layout of a tablet or a Steam Deck has no room.
  It is drawn for a live feed as well as for a log file.
- New strings: 3100 to 3104 (`STR_LV_WIN_CHAT`, `STR_LV_CHAT_NONE`,
  `STR_LV_CHAT_TEAM`, `STR_LV_CHAT_TO`, `STR_LV_CHAT_SPECTATOR`). Other
  open branches use ids up to 3036.

### Where to change it

- Times and line count: `src/logviewer/lv_chat_view.h`.
- The panel: `src/logviewer/imgui/imgui_chat.cpp`.
- The chat parts of a line: `lv_messageTagChat` in `src/logviewer/screen.c`.
- Default arrangement: `src/logviewer/lv_dock_default.h`.
