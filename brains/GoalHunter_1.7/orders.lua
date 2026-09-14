-- =========================================================================
-- GoalHunter/orders.lua — CHAT ORDERS (bot commands, stage 1)
--
-- A human ally types "attack 5" / "all defend base 3" / "socrates retreat"
-- in team (or all) chat.  Every bot on the team reads the SAME line, scores
-- itself, and the cheapest one takes the job.  No server ranking and no
-- coordinator: the auction runs entirely in the brains, on its own /info
-- verbs, so a recorded game replays the same way.
--
-- Wire verbs (internal channel, msg_dest 0 — never seen by a human):
--   /info obd <oid> <cost>   BID.   cost -1 means "no, I'm busy".
--   /info obc <oid> <cost>   CLAIM. the winner (or a stealer) holds it.
--   /info obr <oid>          RELEASE. re-bid it among the rest.
-- They are their own verbs, NOT fields on the /info state slate: that slate
-- is already close to the 124-byte batch budget (C.MSG_BATCH_MAX) and order
-- traffic is bursty, so it rides beside the reposition-vote verbs instead
-- (same outbox + try_send pattern, see reposition_vote.lua).
--
-- ORDER ID: derived from (sender, verb, who, target) ONLY — no tick.  Every
-- bot that hears the line computes the same number without talking to
-- anyone, so bids match.  A tick bucket would NOT be safe here: brains think
-- every 2 game ticks on their own offset, so two bots can read the same chat
-- line on different ticks and a bucket boundary between them would fork the
-- id.  The price of leaving the tick out is that the SAME sender repeating
-- the SAME line produces the same id; that is treated as a refresh of the
-- 60 s focus (which is what a repeated order means anyway).
--
-- Acks go to the ALLIES mask, not the internal channel, so a human (or a
-- test seat) actually sees them.
-- =========================================================================

local C          = require("constants")
local U          = require("util")
local cpf        = require("cpathfinder")
local ally_state = require("ally_state")
local print2     = require("print2")
local bit        = require('bitcompat')

local M = {}

-- -------------------------------------------------------------------------
-- Bot name pools — a verbatim copy of the built-in lists in
-- src/bolo/lobby_bot_pools.c.  The brain finds ITS OWN pool by matching its
-- own name; that is all the pool is used for (picking the ack flavour).
-- Anything not in these lists (numbered bots, custom pools, a human) = plain.
-- Ordered array, never a hash: pairs() order is process-seeded and the pick
-- has to be identical in a replay.
-- -------------------------------------------------------------------------
M.NAME_POOLS = {
  { key = "classic", names = {
    "hal-9000","glados","skynet","wintermute","cortana",
    "marvin","bender","tars","case","roy","sonny","ash" } },
  { key = "painters", names = {
    "van gogh","picasso","monet","rembrandt","dali",
    "vermeer","klimt","cezanne","matisse","kahlo","pollock","warhol" } },
  { key = "musicians", names = {
    "mozart","beethoven","hendrix","bowie","prince",
    "aretha","coltrane","bach","miles","joplin","lennon","cash" } },
  { key = "martial", names = {
    "bruce lee","jackie","jet li","donnie yen","mas oyama",
    "ip man","funakoshi","ueshiba","helio","sonny chiba","royce","khabib" } },
  { key = "scientists", names = {
    "einstein","newton","curie","tesla","darwin",
    "hawking","feynman","turing","hopper","lovelace","sagan","rosalind" } },
  { key = "philosophers", names = {
    "socrates","plato","nietzsche","kant","sartre",
    "aristotle","confucius","hume","camus","beauvoir","marcus","hypatia" } },
  { key = "generals", names = {
    "hannibal","caesar","napoleon","patton","sun tzu",
    "zhukov","saladin","alexander","rommel","khan","boudica","shaka" } },
  { key = "callsigns", names = {
    "maverick","goose","iceman","viper","slider",
    "hollywood","wolfman","merlin","sundown","stinger","ghost rider","charlie" } },
}

-- One or two words before the goal name, never more (design doc).
M.ACKS = {
  classic      = { "Affirmative.", "Acknowledged.", "Processing.", "Compliance.", "I'm afraid I can." },
  painters     = { "Sketching it.", "Bold strokes.", "On canvas.", "Framed.", "Brush up." },
  musicians    = { "On the beat.", "Encore!", "In tune.", "Solo time.", "Playing it." },
  martial      = { "Hai!", "Osu!", "Hiyah!", "Focus.", "Be water." },
  scientists   = { "Eureka!", "Hypothesis:", "Testing it.", "Calculated.", "Observing." },
  philosophers = { "Indeed.", "It follows.", "So be it.", "Necessarily.", "Quite." },
  generals     = { "Advance!", "Charge!", "Orders received.", "To arms!", "Forward." },
  callsigns    = { "Copy.", "Roger.", "Wilco.", "Tally ho.", "Locked on." },
  plain        = { "Got it!", "On it!", "Yes!", "Okay.", "Sure." },
}

-- Which pool a name belongs to ("plain" when it is in none of them).
function M.ack_pool(name)
  if type(name) ~= "string" or name == "" then return "plain" end
  local l = name:lower()
  for i = 1, #M.NAME_POOLS do
    local p = M.NAME_POOLS[i]
    for j = 1, #p.names do
      if p.names[j] == l then return p.key end
    end
  end
  return "plain"
end

