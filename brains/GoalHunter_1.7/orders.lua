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
--   /info obd <oid> <cost>   BID.   cost -1 means "no, I cannot go" and -2
--                            "no, I am busy or on a person's job".  -3 or
--                            lower is "no, I am on a person's job, but I
--                            would switch at cost -3 - <cost>"
--                            (ORDER_NO_FREE_TAKES_LOWEST; M.note_bid).
--                            Older bots read any negative cost as "no".
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
local GA         = require("decoy_getaway")

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
          -- botcmd[sender] = { tid, tick, oid }: the last BOT COMMAND ping
          -- that sender put on an enemy live pill.  An ATTACK ping on the same
          -- pill inside C.PING_SUICIDE_WINDOW_TICKS turns it into a suicide
          -- run (see PING SUICIDE RUN below).
          botcmd = {},
          banner = nil, latch_tx = 0, anchors = {},
          -- early[oid][pn] = { cost, tick }: a bid that came in before this
          -- bot opened its own auction on that order (ORDER_CLAIM_TIEBREAK).
          -- ack_due = { oid, due, line, mx, my }: a solo ack waiting out the
          -- claim window.
          early = {}, ack_due = nil,
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
  local g = text:match("^/info obg (%d)$")
  if g then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "bpings", value = tonumber(g), from = sender, tick = tick }
    print2(string.format("ORDER_RX obg from p%s v=%s t=%d", tostring(sender), g, tick))
    return true
  end
  local h = text:match("^/info obh (%d)$")
  if h then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "bchat", value = tonumber(h), from = sender, tick = tick }
    print2(string.format("ORDER_RX obh from p%s v=%s t=%d", tostring(sender), h, tick))
    return true
  end
  oid = text:match("^/info obr (%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "release", oid = tonumber(oid), from = sender, tick = tick }
    print2(string.format("ORDER_RX obr from p%s oid=%s t=%d", tostring(sender), oid, tick))
    return true
  end
  -- OFFER (ORDER_HOLDER_KEEPS_JOB): "I won this auction but I will not take
  -- it".  The sender bid before it held a person's order, then won another
  -- auction first; it keeps that job (see the settle in M.update).  Every bot
  -- re-opens the auction on an offer, ORDER_NO_HAND_BACK or not: the order
  -- was never taken, so it still needs a bot.
  oid = text:match("^/info obo (%d+)$")
  if oid then
    local o = S(state)
    o.rx[#o.rx + 1] = { kind = "release", offer = true, oid = tonumber(oid),
                        from = sender, tick = tick }
    print2(string.format("ORDER_RX obo from p%s oid=%s t=%d", tostring(sender), oid, tick))
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
    print2(string.format("ORDER_RX obx from p%s oid=%s t=%d", tostring(sender), oid, tick))
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
    print2(string.format("ORDER_GOTO_LOCK t=%d oid=%d tile=(%d,%d)",
           state.tick or 0, h.oid or 0, h.mx, h.my))
  elseif state.command_goal and state.command_goal.kind == "goto_tile" then
    state.command_goal = nil
    print2(string.format("ORDER_GOTO_UNLOCK t=%d", state.tick or 0))
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
--   RELEASED  (cancelled = false)  this bot cannot do the job any more: it
--             died, it got stuck, a newer order took its place, it lost a
--             claim tiebreak.  It broadcasts obr.  Under ORDER_NO_HAND_BACK
--             (the default) no bot re-bids on an obr, so the job ends here;
--             with it off (keel) the bot with the next cheapest route picks
--             the job up.  Either way obr takes this bot out of every other
--             bot's list of holders.
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
-- A DECOY HOLD THAT IS OVER takes its hold goal with it (see GO-THERE DECOY
-- HARD HOLD below), so the next goal selection starts from "none" (an
-- urgent replan) instead of the bot standing on a goto_tile nobody holds.
-- A fight goal is left alone: it is a real goal and normal play may carry
-- on with it.  Every way a decoy slot is emptied comes through here.
-- The hold goal is found by its _decoy mark as well as by the decoy square
-- (Sep 26): after a DECOY GETAWAY step it points at a chain square, and the
-- square test alone left it behind at every hold end.  The square test stays
-- for the ping order's own goto_tile, which decoy_lock keeps in place.
local function decoy_drop_goal(state, h)
  if not (h and h.decoy) then return end
  local g = state.goal
  if g and g.kind == "goto_tile"
     and (g._decoy or (g.mx == h.mx and g.my == h.my)) then
    require("attack").clear_attack_goal(state, "decoy hold over")
  end
end

local function release_held(state, info, why, quiet, cancelled)
  local o = S(state)
  local h = o.held
  if not h then return end
  o.held = nil
  state._order = nil
  goto_lock(state)                      -- the hard lock goes with the order
  decoy_drop_goal(state, h)
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
  print2(string.format("ORDER_REL t=%d oid=%d why=%s cancelled=%s",
         state.tick or 0, h.oid, tostring(why), tostring(cancelled and true or false)))
end
M.release_held = release_held

-- A DECOY HOLD THAT ENDS ENDS FOR THIS BOT ONLY (ORDER_GOTO_DECOY).  A repeat
-- ping can put two bots on one decoy square.  Its end -- the tank died, a
-- caution on it, the pills went down, the clock ran out -- used to go out as
-- obx, and obx makes every bot drop the order, so one decoy's death stopped
-- its partner on the same tick.  With another bot still holding the order
-- (o.gclaims), this bot now lets go of its own share only: an obr, which
-- takes it out of every bot's holder set, and under ORDER_NO_HAND_BACK
-- nobody re-bids it.  Without that knob an obr would hand the square to the
-- next bot, so the old cancel stays.  The partner keeps its hold until its
-- own end comes.  The LAST holder out still cancels (obx), exactly as a solo
-- decoy always did, so the order is forgotten everywhere once nobody holds
-- it.  decoy_left marks the order on this bot so the steal pass does not
-- take it back: standing on the square, this bot would be the cheapest.
-- `line` is what release_held says (nil = in silence).
-- Does another bot still hold the order this bot holds?  Only asked with
-- ORDER_NO_HAND_BACK on; without it the answer is always no (the old cancel).
function M.decoy_partner(state)
  local o = state and state.orders
  local h = o and o.held
  if not (h and C.ORDER_NO_HAND_BACK) then return false end
  for pn in pairs(o.gclaims[h.oid] or {}) do
    if pn ~= state.player_number then return true end
  end
  return false
end

function M.decoy_release(state, info, line)
  local o = S(state)
  local h = o.held
  if not h then return end
  if not M.decoy_partner(state) then
    release_held(state, info, line, line == nil, true)
    return
  end
  local k = o.known[h.oid]
  if k then k.decoy_left = true end
  print2(string.format("ORDER_DECOY_LEAVE t=%d oid=%d -- a partner still holds it",
         state.tick or 0, h.oid or 0))
  release_held(state, info, line, line == nil)
end

-- A DEAD BOT HANDS ITS ORDER BACK.  init.lua's dead-tick block returns before
-- ORD.update ever runs, so nothing cleared the slot: a bot killed while
-- holding a go-there order came back with h.hold still set, parked on its
-- respawn square, and stood there for the rest of the hold doing nothing
-- (Andrew's peer review, Sep 16).  This is a RELEASE, not a cancel: the obr
-- goes out, and under ORDER_NO_HAND_BACK (the default) nobody re-bids, so
-- the job ends with this bot; with it off (keel) the next cheapest bot takes
-- it.  A decoy's order is cancelled instead (below), unless a partner decoy
-- still holds it (M.decoy_release).  Quiet: a death is not a line the team
-- needs.  Safe to call on every dead tick; it does nothing
-- without a slot.
function M.on_death(state, info)
  -- A dead tank's suicide run is over: that is one of its two endings.
  if state and state._suicide then M.suicide_end(state, "tank died") end
  local o = state and state.orders
  if not (o and o.held) then
    if state then state._order = nil end
    return false
  end
  -- A DECOY that dies has done its job (Andrew's end (d)): the order is
  -- CANCELLED, not handed back, or the next cheapest bot drives to the same
  -- square and dies the same way.  With a partner decoy still on the order
  -- this bot only lets go of its own share (M.decoy_release).
  local decoy = o.held.decoy and o.held.hold
  print2(string.format("ORDER_DEATH t=%d oid=%d kind=%s -- %s",
         state.tick or 0, o.held.oid or 0, tostring(o.held.kind),
         decoy and "decoy over"
         or (C.ORDER_NO_HAND_BACK and "dropped" or "handed back")))
  if decoy then
    M.decoy_release(state, info, nil)
  else
    release_held(state, info, nil, true, nil)
  end
  o.sel = nil
  return true
end

-- =========================================================================
-- PING SUICIDE RUN (Andrew, 2026-09-24)
--
-- THE GESTURE: a person puts a BOT COMMAND ping on an enemy live pill, then
-- an ATTACK ping on the SAME pill within C.PING_SUICIDE_WINDOW_TICKS (50
-- brain ticks = 1 s; the brain thinks every second engine tick).  Same
-- sender for both.  Bot command first, attack second: the other order is
-- NOT a trigger, so an attack marker a person puts down for the team and a
-- bot command a moment later stay two ordinary pings.
--
-- WHO GOES: every allied bot whose goal is attack_pill on that pill when
-- the attack ping lands -- the bots the bot command sent (they hold its
-- order) and any bot that was already attacking the pill on its own.  The
-- order spec is marked too, so a bot that wins the bot-command auction
-- AFTER the attack ping (the auction was still open) also goes, and so does
-- a bot added later by a repeat bot-command ping on the same pill.
--
-- WHAT IT DOES: state._suicide holds the run.  pick_goal answers it before
-- anything else (goals.lua), init.lua re-installs it right before the
-- attack substate machine and steering every think (M.suicide_lock), and
-- attack.lua runs it as kill_hardline with every abort taken out.  So no
-- refuel, no flee, no take_cover, no critical-armour pause, no shell gate,
-- no LGM rescue and no stuck-flee can take the tank off the pill.  With no
-- shells left it still drives onto the pill.
--
-- HOW IT ENDS: the tank dies (M.on_death), or the pill is dead -- armour 0,
-- ours, in a tank, or gone (M.suicide_lock).  The ONE escape hatch is a
-- person calling it off: any cancel line that reaches this bot, or a
-- caution ping on the bot or on the pill.  An order lapsing does not end it.
-- =========================================================================

-- Why the run on this pill is over, or nil while it still stands.
local function suicide_over(p)
  if not p then return "pill gone" end
  if p.in_tank then return "pill carried" end
  if p.owner == "friendly" then return "pill ours" end
  if (p.health or 0) <= 0 then return "pill dead" end
  return nil
end
M.suicide_over = suicide_over

-- The goal the run drives.  A NEW table every call: clear_attack_goal wipes
-- the live goal in place, so a shared template would be wiped with it.
function M.suicide_goal(r)
  return { kind = "attack_pill", mx = r.mx, my = r.my,
           wx = U.m2w(r.mx), wy = U.m2w(r.my), target_id = r.tid,
           substate = "kill_hardline", _ping_suicide = true, _ordered = true }
end

-- Start the run on pill `tid` (idempotent: a second trigger on the same
-- pill says nothing).  One chat line per bot, a goal confirmation like the
-- order acks, so "bot chat off" silences it.
function M.suicide_start(state, world, tid, sender, now, why)
  if not C.PING_SUICIDE_ENABLED then return false end
  local p = world and world.pills and world.pills[tid]
  if not p or suicide_over(p) then return false end
  local r = state._suicide
  if r and r.tid == tid then return false end
  state._suicide = { tid = tid, mx = p.mx, my = p.my, sender = sender,
                     since = now }
  sayg(state, string.format("Suicide run on pill #%s", tostring(tid)))
  print2(string.format("PING_SUICIDE_START t=%d pill=%s at (%d,%d) from p%s why=%s",
         now or -1, tostring(tid), p.mx, p.my, tostring(sender), tostring(why)))
  return true
end

-- A HUMAN TEAM-MATE CLOSE BY STARTS THE SAME RUN.  An attack_pill goal the
-- bot was ORDERED to do (o.held is an attack_pill order on the same pill),
-- with a visible human ally within ORDER_HUMAN_NEAR_SUICIDE_TILES of the bot,
-- becomes a full suicide run on that pill, as if the human had pinged it.
-- An attack_pill goal the bot chose for itself does not.  The human is the run's sender,
-- so a bare "cancel" from them ends it.  init.lua calls this every think,
-- so a human who drives up mid-approach counts too.  A run the human
-- cancelled (cancel, caution) does not restart on the same pill until the
-- bot has had some other goal.
function M.human_near_suicide(state, world, info, now)
  if state._suicide or not C.ORDER_HUMAN_NEAR_SUICIDE_RUN then return false end
  local tiles = C.ORDER_HUMAN_NEAR_SUICIDE_TILES or 0
  if tiles <= 0 then return false end
  local g = state.goal
  if not (g and g.kind == "attack_pill" and g.target_id) then
    state._suicide_waived = nil
    return false
  end
  if state._suicide_waived == g.target_id then return false end
  state._suicide_waived = nil
  local h = state.orders and state.orders.held
  if not (h and h.kind == "attack_pill" and h.tid == g.target_id) then
    return false
  end
  local d, pn = U.human_ally_near(info, bit.rshift(info.tankx or 0, 8),
                                  bit.rshift(info.tanky or 0, 8), tiles)
  if not d then return false end
  return M.suicide_start(state, world, g.target_id, pn, now,
                         string.format("human_ally_%d_away", d))
end

-- End the run.  Quiet unless the caller passes a line.
function M.suicide_end(state, why, line)
  local r = state._suicide
  if not r then return false end
  state._suicide = nil
  -- A person called it off: the human-near rule must not start it again.
  if why == "cancel" or why == "caution ping" then state._suicide_waived = r.tid end
  if line then say(state, line) end
  print2(string.format("PING_SUICIDE_END t=%d pill=%s why=%s ran=%d",
         state.tick or 0, tostring(r.tid), tostring(why),
         (state.tick or 0) - (r.since or 0)))
  return true
end

-- THE LOCK.  init.lua calls this every think right before the attack
-- substate machine and steering, so whatever else the think did to the goal
-- (a flee, a refuel, an escape, a stuck handler, a budget drop) is undone
-- before the tank moves.  Ends the run when the pill is dead and hands the
-- goal back to normal play.  Returns true while the run stands.
function M.suicide_lock(state, world, info, now)
  local r = state._suicide
  if not r then return false end
  local p = world.pills and world.pills[r.tid]
  local why = suicide_over(p)
  if why then
    M.suicide_end(state, why)
    local g = state.goal
    if g and g._ping_suicide then
      require("attack").clear_attack_goal(state, "suicide run over: " .. why)
    end
    return false
  end
  local g = state.goal
  if not (g and g._ping_suicide and g.kind == "attack_pill"
          and g.target_id == r.tid) then
    local was = g and g.kind or "nil"
    state.goal = M.suicide_goal(r)
    state.goal_set_tick = now
    state.command_goal = nil
    if state.pf then state.pf.status = "idle" end
    print2(string.format("PING_SUICIDE_LOCK t=%d pill=%s replaced=%s",
           now or -1, tostring(r.tid), tostring(was)))
  end
  return true
end

-- The trigger itself: every bot on this pill goes.  Called by the ATTACK
-- ping once the window test has passed.
local function suicide_trigger(state, world, info, sender, tid, oid, now)
  local o = S(state)
  -- Mark the order: a bot that takes it after this (the auction was still
  -- open, or a repeat bot-command ping adds a bot) goes on the run too.
  local k = oid and o.known[oid]
  if k and k.spec then k.spec.suicide = true end
  local h = o.held
  local g = state.goal
  local holds  = h and h.kind == "attack_pill" and h.tid == tid
  local on_it  = g and g.kind == "attack_pill" and g.target_id == tid
  if holds or on_it then
    M.suicide_start(state, world, tid, sender, now,
                    holds and "holds the order" or "attacking it")
  end
  print2(string.format("PING_SUICIDE_TRIGGER t=%d pill=%s oid=%s from p%s me: holds=%s goal_on_pill=%s",
         now, tostring(tid), tostring(oid), tostring(sender),
         tostring(holds and true or false), tostring(on_it and true or false)))
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
  -- A DECOY GETAWAY (decoy_getaway.lua) that is driving its chain is not
  -- parked, and once it is done it parks on the chain's last square.
  local pmx, pmy = h.mx, h.my
  if h.decoy and h.ga then
    if GA.driving(h) then return false end
    pmx, pmy = GA.park_tile(h)
  end
  local dx = bit.rshift(info.tankx, 8) - pmx
  local dy = bit.rshift(info.tanky, 8) - pmy
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return dx <= 1 and dy <= 1
end

-- =========================================================================
-- GO-THERE DECOY HARD HOLD (Andrew, 2026-09-24)
--
-- THE GESTURE: a person puts a BOT COMMAND ping on open ground.  That was
-- always "go there and hold" (goto_tile), and on arrival the bot stood on
-- the square for ORDER_GOTO_HOLD_TICKS in a SOFT hold: goal selection ran
-- again, and any ORDER_REACTIVE_KINDS row -- take_cover, refuel, a flee, a
-- water escape -- drove it straight off the square.  Andrew pings a square
-- beside an enemy pill to put a bot there as a DECOY, so the pill shoots the
-- bot and not him, and the soft hold threw that away the first time the
-- pill fired: the bot took cover.
--
-- THE RULE, with C.ORDER_GOTO_DECOY on, decided ONCE at the arrival moment
-- (the same one-square test that starts today's hold):
--   * NO enemy pill can shoot the ordered square: the order is over, there
--     and then, in silence.  There is nothing to be a decoy for, and "stand
--     there for ten seconds" was never the point.
--   * One or more can: a HARD hold.  The bot says "decoying 10s", parks on
--     the square and never drives: no refuel, no cover, no flee, no water
--     escape, no stuck handler.  It may still turn and shoot an enemy TANK
--     (attack_tank) or an enemy BUILDER (kill_lgm) inside gun range, from
--     the spot.  It never shoots a pill -- that is not what it was sent for.
--
-- "CAN SHOOT THE SQUARE" is the brain's own pill-threat rule (perception
-- pill_threats): a pill that is not ours (owner not "friendly") and not in a
-- team-mate's tank (owner "allied"), not carried (in_tank) and alive (health
-- > 0).  NEUTRAL pills count: perception treats "hostile AND neutral fire at
-- the tank" the same.  The distance is util.edist, the straight-line tile
-- distance util.lua says every physical reach must use, against the real
-- firing range C.PILL_FIRE_RANGE (8) -- not the 9-tile danger stamp.
--
-- HOW IT ENDS -- any one of these:
--   a) no counted pill is left: the set is re-read EVERY think, so a pill
--      that comes into range later counts too (M.update and M.decoy_lock);
--   b) ORDER_GOTO_HOLD_TICKS after arrival, in silence, like today's hold;
--   c) a CAUTION ping within ORDER_PING_RING of the bot's TANK from any
--      human ally -- "Released", and no retreat on the same ping;
--   d) any cancel line, by the rules every order already follows;
--   e) the tank dies (M.on_death);
--   f) a new order from a person: take_order's "latest order wins".  The
--      decoy is NOT busy (M.busy), so the new order is always taken.
--
-- PING ONLY.  A chat "!goto" and a scenario hint keep today's soft hold:
-- only a person's ping chooses the exact square a decoy needs.  The chat
-- verb `decoy` is a different, unbuilt thing and is left alone.
-- =========================================================================

