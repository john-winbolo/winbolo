# In-game voting plan

Two new player-driven votes during a live game: **Back to Lobby**
and **Surrender**. Both reuse the existing `PACKET_MAP_SKIP_VOTE` /
`PACKET_MAP_SKIP_STATE` shape (per-slot toggle, server tracks
`bool votes[MAX_TANKS]`, broadcasts the full state on every change,
transitions when threshold is met).

## Vote kinds

### Back to Lobby
- **Voters**: every alive human player.
- **Threshold**: **unanimous**. A single dissenter blocks the vote;
  the widget shows e.g. `3 / 4` so the player can see who's holding.
- **Trigger sources** (any of):
  1. Manual via the in-game menu (sibling of "Request Alliance").
  2. Auto: server detects one team owns every base in play.
  3. Auto: a surrender vote just passed (see below).
- **Pass effect**: countdown `3`, `2`, `1` system-chat messages
  (bare numerals, one per second, no ellipses), then transition
  `serverStateGameOver -> serverStateLobby`.
- **Ranked-game indicator**: when the game is ranked AND the trigger
  is manual, the widget title carries `(Draw)` so every voter
  understands the result will log as a draw to their ranked record.
  The two auto-triggers have a clear winning team (base monopoly =
  monopolising team wins; post-surrender = non-surrendering team
  wins) and omit the `(Draw)` tag.
- **Widget title per trigger**:
  - Manual (ranked):     `"Vote: Return to lobby (Draw)"`
  - Manual (unranked):   `"Vote: Return to lobby"`
  - Base monopoly:       `"Vote: Return to lobby — <WinningTeam> wins"`
  - Post-surrender:      `"Vote: Return to lobby — <RemainingTeam> wins"`
- **System-message preamble** (only the first line varies; the
  countdown is identical):
  - Manual:        `"Vote to return to lobby started."`
  - Base monopoly: `"Team <Name> controls every base. Returning to lobby on unanimous vote."`
  - Post-surrender: implicit; the surrender announcement printed
                   earlier already explains the cause.

### Surrender
- **Voters**: only members of the surrendering team.
- **Threshold**: **unanimous within the team**. Single dissenter
  blocks; widget shows e.g. `1 / 2`.
- **Pre-condition**: **exactly two teams** are in the game right
  now. Menu item is disabled when not satisfied; hover tooltip on
  the disabled item explains: `"Surrender is only available when
  exactly two teams remain in the game."`
- **Server validation**: server re-checks the two-team condition at
  the moment the vote is cast and at the moment it passes — if a
  third team materialised mid-vote, the surrender vote is
  invalidated and silently closed (rare race).
- **Pass effect**:
  1. Server posts a clearly-formatted, "official" system-chat
     line: `"*** Team <Name> has surrendered. ***"` (or equivalent
     emphasis so it stands out from regular chat).
  2. Server auto-starts a back-to-lobby vote.
  3. The surrendered team's tanks / bases are **mechanically
     unchanged** — surrender is symbolic until the back-to-lobby
     vote also passes. If the back-to-lobby vote fails, the
     surrender stays on record and the game continues.
- **Only one back-to-lobby vote at a time.** If a surrender pass
  triggers a back-to-lobby that one is already active, the
  surrender announcement still fires but the auto-start step
  is skipped.

### Auto-trigger: base monopoly
- Server's per-tick check (alongside existing winner detection)
  looks for "one team owns every base in play AND > 1 team has
  any living members". On the lobby->game transition's rising edge
  of that condition, the server inserts an implicit YES vote for
  every alive human player and broadcasts the back-to-lobby vote
  state as if everyone just clicked the menu item. Players still
  see the widget and can press `No` (toggle their vote off) within
  the 1-minute window — a single dissenter blocks the transition.

## Vote timeout
- **1 minute** from first vote cast on a given vote kind. If
  unanimous isn't reached by the deadline, the vote **fails** and
  the widget dismisses itself.
- Widget shows the remaining time as a small numeric countdown
  alongside the circular progress ring.
- After a vote fails, the menu item remains active — anyone can
  start a new vote of the same kind immediately. (We can add a
  30s per-player same-kind cooldown later if grief becomes a
  problem; not in v1.)

## UI

### Menu items
Sibling rows in the same menu as "Request Alliance":
- `Vote: Return to lobby`
- `Vote: Surrender`

Disabled rules:
- `Vote: Surrender` disabled when not exactly 2 teams in play.
  Hover tooltip on the disabled item explains why.
- `Request Alliance` disabled when the game is ranked. Hover
  tooltip explains.
- `Vote: Return to lobby` always enabled during gameplay.

Clicking a menu item:
- **First click**: starts a new vote (the player's vote is cast YES
  as the first vote) and opens the floating widget for them.
- **While a vote of that kind is already running**: just reopens
  the widget. The player's existing YES/NO/no-vote stays as it
  was. (See "Closed-but-still-running" below.)

### Floating widget
- Anchored next to / over the top-right tank-players HUD
  indicator. Allowed to cover it.
- A **separate top-level window**, NOT a child window of the game
  window. The user can **drag it outside the game window** onto
  another monitor or anywhere on the desktop.
- Window flags: no resize, no scrollbar, no menu — just a small
  draggable popup with title bar / close (X) button.
