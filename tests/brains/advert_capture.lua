-- advert_capture.lua — a scripted ALLY that only ever talks.
--
-- Companion to tests/ally_capture_guard_test.py variants C and D.  It never
-- moves; its whole job is to put a controlled `/info state` slate on the
-- internal bot channel so the bot under test's ally_state slot for this slot
-- says exactly what the test wants, at exactly the tick the test wants.
--
-- WHY A SCRIPT AND NOT A SECOND GoalHunter.  Variant A already uses a real 1.7
-- ally, which is the honest end-to-end case but gives no control over WHEN the
-- advert changes.  The two expiry rules are about timing and nothing else:
--
--   MOVE-ON  the ally's next slate names a different goal, and the block must
--            lift on that tick whatever its age.
--   SILENCE  the ally stops talking, and the block must survive until
--            BUILDER_POOL_ALLY_CAPTURE_TTL ticks after its LAST message.
--
-- A real bot changes its goal when it feels like it.  This one changes it on
-- the tick it is told to.
--
-- messagedest = 0 is the INTERNAL bot channel (brain_data.c): the string is
-- fanned into every ALLIED hosted bot's inbox and never touches the chat wire,
-- which is the same channel GoalHunter's own /info verbs ride.  Alliance comes
-- from the team map (-teams / game.set_team), so this brain must be on the
-- receiver's team or nothing is delivered at all.
--
-- The advert deliberately carries mx/my and NOT a target id: that is the
-- coordinate half of the guard's matching rule (init.lua only spends bytes on
-- the tile when the goal has no object id).  Variant A's real 1.7 ally covers
-- the id half.
--
-- BRAIN_INIT_ARG tokens (';' separated — the scenario's spawn_bot init string
-- and a CLI [..] suffix both split on ',' or ';'):
--   mx=<n> my=<n>     the tile to advertise capture_pill on   (required)
--   switch=<n>        at this THINK number, change the advert to `goal=explore`
--                     (the MOVE-ON case).  Omitted/0 = never switch.
--   every=<n>         resend cadence in thinks (default 50 = ~1 s)
--
-- print2 is not available here (that is a GoalHunter module), so the brain
-- reports through plain print(), which lands in the server's stdout.

-- A scenario that spawns this brain hands it its `spawn_bot{ init = {...} }`
-- table as the global BRAIN_INIT. This brain reads BRAIN_INIT_ARG, the
-- "k=v;k=v" string, so flatten the table into that string before anything
-- parses it. Same rule as brains/GoalHunter_1.7/init.lua: keys sorted so the
-- string is the same every time, a value of "1" or true becoming the bare
-- word, "0" or false dropped, everything else staying k=v. A string already
-- in BRAIN_INIT_ARG -- the command-line path -- keeps its place.
do
  local t = rawget(_G, "BRAIN_INIT")
  if type(t) == "table" then
    local keys = {}
    for k, v in pairs(t) do
      if type(k) == "string" and k ~= "" and v ~= nil then keys[#keys + 1] = k end
    end
    table.sort(keys)
    local toks = {}
    for _, k in ipairs(keys) do
      local v = t[k]
      if type(v) ~= "boolean" then v = tostring(v) end
      if v == true or v == "1" or v == "" then
        toks[#toks + 1] = k
      elseif v == false or v == "0" then
        -- off: no token
      else
        toks[#toks + 1] = k .. "=" .. v
      end
    end
    if #toks > 0 then
      local a = rawget(_G, "BRAIN_INIT_ARG")
      local flat = table.concat(toks, ";")
      if type(a) == "string" and a ~= "" then flat = a .. ";" .. flat end
      rawset(_G, "BRAIN_INIT_ARG", flat)
    end
  end
end

local brain = {}

local mx, my = nil, nil
local switch_at = 0
local every = 50
local n = 0
local said_switch = false

local function parse_arg()
  local a = rawget(_G, "BRAIN_INIT_ARG")
  if type(a) ~= "string" then return end
  for tok in a:gmatch("[^,;]+") do
    tok = tok:gsub("%s", "")
    local k, v = tok:match("^(%a+)=(-?%d+)$")
    if k == "mx" then mx = tonumber(v)
    elseif k == "my" then my = tonumber(v)
    elseif k == "switch" then switch_at = tonumber(v)
    elseif k == "every" then every = tonumber(v) end
  end
end

function brain.open(info)
  parse_arg()
  print(string.format(
    "[advert_capture] p%s open: mx=%s my=%s switch=%s every=%s",
    tostring(info.player_number), tostring(mx), tostring(my),
    tostring(switch_at), tostring(every)))
end

function brain.think(info)
  n = n + 1
  local out = { holdkeys = 0, tapkeys = 0 }
  -- One message per `every` thinks. Each /info state REPLACES the receiver's
  -- slate for this slot (ally_state.set_info clears every key not present), so
  -- the switch below needs no explicit "clear the old target" step.
  if mx and my and (n % every) == 0 then
    local msg
    if switch_at > 0 and n >= switch_at then
      -- MOVE-ON: a different goal, still talking. The receiver's slot stays
      -- fresh, so only the "latest advert no longer names that pill" rule can
      -- lift the block -- which is the point of this variant.
      msg = "/info state goal=explore sub=wander"
      if not said_switch then
        said_switch = true
        print(string.format("[advert_capture] p%s think=%d SWITCHED to explore",
                            tostring(info.player_number), n))
      end
    else
      msg = string.format("/info state goal=capture_pill sub=collect mx=%d my=%d",
                          mx, my)
    end
    out.sendmessage = msg
    out.messagedest = 0
  end
  return out
end

function brain.close(info)
  print(string.format("[advert_capture] p%s closed after %d thinks",
                      tostring(info.player_number), n))
end

return brain