-- The goals a decoy may fight with.  Every other kind is undone by the lock.
local DECOY_FIGHT_KINDS = { attack_tank = true, kill_lgm = true }

-- How many pills can shoot square (mx,my).  See the rule above.
local function decoy_pills(world, mx, my)
  local n = 0
  local r = C.PILL_FIRE_RANGE or 8
  for _, p in pairs((world and world.pills) or {}) do
    if p.owner ~= "friendly" and p.owner ~= "allied" and not p.in_tank
       and (p.health or 0) > 0 and p.mx and p.my
       and U.edist(mx, my, p.mx, p.my) <= r then
      n = n + 1
    end
  end
  return n
end
M.decoy_pills = decoy_pills

-- The hold goal: the same goto_tile pick_goal hands back in a hold, marked
-- so the end of the decoy can tell it from any other goto_tile.  A NEW
-- table every call: clear_attack_goal wipes the live goal in place.
-- With a DECOY GETAWAY under way it points at the chain's next square, or
-- at the square the chain ended on (decoy_getaway.park_tile).
function M.decoy_goal(h)
  local mx, my = GA.park_tile(h)
  return { kind = "goto_tile", mx = mx, my = my,
           wx = U.m2w(mx), wy = U.m2w(my), target_id = -1,
           _ordered = true, _order_hold = true, _decoy = true,
           _getaway = GA.driving(h) or nil }