- **Translucent background** (~50-60% alpha) so the parts of the
  battlefield it covers bleed through and active players can see
  what's happening behind it.
- **Body contents**:
  - Title row: vote kind + tally `3 / 4`. For ranked-manual
    back-to-lobby, title includes `(Draw)`.
  - Below title: circular progress ring (~36 px) — `progress =
    yes_count / threshold`. Borrow the spinner / progress drawing
    code from the existing `imgui_mapchooser` "Loading preview..."
    or any of the editor's progress widgets (`PathArcTo` on the
    window draw list).
  - Beside the ring: small numeric countdown of remaining seconds
    until timeout (`52s`, `51s`, ...).
  - Below: **Yes** and **No** buttons. The widget **stays visible
    after the user presses one**, so they keep seeing tally
    progress. Their own button stays highlighted to show which
    side they're on.
  - Title-bar X (close): see "Closed-but-still-running" below.
- Multiple concurrent votes (e.g. surrender vote and back-to-lobby
  running at the same time) get **separate widget windows**, each
  draggable independently.

### Closed-but-still-running
Pressing X on a widget **does NOT cancel the vote**. It just hides
the widget for that player who pressed it. The vote keeps running
server-side. To re-show:
- Click the same menu item again. Interpretation: pressing the
  menu item means "I want to initiate a vote of this kind"; if
  one is already running, the request collapses to just reopening
  the widget. The player's existing vote (yes / no / unset) is
  preserved.

Rationale: the widget can be distracting in the middle of a tense
firefight. Players who've already cast their vote and don't care
about the progress can dismiss; players who want to retract their
vote / change their mind can reopen via the menu.

## Anti-distraction
- Widget is translucent over the game viewport.
- Auto-fade-out 3 seconds after the vote concludes (pass, fail, or
  timeout — whichever).
- Same translucency treatment for the existing Request Alliance
  popup so dialogs in general don't fully blind active players.

## Ranked-game interaction
- Already wired (`LST_RANKED`): bots banned, gameType=Open banned,
  allowNewPlayers locked closed once the game starts.
- This plan adds:
  - **Alliances disabled** when ranked. Client greys out the menu
    item with a tooltip; server-side rejects `PACKET_ALLIANCE_REQUEST`
    when ranked (load-bearing).
  - **Back-to-lobby (Draw) tag** when manually triggered in a
    ranked game (see above).
  - Otherwise the vote mechanics are identical to unranked.

## Edge cases
- *Player drops mid-vote*: their vote slot zeroes AND the threshold
  shrinks (re-derived on every state change as `count_alive_humans`
  or `count_living_team_members`). A vote that was just shy of
  unanimous can suddenly pass when its last dissenter quits — fine,
  fires the countdown immediately.
- *Player joins mid-vote* (live game w/ allowNewPlayers): new
  joiner defaults to "not voted." Their slot is added to the
  threshold immediately. UX: their widget pops up next time they
  open the menu, but isn't forced on them.
- *Vote race* (surrender passes 1ms before base-monopoly
  triggers): surrender takes precedence. Surrender's announcement
  + back-to-lobby auto-start happen first. Base-monopoly trigger
  notices the back-to-lobby vote is already running and skips its
  own auto-start.
- *Surrender pass while back-to-lobby running*: surrender's
  announcement still fires. The back-to-lobby vote is unchanged.
- *Third team appears between surrender vote start and pass*: the
  surrender vote is invalidated, widget closes server-side, and
  a system message logs the invalidation reason (`"Surrender vote
  cancelled — third team has entered."`).
- *Replays*: vote state should be replay-logged so it can be
  reconstructed for spectators / playback.

## Protocol sketch

New packets:

```
PACKET_GAME_VOTE_TOGGLE  (client -> server)
  { kind 1, on 1 }
    kind   : 1 = back_to_lobby, 2 = surrender
    on     : 0 = no / withdraw, 1 = yes
    on=2   : reopen-widget-only (no vote change) — for the menu-
             item-while-running case

PACKET_GAME_VOTE_STATE  (server -> client)
  { kind 1,
    active 1,            -- 0 = no vote running, 1 = running, 2 = passed,
                            3 = failed, 4 = cancelled
    triggerSrc 1,        -- 0=manual, 1=baseMonopoly, 2=postSurrender
    teamId 1,            -- only meaningful for surrender; 0 = all-teams vote
    threshold 1,
    yesCount 1,
    secondsRemaining 1,  -- 1-byte ceil of remaining time (0..60)
    votes 16 }           -- bitmask of slots that voted yes
```

`PACKET_GAME_VOTE_STATE` is broadcast on every state change AND
once per second while a vote is running (drives the countdown).

## What I'd ship in v1 vs defer

v1:
- Back-to-lobby vote (manual + base-monopoly + post-surrender).
- Surrender vote (2-team gate, team-scoped unanimous).
- Floating draggable translucent widget.
- 1-minute timeout, X-to-close, menu-to-reopen.
- 3/2/1 system-chat countdown.
- (Draw) tag on manual back-to-lobby in ranked.
- Alliance disable in ranked (client + server).

v2 / later:
- 30s same-kind cooldown anti-grief.
- Replay logging of vote state.
- Widget appear / dismiss animations.
- Sound effect on countdown ticks.
