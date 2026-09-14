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

  -- Only lines that START with a verb, a who-word, `!` or a bot name are
  -- read; everything else is ordinary chat and is ignored in silence. Without
  -- this a bot's own ack ("Got it! attack_pill #5") would be answered with
  -- "didn't understand", because it happens to contain a verb word.
  if not forced then
    local p1, e1 = M.match_name(toks[1], roster)
    if not VERBS[toks[1]] and not WHOWORDS[toks[1]]
       and toks[1] ~= "nevermind" and toks[1] ~= "never"
       and p1 == nil and e1 ~= "ambiguous" then
      return nil
    end
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
          out = {}, say = {}, rx = {}, held = nil, sel = nil }
    state.orders = o
  end
  return o
end

local function tx(state, msg)
  local o = S(state)
  o.out[#o.out + 1] = msg
end

local function say(state, line)
  local o = S(state)
  if #o.say < 6 then o.say[#o.say + 1] = line end
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
    return true
  end
  oid, cost = text:match("^/info obc (%d+) (%-?%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "claim", oid = tonumber(oid), cost = tonumber(cost),
                        from = sender, tick = tick }
    return true
  end
  oid = text:match("^/info obr (%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "release", oid = tonumber(oid), from = sender, tick = tick }
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
    say(state, string.format("%s: %s", my_name(state, info), why or "released"))
  end
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
    say(state, string.format("%s: leaving %s #%s for %s #%s",
        my_name(state, info), old.kind, tostring(old.tid), spec.kind, tostring(spec.tid)))
    release_held(state, info, nil, true)
  end
  o.held = {
    oid = spec.oid, kind = spec.kind, tkind = spec.tkind, tid = spec.tid,
    sender = spec.sender, sender_name = spec.sender_name,
    needs_shells = spec.needs_shells, verb = spec.verb,
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

  if cmd.reply then
    -- One bot answers, not all of them: the lowest player number among the
    -- team's bots does the talking.
    if roster[1] and roster[1].pn == me then
      say(state, string.format("%s: %s", my_name(state, info), cmd.reply))
    end
    return true
  end

  if cmd.clear_select then
    if o.sel and o.sel.by == sender then
      local mine = false
      for _, pn in ipairs(o.sel.pns) do if pn == me then mine = true end end
      o.sel = nil
      if mine then say(state, string.format("%s: standing by", my_name(state, info))) end
    end
    return true
  end

  if cmd.select then
    o.sel = { by = sender, pns = cmd.select, until_tick = now + (C.ORDER_SELECT_TICKS or 500) }
    for _, pn in ipairs(cmd.select) do
      if pn == me then
        local busy, reason = M.busy(state, info)
        if busy then
          say(state, string.format("%s: busy (%s)", my_name(state, info), reason))
        else
          say(state, string.format("%s: awaiting command", my_name(state, info)))
        end
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
    if roster[1] and roster[1].pn == me then
      say(state, string.format("%s: %s", my_name(state, info), err))
    end
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
    needs_shells = needs_shells, who = who,
  }
  o.known[spec.oid] = { spec = spec, tick = now }

  -- Repeat of a live order we already hold = refresh the 60 s focus.
  if o.held and o.held.oid == spec.oid then
    o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)
    return true
  end

  local busy, reason = M.busy(state, info)

  if who.mode == "names" then
    local mine = false
    for _, pn in ipairs(who.pns) do if pn == me then mine = true end end
    if mine and not busy then
      take_order(state, world, info, spec, M.travel_cost(state, world, info, spec) or 0,
                 now, #who.pns > 1)
    elseif mine and busy then
      say(state, string.format("%s: busy (%s)", my_name(state, info), reason))
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

  -- ── who = auto: open an auction, everyone bids, cheapest takes it ─────
  local cost = (not busy) and M.travel_cost(state, world, info, spec) or nil
  if cost and cost >= 1e29 then cost = nil end
  o.auctions[spec.oid] = {
    spec = spec, open = now, bids = {}, answered = {},
  }
  o.auctions[spec.oid].bids[me] = cost
  o.auctions[spec.oid].answered[me] = true
  tx(state, string.format("/info obd %d %d", spec.oid,
     cost and math.floor(math.min(cost, 999999)) or -1))
  return true
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
  if g.kind == "escape_water" or (state._stuck_escape_count or 0) > 0 then
    return true, "escaping"
  end
  return false, nil
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
      end
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
      local best_pn, best_cost = nil, nil
      for pn = 0, 15 do
        local c = a.bids[pn]
        if c and (not best_cost or c < best_cost
                  or (c == best_cost and pn < best_pn)) then
          best_pn, best_cost = pn, c
        end
      end
      if best_pn == me then
        take_order(state, world, info, a.spec, best_cost, now)
      elseif best_pn then
        o.claims[oid] = { pn = best_pn, cost = best_cost, tick = now }
      end
      o.auctions[oid] = nil
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
      say(state, string.format("%s: %s", my_name(state, info), why))
      release_held(state, info, nil, true)
      h = nil
    end
  end

  if h and h.needs_shells then
    -- A pause, not an exit: the timer keeps running while the tank tops up.
    local low = (info.shells or 0) < (C.SHELLS_LOW or 20)
    if low and not h.refuel_said then
      h.refuel_said = true
      say(state, string.format("%s: refuelling, coming back", my_name(state, info)))
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
    end
  end
  if o.sel and now >= (o.sel.until_tick or 0) then o.sel = nil end

  state._order = o.held
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
