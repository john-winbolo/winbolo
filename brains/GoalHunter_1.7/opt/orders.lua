-- =========================================================================
-- GoalHunter/orders.lua — CHAT ORDERS (bot commands, stage 1)
--
-- A human ally types "attack 5" / "all defend 3" / "socrates retreat"
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
local CMDS       = require("commands")

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
-- no_fuzzy = true drops tier 3 (the edit-distance tier) and matches only an
-- exact name, a word of one, or a prefix.  THE START OF A LINE IS MATCHED
-- THAT WAY: a typo is a fine thing to forgive once a line is already an
-- order, but "cause we should push" must not become "Case, ...?" and get
-- answered "didn't understand" (Andrew's peer review, Sep 16).  See the gate
-- in M.parse.
-- Returns pn                       on a unique match
--         nil, "unknown"           on no match
--         nil, "ambiguous", {a, b} on two or more in the same tier
function M.match_name(word, roster, no_fuzzy)
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
        if not hit and not no_fuzzy and #w >= 3 then
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
-- `last` is the third who-word: the bot or bots that took the SENDER's own
-- previous order.  The parser only marks the line ("who.mode = last"); the
-- set of bots is worked out in on_chat, where the sender and the order book
-- are both in hand (see M.last_pns).
local WHOWORDS = { all = true, nearby = true, last = true }
local PILLWORDS = { pill = true, pills = true, pillbox = true, pillboxes = true }
local BASEWORDS = { base = true, bases = true }
-- THE TARGET WORD `closest`: the pill nearest the PERSON WHO TYPED THE LINE,
-- not the pill nearest any bot.  "attack closest" is the shortest way to say
-- "the one in front of me" and it is the only target word that needs the
-- sender's own position, so it is resolved in on_chat (M.closest_pill) rather
-- than here -- the parser only marks the line.
local CLOSESTWORDS = { closest = true, nearest = true }
-- Team-wide SETTINGS, not orders: no target, no auction and no 60 s timer,
-- and `cancel all` does not touch them.  `take` is the focus alias.
local SETTINGWORDS = {
  focus = true, take = true, reposition = true, repositioning = true,
  help = true, bot = true, botpings = true, botchat = true,
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

  -- ── THE OLD OPERATOR COMMANDS COME FIRST ──────────────────────────────
  -- "!stop", "!start", "!status", "!attack:5", "!cp:5", "!cb:all", "!goto"
  -- is NOT one of them.  They belong to commands.lua and they are older than
  -- every verb in this file, so a line that parser accepts is handed on
  -- BEFORE any order parsing runs.  This test used to sit at the bottom of
  -- the no-verb branch, which two shapes never reached: "!attack:5" splits
  -- into the tokens "attack" and "5" and was read as a soft order on pill 5,
  -- and "!start" was read as a bare-name SELECT of a bot called TARS (edit
  -- distance 2).  Both were swallowed and commands.lua never saw them
  -- (Andrew's peer review, Sep 16).  Precedence lives here, in one place, so
  -- on_chat and every test see the same answer.
  if forced and CMDS.parse(text) then return nil end

  s = s:lower()
  local toks = {}
  for w in s:gmatch("[%w%-']+") do toks[#toks + 1] = w end
  if #toks == 0 then return nil end

  -- ── "!goto <mx> <my> [<sx> <sy>]" — THREE SHOTS = COME HERE ──────────
  -- Three of one player's shells that run their full range and land on the
  -- same open square, fired inside two seconds of each other and with a
  -- quiet second either side, are an order: go there and hold.  The SERVER
  -- spots the pattern and injects this line with the SHOOTER as the sender,
  -- so the team check every order runs through is the shooter's team,
  -- exactly as for a typed line.
  --
  -- FORCED FORM ONLY.  Players have no coordinates to type, so a bare
  -- "goto 40 40" is somebody's chatter and never an order: without the "!"
  -- the line reaches the start-of-line test below, matches no verb and no
  -- name, and is ignored in silence.
  --
  -- who.mode is "ping", which gives this order the SAME id a bot ping on
  -- that square from the same sender derives (sender | goto | ping | here |
  -- tile), so every bot agrees on it without exchanging anything.
  --
  -- THE RANGE RULE, and the reason for sx, sy.  A three-shot order is for
  -- the bots the SHOOTER could see: sx, sy is the shooter's own tile, put
  -- in the line by the server, and a bot bids only when its tank is within
  -- ORDER_SHOT_VIEW_TILES of it measured the way a screen is — the larger
  -- of the two axes, which is the shooter's 29x29 view.
  --
  -- The short form has no shooter tile, which is what a human typing
  -- "!goto 40 40" sends.  That keeps the old rule: within
  -- ORDER_NEARBY_TILES of the TARGET, by Manhattan distance.
  if forced and toks[1] == "goto" then
    local mx, my = tonumber(toks[2]), tonumber(toks[3])
    local sx, sy = tonumber(toks[4]), tonumber(toks[5])
    local ok = (#toks == 3) or (#toks == 5 and sx and sy
                                and sx >= 0 and sx <= 255
                                and sy >= 0 and sy <= 255)
    if not ok or not mx or not my
       or mx < 0 or mx > 255 or my < 0 or my > 255 then
      return { reply = "didn't understand" }
    end
    mx, my = math.floor(mx), math.floor(my)
    local who
    if #toks == 5 then
      who = { mode = "ping", near = C.ORDER_SHOT_VIEW_TILES or 14,
              from_mx = math.floor(sx), from_my = math.floor(sy) }
    else
      who = { mode = "ping", near = C.ORDER_NEARBY_TILES or 10 }
    end
    return { verb = "goto",
             who = who,
             target = { kind = "here", id = mx * 256 + my, mx = mx, my = my },
             forced = true }
  end

  -- Only lines that START with a verb, a who-word, a setting word, `!` or a
  -- bot NAME are read; everything else is ordinary chat and is ignored in
  -- silence.  Without this a bot's own ack ("Got it! attack_pill #5") would
  -- be answered with "didn't understand", because it happens to contain a
  -- verb word.
  --
  -- A NAME AT THE START IS MATCHED EXACTLY, NOT FUZZILY.  The gate used to
  -- take the full three-tier matcher, and its edit-distance tier turned plain
  -- talk into an order shape: with a bot called Case on the team, "cause we
  -- should push" started with a name, found no verb, and came back "didn't
  -- understand" (reproduced, Andrew's peer review Sep 16).  So:
  --   * VERBS are always EXACT -- "attak pill 5" is ordinary chat.
  --   * a NAME at the start must be exact, a word of a name, or a prefix.
  --   * a TYPO'd name is still read, but only inside a line that is already
  --     an order: there has to be a verb in it, and every word in front of
  --     that verb has to be a name or a who-word ("socrtes attack 5").
  --   * a forced "!" line skips the gate entirely, as it always did.
  --
  -- A LEADING NUMBER opens the gate only when a VERB follows it immediately:
  -- "4 attack 5" is the count who-word, and "5 more shells please" is chat.
  -- The count is read as a who-word further down, where a bot NAMED with
  -- digits still wins over it.
  if not forced then
    local ok1 = VERBS[toks[1]] or WHOWORDS[toks[1]] or SETTINGWORDS[toks[1]]
                or toks[1] == "nevermind" or toks[1] == "never"
                or (toks[1]:match("^%d+$") ~= nil and VERBS[toks[2] or ""] ~= nil)
    if not ok1 then
      local p1, e1 = M.match_name(toks[1], roster, true)   -- no fuzzy tier
      ok1 = (p1 ~= nil) or (e1 == "ambiguous")
    end
    if not ok1 then
      local vfz
      for i = 1, #toks do if VERBS[toks[i]] then vfz = i break end end
      if vfz and vfz > 1 then
        ok1 = true
        for i = 1, vfz - 1 do
          local pn, err = M.match_name(toks[i], roster)
          if pn == nil and err ~= "ambiguous" and not WHOWORDS[toks[i]] then
            ok1 = false
            break
          end
        end
      end
    end
    if not ok1 then return nil end
  end

  -- ── Team-wide settings and help ───────────────────────────────────────
  -- Checked BEFORE the verb scan so "focus pills" is never read as a pill
  -- target and "reposition on" never reaches the order machinery.
  if toks[1] == "help" then
    if #toks == 1 then return { help = true } end
    return { reply = "didn't understand" }
  end

  -- A SUBJECT IN FRONT OF A SETTING IS ALLOWED, AND IGNORED.  Both focus and
  -- repositioning are TEAM-WIDE, so "socrates focus bases" and "last
  -- repositioning on" say exactly what the bare lines say.  Turning them
  -- down would read as the bots refusing a sensible line, so the subject is
  -- dropped and the setting takes effect with the same confirmation.
  -- `si` is where the setting word really starts; everything before it must
  -- be a who-word or a bot name, or the line is not a setting at all and
  -- falls through to the verb scan below.
  local si = 1
  for i = 2, #toks do
    if toks[i] == "focus" or toks[i] == "reposition"
       or toks[i] == "repositioning" or toks[i] == "bot"
       or toks[i] == "botpings" or toks[i] == "botchat" then
      local subject_ok = true
      for j = 1, i - 1 do
        if not WHOWORDS[toks[j]] and M.match_name(toks[j], roster) == nil then
          subject_ok = false
          break
        end
      end
      if subject_ok then si = i end
      break
    end
  end

  if toks[si] == "focus" or toks[si] == "take" then
    local w = toks[si + 1]
    if BASEWORDS[w] then return { setting = "focus", value = "bases" } end
    if PILLWORDS[w] then return { setting = "focus", value = "pills" } end
    if w == "off" or w == "none" then return { setting = "focus", value = "off" } end
    return { reply = "didn't understand" }
  end
  -- "repositioning on|off" is the wording the lobby docs use; "reposition
  -- on|off" is the older one.  They are the same command.
  if toks[si] == "reposition" or toks[si] == "repositioning" then
    if toks[si + 1] == "on"  then return { setting = "reposition", value = true  } end
    if toks[si + 1] == "off" then return { setting = "reposition", value = false } end
    return { reply = "didn't understand" }
  end
  -- "bot pings on|off" is the wording the docs use; "botpings on|off" is the
  -- same command written as one word.  A bare "bot ..." that is not about
  -- pings is not a setting at all, so it falls through to the verb scan.
  if toks[si] == "botpings"
     or (toks[si] == "bot" and toks[si + 1] == "pings") then
    local w = (toks[si] == "botpings") and toks[si + 1] or toks[si + 2]
    if w == "on"  then return { setting = "bot_pings", value = true  } end
    if w == "off" then return { setting = "bot_pings", value = false } end
    return { reply = "didn't understand" }
  end
  -- "bot chat on|off" — the second latch, written the same two ways.  It is
  -- about the bots' SPOKEN goal confirmations ("Got it! attack_pill #5", the
  -- group ack, "Still on it.", "holding 10s"); the answers a person is owed
  -- for a line they just typed (help, "didn't understand", "Busy", the
  -- setting confirmations themselves) are never silenced, or turning chat off
  -- would look exactly like the bots going deaf.
  if toks[si] == "botchat"
     or (toks[si] == "bot" and toks[si + 1] == "chat") then
    local w = (toks[si] == "botchat") and toks[si + 1] or toks[si + 2]
    if w == "on"  then return { setting = "bot_chat", value = true  } end
    if w == "off" then return { setting = "bot_chat", value = false } end
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
    -- A known shape with an unknown verb still deserves an answer.  The name
    -- test is the STRICT one for the same reason the gate above is: a typo'd
    -- name with no verb behind it is ordinary chat, not a broken order.
    -- (The old operator commands were handed to commands.lua at the top of
    -- this function, before any order parsing.)
    local p1 = M.match_name(toks[1], roster, true)
    if forced or WHOWORDS[toks[1]] or p1 ~= nil then
      return { reply = "didn't understand" }
    end
    return nil
  end

  -- ── who ───────────────────────────────────────────────────────────────
  local who = { mode = "auto" }
  if vi > 1 then
    -- `last` NAMES A SET ON ITS OWN, so it cannot share the who slot with a
    -- bot name: "socrates last attack 5" could mean either set and the bots
    -- never guess.  The line is turned down and nothing is ordered.
    for i = 1, vi - 1 do
      if toks[i] == "last" and vi > 2 then
        return { reply = "last or a name, not both" }
      end
    end
    if vi == 2 and WHOWORDS[toks[1]] then
      who = { mode = toks[1] }
    -- ── A COUNT is the fourth who-word: "4 attack 5" ────────────────────
    -- N bots, and which N is decided by the auction that is already there:
    -- it ranks every free bot by travel price to the TARGET, so the N
    -- cheapest are the N closest and `want` is simply set to N.  A bot whose
    -- NAME is digits still wins the slot -- the name test runs first -- so a
    -- team with a bot called "4" behaves as it always did.
    elseif vi == 2 and toks[1]:match("^%d+$")
           and M.match_name(toks[1], roster, true) == nil then
      local n = tonumber(toks[1])
      if not n or n < 1 or n > 16 then return { reply = "didn't understand" } end
      who = { mode = "count", n = math.floor(n) }
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
    -- ── "closest" — THE PILL NEAREST THE SENDER ─────────────────────────
    -- "attack closest", "attack closest pill" and "attack pill closest" are
    -- one thing.  It is always a PILL: a base has a number on the map the
    -- same way a pill does and "closest base" would have to guess which
    -- kind of base, so it is turned down in words rather than redirected.
    -- The pill itself is picked in on_chat, where the sender is known.
    if CLOSESTWORDS[tt[1]] and (#tt == 1 or PILLWORDS[tt[2]]) then
      tgt = { kind = "pill", closest = true }
    elseif PILLWORDS[tt[1]] and CLOSESTWORDS[tt[2]] then
      tgt = { kind = "pill", closest = true }
    elseif CLOSESTWORDS[tt[1]] and BASEWORDS[tt[2]] then
      return { reply = "closest takes a pill" }
    elseif BASEWORDS[tt[1]] and CLOSESTWORDS[tt[2]] then
      return { reply = "closest takes a pill" }
    elseif PILLWORDS[tt[1]] then
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
        -- ATTACK NEVER TAKES AN ALLY.  "attack socrates" used to build a real
        -- attack_tank order on a team-mate: the bot took it, drove at its own
        -- ally and idled there for the ten seconds the goal needed to give up
        -- (Andrew's peer review, Sep 16).  There is no order to build, so the
        -- line is turned down with the name in it and nothing is started.
        -- An ally is either a bot on our own roster or a player all_roster
        -- flagged (M.all_roster reads info.allies); `cancel` is aimed at
        -- allies on purpose and is not touched.
        if verb == "attack" then
          local ally_name = nil
          for i = 1, #roster do
            if roster[i].pn == pn then ally_name = roster[i].name break end
          end
          if not ally_name then
            for i = 1, #all_roster do
              if all_roster[i].pn == pn and all_roster[i].ally then
                ally_name = all_roster[i].name
                break
              end
            end
          end
          if ally_name then
            return { reply = ally_name .. " is on our side" }
          end
        end
        tgt = { kind = "tank", pn = pn }
      elseif err == "ambiguous" then
        return { reply = cands[1] .. " or " .. cands[2] .. "?" }
      else
        return { reply = "didn't understand" }
      end
    end
  end

  if verb == "decoy" then return { reply = "decoy: not yet" } end
  -- DEFEND IS A PILL VERB.  There is no defend_base goal and a base is not
  -- held the way a pill is, so "defend base 3" is turned down here rather
  -- than quietly redirected onto some pill that happens to sit near it.
  if verb == "defend" and tgt and tgt.kind == "base" then
    return { reply = "defend takes a pill" }
  end
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
-- THE ORDER ID: verb + who-word + target, and NOT the sender.  Two humans
-- who ask for the same thing are asking for ONE job, so the second line is a
-- repeat of the first (it refreshes the focus and gets the "Still on it."
-- line) rather than a second order the bots would have to fight over.
function M.order_id(sender, cmd)
  local w = cmd.who or {}
  local wk = w.mode or "auto"
  -- A COUNT IS AN AUCTION WITH A BIGGER `want`, NOT A DIFFERENT JOB.  "attack
  -- 5" and "4 attack 5" name one job on pill 5, so they share an id: the
  -- second line grows the first order the way a repeat ping does, instead of
  -- opening a rival order the holders would announce they were leaving for.
  if wk == "count" then wk = "auto" end
  if w.pns then
    local c = {}
    for i = 1, #w.pns do c[i] = w.pns[i] end
    table.sort(c)
    wk = wk .. ":" .. table.concat(c, ",")
  end
  local t = cmd.target or {}
  local key = string.format("%s|%s|%s|%s", cmd.verb or "",
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
  -- "GO THERE AND HOLD" (a bot ping on open ground, or the three-shot line).
  -- IT RUNS AS goto_tile, WHICH IS A HARD LOCK — the old `!pill:N` / `!base:N`
  -- behaviour: "don't do anything, just go there".  It used to be a take_cover
  -- pinned to the square, which left it bidding inside the goal pools with
  -- every reactive goal still competing, and bots drifted off the spot or
  -- never reached it.  Andrew, Sep 15: a place order is an instruction, not a
  -- suggestion, so it is a command_goal (see the goto_lock below) and goal
  -- selection does not run at all while it stands.  The tile is packed into
  -- the target id as mx * 256 + my so one number identifies it on the wire and
  -- in the order id; the order itself also carries mx / my.
  if v == "goto" then
    if t and t.kind == "here" then return "goto_tile", false, t.id end
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
    end
    -- No "defend" here: defend takes a pill, and the parser turns a base
    -- target down before an order is ever built.
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
          -- last_by[sender] = the order id that sender gave most recently.
          -- It is what the `last` who-word reads; see M.last_pns.
          last_by = {},
          -- stage 2: team-wide latches + the ping anchors
          focus = nil,        -- nil | "bases" | "pills"  (nil = off)
          repo_on = nil,      -- nil = no override | true | false
          -- "bot pings on|off".  nil = nobody has said either, so the game
          -- runs on C.BOT_PINGS_DEFAULT.  Read through M.bot_pings_on.
          bot_pings = nil,
          -- "bot chat on|off".  Same shape, over C.BOT_CHAT_DEFAULT, and read
          -- through M.bot_chat_on.  It gates the SPOKEN goal confirmations
          -- only (sayg below); a reply a person is owed still goes out.
          bot_chat = nil,
          -- seen[pn] = { mx, my, tick }: where an allied HUMAN's tank was the
          -- last time this bot could see it.  Filled once per think from
          -- info.objects, and read only by M.closest_pill, which needs the
          -- tile of the person who typed "attack closest".
          seen = {},
          -- Smart pings this bot wants the engine to place, oldest first.
          -- init.lua drains ONE per think into the think output, which is
          -- all the engine accepts; the engine drops extras anyway, so the
          -- queue is capped short and the OLDEST goes when it overflows.
          pings = {},
          -- atk_ping[target key] = the tick this bot last put an ATTACK
          -- marker on that target.  Stops a re-planned goal re-marking the
          -- same pill on every replan.
          atk_ping = {},
          banner = nil, latch_tx = 0, anchors = {},
          -- The three scenario hints that outlive one order (orders.lua's
          -- hint section). A patrol is its route and which point is next,
          -- an escort is the seat being followed and how far it may drift,
          -- and an avoid is a rectangle this bot keeps out of. nil each
          -- until a hint sets one.
          hint_patrol = nil, hint_escort = nil, hint_avoid = nil }
    state.orders = o
  end
  return o
end

-- EVERY LOOP THAT ACTS ON AN ORDER TABLE WALKS IT IN oid ORDER.
-- o.auctions, o.claims, o.known, o.announce and o.gclaims are hashes keyed by
-- order id, and pairs() over a hash is process-seeded: two orders settling on
-- one tick settled in a different order in two runs of the same recorded
-- game, which is exactly what this file's own determinism note forbids
-- (Andrew's peer review, Sep 16).  Take the keys, sort them, walk the array.
-- The snapshot also makes it safe to clear entries inside the loop.
local function sorted_keys(t)
  local out = {}
  if not t then return out end
  for k in pairs(t) do out[#out + 1] = k end
  table.sort(out)
  return out
end
M.sorted_keys = sorted_keys

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

-- A GOAL CONFIRMATION — the bot saying what it is doing about a job.  These
-- are the lines "bot chat off" silences, and the only ones: an answer a
-- person is owed for the line they just typed goes through plain say().
-- The latch is read here, at the one choke point, so a new confirmation
-- anywhere in this file obeys it by using sayg instead of say.
local function sayg(state, line)
  if not M.bot_chat_on(state) then return end
  say(state, line)
end

-- =========================================================================
-- "closest" — THE PILL NEAREST THE PERSON WHO TYPED THE LINE
--
-- The sender's tile comes from the engine's object scan (an allied tank
-- carries its player number in ob.idnum), and from o.seen when the sender is
-- out of this bot's sight -- a human who ducked behind a forest between
-- typing and the think should still get an answer.  With neither, the bot
-- cannot know what "closest" means and says so once, through the speaker.
--
-- WHICH PILLS COUNT depends on the verb, because the other reading is always
-- wrong: "attack closest" standing on our own pillbox must not order an
-- attack on it, and "defend closest" must not pick an enemy one.  So attack
-- and capture look at pills that are NOT ours, defend looks at ours.
--
-- TIES BREAK ON THE LOWEST PILL NUMBER so two bots that see the sender on the
-- same tile always name the same pill, and with it the same order id.
function M.sender_tile(state, info, sender)
  local o = S(state)
  if sender == state.player_number and info.tankx then
    return bit.rshift(info.tankx, 8), bit.rshift(info.tanky or 0, 8)
  end
  local OT = _G.OBJECT_TANK
  local OH = _G.OBJECT_HOSTILE or 0
  for _, ob in ipairs(info.objects or {}) do
    if ob.type == OT and ob.idnum == sender
       and bit.band(ob.info or 0, OH) == 0 then
      return bit.rshift(ob.x or 0, 8), bit.rshift(ob.y or 0, 8)
    end
  end
  local s = o.seen[sender]
  if s then return s.mx, s.my end
  return nil, nil
end

-- Refresh o.seen for every allied tank this bot can see.  One pass over the
-- object list per think; the list is short and this is the only place the
-- sender's position can be remembered from.
function M.note_seen(state, info, now)
  local OT = _G.OBJECT_TANK
  local OH = _G.OBJECT_HOSTILE or 0
  local allies = info.allies or 0
  local o = S(state)
  for _, ob in ipairs(info.objects or {}) do
    local pn = ob.idnum or -1
    if ob.type == OT and pn >= 0 and bit.band(ob.info or 0, OH) == 0
       and bit.band(allies, bit.lshift(1, pn)) ~= 0 then
      o.seen[pn] = { mx = bit.rshift(ob.x or 0, 8),
                     my = bit.rshift(ob.y or 0, 8), tick = now }
    end
  end
end

-- Returns pill id, or nil plus the line to say.
function M.closest_pill(state, world, info, sender, verb)
  local smx, smy = M.sender_tile(state, info, sender)
  if not smx then return nil, "can't see you" end
  local want_ours = (verb == "defend")
  local best, bestd
  for id, p in pairs(world.pills or {}) do
    local ours = (p.owner == "friendly")
    if ours == want_ours and p.mx then
      local dx, dy = p.mx - smx, p.my - smy
      local d = dx * dx + dy * dy
      if not best or d < bestd or (d == bestd and id < best) then
        best, bestd = id, d
      end
    end
  end
  if not best then
    return nil, want_ours and "no pill of ours known" or "no pill to take known"
  end
  return best
end


-- =========================================================================
-- SMART PINGS THE BOT PLACES ITSELF
--
-- The engine takes one ping per think out of the think output (ping_kind,
-- ping_x, ping_y) and turns it into a CMD_PING from THIS bot's own player
-- slot.  So a bot's marker is drawn, team-filtered and recorded exactly like
-- a marker a person placed, and the engine holds the bot to one ping every
-- 25 ticks on top of the team-wide ping rate limit every sender obeys.
--
-- Queue one with M.ping(state, kind, mx, my); init.lua drains it with
-- M.out_ping right before it returns.  The tile is turned into the WORLD
-- units the engine wants here, at the tile CENTRE, so the marker sits on the
-- square and not on its corner.
-- =========================================================================

-- kind is a PING_KIND_* the engine registered as a Lua global; mx, my are
-- map squares.  Two queued pings are as many as are ever useful (the engine
-- takes one a think and drops the rest inside its own gap), so the queue is
-- short and the OLDEST goes first when it is full: a fresh marker says where
-- the bot is going NOW.
function M.ping(state, kind, mx, my)
  if not kind or not mx or not my then return end
  if mx < 0 or mx > 255 or my < 0 or my > 255 then return end
  local o = S(state)
  o.pings[#o.pings + 1] = { kind = kind,
                            wx = math.floor(mx) * 256 + 128,
                            wy = math.floor(my) * 256 + 128 }
  while #o.pings > 2 do table.remove(o.pings, 1) end
end

-- Drain ONE queued ping into the think output table and hand the table back.
-- Written as a fill-in-the-table call so init.lua's think needs no new local
-- and no new upvalue for it (think sits at Lua's 60-upvalue cap).
function M.out_ping(state, out)
  local o = state and state.orders
  local p = o and o.pings and table.remove(o.pings, 1)
  if p then
    out.ping_kind = p.kind
    out.ping_x    = p.wx
    out.ping_y    = p.wy
  end
  return out
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

-- Every named player, with an `ally` flag on the ones on our own side (self
-- included).  The flag is what lets the parser turn "attack <team mate>"
-- down for a HUMAN ally too: M.bot_roster only holds allied BOTS.
function M.all_roster(info)
  local out = {}
  local names  = info.player_names or {}
  local allies = info.allies or 0
  local me     = info.player_number
  for pn = 0, 15 do
    local nm = names[pn + 1]
    if nm and nm ~= "" then
      out[#out + 1] = { pn = pn, name = nm,
                        ally = (pn == me)
                               or bit.band(allies, bit.lshift(1, pn)) ~= 0 }
    end
  end
  return out
end

-- =========================================================================
-- `last` — THE BOTS THAT TOOK THIS SENDER'S PREVIOUS ORDER
-- =========================================================================
-- Every bot can answer this from what it already has, with nothing new on
-- the wire: o.last_by[sender] is the id of the last order that sender gave
-- (each bot writes it when it reads the line or the ping), and
-- o.gclaims[oid] is the set of bots that claimed that id, which every bot
-- fills from the obc claims the takers broadcast.  So "last attack 7" picks
-- the same set on every bot.
--
-- A LIVE SELECTION FROM THIS SENDER WINS.  A selection is the newer and
-- plainer statement of "these bots", and it is what the player just made.
--
-- Returns nil when there is nothing to point at: the sender has given no
-- order, or the one they gave has been cancelled or has run out (both empty
-- its gclaims row).  The caller then says "no previous order".
-- Registering the order id is note_last below; the two must stay together.
function M.last_pns(o, sender, now)
  local sel = o.sel
  if sel and sel.by == sender and now < (sel.until_tick or 0)
     and #sel.pns > 0 then
    local out = {}
    for i = 1, #sel.pns do out[i] = sel.pns[i] end
    return out
  end
  local oid = o.last_by and o.last_by[sender]
  if not oid then return nil end
  local out = {}
  for pn in pairs(o.gclaims[oid] or {}) do out[#out + 1] = pn end
  if #out == 0 then return nil end
  -- pairs() has no order, and every bot must build the SAME list.
  table.sort(out)
  return out
end

-- Remember an order id as this sender's most recent one.  Called from every
-- place that registers an order in o.known, so a chat line, a bot-command
-- ping and a caution retreat all count as "the last order you gave".
local function note_last(o, sender, oid)
  if sender == nil or oid == nil then return end
  o.last_by = o.last_by or {}
  o.last_by[sender] = oid
end

local function my_name(state, info)
  return state.player_name
         or (info.player_names and info.player_names[(state.player_number or 0) + 1])
         or "bot"
end

local function player_name(info, pn)
  return (info.player_names and info.player_names[(pn or 0) + 1]) or ("p" .. tostring(pn))
end

-- HOW A GOAL IS NAMED IN A SPOKEN LINE.
-- Every other order points at a numbered thing, so "attack_pill #5" reads
-- straight.  A TILE order does not: `retreat` has no target at all, and "go
-- there and hold" has the SQUARE for a target id (packed as mx * 256 + my),
-- so the same format said "take_cover #30325", a number that means nothing to
-- the human who typed the line.  Andrew, Sep 14: say just the word.  Used by
-- every line that prints the goal.
-- The word for a place order is "goto": that is what it does, and it stopped
-- being take_cover when it became a hard lock (Sep 15).
local function goal_label(kind, tid)
  if kind == "take_cover" then return "take_cover" end
  if kind == "goto_tile"  then return "goto" end
  return string.format("%s #%s", tostring(kind), tostring(tid or ""))
end
M.goal_label = goal_label

-- The GROUP lines name the target CLASS ("3 on pill #5") rather than the
-- goal, because several bots on one pill read better that way.  A tile order
-- has no class either, so it falls back to the same bare word.
local function group_label(kind, tkind, tid)
  if kind == "take_cover" then return "take_cover" end
  if kind == "goto_tile"  then return "goto" end
  return string.format("%s #%s", tkind or "pill", tostring(tid or ""))
end
M.group_label = group_label

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
  -- Team-wide LATCHES.  One bot decides (it heard the chat line), every bot
  -- latches the same value; the speaking bot also re-broadcasts them every
  -- ORDER_LATCH_REBROADCAST_TICKS so a bot that joined or respawned late
  -- catches up.  Their own verbs, like the order bids: the /info state slate
  -- is already close to the 124-byte batch budget (C.MSG_BATCH_MAX).
  local f = text:match("^/info obf (%d)$")
  if f then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "focus", value = tonumber(f), from = sender, tick = tick }
    return true
  end
  local r = text:match("^/info obp (%d)$")
  if r then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "repo", value = tonumber(r), from = sender, tick = tick }
    return true
  end
  local g = text:match("^/info obg (%d)$")
  if g then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "bpings", value = tonumber(g), from = sender, tick = tick }
    return true
  end
  local h = text:match("^/info obh (%d)$")
  if h then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "bchat", value = tonumber(h), from = sender, tick = tick }
    return true
  end
  oid = text:match("^/info obr (%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "release", oid = tonumber(oid), from = sender, tick = tick }
    return true
  end
  -- CANCEL, the other half of obr: this order is OVER and nobody may take it
  -- up.  obr means "I am handing it back, somebody go"; obx means "forget it".
  -- Without the two verbs every cancel came back one think later, because the
  -- obr a cancel broadcast re-opened the auction on every other bot.
  oid = text:match("^/info obx (%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "cancel", oid = tonumber(oid), from = sender, tick = tick }
    return true
  end
  return false
end

-- =========================================================================
-- CLAIM / RELEASE
-- =========================================================================

-- =========================================================================
-- THE GO-THERE LOCK.
--
-- A place order ("go there and hold") is the one order that runs OUTSIDE the
-- goal pools.  It is a command_goal — the same slot the old operator lines
-- `!pill:N` and `!base:N` used — and goals.pick_goal answers a command_goal
-- BEFORE goal selection runs, so while the order stands the bot does nothing
-- but drive to the square and stand on it.  That is deliberate: Andrew, Sep
-- 15, "don't do anything, just go there".  The steering layer still shoots an
-- enemy tank that drives into its sights, because that is not a goal.
--
-- Called from three places, and it is the ONLY writer of a goto_tile command
-- goal: when the order is taken, once every think from M.update, and from
-- release_held when the slot empties.  Re-asserting every think is what makes
-- the HOLD work: goals.pick_goal clears a command_goal when the tank arrives,
-- and without this the bot would hand the tick back to the pools and wander.
-- The slot ending (expiry, cancel, a steal, a newer order) is what clears it.
-- BOTH SLOTS, the way the old operator commands did it.  A command goal
-- SUPPRESSES the replan timer (init.lua: "not state.command_goal and
-- timer_fire"), so pick_goal is not called again while the lock is on and
-- setting state.command_goal alone would leave the bot driving to whatever
-- goal it happened to hold when the order arrived.  commands.lua always wrote
-- state.goal at the same time for exactly this reason; so does this.
--
-- THE LOCK IS THE TRAVEL PHASE ONLY (Andrew, Sep 15).  Once the tank has
-- ARRIVED the slot goes into its hold (h.hold) and the hard lock comes OFF:
-- goal selection runs again, the order reject pass still kills every
-- strategic row, and the reactive rows -- attack_tank, kill_lgm -- can win,
-- so a holding bot shoots the tank that drives up to it and kills the man
-- that walks past.  It still does not GO anywhere: steering.lua parks the
-- tank while the slot is holding (see M.steer, "a go-there order in its hold
-- phase"), so the fight happens from the square it was sent to.
local function goto_lock(state)
  local h = state.orders and state.orders.held
  if h and h.kind == "goto_tile" and h.mx and h.my and not h.hold then
    local cg, g = state.command_goal, state.goal
    local cg_ok = cg and cg.kind == "goto_tile" and cg.mx == h.mx and cg.my == h.my
    local g_ok  = g  and g.kind  == "goto_tile" and g.mx  == h.mx and g.my  == h.my
    if cg_ok and g_ok then return end
    state.command_goal = { kind = "goto_tile", id = 0,
                           mx = h.mx, my = h.my,
                           wx = U.m2w(h.mx), wy = U.m2w(h.my) }
    state.goal = { kind = "goto_tile", mx = h.mx, my = h.my,
                   wx = U.m2w(h.mx), wy = U.m2w(h.my) }
    if state.pf then
      state.pf.status = "idle"
      state.pf_fail_logged = false
    end
    state.stuck_for = 0
  elseif state.command_goal and state.command_goal.kind == "goto_tile" then
    state.command_goal = nil
  end
end
M.goto_lock = goto_lock

-- FORGET AN ORDER ALTOGETHER.  Everything this bot remembers about one id
-- goes: the job is over and nothing may start it again.  The wire twin is
-- the obx verb below, which makes every other bot run this same function.
local function forget_order(o, oid)
  o.known[oid]    = nil
  o.claims[oid]   = nil
  o.auctions[oid] = nil
  o.gclaims[oid]  = nil
  o.announce[oid] = nil
  o.anchors[oid]  = nil
end
M.forget_order = forget_order

-- LETTING AN ORDER GO IS TWO DIFFERENT THINGS, and it used to be one.
--
--   RELEASED  (cancelled = false)  the job still stands, this bot cannot do
--             it: it died, it got stuck, a newer order took its place.  It
--             broadcasts obr, and the bot with the next cheapest route picks
--             the job up.  That is the design.
--
--   CANCELLED (cancelled = true)   the job is OVER: a person cancelled it, a
--             caution ping called it off, a retreat replaced it, or it is
--             finished.  Nobody may pick it up.
--
-- release_held always broadcast obr, so every cancel was undone one think
-- later: the order was still in every other bot's o.known, the obr re-opened
-- the auction there, and the next bot went and did the job a person had just
-- called off -- including driving a second bot to a go-there square after the
-- first had finished holding it (Andrew's peer review, Sep 16).  A cancel now
-- broadcasts obx instead, and obx makes every bot forget the id.
--
-- gclaims IS CLEARED EITHER WAY.  It is the "who already holds this" set the
-- auction settle excludes, and nothing ever took a bot out of it, so a bot
-- that let an order go could never win it back and "ping again to add a bot"
-- stopped adding (the same review, item 6).
local function release_held(state, info, why, quiet, cancelled)
  local o = S(state)
  local h = o.held
  if not h then return end
  o.held = nil
  state._order = nil
  goto_lock(state)                      -- the hard lock goes with the order
  if o.gclaims[h.oid] then o.gclaims[h.oid][state.player_number] = nil end
  if cancelled then
    tx(state, string.format("/info obx %d", h.oid))
    forget_order(o, h.oid)
  else
    tx(state, string.format("/info obr %d", h.oid))
  end
  if not quiet then
    sayg(state, why or "Released")
  end
end
M.release_held = release_held

-- A DEAD BOT HANDS ITS ORDER BACK.  init.lua's dead-tick block returns before
-- ORD.update ever runs, so nothing cleared the slot: a bot killed while
-- holding a go-there order came back with h.hold still set, parked on its
-- respawn square, and stood there for the rest of the hold doing nothing
-- (Andrew's peer review, Sep 16).  The job still stands -- somebody else
-- should do it -- so this is a RELEASE, not a cancel: the obr goes out and
-- the next cheapest bot takes it.  Quiet: a death is not a line the team
-- needs.  Safe to call on every dead tick; it does nothing without a slot.
function M.on_death(state, info)
  local o = state and state.orders
  if not (o and o.held) then
    if state then state._order = nil end
    return false
  end
  release_held(state, info, nil, true)
  o.sel = nil
  return true
end

-- THE HOLD PARK, with the distance test it never had.  steering.lua takes the
-- throttle away while a go-there order is in its hold phase; the only tests
-- were "the slot says hold" and "the goal is a park kind", so a bot that died
-- and respawned across the map went on parking -- nowhere near the square a
-- person pointed at.  The park is about STANDING ON THE TILE, so the tank has
-- to be on it (the same one-square radius that starts the hold).
function M.hold_parked(state, info)
  local h = state and state._order
  if not (h and h.hold and h.kind == "goto_tile" and h.mx and h.my) then
    return false
  end
  if not (info and info.tankx and info.tanky) then return false end
  local dx = bit.rshift(info.tankx, 8) - h.mx
  local dy = bit.rshift(info.tanky, 8) - h.my
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return dx <= 1 and dy <= 1
end

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
    sayg(state, string.format("Leaving %s for %s",
        goal_label(old.kind, old.tid), goal_label(spec.kind, spec.tid)))
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
  -- A place order takes hold on the tick it is taken, not on the next think.
  goto_lock(state)
  local ic = math.floor(math.min(cost or 0, 999999))
  tx(state, string.format("/info obc %d %d", spec.oid, ic))
  o.gclaims[spec.oid] = o.gclaims[spec.oid] or {}
  o.gclaims[spec.oid][state.player_number] = ic
  if group then
    o.announce[spec.oid] = { due = now + (C.ORDER_AUCTION_TICKS or 10), spec = spec }
  else
    sayg(state, string.format("%s %s", M.ack_for(my_name(state, info)),
        goal_label(spec.kind, spec.tid)))
  end
  -- "ON MY WAY" — one marker on the place the order names, at the moment the
  -- bot takes it.  This is NOT the "bot pings" setting: it answers a person
  -- who just gave an order, so it is always sent, and on a GROUP order every
  -- taker sends its own, which is what shows the human how many are coming.
  -- The chat ack is unchanged and still carries the words.
  do
    local pmx, pmy = M.target_tile(world, state, spec)
    if pmx and pmy then
      M.ping(state, _G.PING_KIND_ON_MY_WAY or 4, pmx, pmy)
    end
  end
end

-- THE SAME ORDER, SAID AGAIN, TO A BOT THAT ALREADY HOLDS IT.  The repeat
-- refreshes the 60 s focus; it used to do that in silence, which reads like
-- the bot never heard it.  The holder now says one short line back, in the
-- shape of the acks but with no name in front of it.  A GROUP order answers
-- once, from the LOWEST player number among the holders, the way the group
-- ack does.  A bot that does not hold the order says nothing, so a repeat
-- while the auction is still open is silent.
-- ORDER_REPEAT_ACK_TICKS is the floor between two of these lines: a held key
-- repeats the same line every tick otherwise.  One stamp per bot is enough
-- because a bot holds one order at a time.
local function repeat_ack(state, info, oid, now)
  local o = S(state)
  local h = o.held
  if not h or h.oid ~= oid then return end
  if (now - (o.rack or -1e9)) < (C.ORDER_REPEAT_ACK_TICKS or 50) then return end
  local me = state.player_number
  local low, n = nil, 0
  for pn in pairs(o.gclaims[oid] or {}) do
    n = n + 1
    if not low or pn < low then low = pn end
  end
  if n > 1 then
    if low ~= me then return end
    sayg(state, string.format("Still on it. %d on %s", n,
                             group_label(h.kind, h.tkind, h.tid)))
  else
    sayg(state, string.format("Still on it. %s", goal_label(h.kind, h.tid)))
  end
  o.rack = now
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
-- `N` is the count who-word and `closest` the sender-relative target; `decoy`
-- came out of line 1 to make room, because it is the one verb that is not
-- built yet and answers "decoy: not yet" when it is asked for.
M.HELP = {
  "Orders: [all|nearby|last|N|bot name, default nearest] attack|capture|sweep <pill#|base#|closest|tank name>, defend <pill#>,",
  "retreat, cancel [all|bot name], focus bases|pills|off, reposition on|off, bot pings on|off, bot chat on|off",
  "Ping a tile: nearest bot goes, ping again adds one. Ping bots to select them,",
  "then order. Caution ping cancels; caution on a bot retreats it. 3 shots on a tile: come here.",
}
-- There is NO game-start banner any more (Andrew, Sep 14).  The brain says
-- what it can do in the LOBBY instead, through two text files it ships beside
-- about.txt: announce.txt (the one short line the lobby drops into team chat
-- when the bot joins your team) and commands.txt (the long docs the lobby
-- opens when that line is clicked).  Those travel with the lobby brain
-- metadata, not as chat, so the 128-byte chat cap does not apply to them.

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
local function set_bot_pings(state, on)
  local o = S(state)
  o.bot_pings = on
end
local function set_bot_chat(state, on)
  local o = S(state)
  o.bot_chat = on
end

-- Is the team taking bot ATTACK markers?  Nobody has said either way until
-- o.bot_pings is set, and then the game runs on the knob.
function M.bot_pings_on(state)
  local o = state and state.orders
  if o and o.bot_pings ~= nil then return o.bot_pings end
  return C.BOT_PINGS_DEFAULT and true or false
end

-- Are the bots SPEAKING their goal confirmations?  On until somebody says
-- "bot chat off", which is what the game has always done, so the knob's
-- default is true.  Read through sayg, never directly.
function M.bot_chat_on(state)
  local o = state and state.orders
  if o and o.bot_chat ~= nil then return o.bot_chat end
  if C.BOT_CHAT_DEFAULT == nil then return true end
  return C.BOT_CHAT_DEFAULT and true or false
end

-- ATTACK MARKER, on every goal change to an attack.  init.lua calls this at
-- the one point where state.goal takes a new kind or a new target, so an
-- ordered attack and an attack the bot picked for itself both reach it.
--
-- Only while "bot pings" is on, and at most one marker per target per
-- ORDER_PING_REPEAT_TICKS: a bot re-plans the same goal often, and a marker
-- on every replan would bury the map.  The stamp is per BOT, because each bot
-- keeps its own copy of this table, and per TARGET, so switching between two
-- pills marks both.
function M.attack_ping(state, goal, now)
  if not C.BOT_COMMANDS_ENABLED then return end
  if not goal then return end
  if goal.kind ~= "attack_pill" and goal.kind ~= "attack_tank" then return end
  if not M.bot_pings_on(state) then return end
  local mx, my = goal.mx, goal.my
  if not mx or not my then return end
  local o   = S(state)
  local key = goal.kind .. ":" .. tostring(goal.target_id or (mx * 256 + my))
  local gap = C.ORDER_PING_REPEAT_TICKS or 1500
  if o.atk_ping[key] and (now - o.atk_ping[key]) < gap then return end
  o.atk_ping[key] = now
  M.ping(state, _G.PING_KIND_ATTACK or 3, mx, my)
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
-- neighbour beats a diagonal), ties on player number.
--
-- THE RING IS FOR TANKS ONLY (Andrew, 2026-09-15).  A pill or a base is a
-- thing that never moves and you can put the marker right on it, so a ping
-- one square off a pillbox is NOT that pillbox: Andrew pinged beside a pill
-- to send a decoy to that square and got a defend_pill instead.  Beside a
-- pill on open ground now means what it looks like -- "go there".  A tank
-- moves while the ping is in flight, so the ring stays for tanks: an ally
-- bot's tank (select, and the caution retreat) and an enemy tank (attack).
local function ring_candidate(c)
  return c and (c.class == "allybot" or c.class == "enemytank")
end

function M.resolve_ping(state, world, info, mx, my)
  local e = M.entity_at(state, world, info, mx, my)
  if e then e.exact = true return e end
  local ring = C.ORDER_PING_RING or 1
  local best, bestd, bestk
  for dy = -ring, ring do
    for dx = -ring, ring do
      if dx ~= 0 or dy ~= 0 then
        local c = M.entity_at(state, world, info, mx + dx, my + dy)
        if ring_candidate(c) then
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
--   enemy/neutral base -> capture  our pill       -> defend
--   our own base    -> nothing (defend takes a pill)
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
    -- A ping on OUR OWN base means nothing at all: no order and no line.
    -- Defend takes a pill, and a base we already hold has no other verb.
    if hit.owner == "friendly" then return nil end
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
  -- bid at all.  Out of range answers "no" the way a busy bot does, so an
  -- order nobody is near settles with no winner, and since a line is only
  -- ever said by a bot that TAKES an order, nothing is said.
  --
  -- WHAT the range is measured from depends on who.from_mx.  With it, the
  -- tile is the SHOOTER's own tile and the distance is Chebyshev: that is
  -- the shooter's 29x29 view, so the only bots that take a three-shot order
  -- are the ones the shooter could see when it fired.  Without it (a human
  -- typing "!goto x y", who has no tile of their own to send) the tile is
  -- the target and the distance is the old Manhattan one.
  if cost and who.near then
    local tmx = bit.rshift(info.tankx or 0, 8)
    local tmy = bit.rshift(info.tanky or 0, 8)
    if who.from_mx then
      local dx = math.abs(tmx - who.from_mx)
      local dy = math.abs(tmy - who.from_my)
      if (dx > dy and dx or dy) > who.near then cost = nil end
    else
      local mx, my = M.target_tile(world, state, spec)
      if not mx or U.mdist(tmx, tmy, mx, my) > who.near then cost = nil end
    end
  end
  o.auctions[spec.oid] = {
    spec = spec, open = now, bids = {}, answered = {}, want = want or 1,
  }
  o.auctions[spec.oid].bids[me] = cost
  o.auctions[spec.oid].answered[me] = true
  tx(state, string.format("/info obd %d %d", spec.oid,
     cost and math.floor(math.min(cost, 999999)) or -1))
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
  -- test script uses.
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
    return true
  end
  if cmd.setting == "reposition" then
    set_repo(state, cmd.value)
    if M.speaker(state, info) == me then
      say(state, cmd.value and "Repositioning on." or "Repositioning off.")
      tx(state, string.format("/info obp %d", cmd.value and 1 or 0))
    end
    return true
  end
  if cmd.setting == "bot_pings" then
    set_bot_pings(state, cmd.value)
    if M.speaker(state, info) == me then
      say(state, cmd.value and "Bot pings on." or "Bot pings off.")
      tx(state, string.format("/info obg %d", cmd.value and 1 or 0))
    end
    return true
  end
  -- "bot chat on|off".  The confirmation of this one goes out through plain
  -- say(): a person who has just turned the chat off is owed the word that it
  -- IS off, and silencing that line would be indistinguishable from the bots
  -- never hearing it.  The latch is set before the line is queued, so "off"
  -- is the last thing said and "on" is the first.
  if cmd.setting == "bot_chat" then
    set_bot_chat(state, cmd.value)
    if M.speaker(state, info) == me then
      say(state, cmd.value and "Bot chat on." or "Bot chat off.")
      tx(state, string.format("/info obh %d", cmd.value and 1 or 0))
    end
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

  -- ── `last`: the bots that took this sender's previous order ───────────
  -- The team settings are already handled above, so by here `last` can only
  -- stand in front of a real order.  It becomes a plain names who-word: the
  -- bots take the order straight, with no auction, exactly as if the sender
  -- had typed their names.  With nothing to point at the speaking bot says
  -- so and the line ends there.
  local last_pns = nil
  if cmd.who and cmd.who.mode == "last" then
    last_pns = M.last_pns(o, sender, now)
    if not last_pns then
      if M.speaker(state, info) == me then say(state, "no previous order") end
      return true
    end
  end

  -- ── cancel ────────────────────────────────────────────────────────────
  -- EVERY CANCEL IS A CANCEL, not a hand-back (the `cancelled` argument).
  -- `cancel all` was the only one that cleared o.known first, so it was the
  -- only one that stuck: plain `cancel`, `cancel <bot>` and `last cancel` all
  -- left the order in every bot's known table, and the obr release_held
  -- broadcast re-opened the auction there one think later -- another bot went
  -- and did the job a person had just called off.
  if cmd.verb == "cancel" then
    local t = cmd.target
    if t and t.kind == "all" then
      if o.held then release_held(state, info, "released", false, true) end
      o.known = {}
      o.auctions = {}
      o.claims = {}
      o.gclaims = {}
      o.announce = {}
      o.anchors = {}
      o.last_by = {}
    elseif t and t.kind == "tank" then
      if o.held and t.pn == me then release_held(state, info, "released", false, true) end
    elseif last_pns then
      -- "last cancel" releases exactly the bots that took the last order,
      -- and nobody else: a bot holding a DIFFERENT order keeps it.
      local mine = false
      for _, pn in ipairs(last_pns) do if pn == me then mine = true end end
      if o.held and mine then release_held(state, info, "Released", false, true) end
    else
      if o.held and o.held.sender == sender then
        release_held(state, info, "released", false, true)
      end
    end
    o.sel = nil
    return true
  end

  -- ── retreat with no target = cancel AND retreat at once ───────────────
  -- The order it drops is CANCELLED: "come back" is not "somebody else go
  -- and do it", and the obr made exactly that happen.
  if cmd.verb == "retreat" and o.held then
    release_held(state, info, nil, true, true)
  end

  -- ── "closest" becomes a pill NUMBER before anything else reads it ─────
  -- The order id, the auction and every ack are built from the resolved
  -- number, so from here on a closest order is an ordinary pill order.
  if cmd.target and cmd.target.closest then
    local id, why = M.closest_pill(state, world, info, sender, cmd.verb)
    if not id then
      if M.speaker(state, info) == me then say(state, why) end
      return true
    end
    cmd.target = { kind = "pill", id = id }
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
  if last_pns then
    who = { mode = "names", pns = last_pns }
  elseif o.sel and o.sel.by == sender and now < (o.sel.until_tick or 0) then
    who = { mode = "names", pns = o.sel.pns }
  end
  o.sel = nil

  -- `last` is "like naming them", down to the order id: the id is taken
  -- from the RESOLVED names, so "last attack 5" and "socrates attack 5" are
  -- one order, and the second line is a repeat of the first rather than a
  -- rival order the holder would announce it was leaving for.
  local oid_cmd = cmd
  if last_pns then
    oid_cmd = { verb = cmd.verb, who = who, target = cmd.target }
  end

  local spec = {
    oid = M.order_id(sender, oid_cmd), verb = cmd.verb, kind = kind,
    tkind = cmd.target and cmd.target.kind or nil,
    tid = tid, sender = sender, sender_name = player_name(info, sender),
    -- tkind "here" ("go there and hold", which is what a three-shot order
    -- is) carries its own square: there is no pill or base to look the
    -- position up from.
    mx = cmd.target and cmd.target.mx, my = cmd.target and cmd.target.my,
    needs_shells = needs_shells, who = who,
  }
  o.known[spec.oid] = { spec = spec, tick = now }
  note_last(o, sender, spec.oid)

  -- HOW MANY BOTS THE LINE IS FOR.  One, unless a count who-word said more.
  local want = 1
  if who.mode == "count" then want = who.n or 1 end

  -- Repeat of a live order we already hold = refresh the 60 s focus.
  -- A COUNT LINE IS NOT JUST A REPEAT: "4 attack 5" said to a bot that
  -- already holds the order on pill 5 has to re-open the auction for the
  -- three slots still empty, exactly as a repeat ping does.  The bots that
  -- hold it are excluded at settle time, so this only fills what is open.
  if o.held and o.held.oid == spec.oid then
    o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)
    if want <= 1 then
      repeat_ack(state, info, spec.oid, now)
      return true
    end
    start_order(state, world, info, spec, who, now, want)
    local au = o.auctions[spec.oid]
    if au then au.repeated = true end
    return true
  end

  return start_order(state, world, info, spec, who, now, want)
end

-- =========================================================================
-- SCENARIO HINTS — an order that did not arrive as chat
--
-- A scenario script calls game.hint(p, t) and the server hands this bot's VM
-- a flat table: a `verb` and whatever other keys the script wrote.  Every
-- value is TEXT, because that is how the table travels (a script writing
-- base = 3 sends "3"), so every number here goes through tonumber.
--
-- A hint names ONE bot -- the seat the script wrote -- so there is no
-- auction and no who-word to work out: the command is built with the
-- who-word a person gets by typing a bot's name, and everything past that
-- point is the chat path.  Same M.goal_kind, same order id, same
-- start_order, same acks, and a newer order from a person replaces it the
-- way any newer order replaces an older one.  `cancel all` and `cancel
-- <bot>` call one off; a bare `cancel` does not, because that one releases
-- only the SPEAKER's own order and the sender here is the scenario.
--
-- Seven verbs are the documented set (docs/SCENARIO_API.md).  Four of them
-- are one order and nothing more: goto, attack, defend and hold.  Three are
-- a STANDING instruction that outlives a single order -- patrol, escort and
-- avoid -- so they are kept on the order state and M.hint_update carries
-- them a step each think.  An eighth verb somebody invented is ignored in
-- silence: a brain is allowed not to know a word, and answering back would
-- make every scenario written for another brain noisy here.
-- =========================================================================

-- The sender a hint is filed under.  Not a seat: player numbers run 0..15,
-- so a hint can never collide with a person's `last`, and a person cancelling
-- their own order never takes a scripted one away by accident.
local HINT_SENDER = 255
M.HINT_SENDER = HINT_SENDER

-- A hint's numbers are decimal text.  nil for anything that is not a number.
local function hint_num(v)
  local n = tonumber(v)
  if not n then return nil end
  return math.floor(n)
end

local function hint_square(x, y)
  if not x or not y then return nil end
  if x < 0 or x > 255 or y < 0 or y > 255 then return nil end
  return x, y
end

local function hint_tile(t, xk, yk)
  return hint_square(hint_num(t[xk]), hint_num(t[yk]))
end

-- "go there and hold", in the command shape M.parse builds for the three-shot
-- line.  The square is packed into the target id the way that line packs it,
-- so one number identifies it in the order id and on the wire.
local function hint_goto(state, mx, my)
  return { verb = "goto",
           who = { mode = "names", pns = { state.player_number } },
           target = { kind = "here", id = mx * 256 + my, mx = mx, my = my } }
end

-- The middle of a rectangle, for a hint that names a region rather than a
-- square.  A script reads game.region(name) and writes the four numbers out;
-- the brain has no region table of its own to look a name up in.
local function hint_rect_middle(t)
  local x, y = hint_tile(t, "x", "y")
  local w, h = hint_num(t.w), hint_num(t.h)
  if not x then return nil end
  if w and h and w > 0 and h > 0 then
    return hint_square(x + math.floor((w - 1) / 2), y + math.floor((h - 1) / 2))
  end
  return x, y
end

-- The patrol route: x1,y1 x2,y2 ... up to seven points, which is what a
-- sixteen-pair table has room for beside the verb.  Numbered keys rather than
-- one packed string because the table is flat by design and a brain author
-- should not have to parse a value.
local function hint_points(t)
  local pts = {}
  for i = 1, 7 do
    local x, y = hint_tile(t, "x" .. i, "y" .. i)
    if not x then break end
    pts[#pts + 1] = { x = x, y = y }
  end
  return pts
end

-- The rectangle an `avoid` names, as its two corners.
local function hint_box(t)
  local x, y = hint_tile(t, "x", "y")
  if not x then return nil end
  local w = hint_num(t.w) or 1
  local h = hint_num(t.h) or 1
  if w < 1 then w = 1 end
  if h < 1 then h = 1 end
  return { x0 = x, y0 = y, x1 = x + w - 1, y1 = y + h - 1 }
end

local function hint_inside(box, mx, my)
  return box ~= nil and mx ~= nil
         and mx >= box.x0 and mx <= box.x1 and my >= box.y0 and my <= box.y1
end

-- The nearest square outside the box, walked out along the shorter axis.  A
-- bot standing in a place it has been told to keep out of is sent to the edge
-- and one square past it; every other order is left alone.
local function hint_way_out(box, mx, my)
  local left  = mx - box.x0 + 1
  local right = box.x1 - mx + 1
  local up    = my - box.y0 + 1
  local down  = box.y1 - my + 1
  local best, bx, by = left, box.x0 - 1, my
  if right < best then best, bx, by = right, box.x1 + 1, my end
  if up    < best then best, bx, by = up,    mx, box.y0 - 1 end
  if down  < best then best, bx, by = down,  mx, box.y1 + 1 end
  return hint_square(bx, by)
end

-- The tail of on_chat, from a command table to a running order.  Everything
-- it calls is what a typed line calls.
local function hint_start(state, world, info, cmd, now)
  local o = S(state)
  local kind, needs_shells, tid, err = M.goal_kind(cmd, world, info)
  -- err is the line a person would have been answered with.  Nobody is
  -- listening to a hint, so an order that cannot be built is dropped.
  if err or not kind then return false end

  local spec = {
    oid = M.order_id(HINT_SENDER, cmd), verb = cmd.verb, kind = kind,
    tkind = cmd.target and cmd.target.kind or nil,
    tid = tid, sender = HINT_SENDER, sender_name = "scenario",
    mx = cmd.target and cmd.target.mx, my = cmd.target and cmd.target.my,
    needs_shells = needs_shells, who = cmd.who,
  }
  o.known[spec.oid] = { spec = spec, tick = now }
  note_last(o, HINT_SENDER, spec.oid)

  -- The same hint again, while it is still held, refreshes the focus rather
  -- than restarting the job -- what a repeated chat line does.
  if o.held and o.held.oid == spec.oid then
    o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)
    return true
  end
  return start_order(state, world, info, spec, cmd.who, now)
end

-- The four verbs that are one order, as the command a chat line would have
-- produced.  nil means this brain has no order for that word, which is how an
-- unknown verb leaves in silence.
local function hint_command(state, world, info, t)
  local v  = t.verb
  local me = state.player_number
  local who = { mode = "names", pns = { me } }

  if v == "goto" then
    local mx, my = hint_rect_middle(t)
    if not mx then return nil end
    return hint_goto(state, mx, my)
  end

  if v == "hold" then
    -- A hold with no square is "stop where you are".
    local mx, my = hint_rect_middle(t)
    if not mx then
      mx, my = hint_square(bit.rshift(info.tankx or 0, 8),
                           bit.rshift(info.tanky or 0, 8))
    end
    if not mx then return nil end
    return hint_goto(state, mx, my)
  end

  if v == "attack" then
    local pn = hint_num(t.player)
    if not pn or pn < 0 or pn >= ally_state.MAX_TANKS then return nil end
    return { verb = "attack", who = who, target = { kind = "tank", pn = pn } }
  end

  if v == "defend" then
    local pid = hint_num(t.pill)
    if pid then
      return { verb = "defend", who = who, target = { kind = "pill", id = pid } }
    end
    -- DEFENDING A BASE IS STANDING ON IT.  There is no defend_base goal --
    -- a base is not held the way a pill is, which is why M.parse turns
    -- "defend base 3" down -- and a script is not a person who can be asked
    -- to say it another way.  The nearest thing the brain already does is the
    -- go-there lock on the base's own square.
    local bid = hint_num(t.base)
    local b   = bid and world.bases[bid]
    if not b then return nil end
    return hint_goto(state, b.mx, b.my)
  end

  return nil
end

-- =========================================================================
-- HINT ENTRY POINT — called from init.lua once per think for each hint the
-- server queued on this bot's VM.  Returns true when the hint became an
-- order or a standing instruction.
-- =========================================================================
function M.on_scenario_hint(state, world, info, t, now)
  if not C.BOT_COMMANDS_ENABLED then return false end
  if type(t) ~= "table" or type(t.verb) ~= "string" then return false end
  local o = S(state)
  local v = t.verb

  if v == "patrol" then
    local pts = hint_points(t)
    if #pts == 0 then return false end
    o.hint_patrol = { pts = pts, at = 1 }
    o.hint_escort = nil
    return hint_start(state, world, info,
                      hint_goto(state, pts[1].x, pts[1].y), now)
  end

  if v == "escort" then
    local pn = hint_num(t.player)
    if not pn or pn < 0 or pn >= ally_state.MAX_TANKS then return false end
    o.hint_escort = { pn = pn,
                      near = hint_num(t.distance) or C.ORDER_HINT_ESCORT_TILES,
                      mx = nil, my = nil }
    o.hint_patrol = nil
    -- It goes as soon as it knows where that seat is, which may be now.  The
    -- first leg is raised here rather than through M.hint_update, because
    -- that stands a standing hint down when the bot is holding somebody
    -- else's order -- and this hint IS the newer word about this bot, so it
    -- replaces that order the way any other order would.
    local mx = tonumber(ally_state.get_key(pn, "mx"))
    local my = tonumber(ally_state.get_key(pn, "my"))
    if mx and my and not hint_inside(o.hint_avoid, mx, my) then
      o.hint_escort.mx, o.hint_escort.my = mx, my
      hint_start(state, world, info, hint_goto(state, mx, my), now)
    end
    return true
  end

  if v == "avoid" then
    local box = hint_box(t)
    if not box then return false end
    o.hint_avoid = box
    -- Standing in it already is the one thing worth acting on straight away.
    local tmx = bit.rshift(info.tankx or 0, 8)
    local tmy = bit.rshift(info.tanky or 0, 8)
    if hint_inside(box, tmx, tmy) then
      local mx, my = hint_way_out(box, tmx, tmy)
      if mx then return hint_start(state, world, info,
                                   hint_goto(state, mx, my), now) end
    end
    return true
  end

  local cmd = hint_command(state, world, info, t)
  if not cmd then return false end
  -- A one-off order replaces whatever standing instruction was running: the
  -- script has said something newer about this bot.
  o.hint_patrol, o.hint_escort = nil, nil
  -- AND IT MAY NOT BREAK THE AVOID.  A square inside the box the script told
  -- this bot to keep out of is the script contradicting itself, and the
  -- keep-out is the older and broader instruction, so the order is dropped.
  if cmd.target and cmd.target.kind == "here"
     and hint_inside(o.hint_avoid, cmd.target.mx, cmd.target.my) then
    return false
  end
  return hint_start(state, world, info, cmd, now)
end

-- =========================================================================
-- THE STANDING HINTS, one step a think.  Called from init.lua beside
-- ORD.on_events, so an order this raises bids on the same tick a ping's
-- would.
-- =========================================================================
function M.hint_update(state, world, info, now)
  local o = state.orders
  if not o then return false end
  local h = o.held

  -- A STANDING HINT ENDS WHEN SOMEBODY GIVES THIS BOT A DIFFERENT JOB.  A
  -- held order from anyone but the script says a person (or a ping) has
  -- spoken more recently than the scenario did, and the newest word wins --
  -- the same rule the chat orders run on.  NOT holding an order is not that:
  -- the leg lapsed, or the bot was busy when it was raised, and the standing
  -- instruction is what says to try again.
  if h ~= nil and h.sender ~= HINT_SENDER then
    if o.hint_patrol or o.hint_escort then
    end
    o.hint_patrol = nil
    o.hint_escort = nil
    return false
  end

  -- PATROL.  The leg is a go-there order like any other, and arriving is
  -- what advances the route.  A leg is only raised while the bot is free:
  -- start_order answers a busy bot with a spoken "Busy", and asking it every
  -- think would be the only thing the team heard.
  if o.hint_patrol then
    local p = o.hint_patrol
    if h ~= nil and h.arrived then
      p.at = (p.at % #p.pts) + 1
      release_held(state, info, nil, true, true)
      h = nil
    end
    if h == nil and not M.busy(state, info) then
      return hint_start(state, world, info,
                        hint_goto(state, p.pts[p.at].x, p.pts[p.at].y), now)
    end
    return false
  end

  -- ESCORT.  The escorted seat's tile comes off the ally slate, which is
  -- filled by the /info state every ally BOT broadcasts; a human ally sends
  -- none, so a bot told to escort a person has nothing to follow and stays
  -- where it is.  The order is re-aimed only when the seat has moved further
  -- than `near` from where this bot was last sent, so the auction and the ack
  -- do not run every think.
  if o.hint_escort then
    local e  = o.hint_escort
    local mx = tonumber(ally_state.get_key(e.pn, "mx"))
    local my = tonumber(ally_state.get_key(e.pn, "my"))
    if not mx or not my then return false end
    if e.mx ~= nil and h ~= nil
       and U.mdist(e.mx, e.my, mx, my) <= (e.near or 3) then
      return false
    end
    if hint_inside(o.hint_avoid, mx, my) then return false end
    if M.busy(state, info) then return false end
    e.mx, e.my = mx, my
    return hint_start(state, world, info, hint_goto(state, mx, my), now)
  end

  return false
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
    -- Re-open the auction for the extra slot. Bots that already hold the
    -- order are excluded at settle time (o.gclaims), so this only fills what
    -- is still open, and it refreshes the focus for the whole group.
    start_order(state, world, info, k.spec, { mode = "ping" }, now, a.want)
    -- Mark the re-opened auction as a repeat: if it settles with nobody new
    -- (every bot that could go already holds it), the holders answer instead
    -- of the ping doing nothing visible.
    local au = o.auctions[k.spec.oid]
    if au then au.repeated = true end
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
  note_last(o, sender, spec.oid)
  o.anchors[spec.oid] = { sender = sender, mx = axm, my = aym, tick = now, want = 1 }
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
      -- A caution on a bot is a CANCEL of what it holds, not a hand-back:
      -- sending the next cheapest bot to finish the job a person just called
      -- off is the opposite of what the gesture means.
      release_held(state, info, "Released", false, true)
      return
    end
    local cmd  = { verb = "retreat", who = { mode = "names", pns = { me } } }
    local spec = { oid = M.order_id(sender, cmd), verb = "retreat",
                   kind = "take_cover", sender = sender,
                   sender_name = player_name(info, sender),
                   needs_shells = false, who = cmd.who, ping = true }
    o.known[spec.oid] = { spec = spec, tick = now }
    note_last(o, sender, spec.oid)
    local busy, reason = M.busy(state, info)
    if busy then
      say(state, string.format("Busy (%s)", reason))
    else
      take_order(state, world, info, spec, 0, now)
    end
    return
  end

  -- ── otherwise: in the 3x3 of an order's TARGET -> release the group ────
  -- This is about the ORDER, not about what stands on the tile, so the 3x3
  -- stays: a caution near the place the bots are working on calls them off.
  -- With no order on that place a caution does nothing at all -- a caution
  -- beside a pill no bot was sent to is not a cancel of anything.
  local ring = C.ORDER_PING_RING or 1
  local tmx  = (hit and hit.mx) or mx
  local tmy  = (hit and hit.my) or my
  for _, oid in ipairs(sorted_keys(o.known)) do
    local k = o.known[oid]
    local ox, oy
    if k then ox, oy = M.target_tile(world, state, k.spec) end
    if ox and oy and math.abs(ox - tmx) <= ring and math.abs(oy - tmy) <= ring then
      -- A CAUTION IS A CANCEL, so it goes out as obx: the bots that are not
      -- standing here (they never saw this ping's 3x3 the same way, and one
      -- of them may be holding the order) must forget it too, and the old obr
      -- would have sent the next cheapest one to do the very job a person
      -- just called off.
      if o.held and o.held.oid == oid then
        release_held(state, info, "Released", false, true)
      end
      forget_order(o, oid)
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
      -- A BOT'S OWN MARKER IS NEWS, NOT AN ORDER.  Bots now place ATTACK and
      -- ON_MY_WAY markers themselves, and this bot sees its team-mates' and
      -- its own come back through the same event array.  A marker from a bot
      -- says where a bot is going; it never commands anybody.  So every ping
      -- whose sender is a bot is read and dropped here, the BOT_COMMAND kind
      -- included: bots do not send that kind, and if one ever did it would be
      -- a bot ordering a bot, which nothing in this file is meant to do.
      local bots = info.player_bots or 0
      if bit.band(bots, bit.lshift(1, sender)) ~= 0 then
        kind = nil
      end
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

  -- 0. Remember where the allied tanks are, so "attack closest" has a tile
  --    to measure from even when the person who typed it has just driven out
  --    of sight.  One short pass; see M.note_seen.
  M.note_seen(state, info, now)

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
    elseif r.kind == "focus" then
      -- A late joiner (or a respawned bot) latching the team's setting.
      set_focus(state, CODE_FOCUS[r.value] or "off")
    elseif r.kind == "repo" then
      set_repo(state, r.value == 1)
    elseif r.kind == "bpings" then
      set_bot_pings(state, r.value == 1)
    elseif r.kind == "bchat" then
      set_bot_chat(state, r.value == 1)
    elseif r.kind == "cancel" then
      -- THE ORDER IS OVER.  Drop our own hold on it as well: the human who
      -- cancelled it named one bot, and on a group order the rest have to let
      -- go too.  Quiet and with no obx of our own -- the cancel is already on
      -- the wire and a second one would bounce around the team.
      if o.held and o.held.oid == r.oid then
        o.held = nil
        state._order = nil
        goto_lock(state)
      end
      forget_order(o, r.oid)
    elseif r.kind == "release" then
      if o.claims[r.oid] and o.claims[r.oid].pn == r.from then o.claims[r.oid] = nil end
      -- The releaser is not a holder any more, so the settle below must stop
      -- counting it as one -- otherwise it can never win its own order back.
      if o.gclaims[r.oid] then o.gclaims[r.oid][r.from] = nil end
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
  for _, oid in ipairs(sorted_keys(o.auctions)) do
    local a = o.auctions[oid]
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
      -- A REPEAT PING with no room left: the extra slot found nobody (every
      -- eligible bot already holds the order, or the rest cannot go), so the
      -- holders answer it the way a repeated chat line is answered.
      if a.repeated and #won == 0 then repeat_ack(state, info, oid, now) end
      o.auctions[oid] = nil
    end
  end

  -- 2b. Group order: ONE line, from the lowest player number that actually
  --     took it, once the claims have had a window to arrive.
  for _, oid in ipairs(sorted_keys(o.announce)) do
    local an = o.announce[oid]
    if an and now >= an.due then
      local low, n = nil, 0
      for pn in pairs(o.gclaims[oid] or {}) do
        n = n + 1
        if not low or pn < low then low = pn end
      end
      if low == me and n > 0 and o.held and o.held.oid == oid then
        if n > 1 then
          sayg(state, string.format("%d on %s", n,
              group_label(an.spec.kind, an.spec.tkind, an.spec.tid)))
        else
          sayg(state, string.format("%s %s", M.ack_for(my_name(state, info)),
              goal_label(an.spec.kind, an.spec.tid)))
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
      for _, oid in ipairs(sorted_keys(o.claims)) do
        local cl = o.claims[oid]
        local k  = o.known[oid]
        if cl and k and cl.pn ~= me and (now - cl.tick) >= (C.ORDER_STEAL_HOLD_TICKS or 100)
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

  -- 4. Live order housekeeping: THE JOB IS DONE, expiry, refuel pause.
  --
  -- AN ORDER ENDS THE MOMENT ITS TARGET CONDITION IS MET, on the tick the
  -- world shows it — not when the 60 s focus runs out.  Andrew watched a bot
  -- sit on a pill it had already swept until the timer let it go (Sep 14).
  -- Every test below reads the world and the perception this tick, so a group
  -- of bots on one order all let go on the same tick: they see one world.
  --
  -- The lines said here are the ones that were always said.  A met goal adds
  -- no new line of its own: the slot clears and goal selection picks up again
  -- on the next tick.
  local h = o.held
  if h then
    -- ARRIVAL STARTS THE HOLD.  A go-there order used to run on the 60 s
    -- focus whether the bot got there in two seconds or fifty, which made
    -- "go there" mean "and stand there for the rest of the minute".  Andrew,
    -- Sep 15: the hold is about ten seconds AFTER ARRIVAL.  So the focus
    -- bounds the TRAVEL (an order that never arrives still lapses on it), and
    -- the first think the tank is within one square of the target swaps the
    -- clock for ORDER_GOTO_HOLD_TICKS.  One line is said, with the number in
    -- it, so the human knows how long the bot will be standing there.
    if h.kind == "goto_tile" and h.mx and h.my and not h.arrived
       and info.tankx and info.tanky then
      local dx = tile_of(info.tankx) - h.mx
      local dy = tile_of(info.tanky) - h.my
      if dx < 0 then dx = -dx end
      if dy < 0 then dy = -dy end
      if dx <= 1 and dy <= 1 then
        local hold = C.ORDER_GOTO_HOLD_TICKS or 500
        h.arrived = now
        h.hold    = true
        h.expiry  = now + hold
        sayg(state, string.format("holding %ds", math.floor(hold / 50)))
        -- The hard lock goes the moment the hold starts, so goal selection is
        -- free to answer an enemy tank from this same tick.
        goto_lock(state)
      end
    end
    local done, why = false, nil
    if now >= h.expiry then
      done, why = true, "order lapsed"
      -- A go-there order that ARRIVED ends in silence.  The bot already said
      -- "holding 10s" and that number was the whole promise; saying "order
      -- lapsed" ten seconds later is the same fact twice.
      if h.kind == "goto_tile" and h.arrived then why = nil end
    elseif h.tkind == "pill" and h.tid then
      local p = world.pills[h.tid]
      if not p then
        done, why = true, "order lapsed"
      else
        -- ATTACK -> SWEEP, in place.  A pill shot to zero armour cannot be
        -- attacked any further; the natural next move is to pick it up, which
        -- is what capture_pill does.  The order KEEPS ITS ID and its sender,
        -- so there is no second ack and no second auction — it is the same
        -- job, carried on.  A dead pill needs no shells, so the refuel pause
        -- goes with it.
        if h.kind == "attack_pill" and (p.health or 0) == 0 and not p.in_tank then
          h.kind         = "capture_pill"
          h.needs_shells = false
          h.refuel_said  = nil
          state._order   = h
        end
        if h.kind == "attack_pill" or h.kind == "capture_pill" then
          -- DONE is any of three things, and they are all "there is nothing
          -- left to go and do at that square":
          --   * the pill flew our flag again (someone swept it, us or an ally)
          --   * it is off the map, in SOMEBODY's tank (in_tank) -- a carried
          --     pill cannot be attacked or swept by anyone
          --   * it is in OUR OWN cargo, which is the same in_tank flag: a pill
          --     we are carrying reads as "allied", never "friendly", which is
          --     exactly why the old owner-only test never fired for the bot
          --     that did the sweeping.
          if p.in_tank or p.owner == "friendly" then
            done, why = true, string.format("%s #%d done", h.kind, h.tid)
          end
        end
        -- DEFEND ENDS WITH THE PILL (Andrew, Sep 15).  A bot told to defend a
        -- pill that was then shot flat and taken stayed in defend_pill and
        -- did nothing at all: the goal was still "guard that square", and the
        -- reject pass went on killing every other strategic row for it.
        -- There is nothing to defend once the pill is not ours, so the order
        -- is over.  The bot says one line and goes back to its own goals --
        -- which may well decide to go and take the pill back, and that is a
        -- decision for goal selection, not for a dead order.
        --
        -- defend was left out of these rules on purpose once, but that was
        -- about the TIMER (holding the spot is the job, so it runs its clock
        -- out) and not about the target going away.
        if h.kind == "defend_pill" then
          if p.owner ~= "friendly" or (p.health or 0) == 0 or p.in_tank then
            done, why = true, string.format("lost pill #%d", h.tid)
          end
        end
      end
    elseif h.tkind == "base" and h.tid then
      local b = world.bases[h.tid]
      if b and (h.kind == "capture_base" or h.kind == "attack_base")
         and b.owner == "friendly" then
        done, why = true, string.format("base #%d done", h.tid)
      end
    elseif h.tkind == "tank" and h.tid then
      -- A NAMED TANK.  From inside the brain a dead tank and one that drove
      -- out of sight look the same: it stops appearing in the object scan,
      -- and its ghost (the remembered position perception keeps for a while)
      -- ages out too.  M.target_tile reads both lists, so "target_tile has
      -- no answer" is the one test that covers both.  A short blink behind a
      -- forest must not end the order, so it has to hold for
      -- ORDER_TANK_LOST_TICKS.
      if M.target_tile(world, state, h) then
        h.tank_seen = now
      elseif (now - (h.tank_seen or h.since or now))
             >= (C.ORDER_TANK_LOST_TICKS or 500) then
        done, why = true, string.format("Lost %s", player_name(info, h.tid))
      end
    end
    -- defend_pill, take_cover (a retreat) and goto_tile ("go there and hold")
    -- are NOT in this list on purpose.  Holding the spot IS the job, so they
    -- run to a timer.  goto_tile in particular must not end on arrival:
    -- arriving is the START of the hold (it sets the ten-second clock above),
    -- not the end of the order.
    if done then
      if why then sayg(state, why) end
      -- A FINISHED ORDER IS CANCELLED, NOT HANDED BACK.  Every reason above
      -- is "there is nothing left to go and do": the pill is ours, the base
      -- is ours, the tank is gone, the clock ran out, the hold is served.  An
      -- obr here re-opened the auction on every other bot, and a second bot
      -- was sent to a go-there square the first had already finished holding
      -- (Andrew's peer review, Sep 16).
      release_held(state, info, nil, true, true)
      h = nil
    end
  end

  if h and h.needs_shells then
    -- A pause, not an exit: the timer keeps running while the tank tops up.
    local low = (info.shells or 0) < (C.SHELLS_LOW or 20)
    if low and not h.refuel_said then
      h.refuel_said = true
      sayg(state, "Refuelling, coming back")
    elseif not low then
      h.refuel_said = nil
    end
  end

  -- 5. Prune.
  for _, oid in ipairs(sorted_keys(o.known)) do
    local k = o.known[oid]
    if k and (now - k.tick) > (C.ORDER_FOCUS_TICKS or 3000) then
      forget_order(o, oid)
    end
  end
  if o.sel and now >= (o.sel.until_tick or 0) then
    -- A lapsed selection: the selected bots say so once, like `nevermind`.
    for _, pn in ipairs(o.sel.pns or {}) do
      if pn == me then say(state, "Standing by") end
    end
    o.sel = nil
  end

  -- 6. THE SPEAKING BOT: the latch heartbeat.  Only the lowest-numbered bot
  --    on the team talks, so a four-bot team says each line once.
  --    Nothing is said at game start any more: the lobby carries the brain's
  --    announce line and its docs (see the note beside M.HELP).
  if M.speaker(state, info) == me then
    -- Re-broadcast the team settings so a bot that joined or respawned late
    -- latches the same values. They ride their own verbs rather than the
    -- /info state slate, which is already close to the 124-byte batch budget.
    if (o.focus or o.repo_on ~= nil or o.bot_pings ~= nil or o.bot_chat ~= nil)
       and now >= (o.latch_tx or 0) then
      o.latch_tx = now + (C.ORDER_LATCH_REBROADCAST_TICKS or 1500)
      tx(state, string.format("/info obf %d", FOCUS_CODE[o.focus or "off"] or 0))
      if o.repo_on ~= nil then
        tx(state, string.format("/info obp %d", o.repo_on and 1 or 0))
      end
      if o.bot_pings ~= nil then
        tx(state, string.format("/info obg %d", o.bot_pings and 1 or 0))
      end
      if o.bot_chat ~= nil then
        tx(state, string.format("/info obh %d", o.bot_chat and 1 or 0))
      end
    end
  end

  state._order          = o.held
  state._focus          = o.focus
  state._repo_override  = o.repo_on

  -- 7. THE GO-THERE LOCK, re-asserted every think.  This runs before goal
  --    selection (init.lua calls ORD.update first), so a command goal cleared
  --    by arrival last think is back before pick_goal looks at it, and the bot
  --    stays on the square instead of handing the tick to the pools.
  goto_lock(state)
end

-- One line for the goal panel / debug_info.
function M.panel_line(state, info)
  local h = state.orders and state.orders.held
  if not h then return "" end
  local left = math.max(0, (h.expiry or 0) - (state.tick or 0))
  -- A PLACE ORDER IS A HARD LOCK, and the panel has to say so.  It runs as a
  -- command goal, which pick_goal answers before goal selection, so the goal
  -- POOLS are never evaluated while it stands and the pool panel sits empty.
  -- Without this line that empty panel looks like a broken brain; with it the
  -- reason is on screen, next to the square and the time left.
  -- Once it has arrived the lock is off and the pools are live again (only
  -- the reactive rows can win), so the word changes with the phase.
  if h.kind == "goto_tile" then
    return string.format(" ORDER goto (%d,%d) from %s, %d s left, %s",
      h.mx or -1, h.my or -1, h.sender_name or "?", math.floor(left / 50),
      h.hold and "holding" or "hard")
  end
  return string.format(" ORDER %s#%s from %s %ds left",
    h.kind, tostring(h.tid or "-"), h.sender_name or "?", math.floor(left / 50))
end

return M