-- math.random IS the brain's seeded RNG (same source squad.lua's suicider
-- designation draws from), so a -brain-lua-seed run picks the same ack.
function M.ack_for(name)
  local list = M.ACKS[M.ack_pool(name)] or M.ACKS.plain
  return list[math.random(1, #list)]
end

-- =========================================================================
-- NAME MATCHING
-- Case-insensitive; a unique prefix matches; any WORD of a multi-word name
-- matches; typos match at edit distance <= 2.  Tiered: the first tier that
-- produces any candidate decides, so an exact name never loses to a fuzzy
-- one.  Two candidates in the winning tier => ambiguous, and the caller
-- answers "X or Y?" and does nothing.  Never guess.
-- =========================================================================

-- Levenshtein, early-out when the lengths alone are further apart than the
-- cap (the only caller wants <= 2).
local function edist(a, b, cap)
  local la, lb = #a, #b
  if math.abs(la - lb) > cap then return cap + 1 end
  local prev, cur = {}, {}
  for j = 0, lb do prev[j] = j end
  for i = 1, la do
    cur[0] = i
    local ca = a:byte(i)
    for j = 1, lb do
      local m = prev[j] + 1
      local d = cur[j - 1] + 1
      if d < m then m = d end
      d = prev[j - 1] + ((ca == b:byte(j)) and 0 or 1)
      if d < m then m = d end
      cur[j] = m
    end
    prev, cur = cur, prev
  end
  return prev[lb]
end

local function name_words(n)
  local out = {}
  for w in n:gmatch("[%w%-']+") do out[#out + 1] = w end
  return out
end

-- roster: array of { pn = <player number>, name = <display name> }.
-- Returns pn                       on a unique match
--         nil, "unknown"           on no match
--         nil, "ambiguous", {a, b} on two or more in the same tier
function M.match_name(word, roster)
  if type(word) ~= "string" or word == "" or type(roster) ~= "table" then
    return nil, "unknown"
  end
  local w = word:lower()
  local tiers = { {}, {}, {} }   -- 1 exact, 2 prefix, 3 fuzzy
  for i = 1, #roster do
    local e = roster[i]
    local nm = (e.name or ""):lower()
    if nm ~= "" then
      local hit = nil
      if nm == w then
        hit = 1
      else
        local ws = name_words(nm)
        for k = 1, #ws do if ws[k] == w then hit = 1 break end end
        if not hit and #w > 0 and nm:sub(1, #w) == w then hit = 2 end
        if not hit then
          for k = 1, #ws do
            if #w > 0 and ws[k]:sub(1, #w) == w then hit = 2 break end
          end
        end
        if not hit and #w >= 3 then
          if edist(w, nm, 2) <= 2 then
            hit = 3
          else
            for k = 1, #ws do
              if #ws[k] >= 3 and edist(w, ws[k], 2) <= 2 then hit = 3 break end
            end
          end
        end
      end
      if hit then
        local t = tiers[hit]
        local dup = false
        for k = 1, #t do if t[k].pn == e.pn then dup = true break end end
        if not dup then t[#t + 1] = e end
      end
    end
  end
  for t = 1, 3 do
    local list = tiers[t]
    if #list == 1 then return list[1].pn end
    if #list >= 2 then
      -- Stable order so the "X or Y?" reply is the same in a replay.
      table.sort(list, function(a, b) return (a.pn or 0) < (b.pn or 0) end)
      return nil, "ambiguous", { list[1].name, list[2].name }
    end
  end
  return nil, "unknown"
end

-- =========================================================================
-- PARSER — `[who] verb [target]`
-- Only lines that start with a verb, a who-word, `!` or a bot name are read;
-- anything else is ordinary chat and is ignored without a reply.
-- =========================================================================

local VERBS = {
  attack = "attack", capture = "capture", sweep = "capture",
  defend = "defend", decoy = "decoy", retreat = "retreat",
  cancel = "cancel",
}
local WHOWORDS = { all = true, nearby = true }
local PILLWORDS = { pill = true, pills = true, pillbox = true, pillboxes = true }
local BASEWORDS = { base = true, bases = true }
-- Team-wide SETTINGS, not orders: no target, no auction and no 60 s timer,
-- and `cancel all` does not touch them.  `take` is the focus alias.
local SETTINGWORDS = {
  focus = true, take = true, reposition = true, repositioning = true,
  help = true,
}

-- roster = ally BOTS (for `who` and for `cancel <bot name>`)
-- all_roster = every named player (for `attack <tank name>`); defaults to roster.
-- Returns one of:
--   { verb=, who={mode=,pns=}, target={kind=,id=|pn=}, forced= }
--   { select = {pn, ...} }          a line of only bot names
--   { clear_select = true }         "nevermind" / "never mind"
--   { reply = "..." }               something to say, no order
--   nil                             not for us, stay silent
function M.parse(text, roster, all_roster)
  if type(text) ~= "string" then return nil end
  all_roster = all_roster or roster
  local s = text:match("^%s*(.-)%s*$")
  if s == "" or s:sub(1, 1) == "/" then return nil end
  local forced = false
  if s:sub(1, 1) == "!" then
    forced = true
    s = s:sub(2):match("^%s*(.-)%s*$")
  end
  s = s:lower()
  local toks = {}
  for w in s:gmatch("[%w%-']+") do toks[#toks + 1] = w end
  if #toks == 0 then return nil end

  -- ── "!goto <mx> <my>" — THREE SHOTS = GO THERE ───────────────────────
  -- Three of one player's shells that run their full range and land on the
  -- same open square inside two seconds are an order: go there and hold.
  -- The SERVER spots the pattern and injects this line with the SHOOTER as
  -- the sender, so the team check every order runs through is the shooter's
  -- team, exactly as for a typed line.
  --
  -- FORCED FORM ONLY.  Players have no coordinates to type, so a bare
  -- "goto 40 40" is somebody's chatter and never an order: without the "!"
  -- the line reaches the start-of-line test below, matches no verb and no
  -- name, and is ignored in silence.
  --
  -- who.mode is "ping", which gives this order the SAME id a bot ping on
  -- that square from the same sender derives (sender | goto | ping | here |
  -- tile), so every bot agrees on it without exchanging anything.
  -- who.near is the ten-tile rule: a bot further from the square than that
  -- does not bid, and nobody in range means nothing happens and no bot
  -- speaks.
  if forced and toks[1] == "goto" then
    local mx, my = tonumber(toks[2]), tonumber(toks[3])
    if #toks ~= 3 or not mx or not my
       or mx < 0 or mx > 255 or my < 0 or my > 255 then
      return { reply = "didn't understand" }
    end
    mx, my = math.floor(mx), math.floor(my)
    return { verb = "goto",
             who = { mode = "ping", near = C.ORDER_NEARBY_TILES or 10 },
             target = { kind = "here", id = mx * 256 + my, mx = mx, my = my },
             forced = true }
  end

  -- Only lines that START with a verb, a who-word, `!` or a bot name are
  -- read; everything else is ordinary chat and is ignored in silence. Without
  -- this a bot's own ack ("Got it! attack_pill #5") would be answered with
  -- "didn't understand", because it happens to contain a verb word.
  if not forced then
    local p1, e1 = M.match_name(toks[1], roster)
    if not VERBS[toks[1]] and not WHOWORDS[toks[1]] and not SETTINGWORDS[toks[1]]
       and toks[1] ~= "nevermind" and toks[1] ~= "never"
       and p1 == nil and e1 ~= "ambiguous" then
      return nil
    end
  end

  -- ── Team-wide settings and help ───────────────────────────────────────
  -- Checked BEFORE the verb scan so "focus pills" is never read as a pill
  -- target and "reposition on" never reaches the order machinery.
  if toks[1] == "help" then
    if #toks == 1 then return { help = true } end
    return { reply = "didn't understand" }
  end
  if toks[1] == "focus" or toks[1] == "take" then
    local w = toks[2]
    if BASEWORDS[w] then return { setting = "focus", value = "bases" } end
    if PILLWORDS[w] then return { setting = "focus", value = "pills" } end
    if w == "off" or w == "none" then return { setting = "focus", value = "off" } end
    return { reply = "didn't understand" }
  end
  if toks[1] == "reposition" or toks[1] == "repositioning" then
    if toks[2] == "on"  then return { setting = "reposition", value = true  } end
    if toks[2] == "off" then return { setting = "reposition", value = false } end
    return { reply = "didn't understand" }
  end

  local vi, verb
  for i = 1, #toks do
    if VERBS[toks[i]] then vi, verb = i, VERBS[toks[i]] break end
  end

  -- ── No verb: a selection line, "nevermind", or noise ──────────────────
  if not vi then
    if toks[1] == "nevermind" or (toks[1] == "never" and toks[2] == "mind") then
      return { clear_select = true }
    end
    local pns, ok = {}, true
    for i = 1, #toks do
      local pn, err, cands = M.match_name(toks[i], roster)
      if pn then
        pns[#pns + 1] = pn
      elseif err == "ambiguous" then
        return { reply = cands[1] .. " or " .. cands[2] .. "?" }
      else
        ok = false
        break
      end
    end
    if ok and #pns > 0 then return { select = pns } end
    -- A known shape with an unknown verb still deserves an answer.
    local p1 = M.match_name(toks[1], roster)
    if forced or WHOWORDS[toks[1]] or p1 ~= nil then
      return { reply = "didn't understand" }
    end
    return nil
  end

  -- ── who ───────────────────────────────────────────────────────────────
  local who = { mode = "auto" }
  if vi > 1 then
    if vi == 2 and WHOWORDS[toks[1]] then
      who = { mode = toks[1] }
    else
      local pns = {}
      for i = 1, vi - 1 do
        local pn, err, cands = M.match_name(toks[i], roster)
        if pn then
          pns[#pns + 1] = pn
        elseif err == "ambiguous" then
          return { reply = cands[1] .. " or " .. cands[2] .. "?" }
        else
          return { reply = "didn't understand" }
        end
      end
      who = { mode = "names", pns = pns }
    end
  end

  -- ── target ────────────────────────────────────────────────────────────
  local tt = {}
  for i = vi + 1, #toks do tt[#tt + 1] = toks[i] end
  local tgt = nil
  if #tt > 0 then
    if PILLWORDS[tt[1]] then
      local id = tonumber(tt[2])
      if not id then return { reply = "didn't understand" } end
      tgt = { kind = "pill", id = id }
    elseif BASEWORDS[tt[1]] then
      local id = tonumber(tt[2])
      if not id then return { reply = "didn't understand" } end
      tgt = { kind = "base", id = id }
    elseif tonumber(tt[1]) then
      tgt = { kind = "pill", id = tonumber(tt[1]) }   -- a bare number means a pill
    elseif verb == "cancel" and tt[1] == "all" then
      tgt = { kind = "all" }
    else
      local joined = table.concat(tt, " ")
      local pn, err, cands = M.match_name(joined, (verb == "cancel") and roster or all_roster)
      if not pn and err ~= "ambiguous" then
        pn, err, cands = M.match_name(tt[1], (verb == "cancel") and roster or all_roster)
      end
      if pn then
        tgt = { kind = "tank", pn = pn }
      elseif err == "ambiguous" then
        return { reply = cands[1] .. " or " .. cands[2] .. "?" }
      else
        return { reply = "didn't understand" }
      end
    end
  end

  if verb == "decoy" then return { reply = "decoy: not yet" } end
  if (verb == "attack" or verb == "capture" or verb == "defend") and not tgt then
    return { reply = "didn't understand" }
  end
  return { verb = verb, who = who, target = tgt, forced = forced }
end

-- =========================================================================
-- ORDER IDENTITY + GOAL MAPPING
-- =========================================================================

-- djb2 over the canonical order key.  Kept under 8 decimal digits so the
-- wire verbs stay short.
function M.order_id(sender, cmd)
  local w = cmd.who or {}
  local wk = w.mode or "auto"
  if w.pns then
    local c = {}
    for i = 1, #w.pns do c[i] = w.pns[i] end
    table.sort(c)
    wk = wk .. ":" .. table.concat(c, ",")
  end
  local t = cmd.target or {}
  local key = string.format("%d|%s|%s|%s|%s", sender or 0, cmd.verb or "",
                            wk, t.kind or "-", tostring(t.id or t.pn or "-"))
  local h = 5381
  for i = 1, #key do h = (h * 33 + key:byte(i)) % 16777213 end
  return h
end

-- Verb + target + world -> the goal kind the brain will actually run.
-- Returns kind, needs_shells, target_id, err
function M.goal_kind(cmd, world, info)
  local t = cmd.target
  local v = cmd.verb
  if v == "retreat" then return "take_cover", false, nil end
  -- "GO THERE AND HOLD" (a bot ping on open ground).  REUSED MECHANISM:
  -- take_cover, with the pinged tile substituted for find_cover_tile's pick.
  -- take_cover already means "drive to this tile and stop there", it already
  -- has a margin waiver for an ordered retreat, and it is the only existing
  -- goal that holds a position without owning a pill or a base.  The tile is
  -- packed into the target id as mx * 256 + my so one number identifies it on
  -- the wire and in the order id.
  if v == "goto" then
    if t and t.kind == "here" then return "take_cover", false, t.id end
    return nil, false, nil, "didn't understand"
  end
  if v == "cancel" then return nil, false, nil end
  if not t then return nil, false, nil, "didn't understand" end

  if t.kind == "tank" then
    if v ~= "attack" then return nil, false, nil, "didn't understand" end
    return "attack_tank", true, t.pn
  end

  if t.kind == "pill" then
    local p = world.pills[t.id]
    if not p then return nil, false, nil, string.format("pill #%d not known yet", t.id) end
    if v == "attack" then
      -- A dead pill cannot be shot; attacking it means picking it up.
      if (p.health or 0) == 0 then return "capture_pill", false, t.id end
      return "attack_pill", true, t.id
    elseif v == "capture" then
      return "capture_pill", ((p.health or 0) > 0), t.id
    elseif v == "defend" then
      return "defend_pill", true, t.id
    end
    return nil, false, nil, "didn't understand"
  end

  if t.kind == "base" then
    local b = world.bases[t.id]
    if not b then return nil, false, nil, string.format("base #%d not known yet", t.id) end
    if v == "attack" then
      if b.owner == "neutral" then return "capture_base", false, t.id end
      return "attack_base", true, t.id
    elseif v == "capture" then
      if b.owner == "neutral" then return "capture_base", false, t.id end
      return "capture_base", true, t.id       -- a LIVE base needs shells
    elseif v == "defend" then
      -- There is no defend_base goal.  Defending a base means holding the
      -- friendly pill that covers it; with no such pill there is nothing
      -- for the brain to do, so say so rather than invent a goal.
      local best, bid = nil, nil
      for id, p in pairs(world.pills or {}) do
        if p.owner == "friendly" and (p.health or 0) > 0 then
          local d = U.mdist(p.mx, p.my, b.mx, b.my)
          if d <= (C.ORDER_NEARBY_TILES or 10) and (not best or d < best) then
            best, bid = d, id
          end
        end
      end
      if bid then return "defend_pill", true, bid end
      return nil, false, nil, string.format("nothing to defend at base #%d", t.id)
    end
  end
  return nil, false, nil, "didn't understand"
end

-- Map tile the order points at (for travel cost and the `nearby` test).
function M.target_tile(world, state, ord)
  if ord.tkind == "pill" then
    local p = world.pills[ord.tid]
    if p then return p.mx, p.my end
  elseif ord.tkind == "base" then
    local b = world.bases[ord.tid]
    if b then return b.mx, b.my end
  elseif ord.tkind == "tank" then
    local perc = state.perc
    local list = (perc and perc.enemy_tanks) or {}
    for i = 1, #list do
      if list[i].id == ord.tid then return list[i].mx, list[i].my end
    end
    list = (perc and perc.ghost_tanks) or {}
    for i = 1, #list do
      if list[i].id == ord.tid then return list[i].mx, list[i].my end
    end
  elseif ord.tkind == "here" then
    return ord.mx, ord.my
  end
  return nil, nil
end

-- Travel price for the auction.  Dijkstra slate first (the same surface the
-- goal pools bid on), straight line only when no slate has reached the tile.
function M.travel_cost(state, world, info, ord)
  local mx, my = M.target_tile(world, state, ord)
  if not mx then return nil end
  local boat = info.inboat and 1 or 0
  local best = math.huge
  for dy = -1, 1 do
    for dx = -1, 1 do
      local c = cpf.smart_cost_dij_only(cpf.KIND_NORMAL, mx + dx, my + dy, boat)
      if c and c < best then best = c end
    end
  end
  if best >= 1e29 then
    local tmx = bit.rshift(info.tankx or 0, 8)
    local tmy = bit.rshift(info.tanky or 0, 8)
    best = U.mdist(tmx, tmy, mx, my) * (C.ORDER_TILE_COST or 4)
  end
  return best
end

-- =========================================================================
-- STATE
-- =========================================================================

local function S(state)
  local o = state.orders
  if not o then
    o = { auctions = {}, known = {}, claims = {}, gclaims = {}, announce = {},
          out = {}, say = {}, rx = {}, held = nil, sel = nil,
          -- stage 2: team-wide latches + the ping anchors
          focus = nil,        -- nil | "bases" | "pills"  (nil = off)
          repo_on = nil,      -- nil = no override | true | false
          banner = nil, latch_tx = 0, anchors = {} }
    state.orders = o
  end
  return o
end

local function tx(state, msg)
  local o = S(state)
  o.out[#o.out + 1] = msg
end

-- Queue one human-facing line. The queue holds 6 lines and drains one per
-- think (init.lua gives it the chat slot ahead of the internal traffic).
-- When it overflows we drop the OLDEST line, not the newest: a fresh ack is
-- the line the player is waiting for, and a stale "Busy" from ten seconds
-- ago must never keep it out.
local function say(state, line)
  local o = S(state)
  o.say[#o.say + 1] = line
  while #o.say > 6 do table.remove(o.say, 1) end
end

-- Roster of allied BOTS (who-words and cancel target).  Self included.
function M.bot_roster(state, info)
  local out = {}
  local allies = info.allies or 0
  local bots   = info.player_bots or 0
  local names  = info.player_names or {}
  for pn = 0, 15 do
    local nm = names[pn + 1]
    if nm and nm ~= "" then
      local is_self = (pn == state.player_number)
      local is_ally = is_self or bit.band(allies, bit.lshift(1, pn)) ~= 0
      local is_bot  = is_self or bit.band(bots, bit.lshift(1, pn)) ~= 0
      if is_ally and is_bot then out[#out + 1] = { pn = pn, name = nm } end
    end
  end
  return out
end

function M.all_roster(info)
  local out = {}
  local names = info.player_names or {}
  for pn = 0, 15 do
    local nm = names[pn + 1]
    if nm and nm ~= "" then out[#out + 1] = { pn = pn, name = nm } end
  end
  return out
end

local function my_name(state, info)
  return state.player_name
         or (info.player_names and info.player_names[(state.player_number or 0) + 1])
         or "bot"
end

local function player_name(info, pn)
  return (info.player_names and info.player_names[(pn or 0) + 1]) or ("p" .. tostring(pn))
end

-- =========================================================================
-- INBOUND /info obd | obc | obr  (called from comms.process_message)
-- =========================================================================
function M.rx(sender, text, tick, state)
  local oid, cost = text:match("^/info obd (%d+) (%-?%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "bid", oid = tonumber(oid), cost = tonumber(cost),
                        from = sender, tick = tick }
    print2(string.format("ORDER_RX obd from p%s oid=%s cost=%s t=%d", tostring(sender), oid, cost, tick))
    return true
  end
  oid, cost = text:match("^/info obc (%d+) (%-?%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "claim", oid = tonumber(oid), cost = tonumber(cost),
                        from = sender, tick = tick }
    print2(string.format("ORDER_RX obc from p%s oid=%s cost=%s t=%d", tostring(sender), oid, cost, tick))
    return true
  end
  -- Team-wide LATCHES.  One bot decides (it heard the chat line), every bot
  -- latches the same value; the speaking bot also re-broadcasts them every
  -- ORDER_LATCH_REBROADCAST_TICKS so a bot that joined or respawned late
  -- catches up.  Their own verbs, like the order bids: the /info state slate
  -- is already close to the 124-byte batch budget (C.MSG_BATCH_MAX).
  local f = text:match("^/info obf (%d)$")
  if f then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "focus", value = tonumber(f), from = sender, tick = tick }
    print2(string.format("ORDER_RX obf from p%s v=%s t=%d", tostring(sender), f, tick))
    return true
  end
  local r = text:match("^/info obp (%d)$")
  if r then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "repo", value = tonumber(r), from = sender, tick = tick }
    print2(string.format("ORDER_RX obp from p%s v=%s t=%d", tostring(sender), r, tick))
    return true
  end
  oid = text:match("^/info obr (%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "release", oid = tonumber(oid), from = sender, tick = tick }
    print2(string.format("ORDER_RX obr from p%s oid=%s t=%d", tostring(sender), oid, tick))
    return true
  end
  return false
end

-- =========================================================================
-- CLAIM / RELEASE
-- =========================================================================

local function release_held(state, info, why, quiet)
  local o = S(state)
  local h = o.held
  if not h then return end
  o.held = nil
  state._order = nil
  tx(state, string.format("/info obr %d", h.oid))
  if not quiet then
    say(state, why or "Released")
  end
  print2(string.format("ORDER_REL t=%d oid=%d why=%s", state.tick or 0, h.oid, tostring(why)))
end
M.release_held = release_held

-- group = true when several bots take the SAME order (all / nearby / a
-- selection).  Then nobody acks straight away: each taker broadcasts its obc
-- claim, and ORDER_AUCTION_TICKS later the lowest player number among the
-- claimants says ONE line with the count ("3 on pill 5").  A solo order acks
-- at once.
local function take_order(state, world, info, spec, cost, now, group, stolen)
  local o = S(state)
  if o.held and o.held.oid ~= spec.oid then
    -- Latest order wins.  Say what we are leaving so the human can follow it.
    local old = o.held
    say(state, string.format("Leaving %s #%s for %s #%s",
        old.kind, tostring(old.tid), spec.kind, tostring(spec.tid)))
    release_held(state, info, nil, true)
  end
  o.held = {
    oid = spec.oid, kind = spec.kind, tkind = spec.tkind, tid = spec.tid,
    -- tkind "here" ("go there and hold") carries its own tile: there is no
    -- pill or base to look the position up from.
    mx = spec.mx, my = spec.my,
    sender = spec.sender, sender_name = spec.sender_name,
    needs_shells = spec.needs_shells, verb = spec.verb,
    -- group = two or more bots on ONE order. squad.lua reads it so an ordered
    -- pair on the same live pill runs the blitz instead of arriving one at a
    -- time.
    group = group and true or false,
    since = now, expiry = now + (C.ORDER_FOCUS_TICKS or 3000),
    cost = cost or 0,
  }
  state._order = o.held
  local ic = math.floor(math.min(cost or 0, 999999))
  tx(state, string.format("/info obc %d %d", spec.oid, ic))
  o.gclaims[spec.oid] = o.gclaims[spec.oid] or {}
  o.gclaims[spec.oid][state.player_number] = ic
  if group then
    o.announce[spec.oid] = { due = now + (C.ORDER_AUCTION_TICKS or 10), spec = spec }
  else
    say(state, string.format("%s %s #%s", M.ack_for(my_name(state, info)),
        spec.kind, tostring(spec.tid or "")))
  end
  print2(string.format("ORDER_TAKE t=%d oid=%d kind=%s tid=%s cost=%.0f group=%s stolen=%s",
         now, spec.oid, spec.kind, tostring(spec.tid), cost or 0,
         tostring(group or false), tostring(stolen or false)))
end

-- =========================================================================
-- STAGE 2 — THE SPEAKING BOT, TEAM LATCHES, HELP
-- =========================================================================

-- THE SPEAKING BOT: the LOWEST player number among the team's active bots,
-- self included.  Every bot computes it from the same two engine masks
-- (info.player_bots & info.allies), so they all name the same one and only
-- that one talks.  A four-bot team therefore says a line once, not four
-- times.  Returns nil when this bot cannot see a roster yet.
function M.speaker(state, info)
  local roster = M.bot_roster(state, info)   -- already ascending by pn
  return roster[1] and roster[1].pn or nil
end

-- Chat max on the wire is PACKET_MAX_CHAT_MESSAGE = 128 bytes
-- (src/bolo/public/wire_limits.h) and both help lines are longer than that,
-- so each is split at a comma.  Four chunks, one per tick.
M.HELP = {
  "Orders: [all|nearby|bot name, default nearest] attack|capture|sweep|defend|decoy <pill#|base#|tank name>, retreat,",
  "cancel [all|bot name], focus bases|pills|off, reposition on|off",
  "Ping a tile: nearest bot goes, ping again adds one. Ping bots to select them,",
  "then order. Caution ping cancels; caution on a bot retreats it. 3 shots on a tile: come here.",
}
M.BANNER = "Commands available, say help for details."
M.BANNER_REPO =
  'Bot pillbox repositioning disabled with human allies. Say "reposition on" to enable.'

local FOCUS_CODE = { off = 0, bases = 1, pills = 2 }
local CODE_FOCUS = { [1] = "bases", [2] = "pills" }

-- Apply a team setting locally.  Called both from the chat line (every bot
-- heard it) and from the latch verb (a late joiner catching up).
local function set_focus(state, v)
  local o = S(state)
  o.focus = (v ~= "off") and v or nil
  state._focus = o.focus
end
local function set_repo(state, on)
  local o = S(state)
  o.repo_on = on
  state._repo_override = on
end

-- FOCUS pricing.  The OTHER class pays the multiplier; the focused class
-- keeps its real price.  goals.lua asks this question at one choke point in
-- the assembled-pool pass, and again in each panel renderer so the displayed
-- `weighted` matches the cost that actually competed.
M.FOCUS_PILL_KINDS = {
  attack_pill = true, capture_pill = true, defend_pill = true,
  repair_pill = true, reposition = true, place_pill_strategic = true,
}
M.FOCUS_BASE_KINDS = { capture_base = true, attack_base = true }
function M.focus_mult(state, kind)
  local focus = state and state._focus
  if not focus or not kind then return 1.0 end
  local m = C.FOCUS_OTHER_COST_MULT or 1.0
  if m == 1.0 then return 1.0 end
  if focus == "bases" and M.FOCUS_PILL_KINDS[kind] then return m end
  if focus == "pills" and M.FOCUS_BASE_KINDS[kind] then return m end
  return 1.0
end
-- The ONE breakdown string, so every panel spells it the same way.
function M.focus_label(state)
  return string.format("focus: x%.1f (%s)", C.FOCUS_OTHER_COST_MULT or 1.0,
                       tostring(state and state._focus or "off"))
end

-- REPOSITIONING with human allies.  `reposition on` / `off` overrides
-- C.REPOSITION_DISABLE_WITH_HUMAN_ALLIES for this game.  The three gates
-- (goals.eval_reposition_pill, reposition_vote's OPEN gate and its ballot)
-- all ask this one question.  nil = no override, use the constant.
function M.reposition_human_blocked(state)
  local ov = state and state._repo_override
  if ov ~= nil then return not ov end
  return C.REPOSITION_DISABLE_WITH_HUMAN_ALLIES and true or false
end

-- =========================================================================
-- STAGE 2 — PINGS
--
-- The engine delivers a teammate's smart ping as an EVENT_PING carrying
-- [sender, kind, xHi, xLo, yHi, yLo] in WORLD units (braincore.c pushes the
-- kind constants as Lua globals; brain_data.c lets EVENT_PING through the
-- event filter unconditionally).  The server has already decided this brain
-- is entitled to see the ping -- it is a team signal -- and we check the
-- sender against info.allies again anyway, so an enemy ping does nothing.
--
-- ORDER ID for a ping order: the SAME djb2 scheme chat orders use, over
-- sender | verb | who | target kind | target id, with who.mode = "ping".
-- There is NO tick and NO ping sequence counter in it, deliberately: the
-- target is the RESOLVED entity (the pill, base or tank id) or, for a "go
-- there and hold", the ANCHOR TILE of the first ping in the group packed as
-- mx * 256 + my.  A repeat ping inside the 3x3 of the same target therefore
-- derives the SAME id on every bot without anyone exchanging a counter --
-- which is exactly what makes "ping again adds one bot" work.  A sequence
-- counter would fork the id on any bot that missed a ping; a tick bucket
-- would fork it between two bots that think on different offsets.
-- =========================================================================

local function tile_of(w) return bit.rshift(w or 0, 8) end

-- Everything standing on ONE tile, in the order a ping prefers it.  A TANK
-- beats the pill or base it happens to be sitting on: pinging a tank is the
-- finer-grained gesture and the tank is the thing that moves.
function M.entity_at(state, world, info, mx, my)
  local me     = state.player_number
  local bots   = info.player_bots or 0
  local allies = info.allies or 0
  if mx == tile_of(info.tankx) and my == tile_of(info.tanky) then
    return { class = "allybot", pn = me, mx = mx, my = my }
  end
  local OT = _G.OBJECT_TANK
  local OH = _G.OBJECT_HOSTILE or 0
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == OT and tile_of(ob.x) == mx and tile_of(ob.y) == my then
      local pn = ob.idnum or -1
      if bit.band(ob.info or 0, OH) ~= 0 then
        return { class = "enemytank", pn = pn, mx = mx, my = my }
      elseif pn >= 0 and pn ~= me
             and bit.band(bots, bit.lshift(1, pn)) ~= 0
             and bit.band(allies, bit.lshift(1, pn)) ~= 0 then
        return { class = "allybot", pn = pn, mx = mx, my = my }
      end
    end
  end
  for id, pl in pairs(world.pills or {}) do
    if pl.mx == mx and pl.my == my then
      return { class = "pill", id = id, owner = pl.owner,
               health = pl.health or 0, mx = mx, my = my }
    end
  end
  for id, b in pairs(world.bases or {}) do
    if b.mx == mx and b.my == my then
      return { class = "base", id = id, owner = b.owner, mx = mx, my = my }
    end
  end
  return nil
end

-- TILE MATCHING (Andrew, 2026-09-14): the EXACT tile wins first and the ring
-- is NOT searched, so a second bot standing beside the pinged one is never
-- triggered.  Only when the exact tile is empty does the 3x3 ring apply; two
-- candidates in the ring -> the nearer one to the ping tile (an orthogonal
-- neighbour beats a diagonal), ties on player number (or, for a pill or a
-- base, on its id).
function M.resolve_ping(state, world, info, mx, my)
  local e = M.entity_at(state, world, info, mx, my)
  if e then e.exact = true return e end
  local ring = C.ORDER_PING_RING or 1
  local best, bestd, bestk
  for dy = -ring, ring do
    for dx = -ring, ring do
      if dx ~= 0 or dy ~= 0 then
        local c = M.entity_at(state, world, info, mx + dx, my + dy)
        if c then
          local d = dx * dx + dy * dy
          local k = c.pn or c.id or 0
          if not best or d < bestd or (d == bestd and k < bestk) then
            best, bestd, bestk = c, d, k
          end
        end
      end
    end
  end
  if best then best.exact = false end
  return best
end

-- The verb the pinged tile means (the design doc's verb table).
--   enemy live pill -> attack      dead pill      -> capture (sweep)
--   enemy/neutral base -> capture  our pill/base  -> defend
--   an ally BOT     -> select it   an enemy tank  -> attack_tank, pinned
--   open ground     -> go there and hold
function M.ping_command(hit, mx, my)
  if not hit then
    return { verb = "goto",
             target = { kind = "here", id = mx * 256 + my, mx = mx, my = my } }
  end
  if hit.class == "allybot"   then return { select_pn = hit.pn } end
  if hit.class == "enemytank" then
    return { verb = "attack", target = { kind = "tank", pn = hit.pn } }
  end
  if hit.class == "pill" then
    if hit.owner == "friendly" then
      return { verb = "defend", target = { kind = "pill", id = hit.id } }
    end
    if (hit.health or 0) == 0 then
      return { verb = "capture", target = { kind = "pill", id = hit.id } }
    end
    return { verb = "attack", target = { kind = "pill", id = hit.id } }
  end
  if hit.class == "base" then
    if hit.owner == "friendly" then
      return { verb = "defend", target = { kind = "base", id = hit.id } }
    end
    return { verb = "capture", target = { kind = "base", id = hit.id } }
  end
  return nil
end

-- The live PING ORDER this ping belongs to: same sender, and an anchor tile
-- whose ring holds this ping.  That is what turns a second ping into "add one
-- more bot" instead of "open a second order".  Lowest oid wins so every bot
-- picks the same one when two anchors overlap.
local function ping_anchor_match(o, sender, mx, my, now)
  local ring = C.ORDER_PING_RING or 1
  local keep = C.ORDER_PING_MATCH_TICKS or 3000
  local best
  for oid, a in pairs(o.anchors) do
    if a.sender == sender and (now - a.tick) < keep
       and math.abs(a.mx - mx) <= ring and math.abs(a.my - my) <= ring then
      if not best or oid < best then best = oid end
    end
  end
  return best
end

-- =========================================================================
-- START AN ORDER — the one path a chat line AND a ping both run through.
-- `who.mode` is "names" (no auction, the named bots take it), "all",
-- "nearby", or "auto"/"ping" (an auction: every bot posts its travel price
-- and the `want` cheapest take it).  `want` is how many bots the order is
-- for; it grows by one on each repeat ping, and the bots that already hold
-- the order keep it -- the auction only fills the slots still open.
-- =========================================================================
local function start_order(state, world, info, spec, who, now, want)
  local o  = S(state)
  local me = state.player_number
  local busy, reason = M.busy(state, info)

  if who.mode == "names" then
    local mine = false
    for _, pn in ipairs(who.pns) do if pn == me then mine = true end end
    if mine and not busy then
      take_order(state, world, info, spec, M.travel_cost(state, world, info, spec) or 0,
                 now, #who.pns > 1)
    elseif mine and busy then
      say(state, string.format("Busy (%s)", reason))
    end
    return true
  end

  if who.mode == "all" or who.mode == "nearby" then
    local take = not busy
    if take and who.mode == "nearby" then
      local mx, my = M.target_tile(world, state, spec)
      local tmx = bit.rshift(info.tankx or 0, 8)
      local tmy = bit.rshift(info.tanky or 0, 8)
      take = mx ~= nil and U.mdist(tmx, tmy, mx, my) <= (C.ORDER_NEARBY_TILES or 10)
    end
    if take then
      take_order(state, world, info, spec, M.travel_cost(state, world, info, spec) or 0,
                 now, true)
    end
    return true
  end

  -- ── auction: everyone bids, the `want` cheapest take it ────────────────
  local cost = (not busy) and M.travel_cost(state, world, info, spec) or nil
  if cost and cost >= 1e29 then cost = nil end
  -- RANGE RULE, for the three-shot order: only bots within who.near tiles
  -- of the square bid at all.  Out of range answers "no" the way a busy bot
  -- does, so an order nobody is near settles with no winner, and since a
  -- line is only ever said by a bot that TAKES an order, nothing is said.
  if cost and who.near then
    local mx, my = M.target_tile(world, state, spec)
    local tmx = bit.rshift(info.tankx or 0, 8)
    local tmy = bit.rshift(info.tanky or 0, 8)
    if not mx or U.mdist(tmx, tmy, mx, my) > who.near then cost = nil end
  end
  o.auctions[spec.oid] = {
    spec = spec, open = now, bids = {}, answered = {}, want = want or 1,
  }
  o.auctions[spec.oid].bids[me] = cost
  o.auctions[spec.oid].answered[me] = true
  tx(state, string.format("/info obd %d %d", spec.oid,
     cost and math.floor(math.min(cost, 999999)) or -1))
  print2(string.format("ORDER_BID t=%d oid=%d kind=%s cost=%s want=%d busy=%s",
         now, spec.oid, tostring(spec.kind),
         cost and string.format("%.0f", cost) or "no", want or 1, tostring(reason)))
  return true
end

-- =========================================================================
-- CHAT ENTRY POINT — called from init.lua's inbox loop, BEFORE the old
-- operator parser (cp:5 / attack:5 / ... keep working: none of those lines
-- start with a bare verb word, so they never reach an order).
-- Returns true when the line was an order line and the old parser should
-- not see it.
-- =========================================================================
function M.on_chat(state, world, info, sender, text, now, from_ally, sender_is_bot)
  if not C.BOT_COMMANDS_ENABLED then return false end
  if not from_ally then return false end            -- TEAM CHECK: silent on enemies
  if type(text) ~= "string" or text == "" then return false end
  -- A bot ally's own acks are ordinary chat; only an explicit ! line from a
  -- bot (or from ourselves) is read as an order, which is also the shape a
  -- sidecar test uses.
  if sender_is_bot and text:sub(1, 1) ~= "!" then return false end

  local roster = M.bot_roster(state, info)
  local cmd = M.parse(text, roster, M.all_roster(info))
  if not cmd then return false end

  local o = S(state)
  local me = state.player_number

  -- ── help ──────────────────────────────────────────────────────────────
  if cmd.help then
    if M.speaker(state, info) == me then
      for i = 1, #M.HELP do say(state, M.HELP[i]) end
    end
    return true
  end

  -- ── team-wide settings.  EVERY bot latches the value (they all heard the
  -- line); only the speaking bot confirms it and puts it on the wire for a
  -- bot that joins or respawns later.  `cancel all` never touches these.
  if cmd.setting == "focus" then
    set_focus(state, cmd.value)
    if M.speaker(state, info) == me then
      say(state, string.format("Focus: %s.", cmd.value))
      tx(state, string.format("/info obf %d", FOCUS_CODE[cmd.value] or 0))
    end
    print2(string.format("ORDER_FOCUS t=%d -> %s (from p%s)", now,
           tostring(cmd.value), tostring(sender)))
    return true
  end
  if cmd.setting == "reposition" then
    set_repo(state, cmd.value)
    if M.speaker(state, info) == me then
      say(state, cmd.value and "Repositioning on." or "Repositioning off.")
      tx(state, string.format("/info obp %d", cmd.value and 1 or 0))
    end
    print2(string.format("ORDER_REPO t=%d -> %s (from p%s)", now,
           tostring(cmd.value), tostring(sender)))
    return true
  end

  if cmd.reply then
    -- One bot answers, not all of them: the lowest player number among the
    -- team's bots does the talking.
    if M.speaker(state, info) == me then say(state, cmd.reply) end
    return true
  end

  if cmd.clear_select then
    if o.sel and o.sel.by == sender then
      local mine = false
      for _, pn in ipairs(o.sel.pns) do if pn == me then mine = true end end
      o.sel = nil
      if mine then say(state, "Standing by") end
    end
    return true
  end

  if cmd.select then
    o.sel = { by = sender, pns = cmd.select, until_tick = now + (C.ORDER_SELECT_TICKS or 500) }
    for _, pn in ipairs(cmd.select) do
      if pn == me then
        local busy, reason = M.busy(state, info)
        if busy then say(state, string.format("Busy (%s)", reason))
        else            say(state, "Awaiting command") end
      end
    end
    return true
  end

  -- ── cancel ────────────────────────────────────────────────────────────
  if cmd.verb == "cancel" then
    local t = cmd.target
    if t and t.kind == "all" then
      if o.held then release_held(state, info, "released") end
      o.known = {}
      o.auctions = {}
      o.claims = {}
      o.gclaims = {}
      o.announce = {}
    elseif t and t.kind == "tank" then
      if o.held and t.pn == me then release_held(state, info, "released") end
    else
      if o.held and o.held.sender == sender then release_held(state, info, "released") end
    end
    o.sel = nil
    return true
  end

  -- ── retreat with no target = cancel AND retreat at once ───────────────
  if cmd.verb == "retreat" and o.held then
    release_held(state, info, nil, true)
  end

  -- ── build the order spec ──────────────────────────────────────────────
  local kind, needs_shells, tid, err = M.goal_kind(cmd, world, info)
  if err then
    if M.speaker(state, info) == me then say(state, err) end
    return true
  end
  if not kind then return true end

  -- A live selection from this sender replaces the who-word: the selected
  -- bots take it directly, no auction.
  local who = cmd.who
  if o.sel and o.sel.by == sender and now < (o.sel.until_tick or 0) then
    who = { mode = "names", pns = o.sel.pns }
  end
  o.sel = nil

  local spec = {
    oid = M.order_id(sender, cmd), verb = cmd.verb, kind = kind,
    tkind = cmd.target and cmd.target.kind or nil,
    tid = tid, sender = sender, sender_name = player_name(info, sender),
    -- tkind "here" ("go there and hold", which is what a three-shot order
    -- is) carries its own square: there is no pill or base to look the
    -- position up from.
    mx = cmd.target and cmd.target.mx, my = cmd.target and cmd.target.my,
    needs_shells = needs_shells, who = who,
  }
  o.known[spec.oid] = { spec = spec, tick = now }

  -- Repeat of a live order we already hold = refresh the 60 s focus.
  if o.held and o.held.oid == spec.oid then
    o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)
    return true
  end

  return start_order(state, world, info, spec, who, now)
end

-- =========================================================================
-- BUSY — the one gate.  squad.busy is the public name (the design doc asked
-- for squad.busy(state, info)); it forwards here so the rule lives beside
-- the orders that use it.
-- =========================================================================
function M.busy(state, info)
  local g = state.goal or {}
  if (info.man_status or 0) ~= C.LGM_INTANK then return true, "man_out" end
  if g.kind == "capture_pill" and state._lgm_dispatch then return true, "capturing" end
  if state._repo_approved_pid ~= nil and state._repo_approved_pid == g.target_id then
    return true, "repositioning"
  end
  if state.km and state.km.executing then return true, "kill_me" end
  -- "A stuck escape in progress".  state._stuck_escape_count counts
  -- CONSECUTIVE stuck recoveries at ONE tile and is zeroed only when it
  -- reaches its own hard-escape threshold or on respawn, so one stuck moment
  -- left a bot permanently busy and it could never take an order again.
  -- state.stuck_for is the live counter (zeroed the moment the tank moves or
  -- fires) and ORDER_STUCK_BUSY_TICKS is steering.lua's own tile-stuck limit.
  if g.kind == "escape_water"
     or (state.stuck_for or 0) >= (C.ORDER_STUCK_BUSY_TICKS or 150) then
    return true, "escaping"
  end
  return false, nil
end


-- =========================================================================
-- PING ENTRY POINT — one call from init.lua's per-tick event handling.
-- =========================================================================

-- A BOT COMMAND ping (PING_KIND_BOT_COMMAND, 5).
local function ping_bot_command(state, world, info, sender, mx, my, now)
  local o   = S(state)
  local me  = state.player_number
  local hit = M.resolve_ping(state, world, info, mx, my)
  local pc  = M.ping_command(hit, mx, my)
  if not pc then return end

  -- ── SELECT: a ping on an ally BOT's tank, or its 3x3 ──────────────────
  -- More pings on other bots ADD to the selection; each new one answers.
  -- The sender's next order goes to the whole set with no auction.
  if pc.select_pn then
    local sel = o.sel
    if not (sel and sel.by == sender and now < (sel.until_tick or 0)) then
      sel = { by = sender, pns = {} }
    end
    local dup = false
    for _, pn in ipairs(sel.pns) do if pn == pc.select_pn then dup = true end end
    if not dup then sel.pns[#sel.pns + 1] = pc.select_pn end
    sel.until_tick = now + (C.ORDER_SELECT_TICKS or 500)
    o.sel = sel
    if pc.select_pn == me and not dup then
      local busy, reason = M.busy(state, info)
      -- A busy bot answers "Busy" and is NOT selected.
      if busy then
        say(state, string.format("Busy (%s)", reason))
        table.remove(sel.pns)
      else
        say(state, "Awaiting command")
      end
    end
    print2(string.format("PING_SELECT t=%d by=p%s pn=%s n=%d", now,
           tostring(sender), tostring(pc.select_pn), #sel.pns))
    return
  end

  local cmd = { verb = pc.verb, who = { mode = "ping" }, target = pc.target }
  local kind, needs_shells, tid, err = M.goal_kind(cmd, world, info)
  if err or not kind then
    if err and M.speaker(state, info) == me then say(state, err) end
    return
  end

  -- The ANCHOR is the resolved TARGET's tile (the ping tile when the ping hit
  -- open ground), so a second ping anywhere in the target's 3x3 matches.
  local axm = (hit and hit.mx) or mx
  local aym = (hit and hit.my) or my

  -- ── repeat ping on the same target: ADD the next cheapest bot ─────────
  local prev = ping_anchor_match(o, sender, mx, my, now)
  if prev and o.known[prev] then
    local k = o.known[prev]
    local a = o.anchors[prev]
    k.tick, a.tick = now, now
    a.want = (a.want or 1) + 1
    if o.held and o.held.oid == prev then
      o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)   -- focus reset
    end
    print2(string.format("PING_ADD t=%d oid=%d want=%d", now, prev, a.want))
    -- Re-open the auction for the extra slot. Bots that already hold the
    -- order are excluded at settle time (o.gclaims), so this only fills what
    -- is still open, and it refreshes the focus for the whole group.
    start_order(state, world, info, k.spec, { mode = "ping" }, now, a.want)
    return
  end

  -- A live selection from this sender replaces the auction.
  local who = cmd.who
  if o.sel and o.sel.by == sender and now < (o.sel.until_tick or 0)
     and #o.sel.pns > 0 then
    who = { mode = "names", pns = o.sel.pns }
    o.sel = nil
  end

  local spec = {
    oid = M.order_id(sender, cmd), verb = cmd.verb, kind = kind,
    tkind = cmd.target and cmd.target.kind or nil, tid = tid,
    mx = cmd.target and cmd.target.mx, my = cmd.target and cmd.target.my,
    sender = sender, sender_name = player_name(info, sender),
    needs_shells = needs_shells, who = who, ping = true,
  }
  o.known[spec.oid]   = { spec = spec, tick = now }
  o.anchors[spec.oid] = { sender = sender, mx = axm, my = aym, tick = now, want = 1 }
  print2(string.format("PING_ORDER t=%d oid=%d kind=%s tid=%s tile=(%d,%d) hit=%s",
         now, spec.oid, kind, tostring(tid), mx, my,
         hit and hit.class or "open"))
  start_order(state, world, info, spec, who, now, 1)
end

-- A CAUTION ping (PING_KIND_CAUTION, 1).
local function ping_caution(state, world, info, sender, mx, my, now)
  local o   = S(state)
  local me  = state.player_number
  local hit = M.resolve_ping(state, world, info, mx, my)

  -- ── on an ally BOT's tank, or its 3x3: the RETREAT gesture ────────────
  -- Holding an order: the FIRST caution cancels it. With no order (which is
  -- what the cancel leaves behind, so this covers the NEXT caution too) the
  -- caution retreats the bot, alone.
  if hit and hit.class == "allybot" then
    if hit.pn ~= me then return end
    if o.held then
      release_held(state, info, "Released")
      print2(string.format("PING_CAUTION t=%d cancel (held)", now))
      return
    end
    local cmd  = { verb = "retreat", who = { mode = "names", pns = { me } } }
    local spec = { oid = M.order_id(sender, cmd), verb = "retreat",
                   kind = "take_cover", sender = sender,
                   sender_name = player_name(info, sender),
                   needs_shells = false, who = cmd.who, ping = true }
    o.known[spec.oid] = { spec = spec, tick = now }
    local busy, reason = M.busy(state, info)
    if busy then
      say(state, string.format("Busy (%s)", reason))
    else
      take_order(state, world, info, spec, 0, now)
    end
    print2(string.format("PING_CAUTION t=%d retreat busy=%s", now, tostring(reason)))
    return
  end

  -- ── otherwise: in the 3x3 of an order's TARGET -> release the group ────
  local ring = C.ORDER_PING_RING or 1
  local tmx  = (hit and hit.mx) or mx
  local tmy  = (hit and hit.my) or my
  for oid, k in pairs(o.known) do
    local ox, oy = M.target_tile(world, state, k.spec)
    if ox and math.abs(ox - tmx) <= ring and math.abs(oy - tmy) <= ring then
      if o.held and o.held.oid == oid then release_held(state, info, "Released") end
      o.known[oid]    = nil
      o.claims[oid]   = nil
      o.auctions[oid] = nil
      o.gclaims[oid]  = nil
      o.announce[oid] = nil
      o.anchors[oid]  = nil
      print2(string.format("PING_CAUTION t=%d cancel oid=%d", now, oid))
    end
  end
end

-- Called once per think from init.lua, beside the other info.events readers.
-- ev.data is [sender, kind, xHi, xLo, yHi, yLo]; world units >> 8 = tile.
function M.on_events(state, world, info, now)
  if not C.BOT_COMMANDS_ENABLED then return end
  local evs = info.events
  if not evs or #evs == 0 then return end
  local EV     = _G.EVENT_PING
  local K_BOT  = _G.PING_KIND_BOT_COMMAND
  local K_CAUT = _G.PING_KIND_CAUTION
  if not EV then return end
  local me     = state.player_number
  local allies = info.allies or 0
  for i = 1, #evs do
    local ev = evs[i]
    if ev.type == EV and ev.data then
      local d      = ev.data
      local sender = d[1] or 0
      local kind   = d[2]
      local mx = bit.rshift((d[3] or 0) * 256 + (d[4] or 0), 8)
      local my = bit.rshift((d[5] or 0) * 256 + (d[6] or 0), 8)
      -- TEAM CHECK: self or an ally. The server already filters a ping to the
      -- sender's team, but an order is a hard command so the brain checks too.
      if sender == me or bit.band(allies, bit.lshift(1, sender)) ~= 0 then
        if kind == K_BOT then
          ping_bot_command(state, world, info, sender, mx, my, now)
        elseif kind == K_CAUT then
          ping_caution(state, world, info, sender, mx, my, now)
        end
      end
    end
  end
end

-- =========================================================================
-- PER-THINK UPDATE
-- =========================================================================
function M.update(state, world, info, now)
  if not C.BOT_COMMANDS_ENABLED then
    state._order = nil
    return
  end
  local o = S(state)
  local me = state.player_number

  -- 1. Drain the inbound verbs.
  for _, r in ipairs(o.rx) do
    if r.kind == "bid" then
      local a = o.auctions[r.oid]
      if a then
        a.bids[r.from] = (r.cost >= 0) and r.cost or nil
        a.answered[r.from] = true
      end
    elseif r.kind == "claim" then
      o.claims[r.oid] = { pn = r.from, cost = r.cost, tick = r.tick }
      o.gclaims[r.oid] = o.gclaims[r.oid] or {}
      o.gclaims[r.oid][r.from] = r.cost
      o.auctions[r.oid] = nil
      -- Someone else claimed what we hold (a steal): go quiet and let them.
      -- A GROUP order is shared, so a fellow taker's claim is not a steal.
      if o.held and o.held.oid == r.oid and r.from ~= me
         and not o.announce[r.oid] then
        o.held = nil
        state._order = nil
        print2(string.format("ORDER_LOST t=%d oid=%d to=p%s", now, r.oid, tostring(r.from)))
      end
    elseif r.kind == "focus" then
      -- A late joiner (or a respawned bot) latching the team's setting.
      set_focus(state, CODE_FOCUS[r.value] or "off")
    elseif r.kind == "repo" then
      set_repo(state, r.value == 1)
    elseif r.kind == "release" then
      if o.claims[r.oid] and o.claims[r.oid].pn == r.from then o.claims[r.oid] = nil end
      -- Re-bid it: the holder dropped it and the job is still standing.
      local k = o.known[r.oid]
      if k and not o.held and not o.auctions[r.oid]
         and (now - k.tick) < (C.ORDER_FOCUS_TICKS or 3000) then
        local busy = M.busy(state, info)
        local cost = (not busy) and M.travel_cost(state, world, info, k.spec) or nil
        if cost and cost >= 1e29 then cost = nil end
        o.auctions[r.oid] = { spec = k.spec, open = now, bids = { [me] = cost },
                              answered = { [me] = true } }
        tx(state, string.format("/info obd %d %d", r.oid,
           cost and math.floor(math.min(cost, 999999)) or -1))
      end
    end
  end
  o.rx = {}

  -- 2. Settle auctions.  Closed once every active ally has answered, or
  --    after ORDER_AUCTION_TICKS — whichever comes first.
  for oid, a in pairs(o.auctions) do
    local n_allies, n_ans = 0, 0
    for apn in ally_state.iter_active(now, 1750) do
      if apn ~= me then
        n_allies = n_allies + 1
        if a.answered[apn] then n_ans = n_ans + 1 end
      end
    end
    if n_ans >= n_allies or (now - a.open) >= (C.ORDER_AUCTION_TICKS or 10) then
      -- The `want` cheapest bids take the order (1 for a chat order, N after
      -- N ping bursts). Bots that ALREADY hold it keep it, so a repeat ping
      -- only fills the slot it just added. Ties break on player number, and
      -- every bot sorts the same table, so they all name the same winners.
      local want  = a.want or 1
      local heldby = o.gclaims[oid] or {}
      local n_held = 0
      for _ in pairs(heldby) do n_held = n_held + 1 end
      local rank = {}
      for pn = 0, 15 do
        if a.bids[pn] and not heldby[pn] then
          rank[#rank + 1] = { pn = pn, c = a.bids[pn] }
        end
      end
      table.sort(rank, function(x, y)
        if x.c ~= y.c then return x.c < y.c end
        return x.pn < y.pn
      end)
      local need, won = math.max(0, want - n_held), {}
      for i = 1, math.min(need, #rank) do
        won[#won + 1] = rank[i].pn
        if rank[i].pn == me then
          take_order(state, world, info, a.spec, rank[i].c, now, want > 1)
        else
          o.claims[oid] = { pn = rank[i].pn, cost = rank[i].c, tick = now }
        end
      end
      o.auctions[oid] = nil
      print2(string.format("ORDER_SETTLE t=%d oid=%d want=%d held=%d winners=%s",
             now, oid, want, n_held, table.concat(won, ",")))
    end
  end

  -- 2b. Group order: ONE line, from the lowest player number that actually
  --     took it, once the claims have had a window to arrive.
  for oid, an in pairs(o.announce) do
    if now >= an.due then
      local low, n = nil, 0
      for pn in pairs(o.gclaims[oid] or {}) do
        n = n + 1
        if not low or pn < low then low = pn end
      end
      if low == me and n > 0 and o.held and o.held.oid == oid then
        if n > 1 then
          say(state, string.format("%d on %s #%s", n, an.spec.tkind or "pill",
                                   tostring(an.spec.tid)))
        else
          say(state, string.format("%s %s #%s", M.ack_for(my_name(state, info)),
              an.spec.kind, tostring(an.spec.tid or "")))
        end
      end
      o.announce[oid] = nil
    end
  end

  -- 3. Steal, with hysteresis: cheaper by ORDER_STEAL_PCT AND by at least
  --    ORDER_STEAL_MIN_TILES of travel, and only after the holder has had
  --    the job for ORDER_STEAL_HOLD_TICKS.  Only a free bot steals.
  if not o.held then
    local busy = M.busy(state, info)
    if not busy then
      for oid, cl in pairs(o.claims) do
        local k = o.known[oid]
        if k and cl.pn ~= me and (now - cl.tick) >= (C.ORDER_STEAL_HOLD_TICKS or 100)
           and (now - k.tick) < (C.ORDER_FOCUS_TICKS or 3000) then
          local mine = M.travel_cost(state, world, info, k.spec)
          local margin = (C.ORDER_STEAL_MIN_TILES or 2) * (C.ORDER_TILE_COST or 4)
          if mine and mine < 1e29
             and mine <= cl.cost * (1 - (C.ORDER_STEAL_PCT or 0.20))
             and mine <= cl.cost - margin then
            take_order(state, world, info, k.spec, mine, now, nil, true)
            break
          end
        end
      end
    end
  end

  -- 4. Live order housekeeping: expiry, target taken, refuel pause.
  local h = o.held
  if h then
    local done, why = false, nil
    if now >= h.expiry then
      done, why = true, "order lapsed"
    elseif h.tkind == "pill" and h.tid then
      local p = world.pills[h.tid]
      if not p then
        done, why = true, "order lapsed"
      elseif (h.kind == "attack_pill" or h.kind == "capture_pill")
             and p.owner == "friendly" and not p.in_tank then
        done, why = true, string.format("%s #%d done", h.kind, h.tid)
      end
    elseif h.tkind == "base" and h.tid then
      local b = world.bases[h.tid]
      if b and (h.kind == "capture_base" or h.kind == "attack_base")
         and b.owner == "friendly" then
        done, why = true, string.format("base #%d done", h.tid)
      end
    end
    if done then
      say(state, why)
      release_held(state, info, nil, true)
      h = nil
    end
  end

  if h and h.needs_shells then
    -- A pause, not an exit: the timer keeps running while the tank tops up.
    local low = (info.shells or 0) < (C.SHELLS_LOW or 20)
    if low and not h.refuel_said then
      h.refuel_said = true
      say(state, "Refuelling, coming back")
    elseif not low then
      h.refuel_said = nil
    end
  end

  -- 5. Prune.
  for oid, k in pairs(o.known) do
    if (now - k.tick) > (C.ORDER_FOCUS_TICKS or 3000) then
      o.known[oid] = nil
      o.claims[oid] = nil
      o.auctions[oid] = nil
      o.gclaims[oid] = nil
      o.announce[oid] = nil
      o.anchors[oid] = nil
    end
  end
  if o.sel and now >= (o.sel.until_tick or 0) then
    -- A lapsed selection: the selected bots say so once, like `nevermind`.
    for _, pn in ipairs(o.sel.pns or {}) do
      if pn == me then say(state, "Standing by") end
    end
    o.sel = nil
  end

  -- 6. THE SPEAKING BOT: the banner at game start and the latch heartbeat.
  --    Only the lowest-numbered bot on the team talks, so a four-bot team
  --    says each line once.
  if M.speaker(state, info) == me then
    if not o.banner and U.human_ally_count(info) > 0 then
      o.banner = true
      say(state, M.BANNER)
      -- Only when repositioning really is blocked and nobody has overridden
      -- it; otherwise the line would be wrong.
      if C.REPOSITION_DISABLE_WITH_HUMAN_ALLIES and o.repo_on == nil then
        say(state, M.BANNER_REPO)
      end
    end
    -- Re-broadcast the team settings so a bot that joined or respawned late
    -- latches the same values. They ride their own verbs rather than the
    -- /info state slate, which is already close to the 124-byte batch budget.
    if (o.focus or o.repo_on ~= nil) and now >= (o.latch_tx or 0) then
      o.latch_tx = now + (C.ORDER_LATCH_REBROADCAST_TICKS or 1500)
      tx(state, string.format("/info obf %d", FOCUS_CODE[o.focus or "off"] or 0))
      if o.repo_on ~= nil then
        tx(state, string.format("/info obp %d", o.repo_on and 1 or 0))
      end
    end
  end

  state._order          = o.held
  state._focus          = o.focus
  state._repo_override  = o.repo_on
end

-- One line for the goal panel / debug_info.
function M.panel_line(state, info)
  local h = state.orders and state.orders.held
  if not h then return "" end
  local left = math.max(0, (h.expiry or 0) - (state.tick or 0))
  return string.format(" ORDER %s#%s from %s %ds left",
    h.kind, tostring(h.tid or "-"), h.sender_name or "?", math.floor(left / 50))
end

return M