end

-- Is this goal allowed while the decoy stands?  attack_tank on an ENEMY
-- tank (a "kill me" attack_tank aims at an ALLY: km_ally_pn) or kill_lgm,
-- with the target inside gun range of the tank -- C.TANK_COMBAT_ENGAGE_RANGE,
-- the brain's "start shooting at this distance (~max shell range)".  The
-- target's live tile comes from perception when it is in sight; the goal's
-- own tile otherwise.  Out of range is not allowed: a parked tank cannot
-- chase, so the goal would only point the gun at nothing.
function M.decoy_allows(state, info, g)
  if not (g and DECOY_FIGHT_KINDS[g.kind]) then return false end
  -- A getaway that is driving its chain never leaves it to fight.
  if GA.driving(state and state._order) then return false end
  if g.km_ally_pn then return false end
  if not (info and info.tankx and info.tanky) then return false end
  local tx, ty = g.mx, g.my
  local perc = state and state.perc
  if perc then
    local list, key = nil, nil
    if g.kind == "attack_tank" then list, key = perc.enemy_tanks, "id"
    else list, key = perc.enemy_lgms, "idnum" end
    for _, e in ipairs(list or {}) do
      if e[key] == g.target_id and e.mx and e.my then
        tx, ty = e.mx, e.my
        break
      end
    end
  end
  if not (tx and ty) then return false end
  local d = U.edist(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8), tx, ty)
  return d <= (C.TANK_COMBAT_ENGAGE_RANGE or 7)
end

-- The slot, when it is a standing decoy hold.
local function decoy_held(state)
  local h = state and state.orders and state.orders.held
  if h and h.decoy and h.hold and h.kind == "goto_tile" and h.mx and h.my then
    return h
  end
  return nil
end
M.decoy_held = decoy_held

-- End the decoy.  `line` nil = in silence.  A decoy that ends is a job that
-- is over, so it is never handed back to another bot: CANCELLED (obx) when
-- this bot is the only holder, and this bot's share let go when a partner
-- still holds it (M.decoy_release).
local function decoy_end(state, info, why, line)
  local h = decoy_held(state)
  if not h then return false end
  print2(string.format("ORDER_DECOY_END t=%d oid=%d why=%s held=%d",
         state.tick or 0, h.oid or 0, tostring(why),
         (state.tick or 0) - (h.arrived or state.tick or 0)))
  M.decoy_release(state, info, line)
  return true
end
M.decoy_end = decoy_end

-- THE LOCK.  init.lua calls this every think right before the suicide-run
-- lock, the attack substate machine and steering, so whatever the think did
-- to the goal (a refuel, take_cover, a flee, a water escape, a stuck handler,
-- a PF-failed flee, a budget drop, an attack_pill) is undone before the tank
-- moves.  An attack_tank or kill_lgm in gun range stays.  Off the square
-- (pushed, or the one-square radius left) the hold goal drives it back, and
-- only then: steering parks it the moment it is on the square again.
-- Ends the decoy instead when no pill can shoot the square any more.
-- Returns true while the decoy stands.
function M.decoy_lock(state, world, info, now)
  local h = decoy_held(state)
  if not h then return false end
  -- A suicide run is its own lock and a newer order than this one.
  if state._suicide then return false end
  if decoy_pills(world, h.mx, h.my) == 0 then
    decoy_end(state, info, "pills down")
    return false
  end
  -- THE GETAWAY (decoy_getaway.lua): scan, turn, drive after the hit.  It
  -- moves the square the hold goal points at; nothing else here changes.
  GA.update(state, world, info, h, now)
  local pmx, pmy = GA.park_tile(h)
  local g = state.goal
  if g and g.kind == "goto_tile" and g.mx == pmx and g.my == pmy then
    -- On arrival at a chain square the park square is the square it drove
    -- to, so this goal table is kept; its _getaway mark (set only while the
    -- chain moves) is taken off here (Sep 26).
    if g._getaway and not GA.driving(h) then g._getaway = nil end
    return true
  end
  if M.hold_parked(state, info) and M.decoy_allows(state, info, g) then
    return true
  end
  local was = g and g.kind or "nil"
  state.goal = M.decoy_goal(h)
  state.goal_set_tick = now
  state.command_goal = nil
  if state.pf then state.pf.status = "idle" end
  state.stuck_for = 0
  print2(string.format("ORDER_DECOY_LOCK t=%d oid=%d tile=(%d,%d) replaced=%s",
         now or -1, h.oid or 0, pmx, pmy, tostring(was)))
  return true
end

-- NO THROTTLE, EVER, on the square.  steering.lua already takes KEY_FASTER
-- away for the park kinds, but init.lua's kill_lgm crosshair search runs
-- AFTER steering and picks throttle as one of its 27 moves.  The soft hold
-- lets that through; a decoy does not.  Called on the final keys.
-- A DECOY GETAWAY with a chain turns the parked tank to face the chain's
-- first square here too (decoy_getaway.keys).
function M.decoy_keys(state, info, keys, taps)
  local KF = _G.KEY_FASTER
  local h = decoy_held(state)
  if h and M.hold_parked(state, info) then
    if KF then
      if keys then keys = bit.band(keys, bit.bnot(KF)) end
      if taps then taps = bit.band(taps, bit.bnot(KF)) end
    end
    keys, taps = GA.keys(state, info, h, keys, taps)
  end
  return keys, taps
end

-- The getaway overlay, for init.lua (think() is at its upvalue cap, so it
-- is reached through ORD).
M.draw_getaway = GA.draw
-- The getaway's diagonal step, for steering.lua (cpf_path_to).
M.getaway_diagonal = GA.diagonal_next

