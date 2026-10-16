# Notes for AI agents working in this repository

## Bots must not cheat

A bot may act only on what its player could see in the game. No bot, and no
scenario script that steers bots, may reach into another bot's thoughts.

- A brain decides from the world it is given: the map, the pillboxes and bases
  it can see, tanks in view, its own stocks, chat, and orders sent to it.
- A brain never reads another bot's goal, plan, path, targets, timers or other
  brain state.
- A scenario script is the referee and knows everything, but when it sets a
  bot's knobs, init table, hints or orders, it uses only facts that bot's
  player could see. Example: in Pillbox Tag, a hop prize and a built prize
  that stays built look the same on the map, so the hunters must be treated
  the same for both. The holder's private plan must not change what they do.
- The holder's own bot may use the holder's own plan.
- Bots share what they know only through communication: chat, pings and
  bot commands that a human could send and receive too. Communication works
  only between allies. An enemy never gets another bot's thoughts by any
  route.

What the engine gives every bot is accepted and is not a cheat: the brain's
view window (larger than a human screen), stock readings of bases it sees,
sound positions, line of sight under the Sight preset, the AI advantage lobby
modes, and the ally-only internal bot channel. The rule is about one bot, or a
script, reaching into another bot's thoughts, or a script feeding a bot facts
the bot itself could not see (for example a tank hidden in forest).

When you find a cheat, remove it outright. Do not put it behind a knob or a
preset. When you write new bot or script logic, check that every input is
something that player could see.

See also "Fair play" in docs/SCENARIO_API.md.