-- ONE SLOT, ONE BOT (ORDER_CLAIM_TIEBREAK).  A repeat ping adds one slot to a
-- ping order (anchor want + 1), and when the auction for it times out two
-- bots can both see themselves as the cheapest and both take it: a 2-bot
-- order with 3 holders.  The claims (o.gclaims, the same numbers on every
-- bot: each holder's wire cost) are ranked the way the solo tiebreak ranks
-- two claims -- lowest cost first, a tie to the lower player number -- and
-- the first `want` keep the order.  Returns the kept holders in that order
-- and the number of holders, or nil when there is nothing to rank: the knob
-- is off, the order is not a ping order, or it went to a selection (every
-- selected bot holds it with no auction, so `want` does not count them).
function M.slot_keepers(o, oid)
  if not C.ORDER_CLAIM_TIEBREAK then return nil end
  local anc = o.anchors[oid]
  local gc  = o.gclaims[oid]
  if not (anc and gc) or anc.names then return nil end
  local rank = {}
  for pn, c in pairs(gc) do rank[#rank + 1] = { pn = pn, c = c } end
  table.sort(rank, function(x, y)
    if x.c ~= y.c then return x.c < y.c end
    return x.pn < y.pn
  end)
  local keep = {}
  for i = 1, math.min(anc.want or 1, #rank) do keep[i] = rank[i].pn end
  return keep, #rank
end

-- A BOT KEEPS THE JOB A PERSON GAVE IT (ORDER_HOLDER_KEEPS_JOB, Andrew,
-- PR #393).  Ping pill 1, then pill 2 -- half a second later or a minute
-- later, there is no time window -- and the bot on pill 1 stays on pill 1:
-- a person told it to go there.  A FREE bot takes pill 2.  So a bot that
-- holds an order from a person (a ping or a chat line) is out of every
-- auction: it bids "no", as a busy bot does (start_order, the re-bids in
-- M.update), and an auction it won with a bid from before it held anything
-- is offered back to the team (obo, the settle in M.update).  Its own
-- order included: the slots a repeat ping adds are filled by the bots that
-- do NOT hold it yet, and the settle leaves the holders out anyway.
-- A decoy hold is a person's order like any other, so a ping elsewhere no
-- longer ends it (see M.busy).  A scenario hint is not from a person: a
-- bot on a hint still takes a new order, as before.  An order that NAMES
-- bots (names, a selection, `last`, all, nearby) runs no auction and
-- still switches the bots it names.
-- NO FREE BOT (ORDER_NO_FREE_TAKES_LOWEST): the holder's "no" carries its
-- cost (M.switch_bid), and when no free bot can take the order the holder
-- with the lowest cost drops its job for it (the settle in M.update).  A
-- decoy never does.
function M.holds_job(state)
  if not C.ORDER_HOLDER_KEEPS_JOB then return false end
  local h = state and state.orders and state.orders.held
  return h ~= nil and h.sender ~= (M.HINT_SENDER or 255)
end

-- group = true when several bots take the SAME order (all / nearby / a
-- selection).  Then nobody acks straight away: each taker broadcasts its obc
-- claim, and ORDER_AUCTION_TICKS later the lowest player number among the
-- claimants says ONE line with the count ("3 on pill 5").  A solo order acks
-- at once.
-- switched = true when this bot won the order only because no free bot could
-- take it (ORDER_NO_FREE_TAKES_LOWEST).  Its old order is DROPPED, not handed
-- back: a cancel (obx) when this bot is its only holder, else a quiet release
-- of this bot's share (the other holders keep it).
local function take_order(state, world, info, spec, cost, now, group, stolen, switched)
  local o = S(state)
  if o.held and o.held.oid ~= spec.oid then
    -- Latest order wins.  Say what we are leaving so the human can follow it.
    local old = o.held
    sayg(state, string.format("%sLeaving %s for %s",
        switched and "No free bot. " or "",
        goal_label(old.kind, old.tid), goal_label(spec.kind, spec.tid)))
    local drop = nil
    if switched then
      drop = true
      for pn in pairs(o.gclaims[old.oid] or {}) do
        if pn ~= state.player_number then drop = nil end
      end
    end
    release_held(state, info, nil, true, drop)
  end
  o.held = {
    oid = spec.oid, kind = spec.kind, tkind = spec.tkind, tid = spec.tid,
    -- tkind "here" ("go there and hold") carries its own tile: there is no
    -- pill or base to look the position up from.
    mx = spec.mx, my = spec.my,
    sender = spec.sender, sender_name = spec.sender_name,
    needs_shells = spec.needs_shells, verb = spec.verb,
    -- ping = it came from a bot-command ping.  Only a ping order on open
    -- ground can become a decoy hold (see GO-THERE DECOY HARD HOLD).
    ping = spec.ping,
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
  -- THE PING REPAIR BONUS (Andrew, 2026-09-24).  A bot that takes a human's
  -- bot-command ping on one of our LIVE pills (a defend order) weights that
  -- pill's builder-pool repair row x BUILDER_POOL_PING_PILL_BONUS
  -- (builder_pool.goal_weight).  Only the bots that take the order get it.
  -- M.update ends it on the clock, a dead / full / lost pill; a ping on
  -- another of our pills ends it in ping_bot_command.
  if spec.kind == "defend_pill" and spec.ping and spec.tid then
    local p = world and world.pills and world.pills[spec.tid]
    if p and p.owner == "friendly" and (p.health or 0) > 0 then
      state._repair_ping = { tid = spec.tid, sender = spec.sender,
                             until_tick = now + (C.BUILDER_POOL_PING_PILL_TICKS or 500) }
      print2(string.format("REPAIR_PING t=%d pill=%s from p%s until=%d",
             now, tostring(spec.tid), tostring(spec.sender),
             state._repair_ping.until_tick))
    end
  end
  local ic = math.floor(math.min(cost or 0, 999999))
  -- The cost as the wire carries it: a rival claim is compared against this
  -- number, and the rival compares against the same one.
  o.held.claim_cost = ic
  tx(state, string.format("/info obc %d %d", spec.oid, ic))
  o.gclaims[spec.oid] = o.gclaims[spec.oid] or {}
  o.gclaims[spec.oid][state.player_number] = ic
  -- A slot a repeat ping added (a ping order, not a selection) can be taken
  -- twice, and the dearer taker lets it go again (M.slot_keepers).  Its "on
  -- my way" marker waits out the claim window the way a solo ack does, so a
  -- bot that loses the slot never shows the human a marker for it.
  local anc = o.anchors[spec.oid]
  local slot_wait = group and C.ORDER_CLAIM_TIEBREAK and anc and not anc.names
  if group then
    o.announce[spec.oid] = { due = now + (C.ORDER_AUCTION_TICKS or 10), spec = spec }
    if slot_wait then
      local pmx, pmy = M.target_tile(world, state, spec)
      o.ack_due = { oid = spec.oid, due = now + (C.ORDER_AUCTION_TICKS or 10),
                    line = nil, mx = pmx, my = pmy }
    end
  elseif C.ORDER_CLAIM_TIEBREAK then
    -- ONE PING, ONE BOT: the line and the marker wait out the claim window,
    -- and go only if this bot still holds the order then (M.update).
    local pmx, pmy = M.target_tile(world, state, spec)
    o.ack_due = { oid = spec.oid, due = now + (C.ORDER_AUCTION_TICKS or 10),
                  line = string.format("%s %s", M.ack_for(my_name(state, info)),
                                       goal_label(spec.kind, spec.tid)),
                  mx = pmx, my = pmy }
  else
    sayg(state, string.format("%s %s", M.ack_for(my_name(state, info)),
        goal_label(spec.kind, spec.tid)))
  end
  -- "ON MY WAY" — one marker on the place the order names, at the moment the
  -- bot takes it.  This is NOT the "bot pings" setting: it answers a person
  -- who just gave an order, so it is always sent, and on a GROUP order every
  -- taker sends its own, which is what shows the human how many are coming.
  -- The chat ack is unchanged and still carries the words.
  if (group and not slot_wait) or not C.ORDER_CLAIM_TIEBREAK then
    local pmx, pmy = M.target_tile(world, state, spec)
    if pmx and pmy then
      M.ping(state, _G.PING_KIND_ON_MY_WAY or 4, pmx, pmy)
    end
  end
  print2(string.format("ORDER_TAKE t=%d oid=%d kind=%s tid=%s cost=%.0f group=%s stolen=%s",
         now, spec.oid, spec.kind, tostring(spec.tid), cost or 0,
         tostring(group or false), tostring(stolen or false)))
  -- A bot-command order the attack ping already turned into a suicide run
  -- (the auction was still open, or this is a bot a repeat ping added).
  local ks = o.known[spec.oid] and o.known[spec.oid].spec
  if spec.kind == "attack_pill" and (spec.suicide or (ks and ks.suicide)) then
    M.suicide_start(state, world, spec.tid, spec.sender, now, "took a suicide order")
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
  print2(string.format("ORDER_REPEAT t=%d oid=%d held=%d", now, oid, n))
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
  "Ping a tile: nearest free bot goes, else nearest busy one switches (decoys stay). Again: pill/base/tank +1, ground renews.",
  "Ping bots to select them, then order. Caution ping cancels; caution on a bot retreats it. 3 shots on a tile: come here.",
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

-- Are the bots SPEAKING their goal confirmations?  Off until somebody says
-- "bot chat on": the knob's default is false (2026-09-29), and "bot pings"
-- shows the goals on the map instead.  Read through sayg, never directly.
function M.bot_chat_on(state)
  local o = state and state.orders
  if o and o.bot_chat ~= nil then return o.bot_chat end
  if C.BOT_CHAT_DEFAULT == nil then return false end
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
  print2(string.format("ORDER_PING_ATTACK t=%d %s at (%d,%d)", now, key, mx, my))
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
-- A NEW ORDER FROM A PERSON RETIRES EVERY OLDER ONE (Andrew, 2026-09-24).
-- "I'm seeing bots go back to where I said 'go here' a while ago": the bot
-- that took a newer order RELEASED the old one (obr), and another bot picked
-- it up and drove to a square nobody wanted any more.  Now every bot, on
-- hearing a new order (chat line or bot-command ping), forgets every older
-- order it knows about, whoever gave it.  Nothing is cancelled on the wire:
-- a bot the new order is NOT for keeps the order it holds (Andrew: addressed
-- bots only), and so do its group partners.  But once the order is out of
-- everyone else's o.known, an obr for it re-opens no auction anywhere (the
-- "release" rx needs o.known), so a replaced, dead or stuck holder's job is
-- simply over.  Kept in o.known: the new order itself (keep_oid, so a repeat
-- line or a repeat ping is still a repeat), the order this bot holds, and
-- scenario hints (HINT_SENDER), which a script re-sends on its own clock.
-- Also kept: an order whose auction is still open.  Nobody has taken it
-- yet, so a second ping inside ORDER_AUCTION_TICKS would otherwise delete
-- the first order before any bot could take it.
-- ORDER_NEW_CLEARS_ALL = false (keel) keeps the old hand-back behaviour.
-- Two pings close together are still two bots under ORDER_HOLDER_KEEPS_JOB:
-- the bot that took the first order bids "no" on the second, or offers it
-- back if it won both (see M.holds_job).
local function clear_older_orders(state, info, keep_oid, now)
  if not C.ORDER_NEW_CLEARS_ALL then return end
  local o = S(state)
  local hint = M.HINT_SENDER or 255
  local held = o.held and o.held.oid
  local drop = {}
  for oid, k in pairs(o.known) do
    if oid ~= keep_oid and oid ~= held and not o.auctions[oid]
       and not (k.spec and k.spec.sender == hint) then
      drop[#drop + 1] = oid
    end
  end
  for _, oid in ipairs(drop) do forget_order(o, oid) end
  if #drop > 0 then
    print2(string.format("ORDER_CLEAR t=%d keep=%s held=%s forgot=%d",
           now or -1, tostring(keep_oid), tostring(held), #drop))
  end
end
M.clear_older_orders = clear_older_orders

-- A BID THAT CAME EARLY (ORDER_CLAIM_TIEBREAK).  Bots hear a ping on their
-- own thinks, so an ally can bid on an order before this bot has opened its
-- auction on it.  That bid used to be thrown away, the auction then waited
-- out its window without it, and two bots could each see themselves as the
-- cheapest.  M.update keeps such bids in o.early; a new auction takes the
-- ones younger than two auction windows.
-- Keep one bid in an auction.  A negative cost is "no"; BID_BUSY (-2) is a
-- "no" because the bot is busy or holds a person's job, not because it cannot
-- reach the target.  a.busy lists those bots: "All bots busy" is only said
-- when one of them answered (ORDER_HOLDER_KEEPS_JOB).
-- BID_HOLD (-3) and lower is a "no" from a bot on a person's job that WOULD
-- switch if no free bot can go (ORDER_NO_FREE_TAKES_LOWEST): its cost to the
-- new target is BID_HOLD - wire.  It is kept apart in a.hold, so the free
-- bids in a.bids always win first.  One number on the old wire shape: the
-- receiver's `%-?%d+` still matches it, and any negative cost is a "no".
M.BID_BUSY = -2
M.BID_HOLD = -3
function M.note_bid(a, pn, cost)
  a.bids[pn] = (cost >= 0) and cost or nil
  a.answered[pn] = true
  if cost <= M.BID_BUSY then
    a.busy = a.busy or {}
    a.busy[pn] = true
  end
  if cost <= M.BID_HOLD then
    a.hold = a.hold or {}
    a.hold[pn] = M.BID_HOLD - cost
  end
end

-- The wire bid of a bot on a person's job (M.holds_job) that is not busy for
-- any other reason: BID_HOLD - cost when it may be switched to `spec`, else
-- nil (the caller bids BID_BUSY).  Never switched: a bot already on this
-- order, a bot on a decoy hold (Andrew: "decoy stays"), and never for a
-- scenario hint or a three-shot order (those still go to free bots only).
function M.switch_bid(state, world, info, spec)
  if not (C.ORDER_NO_FREE_TAKES_LOWEST and M.holds_job(state)) then return nil end
  local h = state.orders.held
  if h.oid == spec.oid or decoy_held(state) then return nil end
  if spec.sender == (M.HINT_SENDER or 255) then return nil end
  if spec.who and spec.who.near then return nil end
  local c = M.travel_cost(state, world, info, spec)
  if not c or c >= 1e29 then return nil end
  return M.BID_HOLD - math.floor(math.min(c, 999999))
end

function M.merge_early_bids(o, oid, now)
  local e = o.early and o.early[oid]
  local a = o.auctions[oid]
  if not (e and a) then return end
  local win = 2 * (C.ORDER_AUCTION_TICKS or 10)
  for pn, b in pairs(e) do
    if (now - b.tick) <= win and not a.answered[pn] then
      M.note_bid(a, pn, b.cost)
    end
  end
  o.early[oid] = nil
end

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
  -- A bot on a person's order bids "no" (ORDER_HOLDER_KEEPS_JOB).
  if not busy and M.holds_job(state) then busy, reason = true, "holding" end
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
  M.merge_early_bids(o, spec.oid, now)
  local bid = cost and math.floor(math.min(cost, 999999))
              or (busy and M.BID_BUSY or -1)
  -- Holding a person's job and busy for no other reason: bid the cost with
  -- the switch marker (ORDER_NO_FREE_TAKES_LOWEST, M.switch_bid).
  if reason == "holding" then
    bid = M.switch_bid(state, world, info, spec) or bid
  end
  M.note_bid(o.auctions[spec.oid], me, bid)
  o.auctions[spec.oid].bids[me] = cost
  tx(state, string.format("/info obd %d %d", spec.oid, bid))
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
  if cmd.setting == "bot_pings" then
    set_bot_pings(state, cmd.value)
    if M.speaker(state, info) == me then
      say(state, cmd.value and "Bot pings on." or "Bot pings off.")
      tx(state, string.format("/info obg %d", cmd.value and 1 or 0))
    end
    print2(string.format("ORDER_BPINGS t=%d -> %s (from p%s)", now,
           tostring(cmd.value), tostring(sender)))
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
    print2(string.format("ORDER_BCHAT t=%d -> %s (from p%s)", now,
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
      print2(string.format("ORDER_LAST t=%d none (from p%s)", now, tostring(sender)))
      return true
    end
    print2(string.format("ORDER_LAST t=%d n=%d (from p%s)", now, #last_pns,
           tostring(sender)))
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
    -- ── the SUICIDE RUN's escape hatch ─────────────────────────────────
    -- Every cancel that reaches this bot ends its run, by the same rules
    -- the order cancels below use: `cancel all` -- everyone; `cancel <bot>`
    -- -- that bot; `last cancel` -- the bots of the last order; a bare
    -- `cancel` -- the runs that sender started.  A bot that holds an order
    -- says "released" below; one without an order says it here.
    if state._suicide then
      local r, hit = state._suicide, false
      if t and t.kind == "all" then hit = true
      elseif t and t.kind == "tank" then hit = (t.pn == me)
      elseif last_pns then
        for _, pn in ipairs(last_pns) do if pn == me then hit = true end end
      else hit = (r.sender == sender) end
      if hit then
        local line = nil
        if not o.held then line = "released" end
        M.suicide_end(state, "cancel", line)
      end
    end
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
      print2(string.format("ORDER_CLOSEST t=%d none (%s, from p%s)", now,
             tostring(why), tostring(sender)))
      return true
    end
    cmd.target = { kind = "pill", id = id }
    print2(string.format("ORDER_CLOSEST t=%d -> pill %d (from p%s)", now, id,
           tostring(sender)))
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
  clear_older_orders(state, info, spec.oid, now)
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
    needs_shells = needs_shells, who = cmd.who, ping = cmd.ping,
  }
  o.known[spec.oid] = { spec = spec, tick = now }
  note_last(o, HINT_SENDER, spec.oid)

  -- The same hint again, while it is still held, refreshes the focus rather
  -- than restarting the job -- what a repeated chat line does.
  if o.held and o.held.oid == spec.oid then
    o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)
    return true
  end
  print2(string.format("HINT_START t=%d verb=%s kind=%s tid=%s", now,
         tostring(cmd.verb), tostring(kind), tostring(tid)))
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
    local cmd = hint_goto(state, mx, my)
    -- ping = "1": the order is filed as if a bot-command ping had given it,
    -- so arriving on a square a pill can shoot starts the decoy hold (see
    -- GO-THERE DECOY HARD HOLD).  A scenario has no call that places a ping;
    -- this key is how a ROOST round reaches the decoy hold.
    if t.ping == "1" or t.ping == 1 or t.ping == true then cmd.ping = true end
    return cmd
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
      print2(string.format("HINT_STAND_DOWN t=%d oid=%d", now, h.oid or 0))
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
  -- A SUICIDE RUN TAKES NO OTHER ORDER.  It ends on death, on the pill's
  -- death, or on a cancel -- a new order is not one of those, so the bot
  -- answers "Busy (suicide run)" and keeps going.
  if state._suicide then return true, "suicide run" end
  -- THE MAN BEING OUT DOES NOT MAKE A BOT BUSY (Andrew, 2026-09-24:
  -- ORDER_MAN_OUT_TAKES).  The tank drives off on the order and the builder
  -- walks back to it as he always does -- a man sent to pick a pill up
  -- finishes the pickup first.  false = "Busy (man_out)" / "Busy (capturing)".
  if not C.ORDER_MAN_OUT_TAKES then
    if (info.man_status or 0) ~= C.LGM_INTANK then return true, "man_out" end
    if g.kind == "capture_pill" and state._lgm_dispatch then return true, "capturing" end
  end
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
  -- A DECOY HOLD IS NOT BUSY here.  An order that NAMES the bot is one of
  -- its endings (take_order, "latest order wins").  A new PING or chat
  -- auction is not, under ORDER_HOLDER_KEEPS_JOB: the decoy holds a person's
  -- order, so it bids "no" (M.holds_job) and runs to its own end -- pills
  -- down, the clock, a caution, a cancel or its death.  With the knob off
  -- (keel) the auction winner leaves its decoy for the new order, as before.
  -- Nothing above can fire for it either -- the decoy lock keeps
  -- escape_water out, and the stuck detector reads the park as deliberate
  -- so stuck_for stays at 0.
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

  -- ── a ping on ANOTHER of our pills ends the repair bonus ──────────────
  -- The bonus itself belongs only to the bot(s) that TAKE a defend ping
  -- order (take_order).  Every bot hears this ping, so every bot drops a
  -- bonus it holds for a different pill here; a repeat ping on the same pill
  -- leaves it alone (it only adds a bot).
  if kind == "defend_pill" and state._repair_ping
     and state._repair_ping.tid ~= tid then
    print2(string.format("REPAIR_PING_END t=%d pill=%s why=new ping on pill %s",
           now, tostring(state._repair_ping.tid), tostring(tid)))
    state._repair_ping = nil
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
    -- The suicide-run window opens on EVERY bot-command ping on a live
    -- enemy pill, the repeat one included (see PING SUICIDE RUN).
    if kind == "attack_pill" then
      o.botcmd[sender] = { tid = tid, tick = now, oid = prev }
    end
    -- A REPEAT ON PLAIN GROUND ADDS NO BOT (ORDER_LAND_REPEAT_ADDS false).
    -- Only a pill, a base or a tank takes one more bot per ping.  "Go there"
    -- again refreshes the first bot's order: the travel focus, or the hold
    -- clock once it has arrived.  The holder answers "Still on it".  No
    -- auction opens and `want` stays where it was.
    --
    -- That is only while a bot still holds the order.  o.gclaims[prev] is the
    -- holder set every bot shares (obc adds to it, obr and a death take the
    -- bot out), and it includes this bot's own claim.  With nobody on it (the
    -- holder died or let it go, or no bot was free the first time) the
    -- repeat re-runs the auction for the same one slot below, so a bot goes
    -- again.
    local ground = not C.ORDER_LAND_REPEAT_ADDS and k.spec.tkind == "here"
    local manned = next(o.gclaims[prev] or {}) ~= nil
    if ground and manned then
      local h = o.held
      if h and h.oid == prev then
        if h.hold then
          h.expiry = now + (C.ORDER_GOTO_HOLD_TICKS or 500)
        else
          h.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)
        end
        repeat_ack(state, info, prev, now)
      end
      print2(string.format("PING_REFRESH t=%d oid=%d want=%d (ground)", now, prev,
             a.want or 1))
      return
    end
    -- A ground repeat with nobody on it keeps its one slot; any other repeat
    -- adds one.
    a.want = (a.want or 1) + (ground and 0 or 1)
    if o.held and o.held.oid == prev then
      o.held.expiry = now + (C.ORDER_FOCUS_TICKS or 3000)   -- focus reset
    end
    print2(string.format("PING_%s t=%d oid=%d want=%d", ground and "RESEND" or "ADD",
           now, prev, a.want))
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
  clear_older_orders(state, info, spec.oid, now)
  o.known[spec.oid]   = { spec = spec, tick = now }
  note_last(o, sender, spec.oid)
  -- names = the order went to a SELECTION, which puts every selected bot on
  -- it with no auction, so its holders are not counted against `want` (see
  -- M.slot_keepers).
  o.anchors[spec.oid] = { sender = sender, mx = axm, my = aym, tick = now, want = 1,
                          names = (who.mode == "names") or nil }
  if kind == "attack_pill" then
    o.botcmd[sender] = { tid = tid, tick = now, oid = spec.oid }
  end
  print2(string.format("PING_ORDER t=%d oid=%d kind=%s tid=%s tile=(%d,%d) hit=%s",
         now, spec.oid, kind, tostring(tid), mx, my,
         hit and hit.class or "open"))
  start_order(state, world, info, spec, who, now, 1)
end

-- An ATTACK ping (PING_KIND_ATTACK, 3).  On its own it is a marker for the
-- team and orders nothing.  Right after a bot-command ping from the SAME
-- sender on the SAME live enemy pill it is the suicide-run trigger (see PING
-- SUICIDE RUN).  The pill is resolved exactly the way a bot-command ping
-- resolves it (M.resolve_ping: the exact tile, no ring for pills).
local function ping_attack(state, world, info, sender, mx, my, now)
  if not C.PING_SUICIDE_ENABLED then return end
  local o   = S(state)
  local rec = o.botcmd[sender]
  if not rec then return end
  local hit = M.resolve_ping(state, world, info, mx, my)
  if not (hit and hit.class == "pill" and hit.id == rec.tid) then return end
  local gap = now - (rec.tick or -1e9)
  local win = C.PING_SUICIDE_WINDOW_TICKS or 50
  if gap < 0 or gap > win then
    print2(string.format("PING_SUICIDE_MISS t=%d pill=%s gap=%d > window=%d",
           now, tostring(rec.tid), gap, win))
    return
  end
  -- One bot command, one trigger: a second attack ping inside the window is
  -- not a second run.
  o.botcmd[sender] = nil
  suicide_trigger(state, world, info, sender, rec.tid, rec.oid, now)
end

-- A CAUTION ping (PING_KIND_CAUTION, 1).
local function ping_caution(state, world, info, sender, mx, my, now)
  local o   = S(state)
  local me  = state.player_number
  local hit = M.resolve_ping(state, world, info, mx, my)

  -- ── a caution is the SUICIDE RUN's escape hatch ───────────────────────
  -- On the running bot itself, or in the 3x3 of the pill it is running at:
  -- the run is called off.  The order code below then does what a caution
  -- always does (cancel the held order; a second caution on the bot
  -- retreats it).
  do
    local r = state._suicide
    local ring = C.ORDER_PING_RING or 1
    local tmx  = (hit and hit.mx) or mx
    local tmy  = (hit and hit.my) or my
    if r and ((hit and hit.class == "allybot" and hit.pn == me)
              or (math.abs(r.mx - tmx) <= ring and math.abs(r.my - tmy) <= ring)) then
      -- A bot that holds an order says "Released" when the code below lets
      -- it go; one running on its own choice has no order, so it says it here.
      -- With no order the caution was the cancel and nothing more: stop
      -- here, or the retreat branch below reads "no order" as a second
      -- caution and sends the tank off to take cover.
      if not o.held then
        M.suicide_end(state, "caution ping", "Released")
        return
      end
      M.suicide_end(state, "caution ping")
      -- A caution on the PILL cancels the run's order here.  The loop below
      -- finds it through o.known, but a run keeps its order past the 60 s
      -- focus and the prune drops it from o.known by then: the bot would
      -- go on attacking and say "order lapsed" a second later.  A caution
      -- on the bot itself is left to the retreat branch below.
      local h = o.held
      if not (hit and hit.class == "allybot" and hit.pn == me)
         and h.tkind == "pill" and h.tid == r.tid then
        release_held(state, info, "Released", false, true)
      end
    end
  end

  -- ── a caution is the DECOY HOLD's release too ─────────────────────────
  -- Within ORDER_PING_RING of the decoy's own TANK, from any human ally --
  -- not only the one who sent it (on_events has already dropped bot and
  -- enemy pings).  Measured on the raw ping square, so a caution beside the
  -- tank counts even when the ring resolves it to something else (a pill, an
  -- enemy tank).  "Released", and that is the whole event: the retreat
  -- branch below must not read the empty slot as a second caution and send
  -- the tank off to take cover on the same ping.
  do
    local dh = decoy_held(state)
    if dh and info.tankx and info.tanky then
      local ring = C.ORDER_PING_RING or 1
      if math.abs(tile_of(info.tankx) - mx) <= ring
         and math.abs(tile_of(info.tanky) - my) <= ring then
        decoy_end(state, info, "caution ping", "Released")
        print2(string.format("PING_CAUTION t=%d decoy released by p%s",
               now, tostring(sender)))
        return
      end
    end
  end

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
      print2(string.format("PING_CAUTION t=%d cancel (held)", now))
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
    print2(string.format("PING_CAUTION t=%d retreat busy=%s", now, tostring(reason)))
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
      -- A DECOY WITH A PARTNER stands on the target square, so this 3x3
      -- would call off every decoy on it at once.  Its caution is the one on
      -- its own tank (above): one caution, one decoy.  It keeps its hold and
      -- the order.
      if o.held and o.held.oid == oid and decoy_held(state)
         and M.decoy_partner(state) then
        print2(string.format("PING_CAUTION t=%d decoy keeps oid=%d (a partner's caution)",
               now, oid))
      else
        if o.held and o.held.oid == oid then
          release_held(state, info, "Released", false, true)
        end
        forget_order(o, oid)
        print2(string.format("PING_CAUTION t=%d cancel oid=%d", now, oid))
      end
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
  local K_ATK  = _G.PING_KIND_ATTACK
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
        elseif kind == K_ATK and K_ATK ~= nil then
          ping_attack(state, world, info, sender, mx, my, now)
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

  -- The ping repair bonus (see ping_bot_command) ends on the clock, or when
  -- its pill is gone, dead, fully repaired or no longer ours.
  local rp = state._repair_ping
  if rp then
    local p = world.pills and world.pills[rp.tid]
    local why = nil
    if now >= (rp.until_tick or 0) then why = "timer"
    elseif not p then why = "pill gone"
    elseif p.owner ~= "friendly" or p.in_tank then why = "not ours"
    elseif (p.health or 0) <= 0 then why = "pill dead"
    elseif (p.health or 0) >= (C.PILLS_MAX_HEALTH or 15) then why = "repaired" end
    if why then
      state._repair_ping = nil
      print2(string.format("REPAIR_PING_END t=%d pill=%s why=%s",
             now, tostring(rp.tid), why))
    end
  end

  -- 0. Remember where the allied tanks are, so "attack closest" has a tile
  --    to measure from even when the person who typed it has just driven out
  --    of sight.  One short pass; see M.note_seen.
  M.note_seen(state, info, now)

  -- 1. Drain the inbound verbs.
  for _, r in ipairs(o.rx) do
    if r.kind == "bid" then
      local a = o.auctions[r.oid]
      if a then
        M.note_bid(a, r.from, r.cost)
      elseif C.ORDER_CLAIM_TIEBREAK and r.from ~= me then
        -- This bot has not opened its auction on that order yet (it heard
        -- the ping a think later than the bidder).  Keep the bid for it.
        o.early = o.early or {}
        o.early[r.oid] = o.early[r.oid] or {}
        o.early[r.oid][r.from] = { cost = r.cost, tick = now }
      end
    elseif r.kind == "claim" then
      o.claims[r.oid] = { pn = r.from, cost = r.cost, tick = r.tick }
      o.gclaims[r.oid] = o.gclaims[r.oid] or {}
      o.gclaims[r.oid][r.from] = r.cost
      o.auctions[r.oid] = nil
      if o.known[r.oid] then o.known[r.oid].unfilled = nil end
      -- Someone else claimed what we hold.  With ORDER_CLAIM_TIEBREAK the
      -- two claims are ranked below and only the loser lets go; without it
      -- (keel) this bot goes quiet and lets them have it.  A GROUP order is
      -- shared, so a fellow taker's claim is not a rival claim.
      -- A REPEAT PING makes a group too (anchor want > 1): the bot it added
      -- claims the same order, and that is a fellow taker, not a rival.
      -- Without this the first holder dropped the order the moment a repeat
      -- ping put a second bot on it (ORDER_CLAIM_TIEBREAK; keel drops).
      local anc = o.anchors[r.oid]
      local grp = o.announce[r.oid]
                  or (C.ORDER_CLAIM_TIEBREAK and o.held and o.held.oid == r.oid
                      and (o.held.group or (anc and (anc.want or 1) > 1)))
      if o.held and o.held.oid == r.oid and r.from ~= me
         and not grp then
        -- ONE PING, ONE BOT (ORDER_CLAIM_TIEBREAK).  Two bots took the same
        -- solo order.  Both see both claims, and both rank them the same
        -- way -- cost, then player number -- so exactly one keeps it.
        local mine = o.held.claim_cost or math.floor(o.held.cost or 0)
        if C.ORDER_CLAIM_TIEBREAK
           and (mine < r.cost or (mine == r.cost and me < r.from)) then
          o.claims[r.oid] = { pn = me, cost = mine, tick = now }
          if o.gclaims[r.oid] then o.gclaims[r.oid][r.from] = nil end
          print2(string.format("ORDER_KEEP t=%d oid=%d mine=%d p%s=%d",
                 now, r.oid, mine, tostring(r.from), r.cost))
        elseif C.ORDER_CLAIM_TIEBREAK then
          -- The losing side hands it over with a quiet release, so every
          -- other bot stops counting this bot as a holder too.  The release
          -- re-opens nothing: the winner still holds it (see "release").
          print2(string.format("ORDER_LOST t=%d oid=%d to=p%s", now, r.oid, tostring(r.from)))
          local lost = o.held
          release_held(state, info, nil, true)
          if o.ack_due and o.ack_due.oid == r.oid then o.ack_due = nil end
          -- A suicide run on the pill of the order it just lost goes too:
          -- the winner holds the order and runs it.
          local run = state._suicide
          if run and lost.kind == "attack_pill" and run.tid == lost.tid then
            M.suicide_end(state, "order lost")
          end
        else
          decoy_drop_goal(state, o.held)
          o.held = nil
          state._order = nil
          print2(string.format("ORDER_LOST t=%d oid=%d to=p%s", now, r.oid, tostring(r.from)))
        end
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
        decoy_drop_goal(state, o.held)
        o.held = nil
        state._order = nil
        goto_lock(state)
        print2(string.format("ORDER_CANCELLED t=%d oid=%d by=p%s",
               now, r.oid, tostring(r.from)))
      end
      forget_order(o, r.oid)
    elseif r.kind == "release" then
      if o.claims[r.oid] and o.claims[r.oid].pn == r.from then o.claims[r.oid] = nil end
      -- The releaser is not a holder any more, so the settle below must stop
      -- counting it as one -- otherwise it can never win its own order back.
      if o.gclaims[r.oid] then o.gclaims[r.oid][r.from] = nil end
      -- o.claims keeps only the LAST claim heard.  After crossed claims
      -- (ORDER_CLAIM_TIEBREAK) that can be the loser's, and the loser's
      -- release just cleared it while the winner still holds the order.
      -- Name the cheapest remaining holder again, or the steal pass (it
      -- walks o.claims only) could never take this order over.  The steal
      -- hold time counts from now: the real claim tick is not kept.
      if C.ORDER_CLAIM_TIEBREAK and not o.claims[r.oid] and o.gclaims[r.oid] then
        local bpn, bc = nil, nil
        for pn, c in pairs(o.gclaims[r.oid]) do
          if not bpn or c < bc or (c == bc and pn < bpn) then bpn, bc = pn, c end
        end
        if bpn then o.claims[r.oid] = { pn = bpn, cost = bc, tick = now } end
      end
      -- Re-bid it: the holder dropped it and the job is still standing.
      local k = o.known[r.oid]
      -- Somebody else still holds it (the loser of a claim tiebreak is the
      -- releaser): the job is not standing open, so nothing is re-bid.
      local still = false
      if C.ORDER_CLAIM_TIEBREAK then
        for pn in pairs(o.gclaims[r.oid] or {}) do
          if pn ~= r.from then still = true end
        end
      end
      -- "Don't pass the order back" (ORDER_NO_HAND_BACK): nobody re-bids.
      -- An OFFER (obo) is the exception: nobody ever took that order (see
      -- the settle below), so it still needs a bot.  Every bot answers an
      -- offer, a bot holding a job with "no", so the auction settles at once
      -- and "All bots busy" can be said when nobody is free.  The slots it
      -- fills are the ping's (anchor want); the holders are left out as ever.
      if C.ORDER_NO_HAND_BACK and not r.offer then k = nil end
      if k and (r.offer or (not still and not o.held)) and not o.auctions[r.oid]
         and (now - k.tick) < (C.ORDER_FOCUS_TICKS or 3000) then
        local busy = M.busy(state, info) or M.holds_job(state)
                     or (r.offer and o.held ~= nil)
        local cost = (not busy) and M.travel_cost(state, world, info, k.spec) or nil
        if cost and cost >= 1e29 then cost = nil end
        local anc = o.anchors[r.oid]
        local bid = cost and math.floor(math.min(cost, 999999))
                    or (busy and M.BID_BUSY or -1)
        -- An offer is still a person's order nobody took, so a holder may be
        -- switched to it when no free bot can go (M.switch_bid).  A plain
        -- hand-back (obr) never switches a holder: its old order would be
        -- handed back in turn, and so on round the team.
        if r.offer and r.from ~= me and not M.busy(state, info) then
          bid = M.switch_bid(state, world, info, k.spec) or bid
        end
        o.auctions[r.oid] = { spec = k.spec, open = now, bids = {},
                              answered = {},
                              want = r.offer and anc and anc.want or nil }
        M.note_bid(o.auctions[r.oid], me, bid)
        o.auctions[r.oid].bids[me] = cost
        M.merge_early_bids(o, r.oid, now)
        -- This bot's OWN offer, heard back (a bot hears its own chat lines):
        -- its "no" already went out right after the obo, so no second obd.
        -- It still keeps the auction and its own "no" in it, so it settles
        -- the offer like the others do and can be the bot that says "All
        -- bots busy".
        if not (r.offer and r.from == me) then
          tx(state, string.format("/info obd %d %d", r.oid, bid))
        end
      end
    end
  end
  o.rx = {}

  -- 1b. ONE SLOT, ONE BOT (ORDER_CLAIM_TIEBREAK; see M.slot_keepers).  More
  --     holders than the order has slots: the dearest let go, quietly (obr,
  --     no line, no ack, no marker), the same way the solo tiebreak's loser
  --     does.  Every holder ranks the same claims, so they agree on who goes.
  if o.held then
    local oid = o.held.oid
    local keep, n = M.slot_keepers(o, oid)
    if keep and n > #keep and o.gclaims[oid][me] ~= nil then
      local kept = false
      for _, pn in ipairs(keep) do if pn == me then kept = true end end
      if not kept then
        print2(string.format("ORDER_SLOT_LOST t=%d oid=%d want=%d holders=%d",
               now, oid, #keep, n))
        local lost = o.held
        release_held(state, info, nil, true)
        if o.ack_due and o.ack_due.oid == oid then o.ack_due = nil end
        -- As with the solo tiebreak's loser: a suicide run on this order's
        -- pill goes too, since the bots that kept the order run it.
        local run = state._suicide
        if run and lost.kind == "attack_pill" and run.tid == lost.tid then
          M.suicide_end(state, "order lost")
        end
      end
    end
  end

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
    local timed_out = (now - a.open) >= (C.ORDER_AUCTION_TICKS or 10)
    if n_ans >= n_allies or timed_out then
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
      local offer = false
      for i = 1, math.min(need, #rank) do
        won[#won + 1] = rank[i].pn
        if rank[i].pn == me then
          -- WON, BUT ALREADY ON A PERSON'S JOB (ORDER_HOLDER_KEEPS_JOB).  The
          -- bid went out before this bot held anything: two pings inside one
          -- auction window, and it won the other one first.  It keeps that
          -- job and offers this one (obo); the other bots re-open the auction
          -- and this bot answers it "no" (see the obo verb in M.rx).
          if M.holds_job(state) and o.held.oid ~= oid then
            offer = true
          else
            take_order(state, world, info, a.spec, rank[i].c, now, want > 1)
          end
        else
          o.claims[oid] = { pn = rank[i].pn, cost = rank[i].c, tick = now }
        end
      end
      -- NO FREE BOT, THE CHEAPEST BUSY ONE SWITCHES (ORDER_NO_FREE_TAKES_LOWEST,
      -- Andrew, PR #393).  Slots the free bids could not fill go to the bots
      -- on a person's job that bid a switch cost (a.hold, BID_HOLD), lowest
      -- cost first, a tie to the lower player number -- the same sort as the
      -- free bids, over the same table on every bot, so every bot names the
      -- same bot.  Never a bot that already holds this order (heldby), so a
      -- repeat ping adds a DIFFERENT bot.  A decoy never bid a switch cost
      -- (M.switch_bid).  The winner drops its old order (take_order,
      -- switched).  If it is on a decoy or busy by the time the auction
      -- settles, it offers the order on (obo) the way a free winner on a job
      -- does.
      local short = need - #won
      if short > 0 and a.hold and C.ORDER_HOLDER_KEEPS_JOB
         and C.ORDER_NO_FREE_TAKES_LOWEST then
        local taken = {}
        for _, pn in ipairs(won) do taken[pn] = true end
        local hr = {}
        for pn = 0, 15 do
          if a.hold[pn] and not heldby[pn] and not taken[pn] then
            hr[#hr + 1] = { pn = pn, c = a.hold[pn] }
          end
        end
        table.sort(hr, function(x, y)
          if x.c ~= y.c then return x.c < y.c end
          return x.pn < y.pn
        end)
        for i = 1, math.min(short, #hr) do
          won[#won + 1] = hr[i].pn
          if hr[i].pn == me then
            if decoy_held(state) or M.busy(state, info) then
              offer = true
            else
              print2(string.format("ORDER_SWITCH t=%d oid=%d cost=%d -- no free bot, leaving oid=%s",
                     now, oid, hr[i].c, tostring(o.held and o.held.oid)))
              take_order(state, world, info, a.spec, hr[i].c, now, want > 1, nil, true)
            end
          else
            o.claims[oid] = { pn = hr[i].pn, cost = hr[i].c, tick = now }
          end
        end
      end
      -- A REPEAT PING with no room left: the extra slot found nobody (every
      -- eligible bot already holds the order, or the rest cannot go), so the
      -- holders answer it the way a repeated chat line is answered.
      if a.repeated and #won == 0 then repeat_ack(state, info, oid, now) end
      -- NOBODY FREE (ORDER_HOLDER_KEEPS_JOB).  Every bot that answered said
      -- "no", and at least one of them because it is busy or holds a job
      -- (a.busy, from BID_BUSY).  Nobody goes, and the bots keep what they
      -- hold.  An order every bot said "no" to only because it cannot get
      -- there is not "busy", so nothing is said.  ONE bot says so -- the
      -- lowest player number that answered busy, so a dead speaker does not
      -- leave the line unsaid --
      -- because a person gave an order and is owed an answer (plain say, not
      -- the goal-chat latch).  A three-shot order with nobody in range stays
      -- silent, as it always was, and so does an order on a tank this bot
      -- cannot see (nobody could price it: that is not "busy").  The order
      -- stays known (k.unfilled): a bot that comes free inside
      -- ORDER_FOCUS_TICKS takes it (3b below).  With ORDER_NO_FREE_TAKES_LOWEST
      -- this is only reached when no holder could be switched either (all
      -- on decoys, busy, or unable to get there), or the knob is off.
      if C.ORDER_HOLDER_KEEPS_JOB and need > 0 and #won == 0 and not a.repeated then
        local k = o.known[oid]
        if k then k.unfilled = true end
        local low = nil
        for pn = 0, 15 do
          if a.busy and a.busy[pn] then low = pn break end
        end
        local sp = a.spec or {}
        if low == me and not (sp.who and sp.who.near)
           and sp.sender ~= (M.HINT_SENDER or 255)
           and M.target_tile(world, state, sp) then
          say(state, "All bots busy")
        end
      end
      o.auctions[oid] = nil
      if offer then
        -- The offer, then this bot's "no" for the auction it re-opens on the
        -- other bots (they read the two lines in this order), so they need
        -- not wait out the window for this bot's answer.  No auction is
        -- opened here.  This bot hears its own obo on its next think, and
        -- the release handler above opens the auction then with its own
        -- "no" in it, without sending that "no" a second time.
        tx(state, string.format("/info obo %d", oid))
        tx(state, string.format("/info obd %d %d", oid, M.BID_BUSY))
        print2(string.format("ORDER_OFFER t=%d oid=%d -- holding oid=%s",
               now, oid, tostring(o.held and o.held.oid)))
      end
      local bl = {}
      for pn = 0, 15 do
        if a.answered[pn] then
          bl[#bl + 1] = string.format("p%d=%s", pn,
            a.bids[pn] and string.format("%.0f", a.bids[pn])
            or (a.hold and a.hold[pn] and string.format("h%d", a.hold[pn]))
            or "no")
        end
      end
      print2(string.format("ORDER_SETTLE t=%d oid=%d want=%d held=%d winners=%s bids=%s allies=%d/%d%s",
             now, oid, want, n_held, table.concat(won, ","), table.concat(bl, ","),
             n_ans, n_allies, timed_out and " timeout" or ""))
    end
  end

  -- 2b. Group order: ONE line, from the lowest player number that actually
  --     took it, once the claims have had a window to arrive.
  for _, oid in ipairs(sorted_keys(o.announce)) do
    local an = o.announce[oid]
    if an and now >= an.due then
      local low, n = nil, 0
      -- A slot taken twice: only the holders that keep it are counted, so the
      -- line does not name a bot whose release has not arrived yet.
      local keep = M.slot_keepers(o, oid)
      local set = o.gclaims[oid] or {}
      if keep then
        set = {}
        for _, pn in ipairs(keep) do set[pn] = true end
      end
      for pn in pairs(set) do
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

  -- 2c. ONE PING, ONE BOT: the solo ack, once the claim window is over, and
  --     only from the bot that still holds the order (ORDER_CLAIM_TIEBREAK).
  local ad = o.ack_due
  if ad and now >= ad.due then
    o.ack_due = nil
    if o.held and o.held.oid == ad.oid then
      if ad.line then sayg(state, ad.line) end
      if ad.mx and ad.my then
        M.ping(state, _G.PING_KIND_ON_MY_WAY or 4, ad.mx, ad.my)
      end
    end
  end
  -- Early bids nobody opened an auction for.
  if o.early and next(o.early) then
    local win = 2 * (C.ORDER_AUCTION_TICKS or 10)
    for _, oid in ipairs(sorted_keys(o.early)) do
      local e = o.early[oid]
      for pn, b in pairs(e) do
        if (now - b.tick) > win then e[pn] = nil end
      end
      if next(e) == nil then o.early[oid] = nil end
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
        if cl and k and cl.pn ~= me and not k.decoy_left
           and (now - cl.tick) >= (C.ORDER_STEAL_HOLD_TICKS or 100)
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
      -- 3b. AN ORDER NOBODY WAS FREE FOR (ORDER_HOLDER_KEEPS_JOB).  Its
      --     auction found every bot on a job ("All bots busy") and it is
      --     still known.  The first bot to come free -- its job done, or back
      --     from the dead -- takes it, while it is inside ORDER_FOCUS_TICKS.
      --     Two bots freed on one think both take it; the claim tiebreak
      --     keeps one.  Oldest order id first, like every loop here.
      if not o.held and C.ORDER_HOLDER_KEEPS_JOB then
        for _, oid in ipairs(sorted_keys(o.known)) do
          local k  = o.known[oid]
          local sp = k and k.spec
          if k and k.unfilled and sp and not o.auctions[oid]
             and not next(o.gclaims[oid] or {})
             and (now - k.tick) < (C.ORDER_FOCUS_TICKS or 3000)
             and sp.sender ~= (M.HINT_SENDER or 255)
             and not (sp.who and sp.who.near) then
            local mine = M.travel_cost(state, world, info, sp)
            if mine and mine < 1e29 then
              k.unfilled = nil
              print2(string.format("ORDER_PICKUP t=%d oid=%d -- nobody was free for it",
                     now, oid))
              take_order(state, world, info, sp, mine, now)
              break
            end
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
  -- A SUICIDE RUN KEEPS ITS ORDER ALIVE.  The run sticks until the tank or
  -- the pill dies, so the 60 s focus must not lapse under it and say "order
  -- lapsed" while the bot is still driving at the pill.  The pill's death
  -- still ends the order through the tests below.
  local sr = state._suicide
  if h and sr and h.tkind == "pill" and h.tid == sr.tid
     and (h.expiry or 0) < now + 50 then
    h.expiry = now + 50
  end
  if h then
    -- ARRIVAL STARTS THE HOLD.  A go-there order used to run on the 60 s
    -- focus whether the bot got there in two seconds or fifty, which made
    -- "go there" mean "and stand there for the rest of the minute".  Andrew,
    -- Sep 15: the hold is about ten seconds AFTER ARRIVAL.  So the focus
    -- bounds the TRAVEL (an order that never arrives still lapses on it), and
    -- the first think the tank is within one square of the target swaps the
    -- clock for ORDER_GOTO_HOLD_TICKS.  One line is said, with the number in
    -- it, so the human knows how long the bot will be standing there.
    --
    -- A PING order with C.ORDER_GOTO_DECOY on decides here, once, between
    -- the two decoy outcomes (see GO-THERE DECOY HARD HOLD): no pill can
    -- shoot the square -> the order is over in silence (decoy_none); one or
    -- more can -> the hard hold, "decoying 10s".
    local decoy_none = false
    if h.kind == "goto_tile" and h.mx and h.my and not h.arrived
       and info.tankx and info.tanky then
      local dx = tile_of(info.tankx) - h.mx
      local dy = tile_of(info.tanky) - h.my
      if dx < 0 then dx = -dx end
      if dy < 0 then dy = -dy end
      if dx <= 1 and dy <= 1 then
        local hold = C.ORDER_GOTO_HOLD_TICKS or 500
        local npills = (C.ORDER_GOTO_DECOY and h.ping)
                       and decoy_pills(world, h.mx, h.my) or nil
        if npills == 0 then
          decoy_none = true
          h.arrived  = now
          print2(string.format("ORDER_DECOY_NONE t=%d oid=%d tile=(%d,%d) -- no pill in range, order over",
                 now, h.oid or 0, h.mx, h.my))
        else
          h.arrived = now
          h.hold    = true
          h.expiry  = now + hold
          if npills then
            h.decoy = true
            sayg(state, string.format("decoying %ds", math.floor(hold / 50)))
          else
            sayg(state, string.format("holding %ds", math.floor(hold / 50)))
          end
          -- The hard lock goes the moment the hold starts, so goal selection is
          -- free to answer an enemy tank from this same tick.
          goto_lock(state)
          if npills then
            print2(string.format("ORDER_DECOY_HOLD t=%d oid=%d tile=(%d,%d) pills=%d until=%d",
                   now, h.oid or 0, h.mx, h.my, npills, h.expiry))
          else
            print2(string.format("ORDER_GOTO_HOLD t=%d oid=%d tile=(%d,%d) until=%d",
                   now, h.oid or 0, h.mx, h.my, h.expiry))
          end
        end
      end
    end
    local done, why = false, nil
    if decoy_none then
      -- Nothing to be a decoy for.  No "holding", no "order lapsed": the
      -- order simply ends and goal selection takes over.  The travel goal
      -- goes with it (an urgent replan), so the tank does not roll the last
      -- square onto a spot that no longer means anything.
      done, why = true, nil
      local g = state.goal
      if g and g.kind == "goto_tile" and g.mx == h.mx and g.my == h.my then
        require("attack").clear_attack_goal(state, "decoy: no pill in range")
      end
    elseif now >= h.expiry then
      done, why = true, "order lapsed"
      -- A go-there order that ARRIVED ends in silence.  The bot already said
      -- "holding 10s" and that number was the whole promise; saying "order
      -- lapsed" ten seconds later is the same fact twice.
      if h.kind == "goto_tile" and h.arrived then why = nil end
      if h.decoy then
        print2(string.format("ORDER_DECOY_END t=%d oid=%d why=timer held=%d",
               now, h.oid or 0, now - (h.arrived or now)))
      end
    elseif h.decoy and h.hold and decoy_pills(world, h.mx, h.my) == 0 then
      -- END (a): every pill that could shoot the square is dead, carried,
      -- ours, or gone.  Re-read every think, so a pill that came into range
      -- after arrival had to go down too.  In silence, like the timer.
      done, why = true, nil
      print2(string.format("ORDER_DECOY_END t=%d oid=%d why=pills down held=%d",
             now, h.oid or 0, now - (h.arrived or now)))
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
          print2(string.format("ORDER_CONVERT t=%d oid=%d attack_pill->capture_pill pill=%d",
                 now, h.oid, h.tid))
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
      -- No record means the base is gone (a scenario removed it): the same
      -- "order lapsed" a pill order ends on.  Without it the order stood for
      -- good, since only a base turned friendly ended it.
      if not b then
        done, why = true, "order lapsed"
      elseif (h.kind == "capture_base" or h.kind == "attack_base")
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
      -- (Andrew's peer review, Sep 16).  A decoy's clock or its pills end
      -- the hold for this bot only (M.decoy_release).
      if h.decoy and h.hold then
        M.decoy_release(state, info, nil)
      else
        release_held(state, info, nil, true, true)
      end
      h = nil
    end
  end

  -- A suicide run never tops up, so it never says it is going to.
  if h and h.needs_shells and not state._suicide then
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
  local sr = state._suicide
  if sr then
    return string.format(" SUICIDE RUN pill#%s from p%s, %d s in",
      tostring(sr.tid), tostring(sr.sender),
      math.floor(((state.tick or 0) - (sr.since or 0)) / 50))
  end
  if not h then return "" end
  local left = math.max(0, (h.expiry or 0) - (state.tick or 0))
  -- A PLACE ORDER IS A HARD LOCK, and the panel has to say so.  It runs as a
  -- command goal, which pick_goal answers before goal selection, so the goal
  -- POOLS are never evaluated while it stands and the pool panel sits empty.
  -- Without this line that empty panel looks like a broken brain; with it the
  -- reason is on screen, next to the square and the time left.
  -- Once it has arrived the lock is off and the pools are live again (only
  -- the reactive rows can win), so the word changes with the phase.
  -- A DECOY hold (a ping order with pills on the square) is a hard hold with
  -- its own lock, so it gets its own word.
  if h.kind == "goto_tile" then
    return string.format(" ORDER goto (%d,%d) from %s, %d s left, %s",
      h.mx or -1, h.my or -1, h.sender_name or "?", math.floor(left / 50),
      h.decoy and "decoy" or (h.hold and "holding" or "hard"))
  end
  return string.format(" ORDER %s#%s from %s %ds left",
    h.kind, tostring(h.tid or "-"), h.sender_name or "?", math.floor(left / 50))
end

return M
