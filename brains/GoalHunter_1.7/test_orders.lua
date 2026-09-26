-- =========================================================================
-- test_orders.lua — standalone unit tests for the chat-order parser and the
-- bot-name matcher (orders.lua).  No engine, no world: pure string work.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build-own/_deps/luajit_src-src/src/luajit.exe test_orders.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local ORD = require("orders")
local C   = require("constants")

-- A roster shaped like a real lobby: one human, four bots.  Socrates and
-- Seneca share a prefix on purpose (the ambiguity case); Bruce Lee is the
-- multi-word name.
local ROSTER = {
  { pn = 1, name = "Socrates" },
  { pn = 2, name = "Seneca" },
  { pn = 3, name = "Bruce Lee" },
  { pn = 4, name = "Plato" },
}
-- `ally` is what M.all_roster puts on a player on OUR side (it reads
-- info.allies), and it is how the parser turns "attack <team mate>" down for
-- a HUMAN ally: ROSTER holds allied BOTS only.
local ALL = {
  { pn = 0, name = "Andrew", ally = true },   -- our own human
  { pn = 1, name = "Socrates", ally = true },
  { pn = 2, name = "Seneca", ally = true },
  { pn = 3, name = "Bruce Lee", ally = true },
  { pn = 4, name = "Plato", ally = true },
  { pn = 7, name = "Hannibal" },   -- an enemy tank, for `attack <tank name>`
}

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

local function P(text) return ORD.parse(text, ROSTER, ALL) end

local function shape(c)
  if not c then return "nil" end
  if c.reply then return "reply:" .. c.reply end
  if c.help then return "help" end
  if c.setting then return "set:" .. c.setting .. "=" .. tostring(c.value) end
  if c.select then return "select:" .. table.concat(c.select, ",") end
  if c.clear_select then return "clear_select" end
  local w = c.who.mode
  if c.who.n then w = w .. "[" .. c.who.n .. "]" end
  if c.who.pns then w = w .. "(" .. table.concat(c.who.pns, ",") .. ")" end
  local t = "-"
  if c.target then t = c.target.kind .. ":" .. tostring(c.target.id or c.target.pn) end
  return string.format("%s who=%s tgt=%s", c.verb, w, t)
end

print("orders.lua — parser")

check("attack 5",              shape(P("attack 5")) == "attack who=auto tgt=pill:5", shape(P("attack 5")))
check("sweep 7 (= capture)",   shape(P("sweep 7")) == "capture who=auto tgt=pill:7", shape(P("sweep 7")))
check("capture pill 7",        shape(P("capture pill 7")) == "capture who=auto tgt=pill:7", shape(P("capture pill 7")))
check("defend 5",              shape(P("defend 5")) == "defend who=auto tgt=pill:5", shape(P("defend 5")))
check("defend pill 5",         shape(P("defend pill 5")) == "defend who=auto tgt=pill:5", shape(P("defend pill 5")))
-- DEFEND TAKES A PILL: a base target is turned down, whoever it is aimed at.
check("defend base 3 rejected", shape(P("defend base 3")) == "reply:defend takes a pill", shape(P("defend base 3")))
check("all attack 7",          shape(P("all attack 7")) == "attack who=all tgt=pill:7", shape(P("all attack 7")))
-- `last` — the third who-word.  The parser only marks the line; the set of
-- bots is worked out in on_chat, which is what the runtime block below tests.
check("last attack 7",         shape(P("last attack 7")) == "attack who=last tgt=pill:7", shape(P("last attack 7")))
check("last cancel",           shape(P("last cancel")) == "cancel who=last tgt=-", shape(P("last cancel")))
check("last defend 9",         shape(P("last defend 9")) == "defend who=last tgt=pill:9", shape(P("last defend 9")))
check("LAST ATTACK 7 (case)",  shape(P("LAST ATTACK 7")) == "attack who=last tgt=pill:7", shape(P("LAST ATTACK 7")))
-- `last` names a set on its own, so it cannot share the who slot with a name.
check("socrates last attack 5 rejected",
                               shape(P("socrates last attack 5")) == "reply:last or a name, not both", shape(P("socrates last attack 5")))
check("last socrates attack 5 rejected",
                               shape(P("last socrates attack 5")) == "reply:last or a name, not both", shape(P("last socrates attack 5")))
check("last alone -> didn't understand",
                               shape(P("last")) == "reply:didn't understand", shape(P("last")))
check("nearby defend base 3 rejected", shape(P("nearby defend base 3")) == "reply:defend takes a pill", shape(P("nearby defend base 3")))
check("socrates attack 5",     shape(P("socrates attack 5")) == "attack who=names(1) tgt=pill:5", shape(P("socrates attack 5")))
check("soc attack 5 (prefix)", shape(P("soc attack 5")) == "attack who=names(1) tgt=pill:5", shape(P("soc attack 5")))
check("socrtes attack 5 (typo)", shape(P("socrtes attack 5")) == "attack who=names(1) tgt=pill:5", shape(P("socrtes attack 5")))
check("SOCRATES ATTACK 5 (case)", shape(P("SOCRATES ATTACK 5")) == "attack who=names(1) tgt=pill:5", shape(P("SOCRATES ATTACK 5")))
check("socrates plato attack 5", shape(P("socrates plato attack 5")) == "attack who=names(1,4) tgt=pill:5", shape(P("socrates plato attack 5")))
check("bruce (word of a two-word name, bare = select)",
                               shape(P("bruce")) == "select:3", shape(P("bruce")))
check("socrates plato (select set)",
                               shape(P("socrates plato")) == "select:1,4", shape(P("socrates plato")))
check("ambiguous who -> 'X or Y?'",
                               shape(P("s attack 5")) == "reply:Socrates or Seneca?", shape(P("s attack 5")))
check("ambiguous bare name -> 'X or Y?'",
                               shape(P("s")) == "reply:Socrates or Seneca?", shape(P("s")))
check("attack <enemy tank name>",
                               shape(P("attack hannibal")) == "attack who=auto tgt=tank:7", shape(P("attack hannibal")))
check("cancel",                shape(P("cancel")) == "cancel who=auto tgt=-", shape(P("cancel")))
check("cancel all",            shape(P("cancel all")) == "cancel who=auto tgt=all:nil", shape(P("cancel all")))
check("cancel socrates",       shape(P("cancel socrates")) == "cancel who=auto tgt=tank:1", shape(P("cancel socrates")))
check("retreat",               shape(P("retreat")) == "retreat who=auto tgt=-", shape(P("retreat")))
check("socrates retreat",      shape(P("socrates retreat")) == "retreat who=names(1) tgt=-", shape(P("socrates retreat")))
check("decoy -> not yet",      shape(P("decoy pill 5")) == "reply:decoy: not yet", shape(P("decoy pill 5")))
check("nevermind",             shape(P("nevermind")) == "clear_select", shape(P("nevermind")))
check("never mind",            shape(P("never mind")) == "clear_select", shape(P("never mind")))
check("!attack 5 (forced)",    shape(P("!attack 5")) == "attack who=auto tgt=pill:5", shape(P("!attack 5")))
check("!attack 5 sets forced", P("!attack 5").forced == true, tostring(P("!attack 5").forced))
check("plain chat ignored",    P("anyone got a spare pillbox over there") == nil, shape(P("anyone got a spare pillbox over there")))
check("plain chat ignored 2",  P("hello there") == nil, shape(P("hello there")))
check("an ack line is ignored", P("Got it! attack_pill #5") == nil, shape(P("Got it! attack_pill #5")))
check("an /info verb is ignored", P("/info state goal=explore") == nil, shape(P("/info state goal=explore")))
check("unknown verb, known shape -> didn't understand",
                               shape(P("all bombard 5")) == "reply:didn't understand", shape(P("all bombard 5")))
check("unknown verb after a name -> didn't understand",
                               shape(P("socrates bombard 5")) == "reply:didn't understand", shape(P("socrates bombard 5")))
check("forced nonsense -> didn't understand",
                               shape(P("!wibble")) == "reply:didn't understand", shape(P("!wibble")))
check("attack with no target -> didn't understand",
                               shape(P("attack")) == "reply:didn't understand", shape(P("attack")))

print("orders.lua — name matcher")
local function m(w)
  local pn, err, cands = ORD.match_name(w, ROSTER)
  if pn then return tostring(pn) end
  if err == "ambiguous" then return cands[1] .. "|" .. cands[2] end
  return "unknown"
end
check("exact",        m("socrates") == "1", m("socrates"))
check("case",         m("SoCrAtEs") == "1", m("SoCrAtEs"))
check("prefix",       m("plat") == "4", m("plat"))
check("word",         m("lee") == "3", m("lee"))
check("word prefix",  m("bru") == "3", m("bru"))
check("typo d=1",     m("socrtes") == "1", m("socrtes"))
check("typo d=2",     m("sokrats") == "1", m("sokrats"))
check("ambiguous",    m("s") == "Socrates|Seneca", m("s"))
check("unknown",      m("zaphod") == "unknown", m("zaphod"))
check("exact beats fuzzy", m("plato") == "4", m("plato"))

print("orders.lua — team check")
-- on_chat returns false without touching state when the sender is not an
-- ally: an enemy typing "attack 5" in all chat does nothing, no reply.
local st = { player_number = 1, tick = 0, goal = {} }
local took = ORD.on_chat(st, { pills = {}, bases = {} }, { allies = 0 },
                         9, "attack 5", 0, false, false)
check("enemy sender ignored", took == false and st.orders == nil, tostring(took))
-- A bot ally's own chatter is not read as an order unless it is forced with !
local took2 = ORD.on_chat(st, { pills = {}, bases = {} }, { allies = 0 },
                          2, "attack 5", 0, true, true)
check("bot ally chatter needs !", took2 == false, tostring(took2))

print("orders.lua — ack pools")
check("pool: philosophers", ORD.ack_pool("Socrates") == "philosophers", ORD.ack_pool("Socrates"))
check("pool: martial",      ORD.ack_pool("Bruce Lee") == "martial", ORD.ack_pool("Bruce Lee"))
check("pool: classic",      ORD.ack_pool("HAL-9000") == "classic", ORD.ack_pool("HAL-9000"))
check("pool: generals",     ORD.ack_pool("Sun Tzu") == "generals", ORD.ack_pool("Sun Tzu"))
check("pool: plain",        ORD.ack_pool("Bot-03") == "plain", ORD.ack_pool("Bot-03"))
check("pool: plain (nil)",  ORD.ack_pool(nil) == "plain", ORD.ack_pool(nil))
check("ack is from the pool", (function()
  math.randomseed(42)
  local a = ORD.ack_for("Socrates")
  for _, v in ipairs(ORD.ACKS.philosophers) do if v == a then return true end end
  return false
end)(), "?")


-- =========================================================================
-- STAGE 2 — team settings, help, and the ping rules
-- =========================================================================

print("orders.lua — focus / take")
check("focus bases",      shape(P("focus bases"))      == "set:focus=bases", shape(P("focus bases")))
check("take bases",       shape(P("take bases"))       == "set:focus=bases", shape(P("take bases")))
check("focus base",       shape(P("focus base"))       == "set:focus=bases", shape(P("focus base")))
check("focus pills",      shape(P("focus pills"))      == "set:focus=pills", shape(P("focus pills")))
check("take pills",       shape(P("take pills"))       == "set:focus=pills", shape(P("take pills")))
check("focus pill",       shape(P("focus pill"))       == "set:focus=pills", shape(P("focus pill")))
check("focus pillbox",    shape(P("focus pillbox"))    == "set:focus=pills", shape(P("focus pillbox")))
check("focus pillboxes",  shape(P("focus pillboxes"))  == "set:focus=pills", shape(P("focus pillboxes")))
check("FOCUS BASES (case)", shape(P("FOCUS BASES"))    == "set:focus=bases", shape(P("FOCUS BASES")))
check("focus off",        shape(P("focus off"))        == "set:focus=off",   shape(P("focus off")))
check("focus nonsense",   shape(P("focus wibble"))     == "reply:didn't understand", shape(P("focus wibble")))

print("orders.lua — reposition switch")
check("reposition on",    shape(P("reposition on"))    == "set:reposition=true",  shape(P("reposition on")))
check("reposition off",   shape(P("reposition off"))   == "set:reposition=false", shape(P("reposition off")))
check("repositioning on", shape(P("repositioning on")) == "set:reposition=true",  shape(P("repositioning on")))
check("reposition alone", shape(P("reposition"))       == "reply:didn't understand", shape(P("reposition")))
check("repositioning off",shape(P("repositioning off")) == "set:reposition=false", shape(P("repositioning off")))
check("repositioning alone", shape(P("repositioning"))  == "reply:didn't understand", shape(P("repositioning")))
-- A SUBJECT IN FRONT OF A SETTING is allowed and ignored: both settings are
-- team-wide, so the subject cannot mean anything and turning the line down
-- would read as a refusal.
check("socrates focus bases",  shape(P("socrates focus bases"))    == "set:focus=bases", shape(P("socrates focus bases")))
check("socrates plato focus pills",
                               shape(P("socrates plato focus pills")) == "set:focus=pills", shape(P("socrates plato focus pills")))
check("all focus off",         shape(P("all focus off"))           == "set:focus=off", shape(P("all focus off")))
check("nearby repositioning on",
                               shape(P("nearby repositioning on"))  == "set:reposition=true", shape(P("nearby repositioning on")))
check("last reposition off",   shape(P("last reposition off"))      == "set:reposition=false", shape(P("last reposition off")))
check("socrates repositioning on",
                               shape(P("socrates repositioning on")) == "set:reposition=true", shape(P("socrates repositioning on")))
check("a subject with junk after it is still nonsense",
                               shape(P("socrates focus wibble"))    == "reply:didn't understand", shape(P("socrates focus wibble")))

print("orders.lua — bot pings switch")
check("bot pings on",  shape(P("bot pings on"))  == "set:bot_pings=true",  shape(P("bot pings on")))
check("bot pings off", shape(P("bot pings off")) == "set:bot_pings=false", shape(P("bot pings off")))
check("botpings on",   shape(P("botpings on"))   == "set:bot_pings=true",  shape(P("botpings on")))
check("botpings off",  shape(P("botpings off"))  == "set:bot_pings=false", shape(P("botpings off")))
check("BOT PINGS ON (case)", shape(P("BOT PINGS ON")) == "set:bot_pings=true", shape(P("BOT PINGS ON")))
check("bot pings alone",  shape(P("bot pings")) == "reply:didn't understand", shape(P("bot pings")))
check("bot pings wibble", shape(P("bot pings wibble")) == "reply:didn't understand", shape(P("bot pings wibble")))
-- A subject in front is allowed and ignored, the same as focus: the setting
-- is team-wide, so the subject cannot mean anything.
check("socrates bot pings on",
      shape(P("socrates bot pings on")) == "set:bot_pings=true", shape(P("socrates bot pings on")))
check("all botpings off",
      shape(P("all botpings off")) == "set:bot_pings=false", shape(P("all botpings off")))

print("orders.lua — bot chat switch")
check("bot chat on",  shape(P("bot chat on"))  == "set:bot_chat=true",  shape(P("bot chat on")))
check("bot chat off", shape(P("bot chat off")) == "set:bot_chat=false", shape(P("bot chat off")))
check("botchat on",   shape(P("botchat on"))   == "set:bot_chat=true",  shape(P("botchat on")))
check("botchat off",  shape(P("botchat off"))  == "set:bot_chat=false", shape(P("botchat off")))
check("BOT CHAT OFF (case)", shape(P("BOT CHAT OFF")) == "set:bot_chat=false", shape(P("BOT CHAT OFF")))
check("bot chat alone",  shape(P("bot chat")) == "reply:didn't understand", shape(P("bot chat")))
check("bot chat wibble", shape(P("bot chat wibble")) == "reply:didn't understand", shape(P("bot chat wibble")))
check("socrates bot chat off",
      shape(P("socrates bot chat off")) == "set:bot_chat=false", shape(P("socrates bot chat off")))
check("all botchat on",
      shape(P("all botchat on")) == "set:bot_chat=true", shape(P("all botchat on")))
-- The two latches are separate words and must not be read as each other.
check("bot pings is not bot chat",
      shape(P("bot pings off")) == "set:bot_pings=false", shape(P("bot pings off")))

-- =========================================================================
-- A COUNT IS THE FOURTH WHO-WORD: "4 attack 5" = the four bots closest to
-- pill 5.  The parser only marks the number; the auction already ranks every
-- free bot by travel price to the target, so `want = N` picks the N nearest.
-- =========================================================================
print("orders.lua — a count who-word")
check("4 attack 5",     shape(P("4 attack 5")) == "attack who=count[4] tgt=pill:5", shape(P("4 attack 5")))
check("1 attack 5",     shape(P("1 attack 5")) == "attack who=count[1] tgt=pill:5", shape(P("1 attack 5")))
check("16 attack 5",    shape(P("16 attack 5")) == "attack who=count[16] tgt=pill:5", shape(P("16 attack 5")))
check("3 defend pill 9", shape(P("3 defend pill 9")) == "defend who=count[3] tgt=pill:9", shape(P("3 defend pill 9")))
check("2 capture base 3", shape(P("2 capture base 3")) == "capture who=count[2] tgt=base:3", shape(P("2 capture base 3")))
check("2 attack hannibal", shape(P("2 attack hannibal")) == "attack who=count[2] tgt=tank:7", shape(P("2 attack hannibal")))
-- Out of range is a mistake, not an order, and it is ANSWERED: the line has
-- a verb in it, so the sender is plainly talking to the bots.
check("0 attack 5 rejected",  shape(P("0 attack 5")) == "reply:didn't understand", shape(P("0 attack 5")))
check("17 attack 5 rejected", shape(P("17 attack 5")) == "reply:didn't understand", shape(P("17 attack 5")))
-- A NUMBER WITH NO VERB BEHIND IT IS ORDINARY CHAT.  Without this the gate
-- would read half the numbers people type at each other as orders.
check("'5 more shells please' is chat", P("5 more shells please") == nil, shape(P("5 more shells please")))
check("'3' alone is chat",              P("3") == nil, shape(P("3")))
check("'2 pills left' is chat",         P("2 pills left") == nil, shape(P("2 pills left")))
-- A count cannot be a name AND a count, so the count only reads when the
-- number is not a bot's name (tested against a digit-named roster below).
check("count does not shadow a real name", (function()
  local R2 = { { pn = 1, name = "4" }, { pn = 2, name = "Plato" } }
  return shape(ORD.parse("4 attack 5", R2, R2)) == "attack who=names(1) tgt=pill:5"
end)(), shape((function()
  local R2 = { { pn = 1, name = "4" }, { pn = 2, name = "Plato" } }
  return ORD.parse("4 attack 5", R2, R2)
end)()))
-- "4 attack 5" and "attack 5" are ONE job on pill 5, so they share an order
-- id: the count line grows the first order instead of opening a rival one.
check("a count shares the plain order's id",
      ORD.order_id(0, P("4 attack 5")) == ORD.order_id(0, P("attack 5")),
      tostring(ORD.order_id(0, P("4 attack 5"))))
check("a count is still a different order from `all`",
      ORD.order_id(0, P("4 attack 5")) ~= ORD.order_id(0, P("all attack 5")), "?")

-- =========================================================================
-- "closest" — THE PILL NEAREST THE SENDER.  The parser marks it; the pill
-- number is chosen in on_chat, where the sender's tile is known.
-- =========================================================================
print("orders.lua — the closest target word")
local function closest_shape(t)
  local c = P(t)
  if not c then return "nil" end
  if c.reply then return "reply:" .. c.reply end
  if not c.target then return "no target" end
  return string.format("%s %s closest=%s", c.verb, c.target.kind,
                       tostring(c.target.closest))
end
check("attack closest",       closest_shape("attack closest") == "attack pill closest=true", closest_shape("attack closest"))
check("attack closest pill",  closest_shape("attack closest pill") == "attack pill closest=true", closest_shape("attack closest pill"))
check("attack pill closest",  closest_shape("attack pill closest") == "attack pill closest=true", closest_shape("attack pill closest"))
check("capture closest",      closest_shape("capture closest") == "capture pill closest=true", closest_shape("capture closest"))
check("defend closest",       closest_shape("defend closest") == "defend pill closest=true", closest_shape("defend closest"))
check("nearest is the same word", closest_shape("attack nearest") == "attack pill closest=true", closest_shape("attack nearest"))
check("4 attack closest",     shape(P("4 attack closest")) == "attack who=count[4] tgt=pill:nil", shape(P("4 attack closest")))
check("all attack closest",   shape(P("all attack closest")) == "attack who=all tgt=pill:nil", shape(P("all attack closest")))
check("socrates attack closest",
      shape(P("socrates attack closest")) == "attack who=names(1) tgt=pill:nil", shape(P("socrates attack closest")))
-- CLOSEST IS A PILL WORD.  A base has a number on the map the same way a
-- pill does, so "closest base" is turned down in words rather than quietly
-- redirected onto a pill.
check("closest base rejected", closest_shape("attack closest base") == "reply:closest takes a pill", closest_shape("attack closest base"))
check("base closest rejected", closest_shape("attack base closest") == "reply:closest takes a pill", closest_shape("attack base closest"))
-- A bare "closest" is not an order at all: no verb, so it is ordinary chat.
check("'closest' alone is chat", P("closest") == nil, shape(P("closest")))

print("orders.lua — help")
check("help",             shape(P("help")) == "help", shape(P("help")))
check("help with junk",   shape(P("help me")) == "reply:didn't understand", shape(P("help me")))
check("help: 4 lines",    #ORD.HELP == 4, tostring(#ORD.HELP))
check("help line 1 lists the last and count who-words",
      ORD.HELP[1]:find("all|nearby|last|N|bot name", 1, true) ~= nil, ORD.HELP[1])
check("help line 1 lists the closest target",
      ORD.HELP[1]:find("closest", 1, true) ~= nil, ORD.HELP[1])
check("help line 2 lists the bot chat latch",
      ORD.HELP[2]:find("bot chat on|off", 1, true) ~= nil, ORD.HELP[2])
check("help lines fit the 128-byte chat max", (function()
  for _, l in ipairs(ORD.HELP) do if #l > 128 then return false end end
  return true
end)(), "?")
-- The game-start banner is gone: the lobby says it now, out of announce.txt.
check("no game-start banner constants",
      ORD.BANNER == nil and ORD.BANNER_REPO == nil, "?")

-- =========================================================================
-- PING RULES.  A fake world: two pills, one base, three tanks.
-- cpathfinder's Dijkstra slate is a C call that does not exist outside the
-- game, so the travel price is stubbed — the auction is not what these test.
-- =========================================================================
_G.OBJECT_TANK    = 1
_G.OBJECT_HOSTILE = 0x80
local cpf = require("cpathfinder")
cpf.smart_cost_dij_only = function() return nil end

local function W()
  return {
    pills = {
      [5] = { mx = 20, my = 20, owner = "hostile",  health = 10 },
      [7] = { mx = 40, my = 40, owner = "hostile",  health = 0  },
      [9] = { mx = 60, my = 60, owner = "friendly", health = 12 },
    },
    bases = {
      [3] = { mx = 30, my = 30, owner = "neutral"  },
      [4] = { mx = 50, my = 50, owner = "friendly" },
    },
  }
end
-- me = p1 at (10,10).  p2 is an ally bot at (12,12).  p7 is an enemy tank at
-- (14,14).  Objects never include our own tank (the engine leaves it out).
local function I(extra)
  local i = {
    tankx = 10 * 256 + 128, tanky = 10 * 256 + 128,
    player_number = 1, allies = 0x16, player_bots = 0x16,
    player_names = { [1] = "Andrew", [2] = "Socrates", [3] = "Seneca",
                     [5] = "Plato", [8] = "Hannibal" },
    man_status = 0, shells = 40, armour = 40, objects = {
      { type = 1, idnum = 2, x = 12 * 256, y = 12 * 256, info = 0 },
      { type = 1, idnum = 7, x = 14 * 256, y = 14 * 256, info = 0x80 },
    },
    events = {},
  }
  for k, v in pairs(extra or {}) do i[k] = v end
  return i
end
local function ST()
  return { player_number = 1, tick = 100, goal = {}, stuck_for = 0 }
end

print("orders.lua — ping tile matching")
local st, w, inf = ST(), W(), I()
check("exact tile: the pill",
      (ORD.resolve_ping(st, w, inf, 20, 20) or {}).class == "pill",
      tostring((ORD.resolve_ping(st, w, inf, 20, 20) or {}).class))
check("exact tile: our own tank is an ally bot",
      (ORD.resolve_ping(st, w, inf, 10, 10) or {}).pn == 1,
      tostring((ORD.resolve_ping(st, w, inf, 10, 10) or {}).pn))
check("exact tile: an ally bot",
      (ORD.resolve_ping(st, w, inf, 12, 12) or {}).class == "allybot",
      tostring((ORD.resolve_ping(st, w, inf, 12, 12) or {}).class))
check("exact tile: an enemy tank",
      (ORD.resolve_ping(st, w, inf, 14, 14) or {}).class == "enemytank",
      tostring((ORD.resolve_ping(st, w, inf, 14, 14) or {}).class))
check("open ground resolves to nothing",
      ORD.resolve_ping(st, w, inf, 100, 100) == nil, "?")
-- THE RING IS FOR TANKS ONLY (Andrew, Sep 15).  He pinged one square off a
-- pillbox to send a decoy to that square and the bot read it as defend_pill.
-- A pill and a base never move and you can put the marker right on them, so
-- beside one is not it: the ping falls through to open ground and means "go
-- there".  A TANK moves while the ping is in flight, so its ring stays.
check("ring: a neighbour of the pill is NOT the pill",
      ORD.resolve_ping(st, w, inf, 21, 20) == nil,
      tostring((ORD.resolve_ping(st, w, inf, 21, 20) or {}).class))
check("ring: a neighbour of a base is NOT the base",
      ORD.resolve_ping(st, w, inf, 31, 30) == nil,
      tostring((ORD.resolve_ping(st, w, inf, 31, 30) or {}).class))
check("ring: a neighbour of an ally bot IS that bot",
      (function()
         local h = ORD.resolve_ping(st, w, inf, 12, 11)
         return h and h.class == "allybot" and h.pn == 2 and h.exact == false
       end)(), tostring((ORD.resolve_ping(st, w, inf, 12, 11) or {}).class))
check("ring: a neighbour of an enemy tank IS that tank",
      (function()
         local h = ORD.resolve_ping(st, w, inf, 15, 14)
         return h and h.class == "enemytank" and h.pn == 7 and h.exact == false
       end)(), tostring((ORD.resolve_ping(st, w, inf, 15, 14) or {}).class))
check("exact still finds the pill",
      (ORD.resolve_ping(st, w, inf, 20, 20) or {}).exact == true, "?")
-- EXACT BEATS RING: p2's tank is at (12,12) and p1's at (10,10); a ping on
-- (11,11) sits in BOTH rings, but a ping on (12,12) must pick p2 alone.
check("exact beats ring: two tanks a tile apart",
      (function()
         local h = ORD.resolve_ping(st, w, inf, 12, 12)
         return h and h.pn == 2 and h.exact == true
       end)(), "?")
-- RING PICKS THE NEARER: from (13,13) the enemy tank at (14,14) is diagonal
-- (d=2) and the ally at (12,12) is diagonal too (d=2) -- so the tie breaks on
-- player number and the ALLY (p2) wins.  From (13,14) the enemy is
-- orthogonal (d=1) and the ally is (d=5), so the enemy wins.
check("ring tie breaks on player number",
      (function()
         local h = ORD.resolve_ping(st, w, inf, 13, 13)
         return h and h.pn == 2
       end)(), "?")
check("ring picks the nearer of two",
      (function()
         local h = ORD.resolve_ping(st, w, inf, 13, 14)
         return h and h.pn == 7 and h.class == "enemytank"
       end)(), "?")

print("orders.lua — ping verb table")
local function pv(mx, my)
  local c = ORD.ping_command(ORD.resolve_ping(st, w, inf, mx, my), mx, my)
  if not c then return "nil" end
  if c.select_pn then return "select:" .. c.select_pn end
  local t = c.target or {}
  return c.verb .. ":" .. (t.kind or "-") .. ":" .. tostring(t.id or t.pn or "-")
end
check("enemy live pill -> attack",   pv(20, 20) == "attack:pill:5",   pv(20, 20))
check("dead pill -> capture",        pv(40, 40) == "capture:pill:7",  pv(40, 40))
check("our pill -> defend",          pv(60, 60) == "defend:pill:9",   pv(60, 60))
check("neutral base -> capture",     pv(30, 30) == "capture:base:3",  pv(30, 30))
check("our base -> nothing",         pv(50, 50) == "nil",             pv(50, 50))
check("enemy tank -> attack",        pv(14, 14) == "attack:tank:7",   pv(14, 14))
check("ally bot -> select",          pv(12, 12) == "select:2",        pv(12, 12))
check("open ground -> go there",     pv(100, 99) == "goto:here:" .. (100 * 256 + 99), pv(100, 99))
-- BESIDE a pill is open ground, which is the decoy Andrew was trying to send.
check("beside a pill -> go there",   pv(21, 20) == "goto:here:" .. (21 * 256 + 20), pv(21, 20))
check("beside a base -> go there",   pv(31, 30) == "goto:here:" .. (31 * 256 + 30), pv(31, 30))
check("beside an ally bot -> select", pv(12, 11) == "select:2",       pv(12, 11))
check("beside an enemy tank -> attack", pv(15, 14) == "attack:tank:7", pv(15, 14))

print("orders.lua — ping events")
-- The engine hands the brain [sender, kind, xHi, xLo, yHi, yLo] in WORLD
-- units.  These build that shape by hand.
_G.EVENT_PING = 13
_G.PING_KIND_CAUTION = 1
_G.PING_KIND_BOT_COMMAND = 5
local function ping(kind, sender, mx, my)
  local wx, wy = mx * 256 + 128, my * 256 + 128
  return { type = 13, data = { sender, kind,
           math.floor(wx / 256), wx % 256, math.floor(wy / 256), wy % 256 } }
end

-- SELECT by ping: p1 is pinged, so p1 answers "Awaiting command".
st, w, inf = ST(), W(), I()
inf.events = { ping(5, 0, 10, 10) }
inf.allies = 0x17   -- p0 (the human) is an ally too
ORD.on_events(st, w, inf, 100)
check("select ping: selected",
      st.orders and st.orders.sel and st.orders.sel.pns[1] == 1,
      st.orders and st.orders.sel and tostring(st.orders.sel.pns[1]) or "nil")
check("select ping: answers",
      st.orders.say[1] == "Awaiting command", tostring(st.orders.say[1]))

-- An ENEMY's ping does nothing at all.
st, w, inf = ST(), W(), I()
inf.events = { ping(5, 9, 10, 10) }
ORD.on_events(st, w, inf, 100)
check("enemy ping ignored", st.orders == nil or st.orders.sel == nil, "?")

-- A bot ping on an enemy pill opens an auction, and a SECOND ping in its 3x3
-- grows the order to two bots instead of opening a second order.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
inf.events = { ping(5, 0, 20, 20) }
ORD.on_events(st, w, inf, 100)
local oid = next(st.orders.auctions)
check("bot ping opens ONE auction",
      oid ~= nil and st.orders.auctions[oid].want == 1,
      oid and tostring(st.orders.auctions[oid].want) or "nil")
inf.events = { ping(5, 0, 21, 20) }
ORD.on_events(st, w, inf, 110)
check("repeat ping in the 3x3 grows the SAME order",
      st.orders.auctions[oid] ~= nil and st.orders.auctions[oid].want == 2,
      st.orders.auctions[oid] and tostring(st.orders.auctions[oid].want) or "nil")
check("repeat ping opens no second order",
      (function()
         local n = 0
         for _ in pairs(st.orders.known) do n = n + 1 end
         return n == 1
       end)(), "?")

-- CAUTION on our own tank with NO order = retreat.  With an order = cancel.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
inf.events = { ping(1, 0, 10, 10) }
ORD.on_events(st, w, inf, 100)
check("caution on a bot with no order -> retreat",
      st.orders.held ~= nil and st.orders.held.kind == "take_cover",
      st.orders.held and st.orders.held.kind or "nil")
inf.events = { ping(1, 0, 10, 10) }
ORD.on_events(st, w, inf, 120)
check("caution on a bot holding an order -> cancel",
      st.orders.held == nil, "?")

-- CAUTION in the 3x3 of an order's TARGET releases the group.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
st.orders = nil
-- player_names is indexed pn+1, so THIS bot (p1) is "Socrates": the order is
-- addressed straight at us, no auction.
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 100, true, false)
check("a chat order addressed by name is held",
      st.orders.held ~= nil and st.orders.held.kind == "attack_pill"
      and st.orders.held.tid == 5,
      st.orders.held and st.orders.held.kind or "nil")
inf.events = { ping(1, 0, 21, 21) }
ORD.on_events(st, w, inf, 130)
check("caution near the target releases the order",
      st.orders.held == nil and st.orders.known[1] == nil, "?")

-- A CAUTION BESIDE A PILL NOBODY WAS SENT TO DOES NOTHING.  The cancel is
-- about the ORDER, not about what stands on the tile, so with no order on
-- that place the caution starts nothing and says nothing.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
inf.events = { ping(1, 0, 21, 20) }
ORD.on_events(st, w, inf, 100)
check("caution beside a pill with no order does nothing",
      st.orders == nil
      or (st.orders.held == nil and #(st.orders.say or {}) == 0),
      tostring(st.orders and st.orders.held and st.orders.held.kind))

print("orders.lua — the speaking bot")
check("speaker = the lowest bot player number",
      ORD.speaker({ player_number = 1 }, I()) == 1,
      tostring(ORD.speaker({ player_number = 1 }, I())))
check("speaker is the same answer from every bot",
      ORD.speaker({ player_number = 5 }, I()) == 1,
      tostring(ORD.speaker({ player_number = 5 }, I())))

print("orders.lua — settings over chat (the runtime path)")
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "focus bases", 200, true, false)
check("focus bases latches", st._focus == "bases", tostring(st._focus))
check("the speaking bot confirms it",
      st.orders.say[1] == "Focus: bases.", tostring(st.orders.say[1]))
check("and puts it on the wire",
      st.orders.out[1] == "/info obf 1", tostring(st.orders.out[1]))
ORD.on_chat(st, w, inf, 0, "focus off", 210, true, false)
check("focus off clears it", st._focus == nil, tostring(st._focus))
ORD.on_chat(st, w, inf, 0, "reposition on", 220, true, false)
check("reposition on latches", st._repo_override == true, tostring(st._repo_override))
check("reposition on is not blocked",
      ORD.reposition_human_blocked(st) == false, "?")
ORD.on_chat(st, w, inf, 0, "reposition off", 230, true, false)
check("reposition off latches", st._repo_override == false, tostring(st._repo_override))
check("reposition off is blocked",
      ORD.reposition_human_blocked(st) == true, "?")
-- a late joiner latching the same values off the wire
local st2 = ST()
ORD.rx(1, "/info obf 2", 300, st2)
ORD.rx(1, "/info obp 1", 300, st2)
ORD.update(st2, W(), I({ allies = 0x17 }), 300)
check("a late bot latches focus from the verb", st2._focus == "pills", tostring(st2._focus))
check("a late bot latches reposition from the verb", st2._repo_override == true, tostring(st2._repo_override))

st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "help", 240, true, false)
check("help answers 4 lines", #st.orders.say == 4, tostring(#st.orders.say))

-- NOTHING is said at game start any more.  The brain's announce line and its
-- docs are lobby text files now (announce.txt / commands.txt), so a bot that
-- has heard no order stays silent through its first ticks, human ally or not.
st, w, inf = ST(), W(), I()
inf.allies = 0x17   -- p0 is a human ally (player_bots has no bit 0)
ORD.update(st, w, inf, 50)
check("no game-start line with a human ally", #st.orders.say == 0,
      tostring(#st.orders.say))
ORD.update(st, w, inf, 60)
check("still silent on the next tick", #st.orders.say == 0,
      tostring(#st.orders.say))
local st3 = ST()
ORD.update(st3, W(), I({ allies = 0x16, player_bots = 0x16 }), 50)
check("no game-start line with no human ally",
      #st3.orders.say == 0, tostring(#st3.orders.say))

-- =========================================================================
-- "bot chat off" — THE SPOKEN GOAL CONFIRMATIONS, AND ONLY THOSE.
-- The latch rides its own wire verb (obh) like the other three, and a bot
-- that joins late catches it off the heartbeat.
-- =========================================================================
print("orders.lua — bot chat off (the runtime path)")
st, w, inf = ST(), W(), I()
inf.allies = 0x17
check("bot chat is ON before anybody says otherwise",
      ORD.bot_chat_on(st) == true, tostring(ORD.bot_chat_on(st)))
ORD.on_chat(st, w, inf, 0, "bot chat off", 200, true, false)
check("bot chat off latches", ORD.bot_chat_on(st) == false, tostring(ORD.bot_chat_on(st)))
-- The confirmation of the latch ITSELF is still said: a person who has just
-- turned the chat off is owed the word that it is off.
check("the speaking bot confirms the latch",
      st.orders.say[1] == "Bot chat off.", tostring(st.orders.say[1]))
check("and puts it on the wire",
      st.orders.out[1] == "/info obh 0", tostring(st.orders.out[1]))
local n_before = #st.orders.say
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 210, true, false)
check("the order is still taken with chat off",
      st.orders.held ~= nil and st.orders.held.tid == 5,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
check("but the ack is NOT said", #st.orders.say == n_before,
      tostring(#st.orders.say) .. " vs " .. tostring(n_before))
-- A REPLY A PERSON IS OWED IS NEVER SILENCED.
ORD.on_chat(st, w, inf, 0, "attack", 220, true, false)
check("a 'didn't understand' still comes back with chat off",
      st.orders.say[#st.orders.say] == "didn't understand",
      tostring(st.orders.say[#st.orders.say]))
ORD.on_chat(st, w, inf, 0, "help", 230, true, false)
check("help still answers with chat off",
      #st.orders.say == n_before + 5, tostring(#st.orders.say))
-- Back on, and the ack comes back with it.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "bot chat off", 300, true, false)
ORD.on_chat(st, w, inf, 0, "bot chat on", 310, true, false)
check("bot chat on latches again", ORD.bot_chat_on(st) == true, tostring(ORD.bot_chat_on(st)))
local n2 = #st.orders.say
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 320, true, false)
-- The solo ack waits out the claim window (ORDER_CLAIM_TIEBREAK).
ORD.update(st, w, inf, 331)
check("the ack is said again once chat is back on",
      #st.orders.say > n2, tostring(#st.orders.say) .. " vs " .. tostring(n2))
-- A late joiner latches it off the wire, exactly like focus and bot pings.
local st4 = ST()
ORD.rx(1, "/info obh 0", 400, st4)
ORD.update(st4, W(), I({ allies = 0x17 }), 400)
check("a late bot latches bot chat from the verb",
      ORD.bot_chat_on(st4) == false, tostring(ORD.bot_chat_on(st4)))

-- =========================================================================
-- A COUNT WHO-WORD AT RUNTIME: the auction is opened with want = N, and the
-- settle hands the order to the N cheapest bids.
-- =========================================================================
print("orders.lua — a count who-word (the runtime path)")
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "4 attack 5", 200, true, false)
local coid = next(st.orders.auctions)
check("a count opens an auction with want = N",
      coid ~= nil and st.orders.auctions[coid].want == 4,
      coid and tostring(st.orders.auctions[coid].want) or "nil")
check("a count is a GROUP order, so it settles into the group ack path",
      (function()
         ORD.update(st, w, inf, 210)
         return st.orders.held ~= nil and st.orders.held.group == true
       end)(),
      st.orders.held and tostring(st.orders.held.group) or "nil held")
-- A plain line is still one bot.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "attack 5", 200, true, false)
local aoid = next(st.orders.auctions)
check("a plain order still wants one bot",
      aoid ~= nil and st.orders.auctions[aoid].want == 1,
      aoid and tostring(st.orders.auctions[aoid].want) or "nil")
-- THE COUNT LINE GROWS AN ORDER THE BOT ALREADY HOLDS, the way a repeat ping
-- does: same id, auction re-opened for the slots still empty.
ORD.update(st, w, inf, 210)
check("the plain order is held", st.orders.held ~= nil and st.orders.held.oid == aoid, "?")
ORD.on_chat(st, w, inf, 0, "3 attack 5", 220, true, false)
check("a count line re-opens the SAME order for more bots",
      st.orders.auctions[aoid] ~= nil and st.orders.auctions[aoid].want == 3,
      st.orders.auctions[aoid] and tostring(st.orders.auctions[aoid].want) or "nil")
check("and opens no second order",
      (function()
         local n = 0
         for _ in pairs(st.orders.known) do n = n + 1 end
         return n == 1
       end)(), "?")

-- =========================================================================
-- "attack closest" AT RUNTIME: the pill nearest the PERSON WHO TYPED IT.
-- p0 is the human; the fixture leaves its tank out of info.objects, which is
-- exactly the "I cannot see you" case.
-- =========================================================================
print("orders.lua — the closest target word (the runtime path)")
-- A human tank object at a tile, added to the fixture's object list.
local function with_human(mx, my)
  local i = I()
  i.allies = 0x17
  i.objects[#i.objects + 1] = { type = 1, idnum = 0, x = mx * 256, y = my * 256, info = 0 }
  return i
end
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "attack closest", 200, true, false)
check("with the sender out of sight the bot says so",
      st.orders.say[1] == "can't see you", tostring(st.orders.say[1]))
check("and starts nothing", st.orders.held == nil and next(st.orders.auctions) == nil, "?")
-- Pill 5 is at (20,20) and pill 7 at (40,40); the human at (19,19) is beside 5.
st, w = ST(), W()
inf = with_human(19, 19)
ORD.on_chat(st, w, inf, 0, "attack closest", 200, true, false)
ORD.update(st, w, inf, 210)
check("the sender beside pill 5 gets pill 5",
      st.orders.held ~= nil and st.orders.held.tid == 5,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
-- The same line from the other side of the map gets the other pill.
st, w = ST(), W()
inf = with_human(38, 38)
ORD.on_chat(st, w, inf, 0, "attack closest", 200, true, false)
ORD.update(st, w, inf, 210)
check("the sender beside pill 7 gets pill 7",
      st.orders.held ~= nil and st.orders.held.tid == 7,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
-- ATTACK NEVER PICKS ONE OF OUR OWN PILLS, even when the sender is standing
-- on it: pill 9 at (60,60) is friendly, so a human at (60,61) still gets 7.
st, w = ST(), W()
inf = with_human(60, 61)
ORD.on_chat(st, w, inf, 0, "attack closest", 200, true, false)
ORD.update(st, w, inf, 210)
check("attack closest skips our own pills",
      st.orders.held ~= nil and st.orders.held.tid == 7,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
-- DEFEND LOOKS THE OTHER WAY: ours only, so the same tile gets pill 9.
st, w = ST(), W()
inf = with_human(60, 61)
ORD.on_chat(st, w, inf, 0, "defend closest", 200, true, false)
ORD.update(st, w, inf, 210)
check("defend closest takes one of OUR pills",
      st.orders.held ~= nil and st.orders.held.tid == 9,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
-- THE LAST KNOWN POSITION. The human is seen on one think and gone on the
-- next; the line still resolves against where the bot last saw them.
st, w = ST(), W()
inf = with_human(19, 19)
ORD.update(st, w, inf, 200)                      -- remembers p0 at (19,19)
local gone = I()
gone.allies = 0x17                               -- p0 is not in the object list
ORD.on_chat(st, w, gone, 0, "attack closest", 210, true, false)
ORD.update(st, w, gone, 220)
check("a sender who drove out of sight still resolves",
      st.orders.held ~= nil and st.orders.held.tid == 5,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
-- A COUNT AND `closest` ON ONE LINE — the README's "4 attack closest".
st, w = ST(), W()
inf = with_human(19, 19)
ORD.on_chat(st, w, inf, 0, "4 attack closest", 200, true, false)
local xoid = next(st.orders.auctions)
check("'4 attack closest' wants 4 bots on the pill beside the sender",
      xoid ~= nil and st.orders.auctions[xoid].want == 4
      and st.orders.auctions[xoid].spec.tid == 5,
      xoid and (tostring(st.orders.auctions[xoid].want) .. "/" ..
                tostring(st.orders.auctions[xoid].spec.tid)) or "nil")

-- =========================================================================
-- THREE SHOTS = GO THERE.  Three of one player's shells that run their full
-- range and land on one open square inside two seconds are an order; the
-- server spots them and injects "!goto <mx> <my>" with the SHOOTER as the
-- sender.  The parser takes that line from any ally, and only with the "!".
-- =========================================================================
print("orders.lua — three shots = go there")
local HERE = 100 * 256 + 99
check("!goto 100 99",
      shape(P("!goto 100 99")) == "goto who=ping tgt=here:" .. HERE,
      shape(P("!goto 100 99")))
check("goto without the ! is not an order",
      P("goto 100 99") == nil, shape(P("goto 100 99")))
check("!goto off the map",
      shape(P("!goto 300 4")) == "reply:didn't understand", shape(P("!goto 300 4")))
check("!goto with no square",
      shape(P("!goto")) == "reply:didn't understand", shape(P("!goto")))
check("!goto with one number",
      shape(P("!goto 40")) == "reply:didn't understand", shape(P("!goto 40")))
check("!goto with four numbers",
      shape(P("!goto 100 99 24")) == "reply:didn't understand",
      shape(P("!goto 100 99 24")))
check("!goto with a shooter tile off the map",
      shape(P("!goto 100 99 24 400")) == "reply:didn't understand",
      shape(P("!goto 100 99 24 400")))

-- The long form the SERVER sends carries the shooter's own tile, and that is
-- what the range rule reads.  The short form -- a human typing it -- has no
-- shooter tile and keeps the old rule.
check("the long form names the shooter's tile",
      P("!goto 100 99 24 25").who.from_mx == 24
      and P("!goto 100 99 24 25").who.from_my == 25,
      tostring(P("!goto 100 99 24 25").who.from_mx))
check("the long form uses the shooter's view",
      P("!goto 100 99 24 25").who.near == C.ORDER_SHOT_VIEW_TILES,
      tostring(P("!goto 100 99 24 25").who.near))
check("the short form keeps the old ten tiles",
      P("!goto 100 99").who.near == C.ORDER_NEARBY_TILES
      and P("!goto 100 99").who.from_mx == nil,
      tostring(P("!goto 100 99").who.near))
check("both forms name the same square",
      shape(P("!goto 100 99 24 25")) == "goto who=ping tgt=here:" .. HERE,
      shape(P("!goto 100 99 24 25")))

-- The id is the one a bot ping on the same square from the same sender
-- derives, so a ping and a burst of shots land on ONE order.
check("the id matches a ping on the same square",
      ORD.order_id(0, P("!goto 100 99")) ==
      ORD.order_id(0, { verb = "goto", who = { mode = "ping" },
                        target = { kind = "here", id = HERE } }), "?")

-- The ten-tile rule.  This bot sits at (10,10).
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 400, true, false)
local goid = next(st.orders.auctions)
check("a three-shot order opens an auction", goid ~= nil, tostring(goid))
check("a bot within 10 tiles bids",
      goid ~= nil and type(st.orders.auctions[goid].bids[1]) == "number",
      goid and tostring(st.orders.auctions[goid].bids[1]) or "nil")

st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "!goto 100 99", 400, true, false)
local foid = next(st.orders.auctions)
check("a bot further than 10 tiles does not bid",
      foid ~= nil and st.orders.auctions[foid].bids[1] == nil,
      foid and tostring(st.orders.auctions[foid].bids[1]) or "no auction")
check("and it says nothing about it",
      #st.orders.say == 0, tostring(#st.orders.say))

-- THE SHOOTER'S VIEW.  With a shooter tile in the line the range is measured
-- from the SHOOTER and the way a screen is -- the larger of the two axes --
-- so the bots that bid are the ones the shooter could see.  This bot sits at
-- (10,10), and the target is 90 tiles away: the old rule would have refused
-- every one of these.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "!goto 100 99 24 24", 400, true, false)
local voidid = next(st.orders.auctions)
check("a bot 14 tiles from the shooter bids",
      voidid ~= nil and type(st.orders.auctions[voidid].bids[1]) == "number",
      voidid and tostring(st.orders.auctions[voidid].bids[1]) or "no auction")

st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "!goto 100 99 25 25", 400, true, false)
local outid = next(st.orders.auctions)
check("a bot 15 tiles from the shooter does not bid",
      outid ~= nil and st.orders.auctions[outid].bids[1] == nil,
      outid and tostring(st.orders.auctions[outid].bids[1]) or "no auction")
check("and it says nothing about that either",
      #st.orders.say == 0, tostring(#st.orders.say))

-- The larger axis, not the sum: (10,10) to (24,10) is 14 across and 0 down,
-- which a screen shows and Manhattan distance would also allow, but
-- (10,10) to (24,24) is 28 by Manhattan and still one screen.
st, w, inf = ST(), W(), I()
inf.allies = 0x17
ORD.on_chat(st, w, inf, 0, "!goto 100 99 24 10", 400, true, false)
local flatid = next(st.orders.auctions)
check("a bot 14 tiles along one axis bids",
      flatid ~= nil and type(st.orders.auctions[flatid].bids[1]) == "number",
      flatid and tostring(st.orders.auctions[flatid].bids[1]) or "no auction")

-- An enemy shooting three shells orders nobody about: the sender is not an
-- ally, so on_chat never reads the line.
st, w, inf = ST(), W(), I()
local took_shot = ORD.on_chat(st, w, inf, 9, "!goto 15 15", 400, false, false)
check("an enemy's three shots are ignored",
      took_shot == false and st.orders == nil, tostring(took_shot))

-- =========================================================================
-- THE OLD OPERATOR COMMANDS.  They now need a leading "!" from everybody,
-- and they are commands.lua's, not orders.lua's. Two things have to hold:
-- orders.lua must hand a "!" line it has no verb for straight through in
-- silence (nil, not "didn't understand", which would swallow it), and a
-- bare word must not be a command any more.
-- =========================================================================
print("orders.lua — the old operator commands pass through")
local CMDS = require("commands")

for _, line in ipairs({ "!stop", "!start", "!status", "!cp:5", "!cb:all",
                        "!cb:3", "!pill:2", "!base:1", "!bpc:4", "!pp:6",
                        "!watch:7" }) do
  check(line .. " is not answered by orders.lua", P(line) == nil, shape(P(line)))
  check(line .. " is a command", CMDS.parse(line) ~= nil, "nil")
end

-- A "!" line that is neither an order nor a command still gets an answer.
check("!wibble still answered", shape(P("!wibble")) == "reply:didn't understand",
      shape(P("!wibble")))
-- And the orders themselves are untouched by the pass-through.
check("!attack 5 still an order", shape(P("!attack 5")) == "attack who=auto tgt=pill:5",
      shape(P("!attack 5")))

print("commands.lua — a bare word is ordinary chat")
for _, line in ipairs({ "stop", "start", "status", "cp:5", "cb:all", "cb:3",
                        "pill:2", "base:1", "bpc:4", "pp:6", "watch:7",
                        "  STOP  " }) do
  check("bare " .. line .. " is ignored", CMDS.parse(line) == nil, "a command")
end
-- Leading and trailing space around the "!" form is still fine.
check("'  !STOP  ' is a command",
      (CMDS.parse("  !STOP  ") or {}).cmd == "stop", "?")
check("'! cp:5' is a command",
      (CMDS.parse("! cp:5") or {}).cmd == "cp", "?")

print("orders.lua — focus pricing")
check("no focus -> x1", ORD.focus_mult({}, "attack_pill") == 1.0, "?")
check("focus bases -> pill goals pay",
      ORD.focus_mult({ _focus = "bases" }, "attack_pill") == C.FOCUS_OTHER_COST_MULT, "?")
check("focus bases -> base goals keep their price",
      ORD.focus_mult({ _focus = "bases" }, "capture_base") == 1.0, "?")
check("focus pills -> base goals pay",
      ORD.focus_mult({ _focus = "pills" }, "capture_base") == C.FOCUS_OTHER_COST_MULT, "?")
check("focus pills -> pill goals keep their price",
      ORD.focus_mult({ _focus = "pills" }, "defend_pill") == 1.0, "?")
check("focus never touches survival",
      ORD.focus_mult({ _focus = "bases" }, "take_cover") == 1.0
      and ORD.focus_mult({ _focus = "pills" }, "refuel_at_base") == 1.0, "?")
check("focus label",
      ORD.focus_label({ _focus = "bases" }) ==
        string.format("focus: x%.1f (bases)", C.FOCUS_OTHER_COST_MULT),
      ORD.focus_label({ _focus = "bases" }))

-- =========================================================================
-- THE SAME ORDER, SAID AGAIN.  The id is what decides it, and the id does
-- not carry the sender: the same words from a second human are the same job.
-- A bot that already holds the order refreshes its focus and says one short
-- line back, no more often than ORDER_REPEAT_ACK_TICKS.
-- =========================================================================
print("orders.lua — the same order said again")

check("the same line has the same id",
      ORD.order_id(0, P("!attack 5")) == ORD.order_id(0, P("!attack 5")), "?")
check("a second sender's copy has the same id",
      ORD.order_id(0, P("!attack 5")) == ORD.order_id(3, P("!attack 5")), "?")
check("another target has another id",
      ORD.order_id(0, P("!attack 5")) ~= ORD.order_id(0, P("!attack 9")), "?")
check("another who-word has another id",
      ORD.order_id(0, P("!attack 5")) ~= ORD.order_id(0, P("!all attack 5")), "?")

-- No allies, so the auction closes on the next update and this bot takes it.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "attack 5", 100, true, false)
ORD.update(st, w, inf, 101)
check("the order is taken",
      (st.orders.held or {}).kind == "attack_pill",
      tostring((st.orders.held or {}).kind))

st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "attack 5", 400, true, false)
check("a repeat is answered",
      st.orders.say[1] == "Still on it. attack_pill #5",
      tostring(st.orders.say[1]))
check("once", #st.orders.say == 1, tostring(#st.orders.say))
check("and it refreshes the focus",
      st.orders.held.expiry == 400 + C.ORDER_FOCUS_TICKS,
      tostring(st.orders.held.expiry))
check("the order is not retaken",
      next(st.orders.auctions) == nil, "an auction")

st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "attack 5", 400 + C.ORDER_REPEAT_ACK_TICKS - 1,
            true, false)
check("a repeat inside the window is silent", #st.orders.say == 0,
      tostring(st.orders.say[1]))

st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "attack 5", 400 + C.ORDER_REPEAT_ACK_TICKS,
            true, false)
check("a repeat after the window is answered",
      st.orders.say[1] == "Still on it. attack_pill #5",
      tostring(st.orders.say[1]))

st.orders.say = {}
ORD.on_chat(st, w, inf, 3, "attack 5", 900, true, false)
check("a second sender's repeat is a repeat",
      st.orders.say[1] == "Still on it. attack_pill #5",
      tostring(st.orders.say[1]))

-- A bot that does NOT hold the order says nothing extra: while the auction
-- is still open there is no holder, so a repeat is silent.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "attack 5", 100, true, false)
ORD.on_chat(st, w, inf, 0, "attack 5", 105, true, false)
check("a repeat during the auction is silent", #st.orders.say == 0,
      tostring(st.orders.say[1]))

-- The GROUP line: two bots hold it, the lower player number answers.  This
-- bot is p1 and the other holder is p2, so this bot is the one that speaks.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "attack 5", 100, true, false)
ORD.update(st, w, inf, 101)
local oid = st.orders.held.oid
st.orders.held.tkind = "pill"
st.orders.gclaims[oid][2] = 10
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "attack 5", 400, true, false)
check("two holders answer with the count",
      st.orders.say[1] == "Still on it. 2 on pill #5",
      tostring(st.orders.say[1]))

-- The same, with a LOWER holder in the group: p0 speaks, not this bot.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "attack 5", 100, true, false)
ORD.update(st, w, inf, 101)
st.orders.gclaims[st.orders.held.oid][0] = 10
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "attack 5", 400, true, false)
check("only the lowest player number answers", #st.orders.say == 0,
      tostring(st.orders.say[1]))

-- =========================================================================
-- `last` AT RUNTIME.  p1 is this bot and its name is Socrates (player_names
-- is indexed pn+1), so a line addressed at "socrates" lands on us with no
-- auction and we become the sender's last holder.
-- =========================================================================
print("orders.lua -- the last who-word")

local function n_auctions(state)
  local n = 0
  for _ in pairs(state.orders.auctions) do n = n + 1 end
  return n
end

-- 1. AFTER A SINGLE-BOT ORDER: `last` sends that one bot, with no auction.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 100, true, false)
check("setup: we hold the named order",
      st.orders.held ~= nil and st.orders.held.tid == 5,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "last attack 7", 200, true, false)
check("last after one bot: the same bot takes the new order",
      st.orders.held ~= nil and st.orders.held.tid == 7,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
check("last after one bot: no auction is opened", n_auctions(st) == 0,
      tostring(n_auctions(st)))

-- 2. AFTER A GROUP ORDER: `last` sends the whole group, and the new order is
-- a group order too (two holders, so nobody acks alone).
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "all attack 5", 100, true, false)
local loid = st.orders.held.oid
st.orders.gclaims[loid][2] = 10          -- Seneca claimed it as well
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "last defend 9", 200, true, false)
check("last after a group: the group takes the new order",
      st.orders.held ~= nil and st.orders.held.tid == 9,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
check("last after a group: it stays a group order",
      st.orders.held ~= nil and st.orders.held.group == true,
      st.orders.held and tostring(st.orders.held.group) or "nil")
check("last after a group: still no auction", n_auctions(st) == 0,
      tostring(n_auctions(st)))

-- 3. NO HISTORY: the speaking bot says so and nothing is ordered.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "last attack 5", 100, true, false)
check("last with no history answers 'no previous order'",
      st.orders.say[1] == "no previous order", tostring(st.orders.say[1]))
check("last with no history orders nothing",
      st.orders.held == nil and n_auctions(st) == 0, "?")

-- 4. `last cancel` releases the bot that took the last order.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 100, true, false)
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "last cancel", 200, true, false)
check("last cancel releases the holder", st.orders.held == nil,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
check("last cancel says Released", st.orders.say[1] == "Released",
      tostring(st.orders.say[1]))

-- 5. A BOT THAT IS NOT IN THE LAST SET KEEPS WHAT IT HOLDS.  Seneca (p2)
-- took the sender's last order, so "last cancel" is not aimed at us and our
-- own order survives it.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 100, true, false)
st.orders.last_by[0] = 999               -- the sender's last order went elsewhere
st.orders.gclaims[999] = { [2] = 10 }    -- and Seneca is the one holding it
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "last cancel", 200, true, false)
check("last cancel leaves a bot outside the set alone",
      st.orders.held ~= nil and st.orders.held.tid == 5,
      st.orders.held and tostring(st.orders.held.tid) or "nil")
check("and that bot says nothing", #st.orders.say == 0,
      tostring(st.orders.say[1]))

-- 6. A LIVE SELECTION WINS over the last order: it is the newer, plainer
-- statement of "these bots".  Seneca is selected, so "last attack 7" is
-- aimed at Seneca and we keep the pill-5 order we already hold.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 100, true, false)
ORD.on_chat(st, w, inf, 0, "seneca", 150, true, false)
check("setup: the selection is live",
      st.orders.sel ~= nil and st.orders.sel.pns[1] == 2,
      st.orders.sel and tostring(st.orders.sel.pns[1]) or "nil")
ORD.on_chat(st, w, inf, 0, "last attack 7", 160, true, false)
check("a live selection beats the last order",
      st.orders.held ~= nil and st.orders.held.tid == 5,
      st.orders.held and tostring(st.orders.held.tid) or "nil")

-- 7. `last` mixed with a bot name is turned down, and the speaking bot says
-- why rather than ordering either set.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates attack 5", 100, true, false)
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "socrates last attack 7", 200, true, false)
check("last with a name is turned down",
      st.orders.say[1] == "last or a name, not both", tostring(st.orders.say[1]))
check("and nothing moves", st.orders.held.tid == 5,
      tostring(st.orders.held.tid))

-- 8. A SUBJECT IN FRONT OF A SETTING still sets it, with the usual line.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "all repositioning on", 200, true, false)
check("`all repositioning on` latches", st._repo_override == true,
      tostring(st._repo_override))
check("and confirms it the same way",
      st.orders.say[1] == "Repositioning on.", tostring(st.orders.say[1]))

-- =========================================================================
-- AN ORDER ENDS WHEN ITS JOB IS DONE.  Not when the 60 s focus runs out:
-- the housekeeping pass reads the world every tick and clears the slot the
-- moment the target condition is met, so goal selection starts again on the
-- next tick.  W()'s pill 5 is a live hostile pill, pill 7 a dead one and
-- pill 9 is already ours.
-- =========================================================================
print("orders.lua -- an order ends when the job is done")

-- 1. SWEEP: the pill flies our flag -> the slot clears.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "sweep 7", 100, true, false)
ORD.update(st, w, inf, 101)
check("setup: the sweep order is held",
      (st.orders.held or {}).kind == "capture_pill",
      tostring((st.orders.held or {}).kind))
w.pills[7].owner = "friendly"
st.orders.say = {}
ORD.update(st, w, inf, 102)
check("a swept pill ends the order on the next tick", st.orders.held == nil,
      st.orders.held and st.orders.held.kind or "nil")
check("and it says the line it always said",
      st.orders.say[1] == "capture_pill #7 done", tostring(st.orders.say[1]))

-- 2. SWEEP: the pill is picked up into a tank -> the slot clears.  A pill in
--    our OWN cargo reads as "allied", never "friendly", which is why the
--    owner-only test never fired for the bot that did the sweeping.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "sweep 7", 100, true, false)
ORD.update(st, w, inf, 101)
w.pills[7].in_tank = true
w.pills[7].owner   = "allied"
ORD.update(st, w, inf, 102)
check("a pill in our cargo ends the order", st.orders.held == nil,
      st.orders.held and st.orders.held.kind or "nil")

-- 3. A pill that has left the world altogether ends it too.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "sweep 7", 100, true, false)
ORD.update(st, w, inf, 101)
w.pills[7] = nil
ORD.update(st, w, inf, 102)
check("a pill that is gone ends the order", st.orders.held == nil, "?")

-- 4. ATTACK -> SWEEP, in place.  A pill shot to zero armour cannot be
--    attacked any further, so the held order becomes capture_pill on the SAME
--    pill: same order id, no second ack.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "attack 5", 100, true, false)
ORD.update(st, w, inf, 101)
check("setup: the attack order is held",
      (st.orders.held or {}).kind == "attack_pill",
      tostring((st.orders.held or {}).kind))
local conv_oid = st.orders.held.oid
st.orders.say = {}
w.pills[5].health = 0
ORD.update(st, w, inf, 102)
check("a dead pill turns attack into sweep",
      (st.orders.held or {}).kind == "capture_pill",
      tostring((st.orders.held or {}).kind))
check("the order keeps its id", (st.orders.held or {}).oid == conv_oid,
      tostring((st.orders.held or {}).oid))
check("and says nothing about it", #st.orders.say == 0,
      tostring(st.orders.say[1]))
check("a dead pill needs no shells",
      st.orders.held.needs_shells == false,
      tostring(st.orders.held.needs_shells))
w.pills[5].owner = "friendly"
ORD.update(st, w, inf, 103)
check("and the converted order ends when the pill is ours",
      st.orders.held == nil, "?")
check("under its new name",
      st.orders.say[1] == "capture_pill #5 done", tostring(st.orders.say[1]))

-- 5. A BASE: friendly owner ends it.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "capture base 3", 100, true, false)
ORD.update(st, w, inf, 101)
check("setup: the base order is held",
      (st.orders.held or {}).kind == "capture_base",
      tostring((st.orders.held or {}).kind))
w.bases[3].owner = "friendly"
ORD.update(st, w, inf, 102)
check("a captured base ends the order", st.orders.held == nil, "?")

-- 6. A NAMED TANK.  target_tile reads the live and ghost lists; when neither
--    has it the tank is dead or out of sight, and the order ends after
--    ORDER_TANK_LOST_TICKS -- not before.
st, w, inf = ST(), W(), I({ allies = 0 })
st.perc = { enemy_tanks = { { id = 7, mx = 14, my = 14 } }, ghost_tanks = {} }
ORD.on_chat(st, w, inf, 0, "attack hannibal", 100, true, false)
ORD.update(st, w, inf, 101)
check("setup: the tank order is held",
      (st.orders.held or {}).kind == "attack_tank"
      and st.orders.held.tid == 7,
      tostring((st.orders.held or {}).kind))
st.perc.enemy_tanks = {}
ORD.update(st, w, inf, 101 + C.ORDER_TANK_LOST_TICKS - 1)
check("a tank out of sight for less than the window is still the target",
      st.orders.held ~= nil, "released")
st.orders.say = {}
ORD.update(st, w, inf, 101 + C.ORDER_TANK_LOST_TICKS)
check("a tank lost for the whole window ends the order",
      st.orders.held == nil, "held")
check("and the bot says it lost the tank",
      st.orders.say[1] == "Lost Hannibal", tostring(st.orders.say[1]))

-- 7. HOLDING A SPOT IS THE JOB.  defend_pill runs to the timer even though
--    its pill is friendly from the first tick.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "defend 9", 100, true, false)
ORD.update(st, w, inf, 101)
check("setup: the defend order is held",
      (st.orders.held or {}).kind == "defend_pill",
      tostring((st.orders.held or {}).kind))
ORD.update(st, w, inf, 500)
check("a friendly pill does not end a defend order",
      st.orders.held ~= nil, "released")

-- 8. And so does take_cover: a retreat holds its spot to the timer.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates retreat", 100, true, false)
check("setup: the retreat is held",
      (st.orders.held or {}).kind == "take_cover",
      tostring((st.orders.held or {}).kind))
ORD.update(st, w, inf, 500)
check("a retreat runs to the timer", st.orders.held ~= nil, "released")

-- =========================================================================
-- THE ACK FOR A TILE ORDER.  `retreat` runs as take_cover and "go there and
-- hold" as goto_tile, whose target id is the packed square (mx * 256 + my).
-- Printing it said "take_cover #30325", a number that means nothing to the
-- human who typed the line.  Andrew, Sep 14: say just the word.  Sep 15: the
-- word for a place order is "goto", because that is now what it runs.
-- =========================================================================
print("orders.lua -- the tile-order ack carries no number")

check("goal_label: a retreat is the bare word",
      ORD.goal_label("take_cover", 100 * 256 + 99) == "take_cover",
      ORD.goal_label("take_cover", 100 * 256 + 99))
check("goal_label: a place order says goto",
      ORD.goal_label("goto_tile", 100 * 256 + 99) == "goto",
      ORD.goal_label("goto_tile", 100 * 256 + 99))
check("goal_label: everything else keeps its number",
      ORD.goal_label("attack_pill", 5) == "attack_pill #5",
      ORD.goal_label("attack_pill", 5))
check("group_label: a retreat is the bare word",
      ORD.group_label("take_cover", "here", 100 * 256 + 99) == "take_cover",
      ORD.group_label("take_cover", "here", 100 * 256 + 99))
check("group_label: a place order says goto",
      ORD.group_label("goto_tile", "here", 100 * 256 + 99) == "goto",
      ORD.group_label("goto_tile", "here", 100 * 256 + 99))
check("group_label: everything else names the target class",
      ORD.group_label("attack_pill", "pill", 5) == "pill #5",
      ORD.group_label("attack_pill", "pill", 5))

st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates retreat", 100, true, false)
-- The solo ack waits out the claim window (ORDER_CLAIM_TIEBREAK).
ORD.update(st, w, inf, 111)
check("a retreat acks with the bare word",
      st.orders.say[1] ~= nil
      and st.orders.say[1]:sub(-#" take_cover") == " take_cover"
      and st.orders.say[1]:find("#", 1, true) == nil,
      tostring(st.orders.say[1]))
-- "GO THERE AND HOLD": the tile really is in the order id, and it still must
-- not reach the chat.  A repeat of it is the one that gets the "Still on it."
-- line, because `retreat` always drops what it holds before it starts.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
-- The solo ack waits out the claim window (ORDER_CLAIM_TIEBREAK).
ORD.update(st, w, inf, 111)
check("setup: the go-there order is held",
      (st.orders.held or {}).kind == "goto_tile",
      tostring((st.orders.held or {}).kind))
check("a go-there order acks with the bare word",
      st.orders.say[1] ~= nil
      and st.orders.say[1]:sub(-#" goto") == " goto"
      and st.orders.say[1]:find("#", 1, true) == nil,
      tostring(st.orders.say[1]))
st.orders.say = {}
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100 + C.ORDER_REPEAT_ACK_TICKS,
            true, false)
check("and the repeat line carries no number either",
      st.orders.say[1] == "Still on it. goto",
      tostring(st.orders.say[1]))

-- =========================================================================
-- A PLACE ORDER IS A HARD LOCK.  It runs as a command_goal — the slot the old
-- `!pill:N` / `!base:N` lines used — so goals.pick_goal answers it before goal
-- selection runs and the bot does nothing else while it stands.  Andrew, Sep
-- 15: "don't do anything, just go there".
-- =========================================================================
print("orders.lua -- a place order is a command goal")

st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)            -- the auction settles and the bot takes it
check("taking a here order sets a goto_tile command goal",
      st.command_goal ~= nil and st.command_goal.kind == "goto_tile",
      tostring(st.command_goal and st.command_goal.kind))
check("the command goal carries the ordered square, in tiles and in world units",
      st.command_goal.mx == 15 and st.command_goal.my == 15
      and st.command_goal.wx == 15 * 256 + 128
      and st.command_goal.wy == 15 * 256 + 128
      and st.command_goal.id == 0,
      string.format("(%s,%s) (%s,%s) id=%s",
        tostring(st.command_goal.mx), tostring(st.command_goal.my),
        tostring(st.command_goal.wx), tostring(st.command_goal.wy),
        tostring(st.command_goal.id)))

-- THE HOLD.  Arrival clears the command goal (goals.pick_goal does that for
-- every command goal); the next think must put it straight back, or the bot
-- hands the tick to the goal pools and drifts off the square.
st.command_goal = nil
ORD.update(st, w, inf, 200)
check("a cleared command goal is re-asserted while the order stands",
      st.command_goal ~= nil and st.command_goal.kind == "goto_tile"
      and st.command_goal.mx == 15,
      tostring(st.command_goal and st.command_goal.kind))

-- AND THE END OF THE ORDER LETS GO.  The 60 s focus runs out, the slot
-- empties, and the command goal goes with it.
ORD.update(st, w, inf, 200 + (C.ORDER_FOCUS_TICKS or 3000))
check("the order lapses",       st.orders.held == nil, "held")
check("and the command goal is cleared with it",
      st.command_goal == nil, tostring(st.command_goal and st.command_goal.kind))

-- =========================================================================
-- THE HOLD IS TEN SECONDS AFTER ARRIVAL (Andrew, Sep 15), not the rest of the
-- 60 s focus.  The focus is the TRAVEL budget; arriving swaps the clock.
-- =========================================================================
print("orders.lua -- a go-there order holds ten seconds after it arrives")

st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
check("travel: the slot is not holding yet",
      st.orders.held ~= nil and st.orders.held.hold ~= true,
      tostring(st.orders.held and st.orders.held.hold))
check("travel: the hard lock is on", st.command_goal ~= nil, "nil")
check("travel: the clock is still the 60 s focus",
      st.orders.held.expiry >= 101 + (C.ORDER_FOCUS_TICKS or 3000) - 10,
      tostring(st.orders.held.expiry))
check("travel: the panel says hard",
      (function() st.tick = 101
         return ORD.panel_line(st, inf):sub(-4) == "hard" end)(),
      ORD.panel_line(st, inf))

-- The solo ack waits out the claim window (ORDER_CLAIM_TIEBREAK).
ORD.update(st, w, inf, 111)
-- ARRIVAL.  The tank is now on the square; the next think is the first one
-- that sees it.
inf.tankx, inf.tanky = 15 * 256 + 128, 15 * 256 + 128
st.orders.say = {}
ORD.update(st, w, inf, 300)
check("arrival sets the hold flag", st.orders.held.hold == true,
      tostring(st.orders.held.hold))
check("arrival says the number, once",
      #st.orders.say == 1 and st.orders.say[1] == "holding 10s",
      tostring(st.orders.say[1]))
check("arrival sets the clock to the hold knob",
      st.orders.held.expiry == 300 + (C.ORDER_GOTO_HOLD_TICKS or 500),
      tostring(st.orders.held.expiry))
check("arrival drops the hard lock so the pools can fight",
      st.command_goal == nil,
      tostring(st.command_goal and st.command_goal.kind))
check("the panel says holding",
      (function() st.tick = 300
         return ORD.panel_line(st, inf):sub(-7) == "holding" end)(),
      ORD.panel_line(st, inf))

-- The hold itself: quiet, no lock, and the order still stands.
st.orders.say = {}
ORD.update(st, w, inf, 400)
check("the hold says nothing more", #st.orders.say == 0,
      tostring(st.orders.say[1]))
check("the lock stays off through the hold", st.command_goal == nil,
      tostring(st.command_goal and st.command_goal.kind))
check("the order still stands inside the ten seconds",
      st.orders.held ~= nil and st.orders.held.hold == true, "?")

ORD.update(st, w, inf, 300 + (C.ORDER_GOTO_HOLD_TICKS or 500))
check("the hold runs out and the order ends", st.orders.held == nil, "held")
check("and it ends in silence", #st.orders.say == 0,
      tostring(st.orders.say[1]))

-- AN ORDER THAT NEVER ARRIVES still lapses on the 60 s focus, as before, and
-- that one DOES say so.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
ORD.update(st, w, inf, 111)
st.orders.say = {}
ORD.update(st, w, inf, 101 + (C.ORDER_FOCUS_TICKS or 3000))
check("a go-there that never arrives lapses on the focus",
      st.orders.held == nil, "held")
check("and that one says so", st.orders.say[1] == "order lapsed",
      tostring(st.orders.say[1]))

-- =========================================================================
-- A DEFEND ORDER ENDS WITH THE PILL.  Andrew watched a bot ordered to defend
-- a pill stay in defend_pill after the pill was shot flat and taken, and then
-- do nothing: the order was still live, so every other strategic row was
-- still being rejected for it.
-- =========================================================================
print("orders.lua -- a defend order ends when the pill is lost")

st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates defend 9", 100, true, false)
-- The solo ack waits out the claim window (ORDER_CLAIM_TIEBREAK).
ORD.update(st, w, inf, 111)
check("setup: a defend order is held",
      (st.orders.held or {}).kind == "defend_pill"
      and st.orders.held.tid == 9,
      tostring((st.orders.held or {}).kind))
st.orders.say = {}
w.pills[9].owner = "hostile"          -- the enemy took it
ORD.update(st, w, inf, 200)
check("the enemy taking the pill ends the order",
      st.orders.held == nil, "held")
check("and the bot says which pill it lost",
      st.orders.say[1] == "lost pill #9", tostring(st.orders.say[1]))

-- The same when it is only shot flat: a dead pill of ours is not a pill to
-- defend either, and the bot's own goals decide whether to go and sweep it.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates defend 9", 100, true, false)
ORD.update(st, w, inf, 111)
st.orders.say = {}
w.pills[9].health = 0
ORD.update(st, w, inf, 200)
check("a pill shot flat ends the defend order too",
      st.orders.held == nil, "held")
check("and it says so once",
      #st.orders.say == 1 and st.orders.say[1] == "lost pill #9",
      tostring(st.orders.say[1]))
check("nothing is left holding the order slot",
      st._order == nil, tostring(st._order))

-- A CANCEL does the same, on the tick it is said.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
check("setup: the lock is on", st.command_goal ~= nil, "nil")
ORD.on_chat(st, w, inf, 0, "cancel", 150, true, false)
check("cancel drops the order",   st.orders.held == nil, "held")
check("cancel drops the lock too", st.command_goal == nil,
      tostring(st.command_goal and st.command_goal.kind))

-- A RETREAT is NOT a place order: it stays a take_cover in the goal pools,
-- with no command goal, so the bot keeps defending itself.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "socrates retreat", 100, true, false)
check("a retreat is still take_cover",
      (st.orders.held or {}).kind == "take_cover",
      tostring((st.orders.held or {}).kind))
check("and a retreat sets no command goal", st.command_goal == nil,
      tostring(st.command_goal and st.command_goal.kind))

-- THE PANEL LINE.  While the lock is on the goal pools are never evaluated,
-- so the panel has to say why it is empty.
st, w, inf = ST(), W(), I({ allies = 0 })
-- (14,13) is inside ORDER_NEARBY_TILES of this bot's tank at (10,10); a
-- square further off than that is nobody's order and the slot stays empty.
ORD.on_chat(st, w, inf, 0, "!goto 14 13", 100, true, false)
ORD.update(st, w, inf, 101)
st.tick = 600
check("the panel line names the square, the sender and the lock",
      ORD.panel_line(st, inf)
        == " ORDER goto (14,13) from Andrew, 50 s left, hard",
      ORD.panel_line(st, inf))

print("orders.lua — the pings a bot places itself")

-- The queue and the drain. One ping leaves per think, because that is all the
-- think output carries, and the tile becomes the WORLD centre of the square.
st = ST()
ORD.ping(st, 3, 20, 30)
local out = ORD.out_ping(st, {})
check("a queued ping reaches the think output",
      out.ping_kind == 3 and out.ping_x == 20 * 256 + 128
      and out.ping_y == 30 * 256 + 128,
      string.format("%s %s %s", tostring(out.ping_kind), tostring(out.ping_x),
                    tostring(out.ping_y)))
check("the queue is emptied by the drain",
      ORD.out_ping(st, {}).ping_kind == nil, "?")
check("no ping queued means no ping fields",
      ORD.out_ping(ST(), {}).ping_kind == nil, "?")
check("a ping off the map is dropped", (function()
  local t = ST()
  ORD.ping(t, 3, 300, 30)
  ORD.ping(t, 3, 20, -1)
  return ORD.out_ping(t, {}).ping_kind == nil
end)(), "?")
check("only the two freshest pings are kept", (function()
  local t = ST()
  ORD.ping(t, 3, 1, 1); ORD.ping(t, 3, 2, 2); ORD.ping(t, 3, 3, 3)
  return ORD.out_ping(t, {}).ping_x == 2 * 256 + 128
end)(), "?")

-- TAKING AN ORDER places an ON MY WAY marker on the target, whatever the
-- "bot pings" setting says: the marker answers a person who just gave an
-- order. "!goto 15 15" is the plainest order with a place in it.
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
ORD.update(st, w, inf, 111)
check("setup: a go-there order is held",
      (st.orders.held or {}).kind == "goto_tile",
      tostring((st.orders.held or {}).kind))
out = ORD.out_ping(st, {})
check("taking an order places an ON MY WAY marker on the target",
      out.ping_kind == 4 and out.ping_x == 15 * 256 + 128
      and out.ping_y == 15 * 256 + 128,
      string.format("%s %s %s", tostring(out.ping_kind), tostring(out.ping_x),
                    tostring(out.ping_y)))

-- ATTACK markers ride the team setting, and it is OFF until somebody says
-- otherwise.
st = ST()
check("bot pings start at the knob",
      ORD.bot_pings_on(st) == (C.BOT_PINGS_DEFAULT and true or false), "?")
ORD.attack_ping(st, { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }, 100)
check("with bot pings off no attack marker is placed",
      ORD.out_ping(st, {}).ping_kind == nil, "?")

-- With the setting on: one marker per target, and not again until
-- ORDER_PING_REPEAT_TICKS has passed.
st = ST()
ORD.ping(st, 3, 1, 1)                 -- makes state.orders exist
ORD.out_ping(st, {})
st.orders.bot_pings = true
ORD.attack_ping(st, { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }, 100)
out = ORD.out_ping(st, {})
check("with bot pings on an attack marker goes on the target",
      out.ping_kind == 3 and out.ping_x == 20 * 256 + 128, tostring(out.ping_kind))
ORD.attack_ping(st, { kind = "attack_pill", target_id = 5, mx = 20, my = 20 },
                100 + (C.ORDER_PING_REPEAT_TICKS or 1500) - 1)
check("the same target is not marked again inside the gap",
      ORD.out_ping(st, {}).ping_kind == nil, "?")
ORD.attack_ping(st, { kind = "attack_pill", target_id = 5, mx = 20, my = 20 },
                100 + (C.ORDER_PING_REPEAT_TICKS or 1500))
check("the same target is marked again once the gap is served",
      ORD.out_ping(st, {}).ping_kind == 3, "?")
ORD.attack_ping(st, { kind = "attack_tank", target_id = 9, mx = 60, my = 60 },
                100 + (C.ORDER_PING_REPEAT_TICKS or 1500))
check("a different target is marked straight away",
      ORD.out_ping(st, {}).ping_x == 60 * 256 + 128, "?")
ORD.attack_ping(st, { kind = "capture_pill", target_id = 7, mx = 40, my = 40 }, 9000)
check("a goal that is not an attack is never marked",
      ORD.out_ping(st, {}).ping_kind == nil, "?")

-- THE LATCH. A late joiner catches the team's setting off its own /info verb.
st = ST()
ORD.rx(2, "/info obg 1", 100, st)
ORD.update(st, W(), I(), 101)
check("the obg latch turns bot pings on",
      ORD.bot_pings_on(st) == true, tostring(ORD.bot_pings_on(st)))
ORD.rx(2, "/info obg 0", 200, st)
ORD.update(st, W(), I(), 201)
check("the obg latch turns them off again",
      ORD.bot_pings_on(st) == false, tostring(ORD.bot_pings_on(st)))

-- A BOT'S OWN MARKER IS NEWS, NOT AN ORDER. p2 is a bot in this fixture
-- (player_bots 0x16), so its BOT_COMMAND ping must start nothing at all.
_G.EVENT_PING = 20
_G.PING_KIND_BOT_COMMAND = 5
_G.PING_KIND_CAUTION = 1
st, w, inf = ST(), W(), I({ events = {
  { type = 20, data = { 2, 5, 0, 20, 0, 20 } },   -- p2, a BOT, on the pill
} })
ORD.on_events(st, w, inf, 100)
check("a bot's BOT_COMMAND marker starts no order",
      st.orders == nil or next(st.orders.auctions or {}) == nil, "?")
st, w, inf = ST(), W(), I({ player_bots = 0, events = {
  { type = 20, data = { 2, 5, 0, 20, 0, 20 } },   -- the same, from a PERSON
} })
ORD.on_events(st, w, inf, 100)
check("the same marker from a person still starts one",
      st.orders ~= nil and next(st.orders.auctions or {}) ~= nil, "?")
_G.EVENT_PING = nil
_G.PING_KIND_BOT_COMMAND = nil
_G.PING_KIND_CAUTION = nil

-- =========================================================================
-- THE PEER REVIEW, Sep 16.  Eight findings on the Lua side; each block below
-- is one of them, and each one failed before the fix beside it.
-- =========================================================================

-- ── 1. THE OLD OPERATOR COMMANDS ARE READ FIRST ─────────────────────────
-- "!start" and "!attack:5" never reached commands.lua: the order parser got
-- there first, and its bare-name SELECT branch and its verb scan both claimed
-- a line that was never theirs.  TARS is the name that made it visible --
-- "start" is two letters from "tars", so a team with TARS on it turned
-- "!start" into a selection of TARS.
print("orders.lua -- the old operator commands are read BEFORE any order")

local TARS_ROSTER = {
  { pn = 1, name = "TARS" },
  { pn = 2, name = "Socrates" },
}
local TARS_ALL = {
  { pn = 0, name = "Andrew", ally = true },
  { pn = 1, name = "TARS", ally = true },
  { pn = 2, name = "Socrates", ally = true },
}
local function PT(text) return ORD.parse(text, TARS_ROSTER, TARS_ALL) end

check("!start is not a selection of TARS", PT("!start") == nil, shape(PT("!start")))
check("!start is still the operator command",
      (CMDS.parse("!start") or {}).cmd == "start", "?")
check("and the bare name still selects TARS",
      shape(PT("tars")) == "select:1", shape(PT("tars")))
check("!attack:5 is not an order on pill 5", PT("!attack:5") == nil,
      shape(PT("!attack:5")))
check("!attack:5 is still the operator command",
      (function()
         local c = CMDS.parse("!attack:5")
         return c and c.cmd == "attack" and c.id == 5
       end)(), "?")
check("!cp:5 is still handed through",  PT("!cp:5") == nil,  shape(PT("!cp:5")))
check("!pill:2 is still handed through", PT("!pill:2") == nil, shape(PT("!pill:2")))
check("!cb:all is still handed through", PT("!cb:all") == nil, shape(PT("!cb:all")))
-- And the ORDER that only looks like one of them is untouched: the space is
-- the whole difference between "!attack:5" and "!attack 5".
check("!attack 5 is still an order",
      shape(PT("!attack 5")) == "attack who=auto tgt=pill:5", shape(PT("!attack 5")))
check("!goto is not an operator command",
      shape(PT("!goto 100 99")) == "goto who=ping tgt=here:" .. (100 * 256 + 99),
      shape(PT("!goto 100 99")))

-- At runtime: on_chat must say "not mine" so init.lua hands the line on.
st, w, inf = ST(), W(), I({ allies = 0x17 })
check("on_chat leaves !start to commands.lua",
      ORD.on_chat(st, w, inf, 0, "!start", 100, true, false) == false, "took it")
check("on_chat leaves !attack:5 to commands.lua",
      ORD.on_chat(st, w, inf, 0, "!attack:5", 100, true, false) == false, "took it")
check("and neither one started an order",
      st.orders == nil or (st.orders.held == nil
                           and next(st.orders.auctions) == nil), "?")

-- ── 4. AN OPERATOR COMMAND IS ALLY-ONLY ─────────────────────────────────
-- init.lua called cmds.parse from OUTSIDE the ally test that guards every
-- other reader on that path, so "!stop" -- the hardest thing anybody can say
-- to a bot -- was the one line the other team could say too.  The check is
-- in the parser now, beside the parse it guards.
print("commands.lua -- only an ally may give an operator command")

check("an ally's !stop is a command",
      (CMDS.parse("!stop", true) or {}).cmd == "stop", "?")
check("an ENEMY's !stop is nothing at all",
      CMDS.parse("!stop", false) == nil, "a command")
check("an enemy's !start is nothing either",
      CMDS.parse("!start", false) == nil, "a command")
check("nor !cp:5, !cb:all or !pill:2", (function()
  for _, line in ipairs({ "!cp:5", "!cb:all", "!pill:2", "!attack:5",
                          "!base:1", "!bpc:4", "!pp:6", "!watch:7",
                          "!status" }) do
    if CMDS.parse(line, false) ~= nil then return false end
    if CMDS.parse(line, true) == nil then return false end
  end
  return true
end)(), "?")
-- nil means "the caller has already checked", which is how orders.lua asks
-- whether a line is one of these at all.
check("no third argument means the caller checked",
      (CMDS.parse("!stop") or {}).cmd == "stop", "?")

-- ── 2 + 6. CANCEL STICKS, AND A RELEASE STILL RE-BIDS ───────────────────
-- A two-bot rig.  Bot A (p1, Socrates) sits at (10,10) and bot B (p2,
-- Seneca) at (50,50); pill 5 is at (20,20), so A always wins the auction.
-- The order verbs are carried between them by hand, the way the message bus
-- would.  The question every case asks is the same one: after the order is
-- called off on A, does B pick it up?
print("orders.lua -- a cancel is a cancel, a release is a hand-back")

local function BOT(pn, mx, my)
  local s = { player_number = pn, tick = 100, goal = {}, stuck_for = 0 }
  local i = I({ allies = 0x17 })
  i.player_number = pn
  i.tankx, i.tanky = mx * 256 + 128, my * 256 + 128
  return { pn = pn, st = s, inf = i, w = W() }
end

-- One tick of the message bus: everything each bot queued reaches the other.
local function pump(a, b, tick)
  local am, bm = {}, {}
  for i, v in ipairs((a.st.orders or {}).out or {}) do am[i] = v end
  for i, v in ipairs((b.st.orders or {}).out or {}) do bm[i] = v end
  if a.st.orders then a.st.orders.out = {} end
  if b.st.orders then b.st.orders.out = {} end
  for _, m in ipairs(am) do ORD.rx(a.pn, m, tick, b.st) end
  for _, m in ipairs(bm) do ORD.rx(b.pn, m, tick, a.st) end
end

-- Give both bots the same order and let it settle on A.  Answers A, B.
local function ordered_pair()
  local a, b = BOT(1, 10, 10), BOT(2, 50, 50)
  ORD.on_chat(a.st, a.w, a.inf, 0, "attack 5", 100, true, false)
  ORD.on_chat(b.st, b.w, b.inf, 0, "attack 5", 100, true, false)
  pump(a, b, 100)                              -- the bids cross
  ORD.update(a.st, a.w, a.inf, 101)            -- A wins and claims
  ORD.update(b.st, b.w, b.inf, 101)
  pump(a, b, 101)                              -- A's claim reaches B
  ORD.update(a.st, a.w, a.inf, 102)
  ORD.update(b.st, b.w, b.inf, 102)
  return a, b
end

-- Did B pick the cancelled order up?  Two ways to see it from outside: a
-- fresh bid on that id, or the id still sitting in B's known table where the
-- next release, steal or repeat can revive it.
local function b_rebid(a, b, oid, t)
  b.st.orders.out = {}
  pump(a, b, t)
  ORD.update(b.st, b.w, b.inf, t + 1)
  ORD.update(b.st, b.w, b.inf, t + 2)
  for _, line in ipairs(b.st.orders.out) do
    if line:match("^/info obd " .. oid .. " %d") then return true end
  end
  return b.st.orders.held ~= nil or b.st.orders.known[oid] ~= nil
end

local pa, pb = ordered_pair()
check("setup: A holds the order and B knows who has it",
      pa.st.orders.held ~= nil and pa.st.orders.held.tid == 5
      and pb.st.orders.held == nil
      and pb.st.orders.claims[pa.st.orders.held.oid] ~= nil,
      tostring(pa.st.orders.held and pa.st.orders.held.tid))
local cancel_oid = pa.st.orders.held.oid
check("setup: B still remembers the order", pb.st.orders.known[cancel_oid] ~= nil, "?")

-- (a) plain `cancel`
pa, pb = ordered_pair()
local plain_oid = pa.st.orders.held.oid
ORD.on_chat(pa.st, pa.w, pa.inf, 0, "cancel", 200, true, false)
ORD.on_chat(pb.st, pb.w, pb.inf, 0, "cancel", 200, true, false)
check("plain cancel: A lets go", pa.st.orders.held == nil, "held")
check("plain cancel: B does not pick it up",
      b_rebid(pa, pb, plain_oid, 200) == false, "re-bid")

-- (b) `cancel <bot>`
pa, pb = ordered_pair()
local named_oid = pa.st.orders.held.oid
ORD.on_chat(pa.st, pa.w, pa.inf, 0, "cancel socrates", 200, true, false)
ORD.on_chat(pb.st, pb.w, pb.inf, 0, "cancel socrates", 200, true, false)
check("cancel <bot>: A lets go", pa.st.orders.held == nil, "held")
check("cancel <bot>: B does not pick it up",
      b_rebid(pa, pb, named_oid, 200) == false, "re-bid")

-- (c) `last cancel`
pa, pb = ordered_pair()
local last_oid = pa.st.orders.held.oid
ORD.on_chat(pa.st, pa.w, pa.inf, 0, "last cancel", 200, true, false)
ORD.on_chat(pb.st, pb.w, pb.inf, 0, "last cancel", 200, true, false)
check("last cancel: A lets go", pa.st.orders.held == nil, "held")
check("last cancel: B does not pick it up",
      b_rebid(pa, pb, last_oid, 200) == false, "re-bid")

-- (d) `retreat`: the old order is cancelled, not handed round the team.
pa, pb = ordered_pair()
ORD.on_chat(pa.st, pa.w, pa.inf, 0, "socrates retreat", 200, true, false)
ORD.on_chat(pb.st, pb.w, pb.inf, 0, "socrates retreat", 200, true, false)
check("retreat: A is on take_cover now",
      (pa.st.orders.held or {}).kind == "take_cover",
      tostring((pa.st.orders.held or {}).kind))
check("retreat: B does not pick the old order up",
      (function()
         pump(pa, pb, 200)
         ORD.update(pb.st, pb.w, pb.inf, 201)
         return pb.st.orders.auctions[cancel_oid] == nil
                and pb.st.orders.known[cancel_oid] == nil
       end)(), "re-bid")

-- (e) a CAUTION ping on the holding bot
_G.EVENT_PING = 13
_G.PING_KIND_CAUTION = 1
_G.PING_KIND_BOT_COMMAND = 5
local function caution(sender, mx, my)
  local wx, wy = mx * 256 + 128, my * 256 + 128
  return { type = 13, data = { sender, 1,
           math.floor(wx / 256), wx % 256, math.floor(wy / 256), wy % 256 } }
end
pa, pb = ordered_pair()
local caut_oid = pa.st.orders.held.oid
pa.inf.events = { caution(0, 10, 10) }         -- on A's own tank
pb.inf.events = { caution(0, 10, 10) }
ORD.on_events(pa.st, pa.w, pa.inf, 200)
ORD.on_events(pb.st, pb.w, pb.inf, 200)
pa.inf.events, pb.inf.events = {}, {}
check("caution on a bot: A lets go", pa.st.orders.held == nil, "held")
check("caution on a bot: B does not pick it up",
      b_rebid(pa, pb, caut_oid, 200) == false, "re-bid")
_G.EVENT_PING = nil
_G.PING_KIND_CAUTION = nil
_G.PING_KIND_BOT_COMMAND = nil

-- (f) A GO-THERE ORDER THAT FINISHED ITS HOLD.  The job is done, so no
-- second bot is sent to stand on the square after the first walked away.
local ga, gb = BOT(1, 14, 14), BOT(2, 50, 50)
ORD.on_chat(ga.st, ga.w, ga.inf, 0, "!goto 15 15", 100, true, false)
ORD.on_chat(gb.st, gb.w, gb.inf, 0, "!goto 15 15", 100, true, false)
pump(ga, gb, 100)
ORD.update(ga.st, ga.w, ga.inf, 101)
ORD.update(gb.st, gb.w, gb.inf, 101)
pump(ga, gb, 101)
ORD.update(gb.st, gb.w, gb.inf, 102)
check("setup: A holds the go-there order",
      (ga.st.orders.held or {}).kind == "goto_tile",
      tostring((ga.st.orders.held or {}).kind))
local goto_oid = ga.st.orders.held.oid
ga.inf.tankx, ga.inf.tanky = 15 * 256 + 128, 15 * 256 + 128
ORD.update(ga.st, ga.w, ga.inf, 200)           -- arrival: the hold starts
check("setup: it arrived and is holding", ga.st.orders.held.hold == true,
      tostring(ga.st.orders.held and ga.st.orders.held.hold))
ORD.update(ga.st, ga.w, ga.inf, 200 + (C.ORDER_GOTO_HOLD_TICKS or 500))
check("the hold runs out and A lets go", ga.st.orders.held == nil, "held")
pump(ga, gb, 800)
ORD.update(gb.st, gb.w, gb.inf, 801)
check("a finished hold sends nobody else to the square",
      gb.st.orders.auctions[goto_oid] == nil
      and gb.st.orders.known[goto_oid] == nil
      and gb.st.orders.held == nil, "a second bot was sent")

-- A RELEASE IS STILL A HAND-BACK.  The other half of the same rule: a bot
-- that got busy or died drops the order with obr, and the next cheapest bot
-- has to go and do it.  (keel: ORDER_NO_HAND_BACK off.)
C.ORDER_NO_HAND_BACK = false
pa, pb = ordered_pair()
local rel_oid = pa.st.orders.held.oid
ORD.release_held(pa.st, pa.inf, nil, true)     -- no `cancelled`: a hand-back
check("a hand-back puts obr on the wire",
      pa.st.orders.out[#pa.st.orders.out] == "/info obr " .. rel_oid,
      tostring(pa.st.orders.out[#pa.st.orders.out]))
pb.st.orders.out = {}
pump(pa, pb, 300)
ORD.update(pb.st, pb.w, pb.inf, 301)
check("and B bids for it", (function()
  for _, line in ipairs(pb.st.orders.out) do
    if line:match("^/info obd " .. rel_oid .. " %d") then return true end
  end
  return false
end)(), table.concat(pb.st.orders.out, " | "))
check("and B ends up holding it",
      (pb.st.orders.held or {}).oid == rel_oid,
      tostring((pb.st.orders.held or {}).oid))
C.ORDER_NO_HAND_BACK = true
-- "DON'T PASS THE ORDER BACK" (ORDER_NO_HAND_BACK, the default): the obr
-- still goes out, but nobody re-bids and nobody else is sent.
pa, pb = ordered_pair()
rel_oid = pa.st.orders.held.oid
ORD.release_held(pa.st, pa.inf, nil, true)
check("no hand-back: obr is still on the wire",
      pa.st.orders.out[#pa.st.orders.out] == "/info obr " .. rel_oid,
      tostring(pa.st.orders.out[#pa.st.orders.out]))
pb.st.orders.out = {}
pump(pa, pb, 300)
ORD.update(pb.st, pb.w, pb.inf, 301)
ORD.update(pb.st, pb.w, pb.inf, 320)
check("no hand-back: B does not bid", #pb.st.orders.out == 0
      and next(pb.st.orders.auctions) == nil,
      table.concat(pb.st.orders.out, " | "))
check("no hand-back: B does not take it", pb.st.orders.held == nil,
      tostring((pb.st.orders.held or {}).oid))
check("keel hands orders back", C.PRESETS.keel.ORDER_NO_HAND_BACK == false, "?")

-- 6. gclaims IS CLEARED, so a bot that let an order go can win it back.
-- It never was, and the settle excludes anybody gclaims still lists as a
-- holder -- so the SAME order said again found "everybody already holds it"
-- and nobody went.
print("orders.lua -- a released bot can win its own order back")
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "attack 5", 100, true, false)
ORD.update(st, w, inf, 101)
local back_oid = st.orders.held.oid
check("setup: the order is held and gclaims names us",
      st.orders.gclaims[back_oid] ~= nil and st.orders.gclaims[back_oid][1] ~= nil,
      "?")
ORD.release_held(st, inf, nil, true)           -- busy: hand it back
check("letting go takes us out of gclaims",
      (st.orders.gclaims[back_oid] or {})[1] == nil,
      tostring((st.orders.gclaims[back_oid] or {})[1]))
ORD.on_chat(st, w, inf, 0, "attack 5", 200, true, false)
check("the same order again opens an auction we are in",
      st.orders.auctions[back_oid] ~= nil
      and type(st.orders.auctions[back_oid].bids[1]) == "number",
      "no bid")
ORD.update(st, w, inf, 201)
check("and the released bot wins it back",
      (st.orders.held or {}).oid == back_oid,
      tostring((st.orders.held or {}).oid))

-- PING AGAIN STILL ADDS THE NEXT CLOSEST BOT.  A holds the pinged order; the
-- second ping grows it to two and B fills the new slot.
_G.EVENT_PING = 13
_G.PING_KIND_BOT_COMMAND = 5
_G.PING_KIND_CAUTION = 1
local function bping(sender, mx, my)
  local wx, wy = mx * 256 + 128, my * 256 + 128
  return { type = 13, data = { sender, 5,
           math.floor(wx / 256), wx % 256, math.floor(wy / 256), wy % 256 } }
end
local qa, qb = BOT(1, 10, 10), BOT(2, 50, 50)
qa.inf.events, qb.inf.events = { bping(0, 20, 20) }, { bping(0, 20, 20) }
ORD.on_events(qa.st, qa.w, qa.inf, 100)
ORD.on_events(qb.st, qb.w, qb.inf, 100)
qa.inf.events, qb.inf.events = {}, {}
pump(qa, qb, 100)
ORD.update(qa.st, qa.w, qa.inf, 101)
ORD.update(qb.st, qb.w, qb.inf, 101)
pump(qa, qb, 101)
ORD.update(qb.st, qb.w, qb.inf, 102)
check("setup: the first ping puts A on it",
      qa.st.orders.held ~= nil and qb.st.orders.held == nil,
      tostring(qa.st.orders.held and qa.st.orders.held.tid))
qa.inf.events, qb.inf.events = { bping(0, 21, 20) }, { bping(0, 21, 20) }
ORD.on_events(qa.st, qa.w, qa.inf, 150)
ORD.on_events(qb.st, qb.w, qb.inf, 150)
qa.inf.events, qb.inf.events = {}, {}
pump(qa, qb, 150)
ORD.update(qa.st, qa.w, qa.inf, 151)
ORD.update(qb.st, qb.w, qb.inf, 151)
check("ping again adds the next closest bot",
      qb.st.orders.held ~= nil and qb.st.orders.held.oid == qa.st.orders.held.oid,
      tostring(qb.st.orders.held and qb.st.orders.held.oid))
-- The first holder keeps it when the added bot's claim arrives: a repeat
-- ping makes a group, and a fellow taker's claim is not a rival's.
pump(qa, qb, 151)
ORD.update(qa.st, qa.w, qa.inf, 152)
ORD.update(qb.st, qb.w, qb.inf, 152)
check("and the first holder keeps it once the new claim arrives",
      qa.st.orders.held ~= nil and qb.st.orders.held ~= nil
      and qa.st.orders.held.oid == qb.st.orders.held.oid,
      tostring(qa.st.orders.held and qa.st.orders.held.oid) .. " "
      .. tostring(qb.st.orders.held and qb.st.orders.held.oid))
-- The same when the added bot is CHEAPER than the holder was: still a group.
do
  local ha, hb = BOT(1, 10, 10), BOT(2, 50, 50)
  ha.inf.events, hb.inf.events = { bping(0, 20, 20) }, { bping(0, 20, 20) }
  ORD.on_events(ha.st, ha.w, ha.inf, 100)
  ORD.on_events(hb.st, hb.w, hb.inf, 100)
  ha.inf.events, hb.inf.events = {}, {}
  pump(ha, hb, 100)
  ORD.update(ha.st, ha.w, ha.inf, 101)
  ORD.update(hb.st, hb.w, hb.inf, 101)
  pump(ha, hb, 101)
  ORD.update(hb.st, hb.w, hb.inf, 102)
  hb.inf.tankx, hb.inf.tanky = 23 * 256 + 128, 23 * 256 + 128
  ha.inf.events, hb.inf.events = { bping(0, 21, 20) }, { bping(0, 21, 20) }
  ORD.on_events(ha.st, ha.w, ha.inf, 150)
  ORD.on_events(hb.st, hb.w, hb.inf, 150)
  ha.inf.events, hb.inf.events = {}, {}
  pump(ha, hb, 150)
  ORD.update(ha.st, ha.w, ha.inf, 151)
  ORD.update(hb.st, hb.w, hb.inf, 151)
  pump(ha, hb, 151)
  ORD.update(ha.st, ha.w, ha.inf, 152)
  ORD.update(hb.st, hb.w, hb.inf, 152)
  check("a cheaper added bot does not knock the first holder off",
        ha.st.orders.held ~= nil and hb.st.orders.held ~= nil
        and ha.st.orders.held.oid == hb.st.orders.held.oid,
        tostring(ha.st.orders.held and ha.st.orders.held.oid) .. " "
        .. tostring(hb.st.orders.held and hb.st.orders.held.oid))
end
_G.EVENT_PING = nil
_G.PING_KIND_BOT_COMMAND = nil
_G.PING_KIND_CAUTION = nil

-- ── 3. THE ORDER TABLES ARE WALKED IN oid ORDER ─────────────────────────
-- pairs() over a hash is process-seeded, so two orders settling on one tick
-- settled in a different order in two runs of one recorded game.  Every loop
-- takes the keys, sorts them and walks the array.
print("orders.lua -- two orders settling in one tick settle in oid order")

check("sorted_keys sorts", (function()
  local k = ORD.sorted_keys({ [9000] = 1, [11] = 1, [700] = 1 })
  return #k == 3 and k[1] == 11 and k[2] == 700 and k[3] == 9000
end)(), "?")
check("sorted_keys of nothing is empty",
      #ORD.sorted_keys(nil) == 0 and #ORD.sorted_keys({}) == 0, "?")

-- Two live auctions this bot is the only bidder in.  The obc lines it puts on
-- the wire as each settles are the settle order, read from outside.
local function settle_oids(first_low)
  local s, ww, ii = ST(), W(), I({ allies = 0 })
  ORD.ping(s, 3, 1, 1)                       -- makes state.orders exist
  s.orders.pings = {}
  local lo = { oid = 111111, verb = "attack", kind = "attack_pill",
               tkind = "pill", tid = 5, sender = 0, sender_name = "Andrew",
               needs_shells = true }
  local hi = { oid = 222222, verb = "defend", kind = "defend_pill",
               tkind = "pill", tid = 9, sender = 0, sender_name = "Andrew",
               needs_shells = true }
  local a1, a2 = lo, hi
  if not first_low then a1, a2 = hi, lo end   -- the other insertion order
  for _, sp in ipairs({ a1, a2 }) do
    s.orders.known[sp.oid]    = { spec = sp, tick = 100 }
    s.orders.auctions[sp.oid] = { spec = sp, open = 0, want = 1,
                                  bids = { [1] = 10 }, answered = { [1] = true } }
  end
  ORD.update(s, ww, ii, 200)
  local seen = {}
  -- The first settle takes its order (obc).  The second one finds the bot
  -- on that job (ORDER_HOLDER_KEEPS_JOB) and offers it back (obo); with the
  -- knob off it takes that one too.  Either line marks a settle.
  for _, line in ipairs(s.orders.out) do
    local id = line:match("^/info obc (%d+) ") or line:match("^/info obo (%d+)$")
    if id then seen[#seen + 1] = tonumber(id) end
  end
  return table.concat(seen, ",")
end
check("the lower oid settles first, whatever order the table was built in",
      settle_oids(true) == "111111,222222", settle_oids(true))
check("and the other insertion order settles exactly the same way",
      settle_oids(false) == settle_oids(true), settle_oids(false))

-- ── 5. A BOT THAT DIES LETS THE ORDER GO ────────────────────────────────
-- init.lua's dead-tick block returns before ORD.update runs, so nothing
-- cleared the slot: a bot killed mid-hold came back with hold still set and
-- stood parked on its RESPAWN square for the rest of it.
print("orders.lua -- a death hands the order back, and the park needs the tile")

st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
inf.tankx, inf.tanky = 15 * 256 + 128, 15 * 256 + 128
ORD.update(st, w, inf, 200)
check("setup: the order is in its hold", st.orders.held.hold == true, "?")
local death_oid = st.orders.held.oid
st.orders.out = {}
check("on_death lets the order go", ORD.on_death(st, inf) == true, "nothing held")
check("the slot is empty after a death", st.orders.held == nil and st._order == nil, "held")
check("and the hard lock went with it", st.command_goal == nil,
      tostring(st.command_goal and st.command_goal.kind))
check("a death is a HAND-BACK, so obr goes out",
      st.orders.out[1] == "/info obr " .. death_oid, tostring(st.orders.out[1]))
check("a death with no order does nothing", ORD.on_death(ST(), inf) == false, "?")

-- THE PARK.  It used to ask only whether the slot said "holding".
st, w, inf = ST(), W(), I({ allies = 0 })
ORD.on_chat(st, w, inf, 0, "!goto 15 15", 100, true, false)
ORD.update(st, w, inf, 101)
check("travel: not parked yet", ORD.hold_parked(st, inf) == false, "parked")
inf.tankx, inf.tanky = 15 * 256 + 128, 15 * 256 + 128
ORD.update(st, w, inf, 200)
check("standing on the square: parked", ORD.hold_parked(st, inf) == true, "not parked")
check("one square off is still parked",
      ORD.hold_parked(st, I({ tankx = 16 * 256 + 128,
                              tanky = 14 * 256 + 128 })) == true, "not parked")
check("respawned across the map: NOT parked",
      ORD.hold_parked(st, I({ tankx = 90 * 256 + 128,
                              tanky = 90 * 256 + 128 })) == false, "parked")
check("no order at all: not parked", ORD.hold_parked(ST(), inf) == false, "parked")

-- ── 7. ATTACK NEVER TAKES AN ALLY ───────────────────────────────────────
-- "attack socrates" built a real attack_tank order on a team-mate, and the
-- bot drove at it and idled there for ten seconds.
print("orders.lua -- attack on an ally is refused")

check("attack <ally bot> is refused",
      shape(P("attack socrates")) == "reply:Socrates is on our side",
      shape(P("attack socrates")))
check("attack <ally human> is refused too",
      shape(P("attack andrew")) == "reply:Andrew is on our side",
      shape(P("attack andrew")))
check("a prefix of an ally name is refused as well",
      shape(P("attack plat")) == "reply:Plato is on our side",
      shape(P("attack plat")))
check("all attack <ally> is refused",
      shape(P("all attack plato")) == "reply:Plato is on our side",
      shape(P("all attack plato")))
check("an ENEMY tank name is still an order",
      shape(P("attack hannibal")) == "attack who=auto tgt=tank:7",
      shape(P("attack hannibal")))
check("cancel <bot> is untouched: it is aimed at an ally on purpose",
      shape(P("cancel socrates")) == "cancel who=auto tgt=tank:1",
      shape(P("cancel socrates")))
-- At runtime the speaking bot says the line and nothing is ordered.
st, w, inf = ST(), W(), I({ allies = 0x17 })
ORD.on_chat(st, w, inf, 0, "attack seneca", 100, true, false)
check("runtime: the bot says so and starts nothing",
      st.orders.say[1] == "Seneca is on our side"
      and st.orders.held == nil and next(st.orders.auctions) == nil,
      tostring(st.orders.say[1]))

-- ── 8. ORDINARY CHAT IS NEVER ANSWERED "didn't understand" ──────────────
-- The gate matched the first word with the full three-tier matcher, and its
-- edit-distance tier turned plain talk into an order shape.  With a bot
-- called Case on the team, "cause we should push" was answered.
print("orders.lua -- ordinary chat gets no answer")

local CASE_ROSTER = {
  { pn = 1, name = "Case" },
  { pn = 2, name = "Socrates" },
}
local CASE_ALL = {
  { pn = 0, name = "Andrew", ally = true },
  { pn = 1, name = "Case", ally = true },
  { pn = 2, name = "Socrates", ally = true },
}
local function PC(text) return ORD.parse(text, CASE_ROSTER, CASE_ALL) end

check("'cause we should push' is ordinary chat",
      PC("cause we should push") == nil, shape(PC("cause we should push")))
check("'we should take the middle' is ordinary chat",
      PC("we should take the middle") == nil, shape(PC("we should take the middle")))
check("'nice shot' is ordinary chat", PC("nice shot") == nil, shape(PC("nice shot")))
check("'cant believe that' is ordinary chat",
      PC("cant believe that") == nil, shape(PC("cant believe that")))
-- VERBS ARE EXACT.  A typo in the verb is not an order at all, so it is not
-- answered either -- this is the rule commands.txt now states.
check("a typo'd VERB is ordinary chat", PC("attak pill 5") == nil,
      shape(PC("attak pill 5")))
check("'sweeping the floor' is ordinary chat",
      PC("sweeping the floor") == nil, shape(PC("sweeping the floor")))
-- And everything that DOES address the bots still lands.
check("the exact name still selects", shape(PC("case")) == "select:1", shape(PC("case")))
check("a prefix of the name still selects", shape(PC("soc")) == "select:2", shape(PC("soc")))
check("the exact name with a verb is still an order",
      shape(PC("case attack 5")) == "attack who=names(1) tgt=pill:5",
      shape(PC("case attack 5")))
-- A TYPO'D NAME is still forgiven, but only inside a line that is already an
-- order: there has to be a verb, and every word in front of it has to be a
-- name or a who-word.
check("a typo'd name in front of a verb is still an order",
      shape(P("socrtes attack 5")) == "attack who=names(1) tgt=pill:5",
      shape(P("socrtes attack 5")))
check("a typo'd name with no verb behind it says nothing",
      P("socrtes") == nil, shape(P("socrtes")))
check("a typo'd name and a verb, with junk between, says nothing",
      PC("cause we should attack it") == nil, shape(PC("cause we should attack it")))
-- The "!" still forces a line through the gate, as it always did.
check("!wibble is still answered", shape(PC("!wibble")) == "reply:didn't understand",
      shape(PC("!wibble")))
check("a bot's own ack is still ignored",
      PC("Got it! attack_pill #5") == nil, shape(PC("Got it! attack_pill #5")))
-- At runtime: no reply queued at all.
st, w, inf = ST(), W(), I({ allies = 0x17 })
check("runtime: ordinary chat is not even taken",
      ORD.on_chat(st, w, inf, 0, "cause we should push", 100, true, false) == false,
      "took it")
check("runtime: and nothing is said", st.orders == nil, "?")

-- =========================================================================
-- "Bots <= 7 tiles from a human will suicide when attacking a pill (no wall
-- blockers built)" — the lobby docs' last line.
--
-- The whole of the new input is util.human_ally_near: how far the nearest
-- HUMAN team-mate is, or nil. Everything downstream of it is the line
-- attack.lua already ran for a designated pill_suicider — _is_ppt off, so no
-- shield scan, no gather_trees and no build_walls — so this is the part worth
-- pinning, and pinning it exactly.
--
-- IT CANNOT BE A ROOST ROUND. Every seat in a headless round is a bot: the
-- server stamps PLAYER_FLAG_BOT as it seats one and no scenario op clears it
-- (docs/SCENARIO_API.md, "Bots and seats"), so a round cannot put a human
-- five squares from anything. See tests/roost/README.md.
-- =========================================================================
print("util.lua — a human team-mate standing close")
local UTIL = require("util")
-- me = p1. p0 is a human ally, p2 an ally BOT, p7 an enemy: allies has bits
-- 0,1,2 and player_bots has bits 1,2, which is the shape init.lua reads.
local function HI(list)
  local i = { player_number = 1, allies = 0x07, player_bots = 0x06, objects = {} }
  for _, e in ipairs(list) do
    i.objects[#i.objects + 1] = { type = 1, idnum = e[1],
                                  x = e[2] * 256, y = e[3] * 256, info = e[4] or 0 }
  end
  return i
end
local function near(list, tiles)
  return UTIL.human_ally_near(HI(list), 100, 100, tiles)
end
check("a human ally 5 away is seen at 7",   near({ { 0, 105, 100 } }, 7) == 5,
      tostring(near({ { 0, 105, 100 } }, 7)))
check("a human ally exactly 7 away is seen", near({ { 0, 100, 93 } }, 7) == 7,
      tostring(near({ { 0, 100, 93 } }, 7)))
check("a human ally 8 away is not",          near({ { 0, 108, 100 } }, 7) == nil,
      tostring(near({ { 0, 108, 100 } }, 7)))
check("a human ally 12 away is not",         near({ { 0, 100, 112 } }, 7) == nil,
      tostring(near({ { 0, 100, 112 } }, 7)))
-- THE DISCRIMINATION THAT MATTERS. A bot team-mate is not a person watching,
-- so it must never trigger the rule -- two bots side by side keep building.
check("an ally BOT standing on top of us is not a human",
      near({ { 2, 101, 100 } }, 7) == nil, tostring(near({ { 2, 101, 100 } }, 7)))
check("an ENEMY tank close by is not a human ally",
      near({ { 7, 101, 100, 0x80 } }, 7) == nil, tostring(near({ { 7, 101, 100, 0x80 } }, 7)))
-- Chebyshev, the way a screen is measured: a diagonal at (5,5) is 5, not 10.
check("distance is Chebyshev", near({ { 0, 105, 105 } }, 7) == 5,
      tostring(near({ { 0, 105, 105 } }, 7)))
check("the NEAREST human is the answer",
      near({ { 0, 106, 100 } }, 7) == 6, tostring(near({ { 0, 106, 100 } }, 7)))
check("no objects at all is nil", near({}, 7) == nil, tostring(near({}, 7)))
-- 0 tiles is the knob's OFF value and must short-circuit, whatever is there.
check("0 tiles turns the whole question off",
      near({ { 0, 100, 100 } }, 0) == nil, tostring(near({ { 0, 100, 100 } }, 0)))
check("keel ships it off",
      C.PRESETS.keel.ORDER_HUMAN_NEAR_SUICIDE_TILES == 0,
      tostring(C.PRESETS.keel.ORDER_HUMAN_NEAR_SUICIDE_TILES))
check("the live value is the 7 the docs promise",
      C.ORDER_HUMAN_NEAR_SUICIDE_TILES == 7,
      tostring(C.ORDER_HUMAN_NEAR_SUICIDE_TILES))

-- =========================================================================
-- PING SUICIDE RUN.  A bot-command ping on an enemy pill, then an ATTACK
-- ping from the same person on the same pill inside
-- PING_SUICIDE_WINDOW_TICKS (50 thinks = 1 s), sends every bot on that pill
-- at it until the tank or the pill dies.  One bot, p1 at (10,10); pill 5 at
-- (20,20).  Think ticks: 50 a second.
-- =========================================================================
print("orders.lua -- ping suicide run")
_G.EVENT_PING = 13
_G.PING_KIND_CAUTION = 1
_G.PING_KIND_BOT_COMMAND = 5
_G.PING_KIND_ATTACK = 3
local GOALS  = require("goals")
local ATTACK = require("attack")

-- A lone bot takes a bot-command order on pill 5 at t=100.  The auction has
-- nobody else to hear from, so a few updates settle it.
local function sbot()
  local b = BOT(1, 10, 10)
  b.inf.events = { ping(5, 0, 20, 20) }
  ORD.on_events(b.st, b.w, b.inf, 100)
  b.inf.events = {}
  return b
end
local function settle(b, from, to)
  for t = from, to do b.st.tick = t; ORD.update(b.st, b.w, b.inf, t) end
end
local function attack_ping(b, t, mx, my, sender)
  b.inf.events = { ping(3, sender or 0, mx or 20, my or 20) }
  b.st.tick = t
  ORD.on_events(b.st, b.w, b.inf, t)
  b.inf.events = {}
end
local function said(b, pat)
  for _, l in ipairs((b.st.orders or {}).say or {}) do
    if l:match(pat) then return true end
  end
  return false
end

-- 1. INSIDE THE WINDOW: attack ping 25 thinks (0.5 s) after the bot command.
local b = sbot()
settle(b, 101, 115)
check("suicide setup: the bot holds the attack_pill order",
      b.st.orders.held ~= nil and b.st.orders.held.kind == "attack_pill"
      and b.st.orders.held.tid == 5,
      tostring(b.st.orders.held and b.st.orders.held.kind))
attack_ping(b, 125)
check("attack ping 0.5 s after the bot command starts the run",
      b.st._suicide ~= nil and b.st._suicide.tid == 5 and b.st._suicide.sender == 0,
      tostring(b.st._suicide and b.st._suicide.tid))
check("the bot says one team-chat line for it",
      said(b, "^Suicide run on pill #5$"), table.concat(b.st.orders.say or {}, " | "))
check("a suicide bot is busy for new orders",
      select(2, ORD.busy(b.st, b.inf)) == "suicide run",
      tostring(select(2, ORD.busy(b.st, b.inf))))
check("the panel shows the run",
      ORD.panel_line(b.st, b.inf):match("SUICIDE RUN pill#5") ~= nil,
      ORD.panel_line(b.st, b.inf))
-- A second attack ping is not a second run and not a second chat line.
local nsay = #b.st.orders.say
attack_ping(b, 130)
check("a second attack ping inside the window says nothing more",
      #b.st.orders.say == nsay, tostring(#b.st.orders.say))

-- Exactly on the window edge (50 thinks = 1.0 s) still counts.
b = sbot(); settle(b, 101, 115)
attack_ping(b, 150)
check("attack ping at exactly 1.0 s still starts the run",
      b.st._suicide ~= nil, "nil")

-- 2. OUTSIDE THE WINDOW: 75 thinks = 1.5 s.
b = sbot(); settle(b, 101, 115)
attack_ping(b, 175)
check("attack ping 1.5 s after the bot command does NOT start the run",
      b.st._suicide == nil, tostring(b.st._suicide and b.st._suicide.tid))
check("and the bot keeps its ordinary order",
      b.st.orders.held ~= nil and b.st.orders.held.tid == 5, "?")
-- 51 thinks: one past the edge.
b = sbot(); settle(b, 101, 115)
attack_ping(b, 151)
check("attack ping at 1.02 s does NOT start the run", b.st._suicide == nil, "?")

-- The pair must match: same sender, same pill, bot command FIRST.
b = sbot(); settle(b, 101, 115)
attack_ping(b, 120, 20, 20, 2)          -- p2 sent the attack ping, p0 the command
check("another player's attack ping does not trigger", b.st._suicide == nil, "?")
b = sbot(); settle(b, 101, 115)
b.w.pills[6] = { mx = 25, my = 25, owner = "hostile", health = 10 }
attack_ping(b, 120, 25, 25)
check("an attack ping on a different pill does not trigger", b.st._suicide == nil, "?")
do
  local r = BOT(1, 10, 10)
  attack_ping(r, 100)
  r.inf.events = { ping(5, 0, 20, 20) }
  ORD.on_events(r.st, r.w, r.inf, 110)
  r.inf.events = {}
  settle(r, 111, 125)
  check("attack THEN bot command does not trigger (order matters)",
        r.st._suicide == nil, "?")
end
-- An attack ping on its own orders nothing.
do
  local r = BOT(1, 10, 10)
  attack_ping(r, 100)
  check("a lone attack ping orders nothing",
        r.st._suicide == nil and (r.st.orders == nil or r.st.orders.held == nil), "?")
end

-- The knob off (the keel value) turns the trigger off.
C.PING_SUICIDE_ENABLED = false
b = sbot(); settle(b, 101, 115)
attack_ping(b, 125)
check("PING_SUICIDE_ENABLED=false: no run", b.st._suicide == nil, "?")
C.PING_SUICIDE_ENABLED = true
check("keel turns the suicide run off",
      C.PRESETS.keel.PING_SUICIDE_ENABLED == false,
      tostring(C.PRESETS.keel.PING_SUICIDE_ENABLED))
check("the window is 50 thinks (1 s) live and in keel",
      C.PING_SUICIDE_WINDOW_TICKS == 50 and C.PRESETS.keel.PING_SUICIDE_WINDOW_TICKS == 50,
      tostring(C.PING_SUICIDE_WINDOW_TICKS))

-- The attack ping lands while the auction is still open: the bot that wins
-- it afterwards goes too.
b = sbot()
attack_ping(b, 104)
check("attack ping during the auction: no run yet (nobody holds it)",
      b.st._suicide == nil, "?")
settle(b, 105, 120)
check("the bot that wins the auction afterwards starts the run",
      b.st._suicide ~= nil and b.st._suicide.tid == 5,
      tostring(b.st._suicide and b.st._suicide.tid))

-- A bot already attacking pill 5 on its own choice (no order) goes too.
-- The man is out of the tank, so the bot is busy and takes no order.
do
  local r = BOT(1, 10, 10)
  r.st.goal = { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }
  r.inf.man_status = 99
  r.inf.events = { ping(5, 0, 20, 20) }
  ORD.on_events(r.st, r.w, r.inf, 100)
  r.inf.events = {}
  attack_ping(r, 110)
  check("a bot already attacking the pill with no order joins the run",
        r.st._suicide ~= nil and r.st._suicide.tid == 5
        and r.st.orders.held == nil,
        tostring(r.st._suicide and r.st._suicide.tid))
end

-- 3. DURING THE RUN: no refuel, no flee, no armour or shell gate.
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
b.inf.armour, b.inf.shells = 2, 0
local g = GOALS.pick_goal(b.st, b.w, b.inf, true)
check("pick_goal at armour 2, 0 shells: the suicide goal, nothing else",
      g and g.kind == "attack_pill" and g.target_id == 5 and g._ping_suicide == true
      and g.substate == "kill_hardline",
      tostring(g and g.kind))
b.st.orders.say = {}
settle(b, 126, 200)
check("no refuel line while on the run at 0 shells",
      not said(b, "Refuelling"), table.concat(b.st.orders.say or {}, " | "))
-- The lock undoes whatever else replaced the goal this think.
for _, kind in ipairs({ "flee_pill", "refuel_at_base", "take_cover", "escape_water",
                        "rescue_lgm" }) do
  b.st.goal = { kind = kind, mx = 3, my = 3 }
  local on = ORD.suicide_lock(b.st, b.w, b.inf, 201)
  check("the lock replaces a " .. kind .. " goal with the run",
        on and b.st.goal.kind == "attack_pill" and b.st.goal._ping_suicide
        and b.st.goal.target_id == 5,
        tostring(b.st.goal.kind))
end
-- The substate machine keeps it in kill_hardline and retries a dead end.
b.st.goal.substate = "plan_position"
b.st.goal._hardline_abort = "no navigable tile beside pill"
b.st.goal._hardline_bad = { [1] = true }
ATTACK.update_attack_substate(b.st.goal, b.st, b.w, b.inf)
check("the substate machine holds kill_hardline",
      b.st.goal.kind == "attack_pill" and b.st.goal.substate == "kill_hardline",
      tostring(b.st.goal.substate))
check("a kill_hardline dead end is retried, not given up",
      b.st.goal._hardline_abort == nil and b.st.goal._hardline_bad == nil
      and b.st._suicide ~= nil, tostring(b.st.goal._hardline_abort))
check("the order does not lapse under the run",
      b.st.orders.held ~= nil and (b.st.orders.held.expiry or 0) >= 200,
      tostring(b.st.orders.held and b.st.orders.held.expiry))
b.st.orders.held.expiry = 150
settle(b, 201, 205)
check("an expired order focus is kept alive while the run stands",
      b.st.orders.held ~= nil and b.st._suicide ~= nil, "?")

-- 4. HOW IT ENDS.
-- Pill dead (armour 0): the lock ends the run and clears the goal.
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
ORD.suicide_lock(b.st, b.w, b.inf, 126)
b.w.pills[5].health = 0
local on = ORD.suicide_lock(b.st, b.w, b.inf, 127)
check("pill dead: the run ends", not on and b.st._suicide == nil, "?")
check("pill dead: the suicide goal is cleared",
      not (b.st.goal and b.st.goal._ping_suicide), tostring(b.st.goal and b.st.goal.kind))
-- Pill captured by us, pill in a tank, pill gone.
for _, case in ipairs({ { "ours",    function(p) p.owner = "friendly" end },
                        { "carried", function(p) p.in_tank = true end },
                        { "gone",    function(p, w) w.pills[5] = nil end } }) do
  b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
  case[2](b.w.pills[5], b.w)
  ORD.suicide_lock(b.st, b.w, b.inf, 126)
  check("pill " .. case[1] .. ": the run ends", b.st._suicide == nil, "?")
end
-- The substate machine ends it too when the pill dies between locks.
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
ORD.suicide_lock(b.st, b.w, b.inf, 126)
b.w.pills[5].health = 0
ATTACK.update_attack_substate(b.st.goal, b.st, b.w, b.inf)
check("substate machine: pill dead clears the suicide goal",
      not (b.st.goal and b.st.goal._ping_suicide), tostring(b.st.goal and b.st.goal.kind))
-- Tank died.
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
ORD.on_death(b.st, b.inf)
check("tank died: the run ends", b.st._suicide == nil, "?")
-- The escape hatch: cancel.
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
ORD.on_chat(b.st, b.w, b.inf, 0, "cancel", 130, true, false)
check("a bare cancel from the sender ends the run", b.st._suicide == nil, "?")
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
ORD.on_chat(b.st, b.w, b.inf, 0, "cancel all", 130, true, false)
check("cancel all ends the run", b.st._suicide == nil, "?")
-- The escape hatch: caution on the bot, or on the pill.
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
b.inf.events = { ping(1, 0, 10, 10) }
ORD.on_events(b.st, b.w, b.inf, 130)
check("caution on the bot ends the run", b.st._suicide == nil, "?")
b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
b.inf.events = { ping(1, 0, 20, 20) }
ORD.on_events(b.st, b.w, b.inf, 130)
check("caution on the pill ends the run", b.st._suicide == nil, "?")
-- The same caution after the run has kept its order past the 60 s focus:
-- the prune has dropped the order from o.known by then, and the caution
-- must still cancel it, not leave the bot attacking until "order lapsed".
do
  b = sbot(); settle(b, 101, 115); attack_ping(b, 125)
  local oid = b.st.orders.held and b.st.orders.held.oid
  settle(b, 3300, 3300)
  check("setup: a 64 s run still holds its order, pruned from o.known",
        b.st._suicide ~= nil and b.st.orders.held ~= nil
        and b.st.orders.held.oid == oid and b.st.orders.known[oid] == nil, "?")
  b.st.orders.say, b.st.orders.out = {}, {}
  b.inf.events = { ping(1, 0, 20, 20) }
  ORD.on_events(b.st, b.w, b.inf, 3301)
  b.inf.events = {}
  check("a late caution on the pill ends the run AND cancels the order",
        b.st._suicide == nil and b.st.orders.held == nil,
        tostring(b.st.orders.held and b.st.orders.held.kind))
  check("it says Released", said(b, "^Released$"),
        table.concat(b.st.orders.say or {}, " | "))
  check("and the cancel goes out as obx",
        (b.st.orders.out[1] or ""):match("^/info obx ") ~= nil,
        tostring(b.st.orders.out[1]))
end
-- A bot running on its own choice (no order): the caution ends the run and
-- does NOT go on to retreat it.
do
  local r = BOT(1, 10, 10)
  r.st.goal = { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }
  r.inf.man_status = 99
  r.inf.events = { ping(5, 0, 20, 20) }
  ORD.on_events(r.st, r.w, r.inf, 100)
  attack_ping(r, 110)
  r.inf.man_status = 0
  r.inf.events = { ping(1, 0, 10, 10) }
  ORD.on_events(r.st, r.w, r.inf, 120)
  check("caution on an order-less runner ends the run, no retreat",
        r.st._suicide == nil and r.st.orders.held == nil,
        tostring(r.st.orders.held and r.st.orders.held.kind))
end

-- A HUMAN TEAM-MATE CLOSE BY starts the same run.  p0 is the human, p1 the
-- bot at (10,10) holding an attack_pill ORDER on pill 5, with the goal that
-- order gives it.  No ping at all.
local function hold_attack(r, tid)
  ORD.update(r.st, r.w, r.inf, 99)               -- builds r.st.orders
  r.st.orders.held = { oid = 4242, kind = "attack_pill", tid = tid or 5,
                       expiry = 1e9 }
end
local function hbot(hx, hy)
  local r = BOT(1, 10, 10)
  r.st.goal = { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }
  r.inf.player_bots = 0x02
  r.inf.objects = { { type = 1, idnum = 0, x = hx * 256, y = hy * 256, info = 0 } }
  hold_attack(r)
  return r
end
do
  local r = hbot(15, 10)
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("human 5 away: the attack_pill goal becomes a suicide run",
        r.st._suicide ~= nil and r.st._suicide.tid == 5 and r.st._suicide.sender == 0,
        tostring(r.st._suicide and r.st._suicide.tid))
  ORD.suicide_lock(r.st, r.w, r.inf, 101)
  check("human-near run: the lock puts the suicide goal in",
        r.st.goal._ping_suicide == true and r.st.goal.target_id == 5, "?")
  r.inf.objects = {}
  ORD.human_near_suicide(r.st, r.w, r.inf, 102)
  ORD.suicide_lock(r.st, r.w, r.inf, 102)
  check("the human drives off: the run stands", r.st._suicide ~= nil, "?")
  ORD.on_chat(r.st, r.w, r.inf, 0, "cancel", 110, true, false)
  check("a bare cancel from that human ends the run", r.st._suicide == nil, "?")
  r.inf.objects = { { type = 1, idnum = 0, x = 15 * 256, y = 10 * 256, info = 0 } }
  r.st.goal = { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }
  hold_attack(r)
  ORD.human_near_suicide(r.st, r.w, r.inf, 111)
  check("after the cancel it does not restart on the same pill", r.st._suicide == nil, "?")
  r.st.goal = { kind = "refuel" }
  ORD.human_near_suicide(r.st, r.w, r.inf, 112)
  r.st.goal = { kind = "attack_pill", target_id = 5, mx = 20, my = 20 }
  ORD.human_near_suicide(r.st, r.w, r.inf, 113)
  check("after some other goal the rule arms again", r.st._suicide ~= nil, "?")
end
do
  local r = hbot(18, 10)
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("human 8 away: no run", r.st._suicide == nil, "?")
  r = hbot(11, 10)
  r.inf.player_bots = 0x03
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("an ally BOT close by: no run", r.st._suicide == nil, "?")
  r = hbot(11, 10)
  r.st.goal = { kind = "defend_pill", target_id = 5, mx = 20, my = 20 }
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("a human close by and a non-attack goal: no run", r.st._suicide == nil, "?")
  r = hbot(11, 10)
  r.st.orders.held = nil
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("a human close by and a self-chosen attack_pill goal: no run",
        r.st._suicide == nil, "?")
  r = hbot(11, 10)
  hold_attack(r, 9)
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("a human close by and an order on a different pill: no run",
        r.st._suicide == nil, "?")
  C.ORDER_HUMAN_NEAR_SUICIDE_RUN = false
  r = hbot(11, 10)
  ORD.human_near_suicide(r.st, r.w, r.inf, 100)
  check("ORDER_HUMAN_NEAR_SUICIDE_RUN=false: no run", r.st._suicide == nil, "?")
  C.ORDER_HUMAN_NEAR_SUICIDE_RUN = true
  check("keel turns the human-near run off",
        C.PRESETS.keel.ORDER_HUMAN_NEAR_SUICIDE_RUN == false, "?")
end

-- =========================================================================
-- GO-THERE DECOY HARD HOLD (Andrew, 2026-09-24).  A bot-command ping on open
-- ground next to an enemy pill: on arrival the bot parks there as a decoy
-- and nothing it would choose for itself takes it off.  With no pill in
-- range the order ends on arrival.  One bot, p1 at (10,10).  Pill 5 (enemy,
-- alive) at (20,20).  The decoy square is (22,24): 4.5 tiles from the pill,
-- clear of every tank's ping ring.  (100,100) is open ground with no pill.
-- =========================================================================
print("orders.lua -- go-there decoy hard hold")

local function dbot(mx, my)
  local b = BOT(1, 10, 10)
  b.inf.events = { ping(5, 0, mx or 22, my or 24) }
  ORD.on_events(b.st, b.w, b.inf, 100)
  b.inf.events = {}
  settle(b, 101, 115)
  return b
end
local function arrive(b, t, mx, my)
  b.inf.tankx, b.inf.tanky = (mx or 22) * 256 + 128, (my or 24) * 256 + 128
  b.st.orders.say = {}
  b.st.tick = t
  ORD.update(b.st, b.w, b.inf, t)
end
local function lock(b, t)
  b.st.tick = t
  return ORD.decoy_lock(b.st, b.w, b.inf, t)
end
local HOLD = C.ORDER_GOTO_HOLD_TICKS or 500

check("decoy knob is on live", C.ORDER_GOTO_DECOY == true, tostring(C.ORDER_GOTO_DECOY))
check("keel turns the decoy hold off",
      C.PRESETS.keel.ORDER_GOTO_DECOY == false,
      tostring(C.PRESETS.keel.ORDER_GOTO_DECOY))
check("pill range is edist <= 8, neutral counts, ours/carried/dead do not",
      (function()
         local w = { pills = {
           [1] = { mx = 8,  my = 0, owner = "neutral",  health = 5 },   -- 8.0: in
           [2] = { mx = 6,  my = 6, owner = "hostile",  health = 5 },   -- 8.49: out
           [3] = { mx = 1,  my = 1, owner = "friendly", health = 5 },
           [4] = { mx = 1,  my = 1, owner = "hostile",  health = 5, in_tank = true },
           [5] = { mx = 1,  my = 1, owner = "hostile",  health = 0 },
           [6] = { mx = 1,  my = 1, owner = "allied",   health = 5 },
         } }
         return ORD.decoy_pills(w, 0, 0) == 1
       end)(), tostring(ORD.decoy_pills({ pills = {} }, 0, 0)))

-- 1. ARRIVAL WITH A PILL IN RANGE -> DECOY.
local d = dbot()
check("decoy setup: the ping order is a go-there, marked as a ping",
      d.st.orders.held ~= nil and d.st.orders.held.kind == "goto_tile"
      and d.st.orders.held.ping == true,
      tostring(d.st.orders.held and d.st.orders.held.kind))
arrive(d, 200)
check("arrival next to an enemy pill: decoy hold",
      d.st.orders.held ~= nil and d.st.orders.held.decoy == true
      and d.st.orders.held.hold == true, "?")
check("the decoy says the number, once",
      #d.st.orders.say == 1 and d.st.orders.say[1] == "decoying 10s",
      table.concat(d.st.orders.say, " | "))
check("the decoy clock is the hold knob",
      d.st.orders.held.expiry == 200 + HOLD, tostring(d.st.orders.held.expiry))
check("the decoy is parked on its square", ORD.hold_parked(d.st, d.inf) == true, "?")
check("the panel says decoy",
      ORD.panel_line(d.st, d.inf):sub(-5) == "decoy", ORD.panel_line(d.st, d.inf))
check("a decoy is NOT busy", ORD.busy(d.st, d.inf) == false,
      tostring(select(2, ORD.busy(d.st, d.inf))))

-- 2. ARRIVAL WITH NO PILL IN RANGE -> THE ORDER ENDS, IN SILENCE.
d = dbot(100, 100)
d.st.goal = { kind = "goto_tile", mx = 100, my = 100 }
d.st.orders.out = {}
arrive(d, 200, 100, 100)
check("no pill in range: the order ends on arrival", d.st.orders.held == nil, "held")
check("and says nothing (no holding, no order lapsed)", #d.st.orders.say == 0,
      table.concat(d.st.orders.say, " | "))
check("and it is a cancel (obx), not a hand-back",
      (function()
         for _, l in ipairs(d.st.orders.out) do
           if l:match("^/info obx ") then return true end
           if l:match("^/info obr ") then return false end
         end
         return false
       end)(), table.concat(d.st.orders.out, " | "))
check("and the travel goal is dropped for normal play",
      d.st.goal.kind == "none", tostring(d.st.goal.kind))

-- 3. THE LOCK UNDOES EVERYTHING THE BOT WOULD CHOOSE FOR ITSELF.
d = dbot(); arrive(d, 200)
for _, kind in ipairs({ "take_cover", "refuel_at_base", "escape_water", "flee_pill",
                        "flee_to_base", "rescue_lgm", "mine_crater", "kill_me_wait",
                        "attack_pill", "capture_pill", "explore", "none" }) do
  d.st.goal = { kind = kind, mx = 3, my = 3, target_id = 5 }
  local on = lock(d, 210)
  check("the decoy lock replaces a " .. kind .. " goal with the hold goal",
        on and d.st.goal.kind == "goto_tile" and d.st.goal._decoy == true
        and d.st.goal.mx == 22 and d.st.goal.my == 24,
        tostring(d.st.goal.kind))
end
-- attack_tank and kill_lgm in gun range stay; out of range, on an ally, or
-- with the tank off the square they do not.
d.st.goal = { kind = "attack_tank", target_id = 7, mx = 25, my = 26 }
check("attack_tank on an enemy in gun range is allowed",
      lock(d, 211) and d.st.goal.kind == "attack_tank", tostring(d.st.goal.kind))
d.st.goal = { kind = "kill_lgm", target_id = 3, mx = 23, my = 24 }
check("kill_lgm in gun range is allowed",
      lock(d, 212) and d.st.goal.kind == "kill_lgm", tostring(d.st.goal.kind))
d.st.goal = { kind = "attack_tank", target_id = 7, mx = 40, my = 40 }
check("attack_tank out of gun range is not (no chase)",
      lock(d, 213) and d.st.goal.kind == "goto_tile", tostring(d.st.goal.kind))
d.st.goal = { kind = "kill_lgm", target_id = 3, mx = 22, my = 34 }
check("kill_lgm out of gun range is not",
      lock(d, 214) and d.st.goal.kind == "goto_tile", tostring(d.st.goal.kind))
d.st.goal = { kind = "attack_tank", target_id = 2, mx = 23, my = 24, km_ally_pn = 2 }
check("a kill-me attack_tank on an ALLY is not",
      lock(d, 215) and d.st.goal.kind == "goto_tile", tostring(d.st.goal.kind))
d.st.perc = { enemy_tanks = { { id = 7, mx = 50, my = 50 } } }
d.st.goal = { kind = "attack_tank", target_id = 7, mx = 23, my = 24 }
check("the enemy tank's LIVE tile from perception decides the range",
      lock(d, 216) and d.st.goal.kind == "goto_tile", tostring(d.st.goal.kind))
d.st.perc = nil
d.inf.tankx, d.inf.tanky = 30 * 256 + 128, 30 * 256 + 128
d.st.goal = { kind = "attack_tank", target_id = 7, mx = 31, my = 31 }
check("off the square: the hold goal drives it back, no fight",
      lock(d, 217) and d.st.goal.kind == "goto_tile" and d.st.goal.mx == 22, "?")
d.inf.tankx, d.inf.tanky = 22 * 256 + 128, 24 * 256 + 128
-- No throttle on the square.  KEY_FASTER is a host global; fake it here.
_G.KEY_FASTER = 0x10
local k, tp = ORD.decoy_keys(d.st, d.inf, 0x11, 0x10)
check("decoy_keys takes KEY_FASTER off keys and taps", k == 0x01 and tp == 0, tostring(k))
local soft = BOT(1, 10, 10)
check("decoy_keys leaves a bot with no decoy alone",
      ORD.decoy_keys(soft.st, soft.inf, 0x11, 0) == 0x11, "?")
_G.KEY_FASTER = nil

-- 4a. THE PILLS GO DOWN -> THE DECOY ENDS, IN SILENCE.
d = dbot(); arrive(d, 200)
d.st.goal = ORD.decoy_goal(d.st.orders.held)
d.w.pills[5].health = 0
d.st.orders.say = {}
ORD.update(d.st, d.w, d.inf, 300)
check("pill dead: the decoy ends", d.st.orders.held == nil, "held")
check("pill dead: in silence", #d.st.orders.say == 0, table.concat(d.st.orders.say, " | "))
check("pill dead: the hold goal is dropped for normal play",
      d.st.goal.kind == "none", tostring(d.st.goal.kind))
for _, case in ipairs({ { "ours",    function(p) p.owner = "friendly" end },
                        { "carried", function(p) p.in_tank = true end },
                        { "gone",    function(p, w) w.pills[5] = nil end } }) do
  d = dbot(); arrive(d, 200)
  case[2](d.w.pills[5], d.w)
  local on = lock(d, 201)
  check("pill " .. case[1] .. ": the lock ends the decoy",
        not on and d.st.orders.held == nil, "held")
end
-- A pill that comes into range after arrival counts too.
d = dbot(); arrive(d, 200)
d.w.pills[6] = { mx = 27, my = 24, owner = "hostile", health = 5 }
d.w.pills[5].health = 0
ORD.update(d.st, d.w, d.inf, 250)
check("a new pill in range keeps the decoy up after the first dies",
      d.st.orders.held ~= nil and d.st.orders.held.decoy == true, "ended")
check("and the lock still stands", lock(d, 251) == true, "off")
d.w.pills[6].health = 0
ORD.update(d.st, d.w, d.inf, 260)
check("when the new pill dies too, the decoy ends", d.st.orders.held == nil, "held")

-- 4b. TEN SECONDS -> ENDS, IN SILENCE.
d = dbot(); arrive(d, 200)
ORD.update(d.st, d.w, d.inf, 200 + HOLD - 1)
check("the decoy stands one tick before the clock", d.st.orders.held ~= nil, "ended")
d.st.orders.say = {}
ORD.update(d.st, d.w, d.inf, 200 + HOLD)
check("ten seconds: the decoy ends", d.st.orders.held == nil, "held")
check("ten seconds: in silence", #d.st.orders.say == 0, table.concat(d.st.orders.say, " | "))
check("and the lock is off", lock(d, 200 + HOLD + 1) == false, "on")

-- 4c. A CAUTION PING BESIDE THE TANK, FROM ANY HUMAN ALLY.
d = dbot(); arrive(d, 200)
d.inf.events = { ping(1, 0, 23, 24) }
ORD.on_events(d.st, d.w, d.inf, 300)
d.inf.events = {}
check("caution beside the tank from the sender: released",
      d.st.orders.held == nil and d.st.orders.say[#d.st.orders.say] == "Released",
      table.concat(d.st.orders.say, " | "))
check("and no retreat on the same ping", d.st.orders.held == nil, "retreat")
d = dbot(); arrive(d, 200)
d.inf.allies = 0x1F                       -- p3 is a HUMAN ally (not in player_bots)
d.inf.events = { ping(1, 3, 21, 23) }
ORD.on_events(d.st, d.w, d.inf, 300)
d.inf.events = {}
check("caution beside the tank from a DIFFERENT human ally: released",
      d.st.orders.held == nil, "held")
d = dbot(); arrive(d, 200)
d.inf.events = { ping(1, 0, 22, 24) }
ORD.on_events(d.st, d.w, d.inf, 300)
d.inf.events = {}
check("caution ON the tank: released, no retreat", d.st.orders.held == nil, "held")
d = dbot(); arrive(d, 200)
d.inf.events = { ping(1, 0, 25, 24) }
ORD.on_events(d.st, d.w, d.inf, 300)
d.inf.events = {}
check("caution 3 tiles away does not end the decoy",
      d.st.orders.held ~= nil and d.st.orders.held.decoy == true, "ended")
d.inf.player_bots = 0x17                   -- p0 is now a bot: its pings are news
d.inf.events = { ping(1, 0, 23, 24) }
ORD.on_events(d.st, d.w, d.inf, 310)
d.inf.events = {}
check("a BOT's caution beside the tank does not end it",
      d.st.orders.held ~= nil, "ended")

-- 4d. CANCEL.
for _, line in ipairs({ "cancel", "cancel all" }) do
  d = dbot(); arrive(d, 200)
  ORD.on_chat(d.st, d.w, d.inf, 0, line, 300, true, false)
  check("'" .. line .. "' ends the decoy", d.st.orders.held == nil
        and lock(d, 301) == false, "held")
end

-- 4e. DEATH.
d = dbot(); arrive(d, 200)
local dead_oid = d.st.orders.held.oid
d.st.orders.out = {}
ORD.on_death(d.st, d.inf)
check("death ends the decoy", d.st.orders.held == nil and ORD.decoy_held(d.st) == nil, "held")
check("a decoy death CANCELS the order (obx), no hand-back to another bot",
      d.st.orders.out[1] == "/info obx " .. dead_oid, tostring(d.st.orders.out[1]))
check("and nothing parks after the respawn",
      ORD.hold_parked(d.st, d.inf) == false and lock(d, 301) == false, "parked")

-- 4f. A NEW PING OR CHAT AUCTION ELSEWHERE DOES NOT END THE DECOY
-- (ORDER_HOLDER_KEEPS_JOB).  The decoy runs to its own end; a free bot
-- would take the new order.  This bot is alone, so nobody goes, and it says
-- "All bots busy".  An order that NAMES the bot still ends the decoy.
d = dbot(); arrive(d, 200)
local old_oid = d.st.orders.held.oid
d.st.orders.say = {}
d.inf.events = { ping(5, 0, 100, 100) }
ORD.on_events(d.st, d.w, d.inf, 300)
d.inf.events = {}
settle(d, 301, 315)
check("a new ping elsewhere does not end the decoy",
      d.st.orders.held ~= nil and d.st.orders.held.oid == old_oid
      and ORD.decoy_held(d.st) ~= nil,
      tostring(d.st.orders.held and d.st.orders.held.oid))
check("and the lone decoy says all bots are busy",
      said(d, "^All bots busy$"), table.concat(d.st.orders.say, " | "))
d = dbot(); arrive(d, 200)
ORD.on_chat(d.st, d.w, d.inf, 0, "attack 5", 300, true, false)
settle(d, 301, 315)
check("a new chat order (no name) does not end the decoy",
      d.st.orders.held ~= nil and d.st.orders.held.oid == old_oid
      and ORD.decoy_held(d.st) ~= nil,
      tostring(d.st.orders.held and d.st.orders.held.kind))
d = dbot(); arrive(d, 200)
ORD.on_chat(d.st, d.w, d.inf, 0, "socrates attack 5", 300, true, false)
settle(d, 301, 315)
check("an order that NAMES the decoy bot replaces the decoy",
      d.st.orders.held ~= nil and d.st.orders.held.kind == "attack_pill",
      tostring(d.st.orders.held and d.st.orders.held.kind))
-- Keel: the auction winner leaves its decoy for the new order, as before.
C.ORDER_HOLDER_KEEPS_JOB = false
d = dbot(); arrive(d, 200)
d.inf.events = { ping(5, 0, 100, 100) }
ORD.on_events(d.st, d.w, d.inf, 300)
d.inf.events = {}
settle(d, 301, 315)
check("HOLDER_KEEPS_JOB off: a new ping order replaces the decoy",
      d.st.orders.held ~= nil and d.st.orders.held.oid ~= old_oid
      and d.st.orders.held.decoy == nil and d.st.orders.held.mx == 100,
      tostring(d.st.orders.held and d.st.orders.held.oid))
d = dbot(); arrive(d, 200)
ORD.on_chat(d.st, d.w, d.inf, 0, "attack 5", 300, true, false)
settle(d, 301, 315)
check("HOLDER_KEEPS_JOB off: a new chat order replaces the decoy",
      d.st.orders.held ~= nil and d.st.orders.held.kind == "attack_pill",
      tostring(d.st.orders.held and d.st.orders.held.kind))
C.ORDER_HOLDER_KEEPS_JOB = true

-- 5. KNOB OFF (keel) AND CHAT ORDERS: today's soft hold, unchanged.
C.ORDER_GOTO_DECOY = false
d = dbot(); arrive(d, 200)
check("ORDER_GOTO_DECOY=false: the soft hold, 'holding 10s'",
      d.st.orders.held ~= nil and d.st.orders.held.decoy == nil
      and d.st.orders.say[1] == "holding 10s",
      table.concat(d.st.orders.say, " | "))
check("ORDER_GOTO_DECOY=false: the lock does nothing",
      (function() d.st.goal = { kind = "take_cover", mx = 3, my = 3 }
         return lock(d, 201) == false and d.st.goal.kind == "take_cover" end)(), "?")
check("ORDER_GOTO_DECOY=false: the panel says holding",
      ORD.panel_line(d.st, d.inf):sub(-7) == "holding", ORD.panel_line(d.st, d.inf))
d = dbot(100, 100); arrive(d, 200, 100, 100)
check("ORDER_GOTO_DECOY=false: no pill in range still holds",
      d.st.orders.held ~= nil and d.st.orders.held.hold == true
      and d.st.orders.say[1] == "holding 10s", table.concat(d.st.orders.say, " | "))
C.ORDER_GOTO_DECOY = true
do
  -- A "!goto x y" reaches the bots within ORDER_NEARBY_TILES of the square.
  local r = BOT(1, 18, 26)
  ORD.on_chat(r.st, r.w, r.inf, 0, "!goto 22 24", 100, true, false)
  settle(r, 101, 115)
  check("chat go-there setup: the bot holds it",
        r.st.orders.held ~= nil and r.st.orders.held.kind == "goto_tile",
        tostring(r.st.orders.held and r.st.orders.held.kind))
  arrive(r, 200)
  check("a chat go-there next to a pill keeps the soft hold",
        r.st.orders.held ~= nil and r.st.orders.held.decoy == nil
        and r.st.orders.say[1] == "holding 10s", table.concat(r.st.orders.say, " | "))
end

-- =========================================================================
-- PING REPAIR BONUS.  A bot-command ping on one of OUR pills weights that
-- pill's builder-pool repair row x BUILDER_POOL_PING_PILL_BONUS (2.0) for
-- BUILDER_POOL_PING_PILL_TICKS (500), in the pool of the bot that takes the
-- order only, until the pill is dead, full, not ours, or another of our
-- pills is pinged.
-- =========================================================================
print("orders.lua / builder_pool.lua -- ping repair bonus")
local BP = require("builder_pool")
local function rping(b, t, mx, my)
  b.inf.events = { ping(5, 0, mx or 60, my or 60) }
  b.st.tick = t
  ORD.on_events(b.st, b.w, b.inf, t)
  b.inf.events = {}
end
-- Ping, then let the lone bot's auction settle so it TAKES the order.
local function rtake(b, t, mx, my)
  rping(b, t, mx, my)
  settle(b, t + 1, t + 15)
end
do
  local r = BOT(1, 10, 10)
  rping(r, 100)
  check("hearing the ping alone gives no bonus (only the taker gets it)",
        r.st._repair_ping == nil, "set")
  settle(r, 101, 115)
  local rp = r.st._repair_ping
  check("the bot that takes the defend order on our pill #9 gets the bonus",
        rp ~= nil and rp.tid == 9 and rp.sender == 0
        and r.st.orders.held ~= nil and r.st.orders.held.kind == "defend_pill",
        tostring(rp and rp.tid))
  local took = rp and (rp.until_tick - 500) or 0
  local w9, src = BP.goal_weight(r.st, { type = "repair", id = 9 })
  check("pill #9's repair row is weighted 2.0 by the ping", w9 == 2.0 and src == "ping",
        tostring(w9) .. " " .. tostring(src))
  check("another pill's row is not", BP.goal_weight(r.st, { type = "repair", id = 5 }) == 1.0, "?")
  check("a farm row is never", BP.goal_weight(r.st, { type = "farm", id = 9 }) == 1.0, "?")
  local g0 = r.st.goal
  r.st.goal = { kind = "defend_pill", target_id = 9 }
  local wb = BP.goal_weight(r.st, { type = "repair", id = 9 })
  check("goal bonus and ping on one pill: the larger (2.0), not 2.4", wb == 2.0, tostring(wb))
  r.st.goal = g0
  ORD.update(r.st, r.w, r.inf, took + 499)
  check("the bonus stands just before 10 s", r.st._repair_ping ~= nil, "ended")
  ORD.update(r.st, r.w, r.inf, took + 500)
  check("the bonus ends at 10 s", r.st._repair_ping == nil, "?")
end
do
  local r = BOT(1, 10, 10)
  rtake(r, 100)
  r.w.pills[9].health = 15
  ORD.update(r.st, r.w, r.inf, 120)
  check("the bonus ends when the pill is fully repaired", r.st._repair_ping == nil, "?")
  r = BOT(1, 10, 10)
  rtake(r, 100)
  r.w.pills[9].health = 0
  ORD.update(r.st, r.w, r.inf, 120)
  check("the bonus ends when the pill is dead", r.st._repair_ping == nil, "?")
  r = BOT(1, 10, 10)
  rtake(r, 100)
  r.w.pills[9].owner = "hostile"
  ORD.update(r.st, r.w, r.inf, 120)
  check("the bonus ends when the pill is no longer ours", r.st._repair_ping == nil, "?")
  -- A ping on another of our pills ends it, even for a bot that does not
  -- go on to take the new order.
  r = BOT(1, 10, 10)
  r.w.pills[11] = { mx = 70, my = 70, owner = "friendly", health = 5 }
  rtake(r, 100)
  check("setup: the bonus is on pill #9",
        r.st._repair_ping ~= nil and r.st._repair_ping.tid == 9, "?")
  rping(r, 200, 70, 70)
  check("a ping on another of our pills ends the old bonus at once",
        r.st._repair_ping == nil, tostring(r.st._repair_ping and r.st._repair_ping.tid))
  settle(r, 201, 215)
  -- ORDER_NO_FREE_TAKES_LOWEST: the lone bot is the only one, so no free bot
  -- exists and it switches to #11 and takes the bonus there.
  check("no free bot: the lone defender switches to the new pill and its bonus",
        r.st.orders.held ~= nil and r.st.orders.held.tid == 11
        and r.st._repair_ping ~= nil and r.st._repair_ping.tid == 11,
        tostring(r.st.orders.held and r.st.orders.held.tid))
  -- Knob off: ORDER_HOLDER_KEEPS_JOB alone.  This bot keeps its defend order
  -- on #9, so it does not take the new one and gets no bonus on #11 either.
  C.ORDER_NO_FREE_TAKES_LOWEST = false
  r = BOT(1, 10, 10)
  r.w.pills[11] = { mx = 70, my = 70, owner = "friendly", health = 5 }
  rtake(r, 100)
  rping(r, 200, 70, 70)
  settle(r, 201, 215)
  check("a bot on a defend order keeps it on a ping on another of our pills",
        r.st.orders.held ~= nil and r.st.orders.held.tid == 9
        and r.st._repair_ping == nil,
        tostring(r.st.orders.held and r.st.orders.held.tid))
  C.ORDER_NO_FREE_TAKES_LOWEST = true
  C.ORDER_HOLDER_KEEPS_JOB = false
  r = BOT(1, 10, 10)
  r.w.pills[11] = { mx = 70, my = 70, owner = "friendly", health = 5 }
  rtake(r, 100)
  rping(r, 200, 70, 70)
  settle(r, 201, 215)
  check("HOLDER_KEEPS_JOB off: the bot takes the new order and its bonus",
        r.st._repair_ping ~= nil and r.st._repair_ping.tid == 11,
        tostring(r.st._repair_ping and r.st._repair_ping.tid))
  C.ORDER_HOLDER_KEEPS_JOB = true
  r = BOT(1, 10, 10)
  rtake(r, 100, 20, 20)
  check("an attack ping order on an ENEMY pill gives no bonus", r.st._repair_ping == nil, "?")
  check("keel has no ping bonus", C.PRESETS.keel.BUILDER_POOL_PING_PILL_BONUS == 1.0, "?")
  C.BUILDER_POOL_PING_PILL_BONUS = 1.0
  r = BOT(1, 10, 10)
  rtake(r, 100)
  check("bonus 1.0: the row is not weighted",
        BP.goal_weight(r.st, { type = "repair", id = 9 }) == 1.0, "?")
  C.BUILDER_POOL_PING_PILL_BONUS = 2.0
end

_G.EVENT_PING = nil
_G.PING_KIND_CAUTION = nil
_G.PING_KIND_BOT_COMMAND = nil
_G.PING_KIND_ATTACK = nil

-- =========================================================================
-- ONE PING, ONE BOT (ORDER_CLAIM_TIEBREAK, 2026-09-24).  Andrew pinged "go
-- here" once and two bots both said they were coming.  Two bots that both
-- take one solo order now settle it on the crossed claims: the cheaper keeps
-- it, the other hands it over in silence and never says the ack.
-- =========================================================================
print("orders.lua -- one ping, one bot")
_G.EVENT_PING = 13
_G.PING_KIND_BOT_COMMAND = 5
_G.PING_KIND_CAUTION = 1
local function gping(b, t, mx, my)
  b.inf.events = { ping(5, 0, mx, my) }
  ORD.on_events(b.st, b.w, b.inf, t)
  b.inf.events = {}
end
-- Every bot's queued traffic reaches every other bot.
local function pumpn(bots, tick)
  local q = {}
  for i, b in ipairs(bots) do
    q[i] = {}
    for j, v in ipairs((b.st.orders or {}).out or {}) do q[i][j] = v end
    if b.st.orders then b.st.orders.out = {} end
  end
  for i, b in ipairs(bots) do
    for k, c in ipairs(bots) do
      if k ~= i then
        for _, m in ipairs(q[i]) do ORD.rx(b.pn, m, tick, c.st) end
      end
    end
  end
end
local function upd(bots, t)
  for _, b in ipairs(bots) do b.st.tick = t; ORD.update(b.st, b.w, b.inf, t) end
end
local function acks(b)
  local n = 0
  for _, l in ipairs(b.st.orders.say or {}) do
    if l:sub(-#" goto") == " goto" then n = n + 1 end
  end
  return n
end
-- A double take, made on purpose: neither bot hears the other's bid (the
-- harness has no ally slots, so each settles alone on its first update).
local function double_take()
  local a, b = BOT(1, 30, 30), BOT(2, 38, 38)     -- B is nearer (35,35)
  gping(a, 100, 35, 35); gping(b, 100, 35, 35)
  a.st.orders.out, b.st.orders.out = {}, {}      -- the bids are lost
  upd({ a, b }, 101)
  return a, b
end
do
  local a, b = double_take()
  check("setup: both bots took the one ping order",
        a.st.orders.held ~= nil and b.st.orders.held ~= nil
        and a.st.orders.held.oid == b.st.orders.held.oid, "?")
  check("setup: neither has said anything yet", acks(a) == 0 and acks(b) == 0,
        tostring(acks(a)) .. " " .. tostring(acks(b)))
  pumpn({ a, b }, 101)                           -- the claims cross
  upd({ a, b }, 103)
  check("the cheaper claim (B) keeps the order", b.st.orders.held ~= nil, "dropped")
  check("the dearer one (A) hands it over", a.st.orders.held == nil, "held")
  check("A lets go with a release, not a cancel",
        (a.st.orders.out[1] or ""):match("^/info obr ") ~= nil,
        tostring(a.st.orders.out[1]))
  check("A's hard lock goes with it",
        a.st.command_goal == nil or a.st.command_goal.kind ~= "goto_tile",
        tostring(a.st.command_goal and a.st.command_goal.kind))
  pumpn({ a, b }, 103)
  upd({ a, b }, 104)
  check("A's release re-opens nothing on B", b.st.orders.held ~= nil
        and next(b.st.orders.auctions) == nil, "?")
  a.st.orders.pings, b.st.orders.pings = {}, {}
  upd({ a, b }, 111)
  check("after the claim window only B says it is coming",
        acks(b) == 1 and acks(a) == 0,
        tostring(acks(a)) .. " " .. tostring(acks(b)))
  check("and only B puts the ON MY WAY marker down",
        #b.st.orders.pings == 1 and #a.st.orders.pings == 0,
        tostring(#a.st.orders.pings) .. " " .. tostring(#b.st.orders.pings))
  upd({ a, b }, 130)
  check("the ack is said once", acks(b) == 1, tostring(acks(b)))
end
do
  -- A cost tie goes to the lower player number, the same on both sides.
  local a, b = double_take()
  a.st.orders.held.claim_cost, b.st.orders.held.claim_cost = 7, 7
  a.st.orders.out = { "/info obc " .. a.st.orders.held.oid .. " 7" }
  b.st.orders.out = { "/info obc " .. b.st.orders.held.oid .. " 7" }
  pumpn({ a, b }, 101)
  upd({ a, b }, 103)
  check("a tie keeps the lower player number (p1)",
        a.st.orders.held ~= nil and b.st.orders.held == nil, "?")
end
do
  -- A THIRD bot sees both claims and the loser's release.  It must not
  -- re-open the order: the winner still holds it.
  local a, b = double_take()
  local c = BOT(4, 90, 90)
  gping(c, 100, 35, 35)
  c.st.orders.out = {}
  upd({ c }, 101)
  pumpn({ a, b, c }, 101)
  upd({ a, b, c }, 103)
  check("setup: only B holds it", b.st.orders.held ~= nil and a.st.orders.held == nil
        and c.st.orders.held == nil, "?")
  pumpn({ a, b, c }, 103)
  upd({ a, b, c }, 104)
  check("the third bot does not re-bid the order B still holds",
        next(c.st.orders.auctions) == nil and c.st.orders.held == nil, "?")
  local g = c.st.orders.gclaims[b.st.orders.held.oid] or {}
  check("and counts only B as its holder", g[2] ~= nil and g[1] == nil,
        tostring(g[1]) .. " " .. tostring(g[2]))
end
do
  -- The third bot hears the LOSER's claim last, so its o.claims names A.
  -- A's release must leave it naming B, the bot that still holds the
  -- order, or the steal pass has nothing to steal from.
  local a, b = double_take()
  local c = BOT(4, 90, 90)
  gping(c, 100, 35, 35)
  c.st.orders.out = {}
  upd({ c }, 101)
  local oid = a.st.orders.held.oid
  pumpn({ b, a, c }, 101)                        -- c hears B, then A
  upd({ a, b, c }, 103)
  check("setup: the third bot's o.claims names the loser",
        c.st.orders.claims[oid] ~= nil and c.st.orders.claims[oid].pn == 1,
        tostring(c.st.orders.claims[oid] and c.st.orders.claims[oid].pn))
  pumpn({ a, b, c }, 103)
  upd({ a, b, c }, 104)
  local cl = c.st.orders.claims[oid]
  check("after the loser's release o.claims names the holder (B)",
        cl ~= nil and cl.pn == 2 and cl.cost == c.st.orders.gclaims[oid][2],
        tostring(cl and cl.pn))
  -- Keel: no tiebreak, so the release clears it as before.
  C.ORDER_CLAIM_TIEBREAK = false
  local cs = { claims = { [77] = { pn = 1, cost = 9, tick = 1 } },
               gclaims = { [77] = { [1] = 9, [2] = 5 } } }
  local s = { player_number = 4, tick = 200, goal = {}, stuck_for = 0 }
  ORD.update(s, W(), I({ allies = 0x17 }), 200)   -- builds s.orders
  s.orders.claims, s.orders.gclaims = cs.claims, cs.gclaims
  ORD.rx(1, "/info obr 77", 200, s)
  ORD.update(s, W(), I({ allies = 0x17 }), 201)
  check("keel: the release just clears o.claims", s.orders.claims[77] == nil,
        tostring(s.orders.claims[77] and s.orders.claims[77].pn))
  C.ORDER_CLAIM_TIEBREAK = true
end
do
  -- AN EARLY BID.  A hears the ping first and bids; the bid reaches B before
  -- B has heard the ping.  B keeps it for its own auction.
  local a, b = BOT(1, 30, 30), BOT(2, 38, 38)
  gping(a, 100, 35, 35)
  local oid = next(a.st.orders.auctions)
  pumpn({ a, b }, 100)
  upd({ b }, 100)                                -- no auction on B yet
  gping(b, 102, 35, 35)
  local au = oid and b.st.orders.auctions[oid]
  check("an early bid is kept and joins the auction",
        au ~= nil and au.answered[1] == true and au.bids[1] ~= nil, "?")
  C.ORDER_CLAIM_TIEBREAK = false
  local a2, b2 = BOT(1, 30, 30), BOT(2, 38, 38)
  gping(a2, 100, 35, 35)
  pumpn({ a2, b2 }, 100)
  upd({ b2 }, 100)
  gping(b2, 102, 35, 35)
  local au2 = oid and b2.st.orders.auctions[oid]
  check("keel: an early bid is lost", au2 ~= nil and au2.answered[1] == nil, "?")
  C.ORDER_CLAIM_TIEBREAK = true
end
do
  C.ORDER_CLAIM_TIEBREAK = false
  local a, b = double_take()
  check("keel: the ack is said on the take",
        acks(a) == 1 and acks(b) == 1, tostring(acks(a)) .. " " .. tostring(acks(b)))
  pumpn({ a, b }, 101)
  upd({ a, b }, 103)
  check("keel: both bots drop the order on the crossed claims",
        a.st.orders.held == nil and b.st.orders.held == nil, "?")
  C.ORDER_CLAIM_TIEBREAK = true
  check("keel has no tiebreak", C.PRESETS.keel.ORDER_CLAIM_TIEBREAK == false, "?")
end
do
  -- Both bots take one attack_pill ping order on pill 5 and both start the
  -- suicide run on it.  The claims cross: the loser lets the order go, and
  -- its suicide run on that pill ends with it.  The winner keeps both.
  _G.PING_KIND_ATTACK = 3
  local a, b = BOT(1, 10, 10), BOT(2, 12, 12)
  for _, x in ipairs({ a, b }) do
    x.inf.events = { ping(5, 0, 20, 20) }
    ORD.on_events(x.st, x.w, x.inf, 100)
    x.inf.events = {}
    x.st.orders.out = {}                         -- the bids are lost
  end
  upd({ a, b }, 101)
  attack_ping(a, 105); attack_ping(b, 105)
  check("setup: both bots hold the order and run at pill 5",
        a.st.orders.held ~= nil and b.st.orders.held ~= nil
        and a.st._suicide ~= nil and b.st._suicide ~= nil
        and a.st._suicide.tid == 5 and b.st._suicide.tid == 5, "?")
  pumpn({ a, b }, 106)                           -- the claims cross
  upd({ a, b }, 107)
  local keep, lose = a, b
  if a.st.orders.held == nil then keep, lose = b, a end
  check("setup: exactly one bot keeps the order",
        keep.st.orders.held ~= nil and lose.st.orders.held == nil, "?")
  check("the tiebreak loser's suicide run ends", lose.st._suicide == nil,
        tostring(lose.st._suicide and lose.st._suicide.tid))
  check("the winner's suicide run stands", keep.st._suicide ~= nil, "?")
  _G.PING_KIND_ATTACK = nil
end

-- =========================================================================
-- ONE SLOT, ONE BOT (ORDER_CLAIM_TIEBREAK, PR #389 review item 9).  A repeat
-- ping adds one slot (want 2).  When its auction times out, two bots can
-- both take that slot: three holders on a 2-bot order.  The dearest holder
-- lets go in silence, and every bot agrees on which one it is.
-- =========================================================================
-- These rigs repeat-ping a GROUND square to add a slot, which only the
-- keel ORDER_LAND_REPEAT_ADDS does (a pill ping adds a slot the same way).
C.ORDER_LAND_REPEAT_ADDS = true
print("orders.lua -- a repeat-ping slot taken twice")
local function slot_rig()
  -- A (p1) is nearest and takes the first ping.  B (p2) is nearer than C (p4).
  local a, b, c = BOT(1, 31, 31), BOT(2, 40, 40), BOT(4, 46, 46)
  local all = { a, b, c }
  for _, x in ipairs(all) do gping(x, 100, 35, 35) end
  pumpn(all, 100); upd(all, 101)
  pumpn(all, 101); upd(all, 102)
  pumpn(all, 102); upd(all, 103)
  -- The repeat ping.  Its bids are lost, so B and C each settle alone.
  for _, x in ipairs(all) do gping(x, 200, 35, 35) end
  for _, x in ipairs(all) do
    x.st.orders.out, x.st.orders.pings, x.st.orders.say = {}, {}, {}
  end
  upd(all, 201)
  return a, b, c, all
end
local function holds(x, oid)
  return x.st.orders.held ~= nil and x.st.orders.held.oid == oid
end
do
  local a, b, c, all = slot_rig()
  local oid = a.st.orders.held and a.st.orders.held.oid
  check("setup: the repeat ping made a 2-slot order",
        oid ~= nil and a.st.orders.anchors[oid].want == 2, "?")
  check("setup: A, B and C all hold it (3 holders, 2 slots)",
        holds(a, oid) and holds(b, oid) and holds(c, oid), "?")
  check("the new takers put no marker down before the claim window",
        #b.st.orders.pings == 0 and #c.st.orders.pings == 0,
        tostring(#b.st.orders.pings) .. " " .. tostring(#c.st.orders.pings))
  pumpn(all, 201)
  upd(all, 203)
  check("the dearest holder (C) lets go", not holds(c, oid), "held")
  check("A and B keep the order", holds(a, oid) and holds(b, oid), "?")
  check("C lets go with a release, not a cancel",
        (c.st.orders.out[1] or ""):match("^/info obr " .. oid .. "$") ~= nil,
        tostring(c.st.orders.out[1]))
  check("C's marker is not due any more", c.st.orders.ack_due == nil, "due")
  pumpn(all, 203)
  upd(all, 204)
  local ga, gb = a.st.orders.gclaims[oid] or {}, b.st.orders.gclaims[oid] or {}
  check("A and B both count exactly A and B as holders",
        ga[1] and ga[2] and not ga[4] and gb[1] and gb[2] and not gb[4], "?")
  check("C's release re-opens nothing", next(a.st.orders.auctions) == nil
        and next(b.st.orders.auctions) == nil and next(c.st.orders.auctions) == nil, "?")
  upd(all, 212)
  check("after the window B puts its marker down, C none",
        #b.st.orders.pings == 1 and #c.st.orders.pings == 0,
        tostring(#b.st.orders.pings) .. " " .. tostring(#c.st.orders.pings))
  check("C says nothing", #c.st.orders.say == 0, tostring(c.st.orders.say[1]))
  upd(all, 400)
  check("C does not take the order back", not holds(c, oid), "held")
end
do
  -- The claims reach the bots in the other order: the same answer.
  local a, b, c = slot_rig()
  local oid = a.st.orders.held.oid
  pumpn({ c, b, a }, 201)
  upd({ c, b, a }, 203)
  check("reversed claim order: still C lets go, A and B keep",
        holds(a, oid) and holds(b, oid) and not holds(c, oid), "?")
end
do
  -- A holder trimmed off an attack_pill order ends its suicide run on that
  -- pill, as the solo tiebreak's loser does.  The kept holder's run stands.
  local a, b, c, all = slot_rig()
  local oid = a.st.orders.held.oid
  for _, x in ipairs(all) do
    x.st.orders.held.kind, x.st.orders.held.tid = "attack_pill", 5
    x.st._suicide = { tid = 5, since = 200 }
  end
  pumpn(all, 201)
  upd(all, 203)
  check("the trimmed holder's suicide run ends",
        not holds(c, oid) and c.st._suicide == nil,
        tostring(c.st._suicide and c.st._suicide.tid))
  check("the kept holders' suicide runs stand",
        a.st._suicide ~= nil and b.st._suicide ~= nil, "?")
end
do
  -- A cost tie between the two new takers goes to the lower player number.
  local a, b, c, all = slot_rig()
  local oid = a.st.orders.held.oid
  for _, x in ipairs({ b, c }) do
    x.st.orders.held.claim_cost = 999
    x.st.orders.gclaims[oid][x.pn] = 999
    x.st.orders.out = { "/info obc " .. oid .. " 999" }
  end
  pumpn(all, 201)
  upd(all, 203)
  check("a tie on the slot keeps the lower player number (B, p2)",
        holds(a, oid) and holds(b, oid) and not holds(c, oid), "?")
end
do
  local o = { anchors = { [5] = { want = 1, names = true } },
              gclaims = { [5] = { [1] = 3, [2] = 4 } } }
  check("a selection order is not trimmed", ORD.slot_keepers(o, 5) == nil, "trimmed")
  o.anchors[5].names = nil
  local keep, n = ORD.slot_keepers(o, 5)
  check("a ping order keeps the cheapest `want`",
        keep ~= nil and #keep == 1 and keep[1] == 1 and n == 2, "?")
  C.ORDER_CLAIM_TIEBREAK = false
  check("keel: nothing is ranked", ORD.slot_keepers(o, 5) == nil, "ranked")
  C.ORDER_CLAIM_TIEBREAK = true
end
do
  -- Keel: the old behaviour exactly.  The markers go on the take, and the
  -- two new takers both keep the added slot.
  C.ORDER_CLAIM_TIEBREAK = false
  local a, b, c, all = slot_rig()
  local oid = b.st.orders.held and b.st.orders.held.oid
  check("keel: the new takers mark at once",
        #b.st.orders.pings == 1 and #c.st.orders.pings == 1,
        tostring(#b.st.orders.pings) .. " " .. tostring(#c.st.orders.pings))
  pumpn(all, 201)
  upd(all, 203)
  check("keel: B and C both keep the added slot",
        oid ~= nil and holds(b, oid) and holds(c, oid), "?")
  C.ORDER_CLAIM_TIEBREAK = true
end

-- =========================================================================
-- TWO DECOYS ON ONE SQUARE (ORDER_GOTO_DECOY, PR #389 review item 10).  A
-- repeat ping puts a second bot on a decoy square.  When one decoy's hold
-- ends (death, caution, clock) it lets go of its own share only; the
-- partner keeps decoying until its own end.  The last one out cancels.
-- =========================================================================
print("orders.lua -- two decoys on one square")
local DHOLD = C.ORDER_GOTO_HOLD_TICKS or 500
local function decoy_pair()
  local a, b = BOT(1, 22, 30), BOT(2, 22, 34)
  local all = { a, b }
  for _, x in ipairs(all) do gping(x, 100, 22, 24) end
  pumpn(all, 100); upd(all, 101)
  pumpn(all, 101); upd(all, 102)
  for _, x in ipairs(all) do gping(x, 150, 22, 24) end
  pumpn(all, 150); upd(all, 151)
  pumpn(all, 151); upd(all, 152)
  pumpn(all, 152); upd(all, 153)
  -- A reaches the square at 210, B beside it at 220.
  a.inf.tankx, a.inf.tanky = 22 * 256 + 128, 24 * 256 + 128
  upd({ a }, 210)
  b.inf.tankx, b.inf.tanky = 23 * 256 + 128, 24 * 256 + 128
  upd({ b }, 220)
  pumpn(all, 220)
  for _, x in ipairs(all) do x.st.orders.out, x.st.orders.say = {}, {} end
  return a, b, all
end
do
  local a, b, all = decoy_pair()
  local oid = a.st.orders.held and a.st.orders.held.oid
  check("setup: both bots decoy on the one order",
        oid ~= nil and ORD.decoy_held(a.st) ~= nil and ORD.decoy_held(b.st) ~= nil
        and b.st.orders.held.oid == oid, "?")
  ORD.on_death(a.st, a.inf)
  check("a decoy death with a partner: a release, not a cancel",
        a.st.orders.out[1] == "/info obr " .. tostring(oid), tostring(a.st.orders.out[1]))
  pumpn(all, 300)
  upd({ b }, 301)
  check("the partner keeps decoying", ORD.decoy_held(b.st) ~= nil
        and b.st.orders.held.oid == oid, "dropped")
  check("the partner stops counting the dead decoy",
        (b.st.orders.gclaims[oid] or {})[1] == nil, "counted")
  -- A respawns next to the square: it must not steal the order back.
  a.inf.tankx, a.inf.tanky = 22 * 256 + 128, 25 * 256 + 128
  upd({ a }, 450)
  check("the dead decoy does not steal the order back", a.st.orders.held == nil,
        tostring(a.st.orders.held and a.st.orders.held.oid))
  b.st.orders.out = {}
  upd({ b }, 220 + DHOLD)
  check("the partner's own clock ends it, as the last holder: a cancel",
        b.st.orders.held == nil and b.st.orders.out[1] == "/info obx " .. tostring(oid),
        tostring(b.st.orders.out[1]))
end
do
  -- A caution beside A's tank only (B is two squares off it).
  local a, b, all = decoy_pair()
  local oid = a.st.orders.held.oid
  for _, x in ipairs(all) do
    x.inf.events = { ping(1, 0, 21, 24) }
    ORD.on_events(x.st, x.w, x.inf, 300)
    x.inf.events = {}
  end
  check("a caution on one decoy releases it", a.st.orders.held == nil
        and a.st.orders.say[1] == "Released", tostring(a.st.orders.say[1]))
  check("with a release, not a cancel",
        a.st.orders.out[1] == "/info obr " .. oid, tostring(a.st.orders.out[1]))
  pumpn(all, 300)
  upd(all, 301)
  check("the partner keeps decoying after the caution",
        ORD.decoy_held(b.st) ~= nil, "dropped")
end
do
  -- The clock: A arrived first, so its hold ends first.
  local a, b, all = decoy_pair()
  local oid = a.st.orders.held.oid
  upd(all, 210 + DHOLD)
  check("A's clock ends A's hold", a.st.orders.held == nil, "held")
  check("with a release, not a cancel (clock)",
        a.st.orders.out[1] == "/info obr " .. oid, tostring(a.st.orders.out[1]))
  pumpn(all, 210 + DHOLD)
  upd(all, 211 + DHOLD)
  check("B holds on to the end of its own clock", ORD.decoy_held(b.st) ~= nil, "dropped")
  check("A does not take the order back", a.st.orders.held == nil, "held")
end
do
  -- A person's cancel still reaches both.
  local a, b, all = decoy_pair()
  for _, x in ipairs(all) do
    ORD.on_chat(x.st, x.w, x.inf, 0, "cancel all", 300, true, false)
  end
  check("'cancel all' still ends both decoys",
        a.st.orders.held == nil and b.st.orders.held == nil, "?")
end
do
  -- Without ORDER_NO_HAND_BACK an obr would hand the square on, so the
  -- decoy's end stays the old cancel.
  C.ORDER_NO_HAND_BACK = false
  local a, b, all = decoy_pair()
  local oid = a.st.orders.held.oid
  ORD.on_death(a.st, a.inf)
  check("no NO_HAND_BACK: a decoy death still cancels (obx)",
        a.st.orders.out[1] == "/info obx " .. oid, tostring(a.st.orders.out[1]))
  pumpn(all, 300)
  upd({ b }, 301)
  check("no NO_HAND_BACK: and the partner drops too (the old behaviour)",
        b.st.orders.held == nil, "held")
  C.ORDER_NO_HAND_BACK = true
end
C.ORDER_LAND_REPEAT_ADDS = false

-- =========================================================================
-- A NEW ORDER RETIRES EVERY OLDER ONE (ORDER_NEW_CLEARS_ALL, 2026-09-24).
-- "I'm seeing bots go back to where I said 'go here' a while ago."
-- =========================================================================
print("orders.lua -- a new order retires the older ones")
local function settle2(a, b, t)
  pumpn({ a, b }, t); upd({ a, b }, t + 1)
  pumpn({ a, b }, t + 1); upd({ a, b }, t + 2)
  pumpn({ a, b }, t + 2); upd({ a, b }, t + 3)
end
do
  -- A takes "go here" X, then a second ping Y, which A takes too.  A lets
  -- X go (obr).  B must not pick X up.  A switching orders is the keel
  -- ORDER_HOLDER_KEEPS_JOB; with it on A keeps X (see the tests below).
  C.ORDER_HOLDER_KEEPS_JOB = false
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  gping(a, 100, 35, 35); gping(b, 100, 35, 35)
  settle2(a, b, 100)
  local x = a.st.orders.held and a.st.orders.held.oid
  check("setup: A holds the first go-there", x ~= nil and b.st.orders.held == nil, "?")
  gping(a, 200, 38, 38); gping(b, 200, 38, 38)
  settle2(a, b, 200)
  check("setup: A took the new order", a.st.orders.held ~= nil
        and a.st.orders.held.oid ~= x, "?")
  settle2(a, b, 210)
  check("B does not go back to the old square", b.st.orders.held == nil,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  check("and B has forgotten the old order", b.st.orders.known[x] == nil, "known")
  C.ORDER_NEW_CLEARS_ALL = false
  C.ORDER_NO_HAND_BACK = false
  local a2, b2 = BOT(1, 32, 32), BOT(2, 90, 90)
  gping(a2, 100, 35, 35); gping(b2, 100, 35, 35)
  settle2(a2, b2, 100)
  gping(a2, 200, 38, 38); gping(b2, 200, 38, 38)
  settle2(a2, b2, 200)
  settle2(a2, b2, 210)
  check("keel: B picks the released old order up",
        b2.st.orders.held ~= nil and b2.st.orders.held.oid == x,
        tostring(b2.st.orders.held and b2.st.orders.held.oid))
  C.ORDER_NEW_CLEARS_ALL = true
  C.ORDER_NO_HAND_BACK = true
  C.ORDER_HOLDER_KEEPS_JOB = true
end
do
  -- An order for somebody else leaves what this bot holds alone.
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  gping(a, 100, 35, 35); gping(b, 100, 35, 35)
  settle2(a, b, 100)
  local x = a.st.orders.held and a.st.orders.held.oid
  gping(a, 200, 86, 86); gping(b, 200, 86, 86)
  settle2(a, b, 200)
  check("setup: B took the new order", b.st.orders.held ~= nil, "?")
  check("A keeps the order it holds", a.st.orders.held ~= nil
        and a.st.orders.held.oid == x, tostring(a.st.orders.held and a.st.orders.held.oid))
  check("and still knows it", a.st.orders.known[x] ~= nil, "forgot")
end
do
  -- Scenario hints stay: a script re-sends them on its own clock.
  local a = BOT(1, 32, 32)
  gping(a, 100, 35, 35)
  local o = a.st.orders
  o.known[424242] = { spec = { oid = 424242, sender = ORD.HINT_SENDER }, tick = 100 }
  o.known[515151] = { spec = { oid = 515151, sender = 0 }, tick = 100 }
  ORD.clear_older_orders(a.st, a.inf, 999, 101)
  check("a hint order is kept", o.known[424242] ~= nil, "forgot")
  check("an older person's order is forgotten", o.known[515151] == nil, "kept")
  C.ORDER_NEW_CLEARS_ALL = false
  o.known[515151] = { spec = { oid = 515151, sender = 0 }, tick = 100 }
  ORD.clear_older_orders(a.st, a.inf, 999, 101)
  check("keel: nothing is forgotten", o.known[515151] ~= nil, "forgot")
  C.ORDER_NEW_CLEARS_ALL = true
  check("keel does not clear", C.PRESETS.keel.ORDER_NEW_CLEARS_ALL == false, "?")
end
do
  -- Two pings inside one auction window: X near A, then Y near B before
  -- either auction has settled.  Y must not forget X while X's auction is
  -- still open, so X is still taken.
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  gping(a, 100, 35, 35); gping(b, 100, 35, 35)
  local x = next(a.st.orders.auctions)
  gping(a, 103, 86, 86); gping(b, 103, 86, 86)
  check("an order with an open auction is not forgotten",
        x ~= nil and a.st.orders.known[x] ~= nil and a.st.orders.auctions[x] ~= nil,
        "forgot")
  settle2(a, b, 103)
  local ha = a.st.orders.held and a.st.orders.held.oid
  local hb = b.st.orders.held and b.st.orders.held.oid
  check("the first of two quick orders is still taken",
        x ~= nil and (ha == x or hb == x), tostring(ha) .. " " .. tostring(hb))
  check("and so is the second", ha ~= nil and hb ~= nil and ha ~= hb,
        tostring(ha) .. " " .. tostring(hb))
end

-- =========================================================================
-- A BOT KEEPS THE JOB A PERSON GAVE IT (ORDER_HOLDER_KEEPS_JOB, PR #393).
-- Ping one square, then another -- a tick later or a minute later -- and
-- the bot on the first stays there; a FREE bot takes the second.  With
-- nobody free, nobody goes and one bot says "All bots busy"; the order
-- waits for a bot to come free.  An order that names a bot still switches
-- it.  A repeat ping on plain ground refreshes the order and adds no bot
-- (ORDER_LAND_REPEAT_ADDS false); on a pill, base or tank it adds one.
-- =========================================================================
print("orders.lua -- a bot keeps the job a person gave it")
check("holder knob is on live", C.ORDER_HOLDER_KEEPS_JOB == true,
      tostring(C.ORDER_HOLDER_KEEPS_JOB))
check("keel: the auction winner switches",
      C.PRESETS.keel.ORDER_HOLDER_KEEPS_JOB == false, "?")
check("land-repeat knob is off live", C.ORDER_LAND_REPEAT_ADDS == false,
      tostring(C.ORDER_LAND_REPEAT_ADDS))
check("keel: a repeat ping on ground adds a bot",
      C.PRESETS.keel.ORDER_LAND_REPEAT_ADDS == true, "?")
local function holders_of(all)
  local by = {}
  for _, b in ipairs(all) do
    local h = b.st.orders.held
    if h then
      by[h.oid] = by[h.oid] or {}
      by[h.oid][#by[h.oid] + 1] = b.pn
    end
  end
  local n, list = 0, {}
  for oid, pns in pairs(by) do
    n = n + 1
    list[#list + 1] = string.format("%d:%s", oid, table.concat(pns, ","))
  end
  return by, n, table.concat(list, " ")
end
local function said_n(all, pat)
  local n = 0
  for _, b in ipairs(all) do
    for _, l in ipairs(b.st.orders.say or {}) do
      if l:match(pat) then n = n + 1 end
    end
  end
  return n
end
-- Two pings `gap` ticks apart.  W (p1) is the nearest bot to both squares.
-- Answers the bots and the first order's id.
local function two_pings(gap, nbots)
  local w, x, y = BOT(1, 31, 31), BOT(2, 60, 60), BOT(4, 70, 70)
  local all = { w, x }
  if nbots == 3 then all[3] = y end
  for _, b in ipairs(all) do gping(b, 100, 35, 35) end
  local first = next(w.st.orders.auctions)
  pumpn(all, 100)
  local t = 100
  while t < 100 + gap - 1 do
    t = t + 1
    upd(all, t); pumpn(all, t)
  end
  for _, b in ipairs(all) do gping(b, 100 + gap, 38, 38) end
  pumpn(all, 100 + gap)
  for t2 = 101 + gap, 130 + gap do upd(all, t2); pumpn(all, t2) end
  return all, first
end
for _, case in ipairs({ { 1, 2 }, { 5, 2 }, { 1, 3 }, { 5, 3 },
                        { 40, 2 }, { 250, 2 }, { 250, 3 } }) do
  local gap, nb = case[1], case[2]
  local all, first = two_pings(gap, nb)
  local by, n, desc = holders_of(all)
  local one_each = n == 2
  for _, pns in pairs(by) do if #pns ~= 1 then one_each = false end end
  check(string.format("pings %d tick(s) apart, %d bots: two orders, one bot each",
        gap, nb), one_each, desc)
  local w = all[1]
  if gap >= 5 then
    -- W took the first order before the second came: it stays on it.
    check(string.format("pings %d ticks apart, %d bots: W stays on the first order",
          gap, nb), w.st.orders.held ~= nil and w.st.orders.held.oid == first, desc)
  else
    check(string.format("pings %d tick apart, %d bots: W holds one of them", gap, nb),
          w.st.orders.held ~= nil, "none")
  end
  check(string.format("pings %d tick(s) apart, %d bots: nobody says it is leaving",
        gap, nb), said_n(all, "^Leaving") == 0, "left")
end
do
  -- A bot on a person's job answers the next auction "no".
  local w, x = BOT(1, 31, 31), BOT(2, 60, 60)
  local all = { w, x }
  for _, b in ipairs(all) do gping(b, 100, 35, 35) end
  pumpn(all, 100); upd(all, 101); pumpn(all, 101); upd(all, 102)
  check("setup: W holds the first order", w.st.orders.held ~= nil, "none")
  w.st.orders.out = {}
  gping(w, 200, 38, 38)
  local second = next(w.st.orders.auctions)
  local no = false
  for _, m in ipairs(w.st.orders.out) do
    local v = second and m:match("^/info obd " .. second .. " (%-%d+)$")
    if v and tonumber(v) <= ORD.BID_HOLD then no = true end
  end
  check("W, on a person's job, bids no with its switch cost on a new order", no,
        table.concat(w.st.orders.out, " | "))
  -- Knob off: the plain busy "no".
  C.ORDER_NO_FREE_TAKES_LOWEST = false
  w.st.orders.out = {}
  gping(w, 300, 40, 40)
  local third = nil
  for oid in pairs(w.st.orders.auctions) do
    if oid ~= second then third = oid end
  end
  no = false
  for _, m in ipairs(w.st.orders.out) do
    if third and m == "/info obd " .. third .. " -2" then no = true end
  end
  check("NO_FREE_TAKES_LOWEST off: W bids a plain -2", no,
        table.concat(w.st.orders.out, " | "))
  C.ORDER_NO_FREE_TAKES_LOWEST = true
end
do
  -- Two pings in one auction window: W bid on both before it held either,
  -- and wins both.  It takes one and offers the other (obo, then its "no").
  local w, x = BOT(1, 31, 31), BOT(2, 60, 60)
  local all = { w, x }
  for _, b in ipairs(all) do gping(b, 100, 35, 35) end
  pumpn(all, 100)
  for _, b in ipairs(all) do gping(b, 101, 38, 38) end
  pumpn(all, 101)
  upd(all, 102)
  local took, offer, no = 0, nil, nil
  for _, m in ipairs(w.st.orders.out) do
    if m:match("^/info obc ") then took = took + 1 end
    offer = offer or m:match("^/info obo (%d+)$")
    no = no or m:match("^/info obd (%d+) %-2$")
  end
  check("W won both: it takes one and offers the other (obo)",
        took == 1 and offer ~= nil and no == offer,
        table.concat(w.st.orders.out, " | "))
  pumpn(all, 102); upd(all, 103); pumpn(all, 103); upd(all, 104)
  check("X takes the offered order",
        x.st.orders.held ~= nil and tostring(x.st.orders.held.oid) == offer,
        tostring(x.st.orders.held and x.st.orders.held.oid))
  -- Keel: W takes both in turn and leaves the first; nothing is offered.
  C.ORDER_HOLDER_KEEPS_JOB = false
  local w2, x2 = BOT(1, 31, 31), BOT(2, 60, 60)
  local all2 = { w2, x2 }
  for _, b in ipairs(all2) do gping(b, 100, 35, 35) end
  pumpn(all2, 100)
  for _, b in ipairs(all2) do gping(b, 101, 38, 38) end
  pumpn(all2, 101)
  upd(all2, 102)
  local offered = false
  for _, m in ipairs(w2.st.orders.out) do
    if m:match("^/info obo ") then offered = true end
  end
  check("HOLDER_KEEPS_JOB off: no offer, W leaves one order for the other",
        not offered and said(w2, "^Leaving"), table.concat(w2.st.orders.out, " | "))
  C.ORDER_HOLDER_KEEPS_JOB = true
end
do
  -- The same two pings, and W hears its own obo and obd, as a bot in the
  -- game does (pumpn does not hand a bot its own lines).  W opens the
  -- auction on the offered order with its own "no" in it, sends no second
  -- "no", and settles it on X's bid like the other bots.
  local w, x = BOT(1, 31, 31), BOT(2, 60, 60)
  local all = { w, x }
  for _, b in ipairs(all) do gping(b, 100, 35, 35) end
  pumpn(all, 100)
  for _, b in ipairs(all) do gping(b, 101, 38, 38) end
  pumpn(all, 101)
  upd(all, 102)
  local own, offer = {}, nil
  for i, m in ipairs(w.st.orders.out) do
    own[i] = m
    offer = offer or tonumber(m:match("^/info obo (%d+)$"))
  end
  check("setup: W offers one of the two orders", offer ~= nil, table.concat(own, " | "))
  -- X is an active ally from here on, so W's auction waits for X's answer
  -- instead of settling on the think it opens (the harness has no allies).
  local AS = require("ally_state")
  if not AS.slots[2] then AS.init() end
  AS.set_info(2, 102, {})
  pumpn(all, 102)
  for _, m in ipairs(own) do ORD.rx(w.pn, m, 102, w.st) end
  upd(all, 103)
  local again = 0
  for _, m in ipairs(w.st.orders.out) do
    if offer and m:match("^/info obd " .. offer .. " ") then again = again + 1 end
  end
  check("W, hearing its own offer, opens the auction on it",
        offer ~= nil and w.st.orders.auctions[offer] ~= nil, "no auction")
  check("W sends no second 'no' for its own offer", again == 0,
        table.concat(w.st.orders.out, " | "))
  pumpn(all, 103); upd(all, 104)
  local cl = offer and w.st.orders.claims[offer]
  check("W settles its own offer on X's bid: X is the claimant",
        cl ~= nil and cl.pn == 2, tostring(cl and cl.pn))
  check("and W has no auction left on it",
        offer ~= nil and w.st.orders.auctions[offer] == nil, "still open")
  AS.clear(2)
end
do
  -- Far apart, with the knob off: the winner still switches (keel).
  C.ORDER_HOLDER_KEEPS_JOB = false
  local all = two_pings(40, 2)
  local _, n, desc = holders_of(all)
  check("HOLDER_KEEPS_JOB off: pings far apart, W switches, one order held",
        n == 1 and all[1].st.orders.held ~= nil, desc)
  C.ORDER_HOLDER_KEEPS_JOB = true
end

-- NOBODY FREE, ORDER_NO_FREE_TAKES_LOWEST OFF (keel of that knob): nobody
-- goes.  A holds a job, B holds a job, C is dead (it hears nothing).  The
-- knob-on rule is tested further down (NO FREE BOT, THE CHEAPEST SWITCHES).
C.ORDER_NO_FREE_TAKES_LOWEST = false
local function busy_pair()
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  local all = { a, b }
  for _, x in ipairs(all) do gping(x, 100, 35, 35) end
  pumpn(all, 100); upd(all, 101); pumpn(all, 101); upd(all, 102); pumpn(all, 102)
  for _, x in ipairs(all) do gping(x, 200, 86, 86) end
  pumpn(all, 200); upd(all, 201); pumpn(all, 201); upd(all, 202); pumpn(all, 202)
  for _, x in ipairs(all) do x.st.orders.say = {} end
  return a, b, all
end
local function third_ping(all)
  for _, x in ipairs(all) do gping(x, 300, 70, 70) end
  local z = next(all[1].st.orders.auctions)
  pumpn(all, 300); upd(all, 301); pumpn(all, 301); upd(all, 302); pumpn(all, 302)
  return z
end
do
  local a, b, all = busy_pair()
  local x = a.st.orders.held and a.st.orders.held.oid
  local y = b.st.orders.held and b.st.orders.held.oid
  check("setup: A holds the first order, B the second",
        x ~= nil and y ~= nil and x ~= y, tostring(x) .. " " .. tostring(y))
  local z = third_ping(all)
  check("nobody free: nobody goes, A and B keep their jobs",
        z ~= nil and a.st.orders.held.oid == x and b.st.orders.held.oid == y,
        tostring(a.st.orders.held.oid) .. " " .. tostring(b.st.orders.held.oid))
  check("one bot says all bots are busy",
        said_n(all, "^All bots busy$") == 1 and said(a, "^All bots busy$"),
        table.concat(a.st.orders.say, " | ") .. " / " .. table.concat(b.st.orders.say, " | "))
  check("the order stays known",
        a.st.orders.known[z] ~= nil and b.st.orders.known[z] ~= nil, "forgot")
  -- A's job ends: A is free and takes the waiting order.
  ORD.release_held(a.st, a.inf, nil, true, true)
  upd(all, 400)
  check("a bot that comes free takes the waiting order",
        a.st.orders.held ~= nil and a.st.orders.held.oid == z,
        tostring(a.st.orders.held and a.st.orders.held.oid))
  pumpn(all, 400); upd(all, 401)
  check("B keeps its job and counts A on the waiting order",
        b.st.orders.held.oid == y and (b.st.orders.gclaims[z] or {})[1] ~= nil, "?")
  upd(all, 500)
  check("B does not take the waiting order too", b.st.orders.held.oid == y, "took")
end
do
  -- The wait is ORDER_FOCUS_TICKS long.
  local a, b, all = busy_pair()
  local z = third_ping(all)
  ORD.release_held(a.st, a.inf, nil, true, true)
  upd({ a }, 300 + (C.ORDER_FOCUS_TICKS or 3000))
  check("after ORDER_FOCUS_TICKS a freed bot does not take the waiting order",
        a.st.orders.held == nil, tostring(a.st.orders.held and a.st.orders.held.oid))
  upd({ a }, 301 + (C.ORDER_FOCUS_TICKS or 3000))
  check("and the order is forgotten", a.st.orders.known[z] == nil, "known")
end
do
  -- A chat auction gets the same answer when nobody is free.
  local a = BOT(1, 32, 32)
  gping(a, 100, 35, 35)
  upd({ a }, 101)
  a.st.orders.say = {}
  ORD.on_chat(a.st, a.w, a.inf, 0, "attack 5", 200, true, false)
  upd({ a }, 201)
  check("a lone bot on a job answers a chat order: all bots busy",
        said(a, "^All bots busy$"), table.concat(a.st.orders.say, " | "))
end
do
  -- "No" because nobody can get there is not "busy".  Two free bots, and
  -- the target is out of reach for both (travel cost nil): nobody goes and
  -- nobody says all bots are busy.
  local real = ORD.travel_cost
  ORD.travel_cost = function() return nil end
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  local all = { a, b }
  local z = third_ping(all)
  ORD.travel_cost = real
  check("unreachable: both bots bid -1, not busy",
        z ~= nil and a.st.orders.held == nil and b.st.orders.held == nil,
        tostring(a.st.orders.held and a.st.orders.held.oid))
  check("unreachable: nobody says all bots are busy",
        said_n(all, "^All bots busy$") == 0,
        table.concat(a.st.orders.say, " | ") .. " / " .. table.concat(b.st.orders.say, " | "))
end
do
  -- One bot cannot get there (p1), the other holds a job (p2): the busy one
  -- says it, although p1 has the lower player number.
  local u, h = BOT(1, 32, 32), BOT(2, 90, 90)
  local all = { u, h }
  for _, x in ipairs(all) do gping(x, 100, 86, 86) end
  pumpn(all, 100); upd(all, 101); pumpn(all, 101); upd(all, 102); pumpn(all, 102)
  local y = h.st.orders.held and h.st.orders.held.oid
  check("setup: p2 holds a job, p1 is free", y ~= nil and u.st.orders.held == nil,
        tostring(u.st.orders.held and u.st.orders.held.oid))
  for _, x in ipairs(all) do x.st.orders.say = {} end
  local real = ORD.travel_cost
  ORD.travel_cost = function(st, ...)
    if st == u.st then return nil end
    return real(st, ...)
  end
  local z = third_ping(all)
  ORD.travel_cost = real
  check("one unreachable, one busy: nobody goes",
        z ~= nil and u.st.orders.held == nil and h.st.orders.held.oid == y, "went")
  check("one unreachable, one busy: the busy bot says all bots are busy",
        said_n(all, "^All bots busy$") == 1 and said(h, "^All bots busy$"),
        table.concat(u.st.orders.say, " | ") .. " / " .. table.concat(h.st.orders.say, " | "))
end
do
  -- Keel: the cheaper bot leaves its job for the new order.
  C.ORDER_HOLDER_KEEPS_JOB = false
  local a, b, all = busy_pair()
  local y = b.st.orders.held and b.st.orders.held.oid
  local z = third_ping(all)
  check("HOLDER_KEEPS_JOB off: the winner leaves its job for the new order",
        b.st.orders.held ~= nil and b.st.orders.held.oid == z and z ~= y,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  check("HOLDER_KEEPS_JOB off: nobody says all bots are busy",
        said_n(all, "^All bots busy$") == 0, "said")
  C.ORDER_HOLDER_KEEPS_JOB = true
end
C.ORDER_NO_FREE_TAKES_LOWEST = true

-- CHAT ORDERS.  An auction ("attack 5", a count) goes to a free bot.  An
-- order that names a bot switches it, and so do `all` and `nearby`, which
-- name every bot (or every bot near): unchanged.
local function chat_pair()
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)      -- A is nearer pill 5
  local all = { a, b }
  for _, x in ipairs(all) do gping(x, 100, 35, 35) end
  pumpn(all, 100); upd(all, 101); pumpn(all, 101); upd(all, 102); pumpn(all, 102)
  return a, b, all
end
local function chat_all(all, line, t)
  for _, x in ipairs(all) do ORD.on_chat(x.st, x.w, x.inf, 0, line, t, true, false) end
  pumpn(all, t); upd(all, t + 1); pumpn(all, t + 1); upd(all, t + 2)
end
do
  local a, b, all = chat_pair()
  local x = a.st.orders.held and a.st.orders.held.oid
  chat_all(all, "attack 5", 200)
  check("chat auction: the free bot takes it, A keeps its job",
        a.st.orders.held.oid == x and b.st.orders.held ~= nil
        and b.st.orders.held.kind == "attack_pill",
        tostring(b.st.orders.held and b.st.orders.held.kind))
  a, b, all = chat_pair()
  chat_all(all, "socrates attack 5", 200)
  check("an order that names A switches A",
        a.st.orders.held ~= nil and a.st.orders.held.kind == "attack_pill"
        and b.st.orders.held == nil, tostring(a.st.orders.held and a.st.orders.held.kind))
  a, b, all = chat_pair()
  chat_all(all, "all attack 5", 200)
  check("'all' still switches A (it names every bot)",
        a.st.orders.held ~= nil and a.st.orders.held.kind == "attack_pill"
        and b.st.orders.held ~= nil and b.st.orders.held.kind == "attack_pill", "?")
end

-- A REPEAT PING ON PLAIN GROUND REFRESHES, ON A PILL IT ADDS A BOT.
local function repeat_pair(mx, my)
  local a, b = BOT(1, 32, 32), BOT(2, 45, 45)
  local all = { a, b }
  for _, x in ipairs(all) do gping(x, 100, mx, my) end
  pumpn(all, 100); upd(all, 101); pumpn(all, 101); upd(all, 102); pumpn(all, 102)
  for _, x in ipairs(all) do x.st.orders.say, x.st.orders.out = {}, {} end
  return a, b, all
end
do
  local a, b, all = repeat_pair(35, 35)
  local x = a.st.orders.held and a.st.orders.held.oid
  check("setup: A holds the ground order, B nothing",
        x ~= nil and b.st.orders.held == nil, "?")
  for _, q in ipairs(all) do gping(q, 200, 35, 35) end
  check("a repeat ping on ground opens no auction",
        next(a.st.orders.auctions) == nil and next(b.st.orders.auctions) == nil, "opened")
  check("and the order stays a one-bot order", a.st.orders.anchors[x].want == 1,
        tostring(a.st.orders.anchors[x].want))
  check("the holder's order is refreshed",
        a.st.orders.held.expiry == 200 + (C.ORDER_FOCUS_TICKS or 3000),
        tostring(a.st.orders.held.expiry))
  check("the holder answers 'Still on it'", said(a, "^Still on it"),
        table.concat(a.st.orders.say, " | "))
  pumpn(all, 200); upd(all, 201); pumpn(all, 201); upd(all, 202)
  check("and no second bot goes", b.st.orders.held == nil,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  -- Keel: the repeat adds a slot and B takes it.
  C.ORDER_LAND_REPEAT_ADDS = true
  a, b, all = repeat_pair(35, 35)
  x = a.st.orders.held and a.st.orders.held.oid
  for _, q in ipairs(all) do gping(q, 200, 35, 35) end
  pumpn(all, 200); upd(all, 201); pumpn(all, 201); upd(all, 202)
  check("LAND_REPEAT_ADDS on: a ground repeat adds B",
        b.st.orders.held ~= nil and b.st.orders.held.oid == x,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  C.ORDER_LAND_REPEAT_ADDS = false
end
-- A repeat ping on ground whose order nobody holds any more sends a bot
-- again.  A takes the order, then A dies (or lets the order go).  A is back
-- but busy, so only B can answer the repeat.  `how` is "death" or "release".
local function ground_repeat_unmanned(how)
  local a, b = BOT(1, 32, 32), BOT(2, 60, 60)
  local all = { a, b }
  for _, q in ipairs(all) do gping(q, 100, 35, 35) end
  pumpn(all, 100); upd(all, 101); pumpn(all, 101); upd(all, 102); pumpn(all, 102)
  local x = a.st.orders.held and a.st.orders.held.oid
  if how == "death" then
    ORD.on_death(a.st, a.inf)
  else
    ORD.release_held(a.st, a.inf, nil, true)
  end
  pumpn(all, 150); upd(all, 151); pumpn(all, 151); upd(all, 152)
  a.st.stuck_for = 10000
  for _, q in ipairs(all) do q.st.orders.say = {} end
  for _, q in ipairs(all) do gping(q, 200, 35, 35) end
  pumpn(all, 200); upd(all, 201); pumpn(all, 201); upd(all, 202); pumpn(all, 202)
  upd(all, 215)
  return a, b, all, x
end
for _, how in ipairs({ "death", "release" }) do
  local a, b, all, x = ground_repeat_unmanned(how)
  check("setup (" .. how .. "): A held the ground order and holds it no more",
        x ~= nil and a.st.orders.held == nil,
        tostring(a.st.orders.held and a.st.orders.held.oid))
  check("a ground repeat after the holder's " .. how .. " sends B",
        b.st.orders.held ~= nil and b.st.orders.held.oid == x,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  check("and the order is still a one-bot order (" .. how .. ")",
        b.st.orders.anchors[x] ~= nil and b.st.orders.anchors[x].want == 1,
        tostring(b.st.orders.anchors[x] and b.st.orders.anchors[x].want))
  check("and nobody says 'Still on it' (" .. how .. ")",
        said_n(all, "^Still on it") == 0, "said")
end
do
  -- Keel: the repeat after the holder's death adds a slot as before.
  C.ORDER_LAND_REPEAT_ADDS = true
  local _, b, _, x = ground_repeat_unmanned("death")
  check("LAND_REPEAT_ADDS on: a ground repeat after a death sends B",
        b.st.orders.held ~= nil and b.st.orders.held.oid == x,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  check("LAND_REPEAT_ADDS on: and adds a slot (want 2)",
        b.st.orders.anchors[x] ~= nil and b.st.orders.anchors[x].want == 2,
        tostring(b.st.orders.anchors[x] and b.st.orders.anchors[x].want))
  C.ORDER_LAND_REPEAT_ADDS = false
end
do
  -- On a pill the repeat still adds one bot.
  local a, b, all = repeat_pair(20, 20)
  local x = a.st.orders.held and a.st.orders.held.oid
  check("setup: A holds the pill order", x ~= nil and b.st.orders.held == nil, "?")
  for _, q in ipairs(all) do gping(q, 200, 20, 20) end
  pumpn(all, 200); upd(all, 201); pumpn(all, 201); upd(all, 202)
  check("a repeat ping on a pill adds B to the same order",
        b.st.orders.held ~= nil and b.st.orders.held.oid == x
        and a.st.orders.held.oid == x, tostring(b.st.orders.held and b.st.orders.held.oid))
  check("and the pill order is a two-bot order now", a.st.orders.anchors[x].want == 2,
        tostring(a.st.orders.anchors[x].want))
end
do
  -- A decoy that is holding: a repeat ping on its square restarts the hold
  -- clock, not the 60 s travel focus.  The tank stands one square east of
  -- the square and the ping lands one square west, so it does not select
  -- the tank.
  local d2 = dbot(); arrive(d2, 200, 23, 24)
  d2.inf.events = { ping(5, 0, 21, 24) }
  ORD.on_events(d2.st, d2.w, d2.inf, 300)
  d2.inf.events = {}
  check("a repeat ping on a holding decoy restarts its hold clock",
        ORD.decoy_held(d2.st) ~= nil and d2.st.orders.held.expiry == 300 + HOLD,
        tostring(d2.st.orders.held and d2.st.orders.held.expiry))
  check("and opens no auction", next(d2.st.orders.auctions) == nil, "opened")
end
do
-- =========================================================================
-- NO FREE BOT, THE CHEAPEST BUSY ONE SWITCHES (ORDER_NO_FREE_TAKES_LOWEST,
-- Andrew, PR #393).  When no free bot can take a ping, the bot on a person's
-- job with the lowest cost to the new target drops its job and goes.  A tie
-- goes to the lower player number.  A decoy never goes.  A repeat ping's
-- extra slot goes to a bot NOT already on that order.
-- =========================================================================
print("orders.lua -- no free bot: the cheapest busy bot switches")
check("no-free knob is on live", C.ORDER_NO_FREE_TAKES_LOWEST == true,
      tostring(C.ORDER_NO_FREE_TAKES_LOWEST))
check("keel: nobody switches", C.PRESETS.keel.ORDER_NO_FREE_TAKES_LOWEST == false, "?")
do
  -- The wire: BID_HOLD - cost, and note_bid reads it back.
  local a = { bids = {}, answered = {} }
  ORD.note_bid(a, 3, ORD.BID_HOLD - 160)
  ORD.note_bid(a, 4, ORD.BID_BUSY)
  ORD.note_bid(a, 5, -1)
  check("switch bid: kept in a.hold with its cost, not in a.bids",
        a.hold[3] == 160 and a.bids[3] == nil and a.answered[3], tostring(a.hold[3]))
  check("switch bid: counts as busy for 'All bots busy'", a.busy[3] == true, "?")
  check("plain -2: busy, no switch cost", a.busy[4] == true and a.hold[4] == nil, "?")
  check("-1: neither busy nor a switch cost",
        (a.busy[5] == nil) and a.hold[5] == nil and a.answered[5], "?")
  check("switch bid still matches the receiver's pattern",
        ORD.rx(3, "/info obd 7 " .. (ORD.BID_HOLD - 160), 1, BOT(1, 1, 1).st) == true, "?")
end
-- Give one bot its own ping order, alone (nobody else hears it).
local function own_job(b, t, mx, my)
  gping(b, t, mx, my)
  settle(b, t + 1, t + 15)
  b.st.orders.out, b.st.orders.say = {}, {}
  return b.st.orders.held and b.st.orders.held.oid
end
-- Ping every bot at (mx,my) at t, carry the traffic and settle.
local function ping_all(all, t, mx, my)
  for _, x in ipairs(all) do gping(x, t, mx, my) end
  local z = nil
  for _, x in ipairs(all) do
    for oid in pairs(x.st.orders.auctions) do z = oid end
  end
  pumpn(all, t); upd(all, t + 1); pumpn(all, t + 1); upd(all, t + 2); pumpn(all, t + 2)
  return z
end
local function sayings(all)
  local s = {}
  for _, x in ipairs(all) do
    s[#s + 1] = "p" .. x.pn .. ": " .. table.concat(x.st.orders.say or {}, " | ")
  end
  return table.concat(s, " / ")
end
do
  -- (a) One bot on a job (A), the other busy escaping (B): A switches.
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  local x = own_job(a, 100, 35, 35)
  b.st.stuck_for = 10000
  local all = { a, b }
  local z = ping_all(all, 300, 70, 70)
  check("(a) one holder, no free bot: the holder takes the new ping",
        a.st.orders.held ~= nil and a.st.orders.held.oid == z and z ~= x,
        tostring(a.st.orders.held and a.st.orders.held.oid))
  check("(a) and says it left its job because no bot was free",
        said(a, "^No free bot%. Leaving goto for goto$"), sayings(all))
  check("(a) its old order is dropped (cancelled everywhere, not handed back)",
        a.st.orders.known[x] == nil and b.st.orders.known[x] == nil,
        tostring(a.st.orders.known[x]))
  check("(a) the busy bot does not go, nobody says all bots are busy",
        b.st.orders.held == nil and said_n(all, "^All bots busy$") == 0, sayings(all))
  check("(a) no order is left waiting", not (a.st.orders.known[z] or {}).unfilled
        and not (b.st.orders.known[z] or {}).unfilled, "unfilled")
end
do
  -- (b) + (f) Two holders: the cheaper one switches, and both bots name it.
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  local all = { a, b }
  local x = own_job(a, 100, 35, 35)
  local y = own_job(b, 100, 88, 88)
  local z = ping_all(all, 300, 70, 70)
  check("(b) two holders: the cheaper (B, 20+20 tiles) switches",
        b.st.orders.held ~= nil and b.st.orders.held.oid == z and z ~= y,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  check("(b) the dearer (A) keeps its job", a.st.orders.held ~= nil
        and a.st.orders.held.oid == x, tostring(a.st.orders.held and a.st.orders.held.oid))
  check("(f) A names B as the holder of the new order",
        a.st.orders.claims[z] ~= nil and a.st.orders.claims[z].pn == 2,
        tostring(a.st.orders.claims[z] and a.st.orders.claims[z].pn))
  check("(b) only B says it is leaving", said(b, "^No free bot%. Leaving")
        and said_n(all, "^No free bot") == 1, sayings(all))
  -- Tie: A at (50,52) and B at (90,88) are both 38 tiles from (70,70).
  a, b = BOT(1, 50, 52), BOT(2, 90, 88)
  all = { a, b }
  x = own_job(a, 100, 48, 48)
  y = own_job(b, 100, 92, 92)
  check("setup: A and B price (70,70) the same",
        ORD.travel_cost(a.st, a.w, a.inf, { tkind = "here", mx = 70, my = 70 })
        == ORD.travel_cost(b.st, b.w, b.inf, { tkind = "here", mx = 70, my = 70 }), "?")
  z = ping_all(all, 300, 70, 70)
  check("(b) a tie goes to the lower player number (A)",
        a.st.orders.held ~= nil and a.st.orders.held.oid == z
        and b.st.orders.held ~= nil and b.st.orders.held.oid == y,
        tostring(a.st.orders.held and a.st.orders.held.oid))
  check("(f) on a tie B names A too",
        b.st.orders.claims[z] ~= nil and b.st.orders.claims[z].pn == 1,
        tostring(b.st.orders.claims[z] and b.st.orders.claims[z].pn))
end
do
  -- A free bot still wins over every holder, even a much cheaper holder.
  local a, b = BOT(1, 68, 68), BOT(2, 10, 10)
  local all = { a, b }
  local x = own_job(a, 100, 66, 66)
  local z = ping_all(all, 300, 70, 70)
  check("a free bot far away beats a holder next door",
        b.st.orders.held ~= nil and b.st.orders.held.oid == z
        and a.st.orders.held.oid == x, tostring(b.st.orders.held and b.st.orders.held.oid))
  check("and nobody says it is leaving", said_n(all, "^No free bot") == 0, sayings(all))
end
do
  -- (c) A decoy is never taken.  The decoy (p1) bids a plain -2.
  local d = dbot(); arrive(d, 200)
  local dx = d.st.orders.held.oid
  d.st.orders.out = {}
  local h = BOT(2, 90, 90)
  local all = { d, h }
  h.st.stuck_for = 10000              -- the other bot is busy escaping
  for _, q in ipairs(all) do gping(q, 300, 100, 100) end
  local z = next(d.st.orders.auctions)
  local plain = false
  for _, m in ipairs(d.st.orders.out) do
    if m == "/info obd " .. tostring(z) .. " -2" then plain = true end
  end
  check("(c) a decoy bids a plain -2 (no switch cost)", plain,
        table.concat(d.st.orders.out, " | "))
  pumpn(all, 300); upd(all, 301); pumpn(all, 301); upd(all, 302)
  check("(c) decoy + busy bot: nobody goes, the decoy stays",
        d.st.orders.held ~= nil and d.st.orders.held.oid == dx
        and ORD.decoy_held(d.st) ~= nil and h.st.orders.held == nil, "went")
  check("(c) one bot says all bots are busy",
        said_n(all, "^All bots busy$") == 1, sayings(all))
  check("(c) and the order waits for a free bot",
        (d.st.orders.known[z] or {}).unfilled == true, "not waiting")
  -- A decoy and a holder: the holder switches, the decoy stays.
  d = dbot(); arrive(d, 200)
  dx = d.st.orders.held.oid
  h = BOT(2, 90, 90)
  local y = own_job(h, 100, 88, 88)
  all = { d, h }
  z = ping_all(all, 300, 100, 100)
  check("decoy + holder: the holder switches although the decoy is nearer",
        h.st.orders.held ~= nil and h.st.orders.held.oid == z and z ~= y,
        tostring(h.st.orders.held and h.st.orders.held.oid))
  check("decoy + holder: the decoy stays", d.st.orders.held ~= nil
        and d.st.orders.held.oid == dx and ORD.decoy_held(d.st) ~= nil, "left")
end
do
  -- (d) A repeat ping on a pill with no free bot: a DIFFERENT holder joins.
  -- A (p1) holds pill 5.  B (p2, far) and Cc (p4, nearer) hold other jobs.
  local a, b, c = BOT(1, 18, 18), BOT(2, 90, 90), BOT(4, 70, 70)
  local all = { a, b, c }
  local p = ping_all(all, 100, 20, 20)
  check("setup: A holds the pill order",
        a.st.orders.held ~= nil and a.st.orders.held.oid == p, "?")
  -- B and Cc take other jobs only they hear.
  local y = own_job(b, 150, 92, 92)
  local w = own_job(c, 150, 72, 72)
  check("setup: B and Cc hold other jobs", y ~= nil and w ~= nil and y ~= p and w ~= p, "?")
  for _, q in ipairs(all) do q.st.orders.out, q.st.orders.say = {}, {} end
  ping_all(all, 200, 20, 20)
  check("(d) the repeat puts the cheaper other holder (Cc) on the pill",
        c.st.orders.held ~= nil and c.st.orders.held.oid == p,
        tostring(c.st.orders.held and c.st.orders.held.oid))
  check("(d) A stays on the pill and B keeps its job",
        a.st.orders.held.oid == p and b.st.orders.held ~= nil and b.st.orders.held.oid == y,
        tostring(b.st.orders.held and b.st.orders.held.oid))
  check("(d) A never says it is leaving", not said(a, "^No free bot")
        and not said(a, "^Leaving"), sayings(all))
  -- B and Cc forgot the pill order when their own pings came (a newer ping
  -- retires the orders a bot does not hold: clear_older_orders), so only A
  -- still counts itself.  What every bot must agree on is the joiner: Cc.
  check("(f) A counts A and Cc on the pill order, B counts Cc",
        (a.st.orders.gclaims[p] or {})[1] ~= nil and (a.st.orders.gclaims[p] or {})[4] ~= nil
        and (b.st.orders.gclaims[p] or {})[4] ~= nil
        and (b.st.orders.gclaims[p] or {})[2] == nil,
        (function()
           local t = {}
           for _, q in ipairs(all) do
             local l = {}
             for pn in pairs(q.st.orders.gclaims[p] or {}) do l[#l + 1] = pn end
             table.sort(l)
             t[#t + 1] = "p" .. q.pn .. "={" .. table.concat(l, ",") .. "}"
           end
           return table.concat(t, " ")
         end)())
  -- With only A on a job and the other bot busy, a repeat adds nobody.
  local a2, b2 = BOT(1, 18, 18), BOT(2, 90, 90)
  local all2 = { a2, b2 }
  local p2 = ping_all(all2, 100, 20, 20)
  b2.st.stuck_for = 10000
  for _, q in ipairs(all2) do q.st.orders.say = {} end
  ping_all(all2, 200, 20, 20)
  check("(d) only the holder itself could go: nobody is added, A holds on",
        a2.st.orders.held.oid == p2 and b2.st.orders.held == nil
        and not said(a2, "^No free bot"), sayings(all2))
end
do
  -- (e) Knob off: the two holders both keep their jobs, one says busy.
  C.ORDER_NO_FREE_TAKES_LOWEST = false
  local a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  local all = { a, b }
  local x = own_job(a, 100, 35, 35)
  local y = own_job(b, 100, 88, 88)
  local z = ping_all(all, 300, 70, 70)
  check("(e) NO_FREE_TAKES_LOWEST off: nobody switches",
        a.st.orders.held.oid == x and b.st.orders.held.oid == y, "switched")
  check("(e) NO_FREE_TAKES_LOWEST off: one bot says all bots are busy",
        said_n(all, "^All bots busy$") == 1 and said_n(all, "^No free bot") == 0,
        sayings(all))
  check("(e) NO_FREE_TAKES_LOWEST off: the order waits",
        (a.st.orders.known[z] or {}).unfilled == true, "?")
  C.ORDER_NO_FREE_TAKES_LOWEST = true
  -- HOLDER_KEEPS_JOB off: the knob does nothing; the plain auction winner
  -- (the cheaper bot) switches with the old "Leaving" line.
  C.ORDER_HOLDER_KEEPS_JOB = false
  a, b = BOT(1, 32, 32), BOT(2, 90, 90)
  all = { a, b }
  x = own_job(a, 100, 35, 35)
  y = own_job(b, 100, 88, 88)
  z = ping_all(all, 300, 70, 70)
  check("(e) HOLDER_KEEPS_JOB off: plain switch, no 'No free bot' line",
        b.st.orders.held.oid == z and said(b, "^Leaving") and said_n(all, "^No free bot") == 0,
        sayings(all))
  C.ORDER_HOLDER_KEEPS_JOB = true
end
end
_G.EVENT_PING = nil
_G.PING_KIND_BOT_COMMAND = nil
_G.PING_KIND_CAUTION = nil

-- THE MAN BEING OUT DOES NOT MAKE A BOT BUSY (ORDER_MAN_OUT_TAKES).
print("orders.lua -- a bot with its man out still takes an order")
do
  local st2, inf2 = ST(), I({ allies = 0x17 })
  inf2.man_status = C.LGM_MOVING
  local busy = ORD.busy(st2, inf2)
  check("man out: not busy", busy == false, tostring(busy))
  ORD.on_chat(st2, W(), inf2, 0, "socrates attack 5", 100, true, false)
  check("man out: a named order is taken",
        st2.orders.held ~= nil and st2.orders.held.tid == 5,
        tostring(st2.orders.held and st2.orders.held.tid))
  C.ORDER_MAN_OUT_TAKES = false
  local b2, why = ORD.busy(ST(), inf2)
  check("keel: man out is busy", b2 == true and why == "man_out", tostring(why))
  C.ORDER_MAN_OUT_TAKES = true
  check("keel keeps man_out busy", C.PRESETS.keel.ORDER_MAN_OUT_TAKES == false, "?")
  local st3, inf3 = ST(), I({ allies = 0x17 })
  inf3.man_status = C.LGM_MOVING
  st3.goal = { kind = "capture_pill", target_id = 7 }
  st3._lgm_dispatch = { x = 40, y = 40, tick = 90 }
  local b3 = ORD.busy(st3, inf3)
  check("capturing with the man sent out: not busy", b3 == false, tostring(b3))
  C.ORDER_MAN_OUT_TAKES = false
  inf3.man_status = C.LGM_INTANK
  local b4, why4 = ORD.busy(st3, inf3)
  check("keel: capturing is busy", b4 == true and why4 == "capturing", tostring(why4))
  C.ORDER_MAN_OUT_TAKES = true
end

-- =========================================================================
-- DECOY GETAWAY (Andrew, 2026-09-24).  A decoy hold scores the squares
-- around it for a way out: a chain of up to DECOY_GETAWAY_MAX_STEPS
-- neighbouring squares, each shielded from the counted pills by a wall or
-- one of our pills, or out of their range.  It turns to face the chain's
-- first square, drives the chain after the first hit, and parks at its end.
-- The terrain and the shell trace are C calls, so they are stubbed here:
-- get_terrain reads a table (grass by default) and cpf.simulate_shot walks
-- a straight line in 16 wu steps out to 2100 wu, one entry per tile.
-- =========================================================================
print("decoy_getaway.lua -- the decoy getaway")
-- A function, not a do block: the file's top-level locals and this
-- section's together pass Lua's 200-locals-per-function limit.
;(function()
  local GA = require("decoy_getaway")
  local saved = { gt = _G.get_terrain, tm = _G.TERRAIN_MASK, mf = _G.TERRAIN_MINE_FLAG,
                  kl = _G.KEY_TURNLEFT, kr = _G.KEY_TURNRIGHT, kf = _G.KEY_FASTER,
                  sim = cpf.simulate_shot, block = GA.block }
  local TMAP = {}
  _G.TERRAIN_MASK = 0x0F
  _G.TERRAIN_MINE_FLAG = 0x40
  _G.EVENT_PING, _G.PING_KIND_CAUTION, _G.PING_KIND_BOT_COMMAND = 13, 1, 5
  -- The turn uses U.aim_at_f, which needs init.lua's two-argument math.atan
  -- (LuaJIT's own takes one); init.lua is not loaded here.
  local atan1 = math.atan
  if math.atan2 then
    math.atan = function(y, x) if x == nil then return atan1(y) end return math.atan2(y, x) end
  end
  _G.get_terrain = function(mx, my) return TMAP[my * 256 + mx] or C.T_GRASS end
  cpf.simulate_shot = function(ox, oy, tx, ty)
    local dx, dy = tx - ox, ty - oy
    local d = math.sqrt(dx * dx + dy * dy)
    local out, seen = {}, {}
    if d == 0 then return out end
    for st = 0, 2100, 16 do
      local mx = math.floor((ox + dx / d * st) / 256)
      local my = math.floor((oy + dy / d * st) / 256)
      local k = my * 256 + mx
      if not seen[k] then seen[k] = true; out[#out + 1] = { mx = mx, my = my } end
    end
    return out
  end
  local function near(a, b) return a and b and math.abs(a - b) < 1e-9 end
  local function pstr(path)
    local t = {}
    for i, c in ipairs(path or {}) do t[i] = c.mx .. "," .. c.my end
    return table.concat(t, " ")
  end

  check("getaway knob is on live, off in keel",
        C.DECOY_GETAWAY == true and C.PRESETS.keel.DECOY_GETAWAY == false, "?")
  check("getaway knobs: 5 steps, 1 hit, last x2, 50 ticks, wall 1.0 / 0.5",
        C.DECOY_GETAWAY_MAX_STEPS == 5 and C.DECOY_GETAWAY_HITS == 1
        and C.DECOY_GETAWAY_LAST_WEIGHT == 2.0 and C.DECOY_GETAWAY_RESCAN_TICKS == 50
        and C.DECOY_GETAWAY_WALL_FULL == 1.0 and C.DECOY_GETAWAY_WALL_DAMAGED == 0.5, "?")

  -- 1. THE REAL BLOCK TEST.  Spot S = (30,30).  Pill A (30,24) is north,
  -- pill B (37,30) is east.  T = (31,30) is S's east neighbour.  A wall at
  -- (31,29) is on A's line to T (A's line to S stays in column 30); B's line
  -- to T is open.  So block(A,T) = 1.0 (wall), block(B,T) = 0, safety 0.5.
  local wa = { pills = {
    [1] = { mx = 30, my = 24, owner = "hostile", health = 15 },
    [2] = { mx = 37, my = 30, owner = "hostile", health = 15 },
  } }
  TMAP[29 * 256 + 31] = C.T_BUILDING
  local P = GA.pill_set(wa, 30, 30)
  check("both pills reach the decoy square: P = {1,2}",
        #P == 2 and P[1].id == 1 and P[2].id == 2, tostring(#P))
  local b1, w1, sx1, sy1 = GA.block(wa, wa.pills[1], 31, 30)
  local b2, w2 = GA.block(wa, wa.pills[2], 31, 30)
  check("the wall stops pill A's shell: block 1.0, wall at (31,29)",
        b1 == 1.0 and w1 == "wall" and sx1 == 31 and sy1 == 29,
        tostring(b1) .. " " .. tostring(w1))
  check("pill B's shell arrives: block 0, open", b2 == 0 and w2 == "open", tostring(w2))
  local path, score, ctx = GA.search(wa, P, 30, 30, 5, false)
  local cT = ctx.cells[30 * 256 + 31]
  check("the wall-shielded neighbour is a getaway square with safety 0.5",
        cT and cT.ok == true and near(cT.s, 0.5), tostring(cT and cT.s))
  check("the overlay text for it is the same sum",
        GA.safety_txt(cT, 2) == "s = (1.000 + 0.000) / 2 = 0.500", GA.safety_txt(cT, 2))
  check("the overlay names the wall per pill",
        GA.term_txt(cT.terms[1]) == "p1 wall full @(31,29) = 1.000", GA.term_txt(cT.terms[1]))
  TMAP[29 * 256 + 31] = C.T_HALFBUILD
  check("a damaged wall blocks 0.5 (DECOY_GETAWAY_WALL_DAMAGED)",
        (GA.block(wa, wa.pills[1], 31, 30)) == 0.5, "?")
  TMAP[29 * 256 + 31] = C.T_FOREST
  check("a tree is not cover: the shell flies over it (open)",
        (select(2, GA.block(wa, wa.pills[1], 31, 30))) == "open", "?")
  TMAP[29 * 256 + 31] = nil
  local ours = { mx = 31, my = 29, owner = "friendly", health = 12 }
  wa.pills[3] = ours
  wa.pill_at = { [29 * 256 + 31] = { { pill = ours } } }
  local b3, w3, _, _, hp3 = GA.block(wa, wa.pills[1], 31, 30)
  check("our pill in the way blocks health / PILLS_MAX_HEALTH (12/15 = 0.8)",
        near(b3, 12 / C.PILLS_MAX_HEALTH) and w3 == "pill" and hp3 == 12, tostring(b3))
  ours.owner = "hostile"
  check("an enemy pill in the way is not a blocker (0)",
        (GA.block(wa, wa.pills[1], 31, 30)) == 0, "?")
  wa.pills[3], wa.pill_at = nil, nil
  check("out of range is 1.0 with no blocker needed",
        (GA.block(wa, wa.pills[1], 30, 33)) == 1.0
        and select(2, GA.block(wa, wa.pills[1], 30, 33)) == "range", "?")
  -- A FAILED TRACE (the C call raises, or gives nothing) is not a short
  -- shell: block 0, "error" (no cover).  goals.sea_shot_reaches says so in a
  -- 4th value; a shell that really runs out still gives nil there.
  ;(function()
  local sim0 = cpf.simulate_shot
  local G, UT0 = require("goals"), require("util")
  cpf.simulate_shot = function() error("simulate_shot failed") end
  local be, we = GA.block(wa, wa.pills[1], 31, 30)
  local r4 = select(4, G.sea_shot_reaches(wa, UT0.m2w(30), UT0.m2w(24), 31, 30))
  cpf.simulate_shot = function() return nil end
  local bn, wn0 = GA.block(wa, wa.pills[1], 31, 30)
  cpf.simulate_shot = function() return { { mx = 30, my = 24 }, { mx = 30, my = 25 } } end
  local bs, ws = GA.block(wa, wa.pills[1], 31, 30)
  local s4 = select(4, G.sea_shot_reaches(wa, UT0.m2w(30), UT0.m2w(24), 31, 30))
  cpf.simulate_shot = sim0
  check("a trace that raises: block 0, error (not 1.0 short); sea_shot_reaches 4th value true",
        be == 0 and we == "error" and r4 == true, tostring(be) .. " " .. tostring(we))
  check("a trace that gives nothing: block 0, error", bn == 0 and wn0 == "error",
        tostring(bn) .. " " .. tostring(wn0))
  check("a shell that really runs out is still 1.0 short, 4th value nil",
        bs == 1.0 and ws == "short" and s4 == nil, tostring(bs) .. " " .. tostring(ws))
  check("the error term reads 'trace error'",
        GA.term_txt({ id = 1, b = 0, why = "error" }) == "p1 trace error = 0.000",
        GA.term_txt({ id = 1, b = 0, why = "error" }))
  end)()

  -- 2. NO BLOCKER ANYWHERE: a pill right beside the square, open grass, so
  -- every square within 5 steps is in range and open.
  local wn = { pills = { [1] = { mx = 30, my = 29, owner = "hostile", health = 15 } } }
  local pn = GA.search(wn, GA.pill_set(wn, 30, 30), 30, 30, 5, false)
  check("no blocker anywhere: no chain", pn == nil, pstr(pn))
  check("no counted pill: no P, no chain",
        #GA.pill_set({ pills = {} }, 30, 30) == 0, "?")

  -- 3. THE CHAIN SCORE (C2) on made-up safety values: GA.block is replaced
  -- by a table of square -> block, and P has one pill, so safety = block.
  local SAFE = {}
  GA.block = function(world, p, mx, my)
    local v = SAFE[my * 256 + mx]
    if v then return v, "wall", mx, my - 1 end
    return 0, "open"
  end
  local P1 = { { id = 1, pill = { mx = 0, my = 0 } } }
  local wg = { pills = {} }
  local function S(list) SAFE = {}; for _, e in ipairs(list) do SAFE[e[2] * 256 + e[1]] = e[3] end end
  -- a longer chain beats a shorter weaker one: east 3 x 0.5 = 0.5+0.5+2x0.5
  -- = 2.0 against west 1 x 0.5 = 2x0.5 = 1.0.
  S({ { 49, 50, 0.5 }, { 51, 50, 0.5 }, { 52, 50, 0.5 }, { 53, 50, 0.5 } })
  local p3, s3 = GA.search(wg, P1, 50, 50, 5, false)
  check("a longer chain beats a shorter weaker one (2.0 vs 1.0)",
        pstr(p3) == "51,50 52,50 53,50" and near(s3, 2.0), pstr(p3) .. " " .. tostring(s3))
  -- one fully safe square west (2x1.0 = 2.0) against five 0.4 squares east
  -- (4x0.4 + 2x0.4 = 2.4): the long one wins ...
  S({ { 49, 50, 1.0 }, { 51, 50, 0.4 }, { 52, 50, 0.4 }, { 53, 50, 0.4 },
      { 54, 50, 0.4 }, { 55, 50, 0.4 } })
  local p4, s4 = GA.search(wg, P1, 50, 50, 5, false)
  check("C2: five 0.4 squares (2.4) beat one safe square (2.0)",
        pstr(p4) == "51,50 52,50 53,50 54,50 55,50" and near(s4, 2.4),
        pstr(p4) .. " " .. tostring(s4))
  check("the score written out is the one the code adds up",
        GA.score_terms(p4) == "0.400 + 0.400 + 0.400 + 0.400 + 2x0.400", GA.score_terms(p4))
  -- ... and five 0.3 squares (4x0.3 + 2x0.3 = 1.8) lose to it.
  S({ { 49, 50, 1.0 }, { 51, 50, 0.3 }, { 52, 50, 0.3 }, { 53, 50, 0.3 },
      { 54, 50, 0.3 }, { 55, 50, 0.3 } })
  local p5, s5 = GA.search(wg, P1, 50, 50, 5, false)
  check("C2: one safe square (2.0) beats five 0.3 squares (1.8)",
        pstr(p5) == "49,50" and near(s5, 2.0), pstr(p5) .. " " .. tostring(s5))
  -- The last square counts twice, so a weak last square is left off:
  -- 1.0 + 2x1.0 = 3.0 beats 1.0 + 1.0 + 2x0.1 = 2.2.
  S({ { 51, 50, 1.0 }, { 52, 50, 1.0 }, { 53, 50, 0.1 } })
  local p6, s6 = GA.search(wg, P1, 50, 50, 5, false)
  check("a weak last square is left off (3.0 beats 2.2)",
        pstr(p6) == "51,50 52,50" and near(s6, 3.0), pstr(p6) .. " " .. tostring(s6))
  -- 4. THE CAP: eight safe squares in a line north, the chain stops at 5.
  S({ { 50, 49, 1 }, { 50, 48, 1 }, { 50, 47, 1 }, { 50, 46, 1 },
      { 50, 45, 1 }, { 50, 44, 1 }, { 50, 43, 1 }, { 50, 42, 1 } })
  local p7, s7 = GA.search(wg, P1, 50, 50, 5, false)
  check("the chain is capped at 5 squares (1+1+1+1+2x1 = 6)",
        #p7 == 5 and pstr(p7) == "50,49 50,48 50,47 50,46 50,45" and near(s7, 6.0),
        pstr(p7))
  -- 5. NO CORNER CUT: the only getaway square is diagonal (51,49); a wall
  -- on either square beside the step refuses it.
  S({ { 51, 49, 1.0 } })
  TMAP[49 * 256 + 50] = C.T_BUILDING
  check("a diagonal step past a wall corner is refused",
        GA.search(wg, P1, 50, 50, 5, false) == nil, "?")
  TMAP[49 * 256 + 50] = nil
  TMAP[50 * 256 + 51] = C.T_HALFBUILD
  check("... on either side", GA.search(wg, P1, 50, 50, 5, false) == nil, "?")
  TMAP[50 * 256 + 51] = nil
  check("with both sides drivable the diagonal step is fine",
        pstr((GA.search(wg, P1, 50, 50, 5, false))) == "51,49", "?")
  -- Not drivable: a mine, deep sea without a boat.
  TMAP[49 * 256 + 51] = C.T_GRASS + 0x40
  check("a known mine is not drivable", GA.search(wg, P1, 50, 50, 5, false) == nil, "?")
  TMAP[49 * 256 + 51] = C.T_DEEPSEA
  check("deep sea is not drivable without a boat",
        GA.search(wg, P1, 50, 50, 5, false) == nil, "?")
  check("... and is in a boat", GA.search(wg, P1, 50, 50, 5, true) ~= nil, "?")
  TMAP[49 * 256 + 51] = nil
  -- 6. THE SAME RESULT EVERY TIME, and a tie goes to the lower first key:
  -- one 1.0 square north (50,49) and one south (50,51), two apart, so no
  -- chain joins them; north has the lower key.
  S({ { 50, 49, 1.0 }, { 50, 51, 1.0 } })
  local a1 = pstr((GA.search(wg, P1, 50, 50, 5, false)))
  local a2 = pstr((GA.search(wg, P1, 50, 50, 5, false)))
  check("a tie goes to the lower first-square key, the same twice",
        a1 == "50,49" and a2 == a1, a1 .. " / " .. a2)
  -- A tie in score goes to the SHORTER chain: 2x1.0 = 2.0 south against
  -- 0.5 + 0.5 + 2x0.5 = 2.0 north.  South (50,51) has the higher key, so
  -- only the length rule picks it.
  S({ { 50, 51, 1.0 }, { 50, 49, 0.5 }, { 50, 48, 0.5 }, { 50, 47, 0.5 } })
  local pt = GA.search(wg, P1, 50, 50, 5, false)
  check("a tie in score goes to the shorter chain", pstr(pt) == "50,51", pstr(pt))

  -- 6b. OUTWARD ONLY.  Every step goes one ring further from the start.
  -- Two 1.0 squares side by side on ring 1: a chain (51,50) -> (51,49)
  -- would score 1 + 2x1 = 3, but that step is sideways (ring 1 to ring 1),
  -- so the best is one square, 2x1 = 2, and the tie goes to the lower key.
  S({ { 51, 50, 1.0 }, { 51, 49, 1.0 } })
  local po, so = GA.search(wg, P1, 50, 50, 5, false)
  check("a sideways step (same ring) is refused",
        pstr(po) == "51,49" and near(so, 2.0), pstr(po) .. " " .. tostring(so))
  -- A backward step: (50,49) -> (50,48) -> (49,49) would be 1 + 1 + 2x1 = 4,
  -- but (49,49) is back on ring 1.  Outward only, the best is 1 + 2x1 = 3,
  -- and two ways reach (50,48): from (49,49) and from (50,49).  The DP keeps
  -- the lower first key, (49,49).
  S({ { 50, 49, 1.0 }, { 50, 48, 1.0 }, { 49, 49, 1.0 } })
  local pb, sb = GA.search(wg, P1, 50, 50, 5, false)
  check("a backward step (ring 2 to ring 1) is refused",
        #pb == 2 and near(sb, 3.0), pstr(pb) .. " " .. tostring(sb))
  check("two ways to one square: the DP keeps the lower first key",
        pstr(pb) == "49,49 50,48", pstr(pb))
  -- Only reached squares are worked out: open grass all round costs ring 1.
  GA.block = saved.block
  local wo = { pills = { [1] = { mx = 30, my = 29, owner = "hostile", health = 15 } } }
  local _, _, cto = GA.search(wo, GA.pill_set(wo, 30, 30), 30, 30, 5, false)
  check("an open field is worked out on ring 1 only (8 squares)", #cto.list == 8,
        tostring(#cto.list))
  GA.block = function(world, p, mx, my)
    local v = SAFE[my * 256 + mx]
    if v then return v, "wall", mx, my - 1 end
    return 0, "open"
  end

  -- 6c. CLOSENESS.  tile(t) = safety(t) + DECOY_GETAWAY_PROX_WEIGHT * prox,
  -- prox = max(0, 1 - d / PILL_FIRE_RANGE), d = distance to the nearest pill.
  check("closeness knob: 0.5 live, 0 in keel",
        C.DECOY_GETAWAY_PROX_WEIGHT == 0.5 and C.PRESETS.keel.DECOY_GETAWAY_PROX_WEIGHT == 0, "?")
  local PP = { { id = 1, pill = { mx = 40, my = 50 } } }
  -- Andrew's path A: t1 safety 0.5 at d = 3, t2 safety 1.0 at d = 4.
  -- (0.5 + 0.5x0.625) + 2x(1.0 + 0.5x0.5) = 0.8125 + 2.5 = 3.3125.
  S({ { 43, 50, 0.5 }, { 44, 50, 1.0 } })
  local pA, sA = GA.search(wg, PP, 42, 50, 5, false)
  check("Andrew's path A scores (0.5+0.3125) + 2x(1.0+0.25) = 3.3125",
        pstr(pA) == "43,50 44,50" and near(sA, 3.3125), pstr(pA) .. " " .. tostring(sA))
  check("the score written out uses the tile values",
        GA.score_terms(pA) == "0.812 + 2x1.250", GA.score_terms(pA))
  check("the panel shows d, prox and weight x prox",
        GA.prox_txt(pA[1]) == "prox: d=3.00, max(0, 1 - 3.00/8) = 0.625, x0.5 = 0.312"
        and GA.tile_txt(pA[1]) == "tile = 0.500 + 0.312 = 0.812",
        GA.prox_txt(pA[1]) .. " | " .. GA.tile_txt(pA[1]))
  -- Path B's squares: d = 6 gives 0.5x0.25 = 0.125; d = 8 or more gives 0.
  -- B = (0.5 + 0.125) + 2x(1.0 + 0) = 2.625 < 3.3125, so A wins.  (Its two
  -- squares cannot be neighbours on the map -- one step moves d by 1.41 at
  -- most -- so each one is checked on its own.)
  S({ { 46, 50, 0.5 } })
  local _, s6 = GA.search(wg, PP, 45, 50, 5, false)
  check("d = 6: tile 0.5 + 0.125, one square scores 2x0.625 = 1.25", near(s6, 1.25), tostring(s6))
  S({ { 48, 50, 1.0 } })
  local _, s8 = GA.search(wg, PP, 47, 50, 5, false)
  check("d = 8: no closeness, one square scores 2x1.0 = 2.0", near(s8, 2.0), tostring(s8))
  check("Andrew's example: A 3.3125 beats B (0.5+0.125) + 2x(1.0+0) = 2.625",
        sA > (0.5 + 0.125) + 2 * (1.0 + 0) + 0.5, "?")
  -- Head to head from one start (44,50), pill (40,50).  West: (43,50) 0.5
  -- d=3, (42,50) 1.0 d=2.  North-east: (44,49) 0.5 d=4.12, (45,48) 1.0 d=5.39.
  -- Weight 0: both 0.5 + 2x1.0 = 2.5, and north-east wins on its lower first
  -- key.  Weight 0.5: west is (0.5+0.3125) + 2x(1.0+0.375) = 3.5625, north-
  -- east about 3.069, so the square closer to the pill wins.
  S({ { 43, 50, 0.5 }, { 42, 50, 1.0 }, { 44, 49, 0.5 }, { 45, 48, 1.0 } })
  C.DECOY_GETAWAY_PROX_WEIGHT = 0
  local ph0, sh0 = GA.search(wg, PP, 44, 50, 5, false)
  check("weight 0: the two paths tie at 2.5, the lower first key wins",
        pstr(ph0) == "44,49 45,48" and near(sh0, 2.5), pstr(ph0) .. " " .. tostring(sh0))
  C.DECOY_GETAWAY_PROX_WEIGHT = 0.5
  local ph1, sh1 = GA.search(wg, PP, 44, 50, 5, false)
  check("weight 0.5: the path nearer the pill wins (3.5625)",
        pstr(ph1) == "43,50 42,50" and near(sh1, 3.5625), pstr(ph1) .. " " .. tostring(sh1))
  -- Closeness never makes an open square usable: the pill's own neighbour
  -- is open, so it is not a getaway square however close it is.
  S({})
  check("an open square next to the pill is still not a getaway square",
        GA.search(wg, PP, 42, 50, 5, false) == nil, "?")

  -- 7. THE HOLD, ONE HIT ONE STEP.  dbot's decoy square is (22,24) beside
  -- pill 5 (20,20).  A chain of three 1.0 squares runs south: (22,25)
  -- (22,26) (22,27).  Closeness is off here so the scores stay whole.  The
  -- blocker step is off through section 11 (open grass: it would step on
  -- every square); section 12 tests it.
  C.DECOY_GETAWAY_PROX_WEIGHT = 0
  C.DECOY_GETAWAY_BLOCKER_STEP = false
  _G.KEY_TURNLEFT, _G.KEY_TURNRIGHT, _G.KEY_FASTER = 0x04, 0x08, 0x10
  S({ { 22, 25, 1.0 }, { 22, 26, 1.0 }, { 22, 27, 1.0 } })
  local d = dbot(); arrive(d, 200)
  d.inf.direction = 64                         -- facing east
  lock(d, 201)
  local h = d.st.orders.held
  check("the scan runs as the decoy starts: chain of 3, score 1+1+2x1 = 4",
        h.ga and pstr(h.ga.path) == "22,25 22,26 22,27" and near(h.ga.score, 4.0)
        and h.ga.phase == "wait" and h.ga.used == 0, h.ga and pstr(h.ga.path))
  check("before the hit the hold goal stays on the decoy square",
        d.st.goal.kind == "goto_tile" and d.st.goal.mx == 22 and d.st.goal.my == 24
        and ORD.hold_parked(d.st, d.inf) == true, tostring(d.st.goal.mx))
  local k1, t1 = ORD.decoy_keys(d.st, d.inf, 0x11, 0x04)
  check("it turns to face the first square (south, from east: turn right), no throttle",
        k1 == 0x01 + 0x08 and t1 == 0, string.format("%x %x", k1, t1))
  d.inf.direction = 128
  local k2 = ORD.decoy_keys(d.st, d.inf, 0x05, 0)
  check("facing it: no turn key (and the other turn keys are cleared)", k2 == 0x01,
        string.format("%x", k2))
  d.st.goal = { kind = "attack_tank", target_id = 7, mx = 25, my = 26 }
  lock(d, 202)
  check("a fight in range still wins before the hit", d.st.goal.kind == "attack_tank", "?")
  check("and a fight aims on its own: no getaway turn",
        ORD.decoy_keys(d.st, d.inf, 0x05, 0) == 0x05, "?")
  d.st.goal = ORD.decoy_goal(h)
  -- HIT 1 on the decoy square.
  d.inf.armour = 35
  lock(d, 204)
  check("one armour loss: it moves, the goal is square 1",
        h.ga.phase == "move" and d.st.goal.mx == 22 and d.st.goal.my == 25
        and d.st.goal._getaway == true, tostring(h.ga.phase))
  check("moving is not parked: the throttle is left alone",
        ORD.hold_parked(d.st, d.inf) == false and ORD.decoy_keys(d.st, d.inf, 0x11, 0) == 0x11, "?")
  d.st.goal = { kind = "attack_tank", target_id = 7, mx = 23, my = 24 }
  lock(d, 205)
  check("moving: it never leaves the chain to fight",
        d.st.goal.kind == "goto_tile" and d.st.goal.my == 25, tostring(d.st.goal.kind))
  -- A hit ON THE WAY does not count.
  d.inf.armour = 30
  lock(d, 206)
  check("a hit on the way is not counted: still moving to square 1",
        h.ga.phase == "move" and d.st.goal.my == 25 and h.ga.hits == 1, tostring(h.ga.hits))
  d.inf.tankx, d.inf.tanky = 22 * 256 + 128, 25 * 256 + 128
  lock(d, 210)
  check("on square 1: it parks there (one hit, one step)",
        h.ga.phase == "wait" and h.ga.used == 1 and d.st.goal.mx == 22 and d.st.goal.my == 25
        and ORD.hold_parked(d.st, d.inf) == true, tostring(h.ga.phase) .. " " .. tostring(d.st.goal.my))
  check("the armour on arrival is the new baseline", h.ga.arm == 30 and h.ga.hits == 0,
        tostring(h.ga.arm))
  -- The overlay, drawn into a recorder: the header says the step, and each
  -- chain square's panel carries the pill terms, the closeness and the tile.
  -- draw_rec(only): only = one overlay id (nil = all six on).  Every call
  -- is recorded with the overlay id it was drawn under.
  local function draw_rec(only)
    local r = { text = {}, detail = {}, circle = 0, line = 0, rect = 0, ids = {}, lines = {} }
    local function seen(id) r.ids[id] = (r.ids[id] or 0) + 1 end
    local V = {
      is_on  = function(id)
        if only then return id == only end
        for _, x in ipairs(GA.VIZ_IDS) do if x == id then return true end end
        return false
      end,
      rect   = function(id) seen(id); r.rect = r.rect + 1 end,
      line   = function(id, x1, y1, x2, y2) seen(id); r.line = r.line + 1
                 r.lines[#r.lines + 1] = { x1, y1, x2, y2 } end,
      circle = function(id) seen(id); r.circle = r.circle + 1 end,
      text   = function(id, x, y, t) seen(id); r.text[#r.text + 1] = t end,
      detail = function(did, kind, g, label, lines) r.detail[#r.detail + 1] = { label, lines } end,
    }
    ORD.draw_getaway(V, d.st)
    return r
  end
  local function has(list, pat)
    for _, t in ipairs(list) do if t:find(pat, 1, true) then return true end end
    return false
  end
  -- The opt/ (production) copy has every viz call stripped, so it draws
  -- nothing; the overlay checks run on the source copy only.
  local gsrc = debug.getinfo(GA.draw, "S").source:gsub("^@", "")
  local gf = io.open(gsrc, "rb")
  local drawn = gf and gf:read("*a"):find("viz.text(ID", 1, true) ~= nil
  if gf then gf:close() end
  if not drawn then print("  skip overlay checks: stripped copy (" .. gsrc .. ")") end
  local r1 = drawn and draw_rec()
  if drawn then
  check("overlay header: on square #1, waiting for hit 1, then the score line",
        has(r1.text, "GETAWAY step 1/3 on square #1, waiting for hit 1 of 1")
        and has(r1.text, "score = 1.000 + 1.000 + 2x1.000 = 4.000")
        and has(r1.text, "P=1 tiles="),
        table.concat(r1.text, " | "))
  -- An overlay text is cut at OVERLAY_TEXT_MAX (128) bytes by the host.
  local longest = 0
  for _, t in ipairs(r1.text) do if #t > longest then longest = #t end end
  check("overlay: every text fits in 127 bytes", longest <= 127, tostring(longest))
  check("overlay: one panel per chain square, numbered from the decoy square",
        #r1.detail == 3 and r1.detail[2][1] == "GETAWAY #2 (22,26)", tostring(#r1.detail))
  check("overlay panel: pill term, closeness term and tile value",
        #r1.detail[2][2] == 4 and r1.detail[2][2][2]:find("^p5 ") ~= nil
        and r1.detail[2][2][3]:find("^prox: d=") ~= nil
        and r1.detail[2][2][4] == "tile = 1.000 + 0.000 = 1.000",
        table.concat(r1.detail[2][2], " | "))
  check("overlay: the pill in P is marked", r1.circle == 1 and has(r1.text, "P p5"), tostring(r1.circle))
  -- SIX OVERLAYS (category Decoy): each one draws only under its own id,
  -- and the six together draw exactly the sum of the six alone.
  ;(function()  -- own function: main is at the 200-local cap
  -- The blocker step is off in this rig; on here so its line is drawn.
  local keep_blk = C.DECOY_GETAWAY_BLOCKER_STEP
  C.DECOY_GETAWAY_BLOCKER_STEP = true
  local r1 = draw_rec()
  check("overlay: six decoy ids", #GA.VIZ_IDS == 6, tostring(#GA.VIZ_IDS))
  local sum_t, sum_l, sum_r, sum_c, sum_d = 0, 0, 0, 0, 0
  local alone = {}
  for _, id in ipairs(GA.VIZ_IDS) do
    local ra = draw_rec(id)
    alone[id] = ra
    local only_own = true
    for k in pairs(ra.ids) do if k ~= id then only_own = false end end
    check("overlay " .. id .. " alone: draws something, only under its own id",
          only_own and (ra.ids[id] or 0) > 0, tostring(ra.ids[id]))
    sum_t, sum_l, sum_r = sum_t + #ra.text, sum_l + ra.line, sum_r + ra.rect
    sum_c, sum_d = sum_c + ra.circle, sum_d + #ra.detail
  end
  check("overlay: all six on = the sum of the six alone",
        #r1.text == sum_t and r1.line == sum_l and r1.rect == sum_r
        and r1.circle == sum_c and #r1.detail == sum_d,
        string.format("%d/%d %d/%d %d/%d", #r1.text, sum_t, r1.line, sum_l, r1.rect, sum_r))
  local ch = alone.decoy_chain
  check("Decoy: chain: numbered squares, the park square, 'scan from', no score",
        has(ch.text, "#1 hit park") and has(ch.text, "#2") and has(ch.text, "scan from")
        and not has(ch.text, "score =") and not has(ch.text, "GETAWAY step"),
        table.concat(ch.text, " | "))
  local tm = alone.decoy_score_terms
  check("Decoy: score terms: the score line and one panel per chain square",
        has(tm.text, "score = 1.000 + 1.000 + 2x1.000 = 4.000") and #tm.detail == 3
        and has(tm.text, "tile = 1.000 + 0.000 = 1.000") and has(tm.text, "prox: d=")
        and has(tm.text, "x2 (last)"), table.concat(tm.text, " | "))
  local pl = alone.decoy_pill_lines
  check("Decoy: pill lines: the pill and one line per chain square",
        pl.circle == 1 and has(pl.text, "P p5") and pl.line == 3, tostring(pl.line))
  local st = alone.decoy_status
  check("Decoy: status header: the order, the step, the scan cost",
        has(st.text, "ORDER decoy (") and has(st.text, "GETAWAY step 1/3 on square #1")
        and has(st.text, "P=1 tiles=") and #st.text == 3, table.concat(st.text, " | "))
  local bl = alone.decoy_blocker_count
  check("Decoy: blocker count: the count's state line",
        has(bl.text, "| steps by: hit"), table.concat(bl.text, " | "))
  local cl = alone.decoy_scan_cells
  check("Decoy: scan cells: one text per worked-out square",
        #cl.text == #d.st.orders.held.ga.viz.cells and not has(cl.text, "score ="),
        tostring(#cl.text))
  C.DECOY_GETAWAY_BLOCKER_STEP = keep_blk
  end)()
  end
  d.inf.direction = 64
  check("parked on square 1 it turns to face square 2 (south)",
        ORD.decoy_keys(d.st, d.inf, 0x11, 0) == 0x01 + 0x08, "?")
  d.inf.direction = 128
  lock(d, 212); lock(d, 230)
  check("no new hit: it stays on square 1",
        h.ga.phase == "wait" and d.st.goal.my == 25, tostring(h.ga.phase))
  -- HIT 2, after arrival: one more square.
  d.inf.armour = 25
  lock(d, 232)
  check("a new hit after arrival: it moves to square 2",
        h.ga.phase == "move" and d.st.goal.my == 26, tostring(d.st.goal.my))
  if drawn then
    check("overlay header while moving: moving to square #2",
          has(draw_rec().text, "moving to square #2"), "?")
    ;(function()
    local mv = draw_rec("decoy_chain")
    local tl = mv.lines[#mv.lines]
    check("Decoy: chain while moving: 'MOVING to (22,26)' and a line from the tank to it",
          has(mv.text, "MOVING to (22,26)") and not has(mv.text, "park")
          and tl and tl[1] == d.inf.tankx / 256 and tl[2] == d.inf.tanky / 256
          and tl[3] == 22.5 and tl[4] == 26.5, table.concat(mv.text, " | "))
    end)()
  end
  d.inf.tanky = 26 * 256 + 128; lock(d, 238)
  check("on square 2: parked, waiting", h.ga.phase == "wait" and h.ga.used == 2
        and d.st.goal.my == 26, tostring(h.ga.phase))
  d.inf.armour = 20; lock(d, 240)
  d.inf.tanky = 27 * 256 + 128; lock(d, 246)
  check("at the end of the chain: done, parked on the last square",
        h.ga.phase == "done" and h.ga.used == 3 and d.st.goal.mx == 22 and d.st.goal.my == 27
        and ORD.hold_parked(d.st, d.inf) == true, tostring(h.ga.phase))
  d.inf.armour = 15; lock(d, 248)
  check("a hit at the end changes nothing", h.ga.phase == "done" and d.st.goal.my == 27, "?")
  check("parked at the end: no throttle, no turn",
        ORD.decoy_keys(d.st, d.inf, 0x11, 0) == 0x01, "?")
  d.st.goal = { kind = "attack_tank", target_id = 7, mx = 23, my = 30 }
  check("parked at the end: a fight in range of the tank is allowed again",
        lock(d, 249) and d.st.goal.kind == "attack_tank", tostring(d.st.goal.kind))
  d.inf.tanky = 30 * 256 + 128
  d.st.goal = { kind = "take_cover", mx = 3, my = 3 }
  lock(d, 250)
  check("pushed off the last square: the hold goal drives back to it",
        d.st.goal.kind == "goto_tile" and d.st.goal.my == 27, tostring(d.st.goal.my))
  d.inf.tanky = 27 * 256 + 128
  -- The hold's endings at the end of the chain.
  ORD.update(d.st, d.w, d.inf, 200 + HOLD - 1)
  check("the hold stands at the chain's end until the clock", d.st.orders.held ~= nil, "ended")
  ORD.update(d.st, d.w, d.inf, 200 + HOLD)
  check("the 10 s clock ends it at the chain's end", d.st.orders.held == nil, "held")
  local function at_end()
    local b = dbot(); arrive(b, 200); b.inf.direction = 128
    lock(b, 201)
    local arm = b.inf.armour
    for i, y in ipairs({ 25, 26, 27 }) do
      arm = arm - 5; b.inf.armour = arm; lock(b, 200 + 10 * i)
      b.inf.tanky = y * 256 + 128; lock(b, 205 + 10 * i)
    end
    return b
  end
  local e = at_end()
  check("at_end rig is parked at the end", e.st.orders.held.ga.phase == "done", "?")
  e.inf.events = { ping(1, 0, 23, 27) }
  ORD.on_events(e.st, e.w, e.inf, 300)
  e.inf.events = {}
  check("a caution beside the tank at the chain's end releases it",
        e.st.orders.held == nil, "held")
  e = at_end()
  e.w.pills[5].health = 0
  check("pills down ends it at the chain's end",
        lock(e, 300) == false and e.st.orders.held == nil, "held")
  e = at_end()
  ORD.on_chat(e.st, e.w, e.inf, 0, "cancel", 300, true, false)
  check("cancel ends it at the chain's end", e.st.orders.held == nil, "held")
  e = at_end()
  ORD.on_death(e.st, e.inf)
  check("death ends it at the chain's end", e.st.orders.held == nil, "held")

  -- 7b. A RESCAN WHILE PARKED ON A CHAIN SQUARE runs from that square with
  -- the steps that are left.  Parked on (22,25) after one step; a longer
  -- chain south appears and a new pill counts (the change).  The fresh scan
  -- from (22,25) has 5 - 1 = 4 steps: (22,26) .. (22,29).
  d = dbot(); arrive(d, 200); lock(d, 201)
  h = d.st.orders.held
  d.inf.armour = 35; lock(d, 202)
  d.inf.tanky = 25 * 256 + 128; lock(d, 204)
  check("rescan rig: parked on square 1", h.ga.phase == "wait" and h.ga.used == 1, "?")
  S({ { 22, 25, 1.0 }, { 22, 26, 1.0 }, { 22, 27, 1.0 }, { 22, 28, 1.0 },
      { 22, 29, 1.0 }, { 22, 30, 1.0 }, { 22, 31, 1.0 } })
  d.w.pills[6] = { mx = 26, my = 24, owner = "hostile", health = 5 }
  lock(d, 240)
  check("no rescan before the check interval", pstr(h.ga.path) == "22,25 22,26 22,27", pstr(h.ga.path))
  lock(d, 260)
  check("the rescan runs from the parked square with the steps left (4)",
        pstr(h.ga.path) == "22,26 22,27 22,28 22,29" and h.ga.viz.sx == 22
        and h.ga.viz.sy == 25 and h.ga.idx == 1 and h.ga.used == 1, pstr(h.ga.path))
  S({ { 22, 25, 1.0 }, { 22, 26, 1.0 }, { 22, 27, 1.0 } })

  -- 8. THE NEXT SQUARE STOPS BEING DRIVABLE while it moves: a fresh scan
  -- from where the tank is with the steps that are left; no chain = park.
  d = dbot(); arrive(d, 200); lock(d, 201)
  h = d.st.orders.held
  d.inf.armour = 35; lock(d, 202)
  d.inf.tanky = 25 * 256 + 128; lock(d, 203)
  d.inf.armour = 30; lock(d, 204)
  check("walled rig: moving to square 2", h.ga.phase == "move" and d.st.goal.my == 26, "?")
  TMAP[26 * 256 + 22] = C.T_BUILDING
  lock(d, 205)
  check("next square walled: fresh scan finds nothing, parks where it is",
        h.ga.phase == "done" and h.ga.park_mx == 22 and h.ga.park_my == 25
        and d.st.goal.my == 25, tostring(h.ga.phase))
  TMAP[26 * 256 + 22] = nil

  -- 9. NO CHAIN: today's hold, and a hit changes nothing.
  S({})
  d = dbot(); arrive(d, 200); d.inf.direction = 64; lock(d, 201)
  h = d.st.orders.held
  check("no chain: the scan says so", h.ga and h.ga.path == nil, "?")
  d.inf.armour = 30; lock(d, 202)
  check("no chain: a hit changes nothing (parked on the decoy square)",
        h.ga.phase == "wait" and d.st.goal.mx == 22 and d.st.goal.my == 24
        and ORD.hold_parked(d.st, d.inf) == true, tostring(h.ga.phase))
  check("no chain: the keys are today's (no turn, no throttle)",
        ORD.decoy_keys(d.st, d.inf, 0x11, 0) == 0x01, "?")
  -- NO CHAIN, THEN A CHAIN: the hit taken with no way out is spent.  A
  -- chain south appears and a new pill counts (the change); the rescan at
  -- the check interval finds the chain.  It does not move on the old hit:
  -- a new baseline, and the next hit moves it.
  S({ { 22, 25, 1.0 }, { 22, 26, 1.0 }, { 22, 27, 1.0 } })
  d.w.pills[6] = { mx = 26, my = 24, owner = "hostile", health = 5 }
  lock(d, 252)
  check("no chain -> a chain: the old hit is spent, it stays (new baseline, 0 hits)",
        pstr(h.ga.path) == "22,25 22,26 22,27" and h.ga.phase == "wait"
        and h.ga.hits == 0 and h.ga.arm == 30 and d.st.goal.my == 24,
        pstr(h.ga.path) .. " " .. tostring(h.ga.phase) .. " " .. tostring(h.ga.hits))
  d.inf.armour = 25; lock(d, 253)
  check("no chain -> a chain: a fresh hit moves it to square 1",
        h.ga.phase == "move" and d.st.goal.my == 25, tostring(h.ga.phase))
  d.w.pills[6] = nil
  S({})

  -- 10. A RESCAN AFTER A PILL DIES.  Two pills: 5 (20,20) and 6 (26,24).
  -- North (22,23) is shielded from pill 5 only, south (22,25) from pill 6
  -- only: 0.5 each, a tie, north wins on the lower key.  Pill 5 dies: north
  -- is open to pill 6 (0) and south is 1.0, so the rescan picks south --
  -- but not before DECOY_GETAWAY_RESCAN_TICKS.
  GA.block = function(world, p, mx, my)
    if p.mx == 20 and mx == 22 and my == 23 then return 1.0, "wall", 22, 22 end
    if p.mx == 26 and mx == 22 and my == 25 then return 1.0, "wall", 23, 25 end
    return 0, "open"
  end
  d = dbot()
  d.w.pills[6] = { mx = 26, my = 24, owner = "hostile", health = 5 }
  arrive(d, 200); lock(d, 201)
  h = d.st.orders.held
  check("two pills: north and south tie at 0.5, north wins",
        pstr(h.ga.path) == "22,23" and near(h.ga.score, 1.0), pstr(h.ga.path))
  local first_scan = h.ga.scan_tick
  lock(d, 251)
  check("nothing changed: no rescan at the next check", h.ga.scan_tick == first_scan, "?")
  d.w.pills[5].health = 0
  lock(d, 261)
  check("pill 5 dies: no rescan before the check interval",
        pstr(h.ga.path) == "22,23", pstr(h.ga.path))
  lock(d, 301)
  check("pill 5 dies: the rescan picks south (shielded from pill 6)",
        pstr(h.ga.path) == "22,25" and near(h.ga.score, 2.0) and h.ga.scan_tick == 301,
        pstr(h.ga.path))
  d.inf.direction = 64
  check("and the turn follows the new first square",
        ORD.decoy_keys(d.st, d.inf, 0, 0) == 0x08, "?")

  -- 11. KNOB OFF (keel): today's hold exactly.
  S({ { 22, 25, 1.0 }, { 22, 26, 1.0 }, { 22, 27, 1.0 } })
  GA.block = function(world, p, mx, my)
    local v = SAFE[my * 256 + mx]
    if v then return v, "wall", mx, my - 1 end
    return 0, "open"
  end
  C.DECOY_GETAWAY = false
  d = dbot(); arrive(d, 200); d.inf.direction = 64; lock(d, 201)
  h = d.st.orders.held
  check("DECOY_GETAWAY=false: no scan", h.ga == nil, "?")
  d.inf.armour = 30; lock(d, 202)
  check("DECOY_GETAWAY=false: a hit changes nothing",
        d.st.goal.mx == 22 and d.st.goal.my == 24 and ORD.hold_parked(d.st, d.inf) == true, "?")
  check("DECOY_GETAWAY=false: keys are today's (throttle off, no turn)",
        ORD.decoy_keys(d.st, d.inf, 0x11, 0) == 0x01, "?")
  C.DECOY_GETAWAY = true
  C.DECOY_GETAWAY_PROX_WEIGHT = 0.5
  C.DECOY_GETAWAY_BLOCKER_STEP = true

  ;(function() -- own function: the main chunk is at its 200-local limit
  -- 12. THE BLOCKER STEP (Andrew, Sep 24: "2 or less shots left", "count
  -- hits", "we don't need to count until it's the last blocker").  dbot's
  -- chain (22,25) (22,26) (22,27), pill 5 at (20,20).  Parked on a chain
  -- square, the closest counted pill's shell line to the tank's square is
  -- walked.  2 or more blockers: it holds, no count.  One (the last): a
  -- wall first seen full is 5 less the hits counted on it, one first seen
  -- damaged is 1, our pill is ceil(armour / damage); shots left <=
  -- DECOY_GETAWAY_BLOCKER_SHOTS (2) moves the tank on without a hit.  None:
  -- it moves.  The line tiles are the mock trace's own, so the blockers below
  -- sit on the line the code walks.
  check("blocker step knobs: on live, off in keel, step at <= 2 shots, wall life 4, pill damage 1",
        C.DECOY_GETAWAY_BLOCKER_STEP == true and C.PRESETS.keel.DECOY_GETAWAY_BLOCKER_STEP == false
        and C.DECOY_GETAWAY_BLOCKER_SHOTS == 2 and C.PRESETS.keel.DECOY_GETAWAY_BLOCKER_SHOTS == 2
        and C.DECOY_GETAWAY_WALL_LIFE == 4 and C.DECOY_GETAWAY_PILL_SHELL_DAMAGE == 1
        and C.DECOY_GETAWAY_BLOCKER_MIN == nil, "?")
  check("shots left: full wall 5, damaged wall 1 (hidden life: the smallest), pill = its armour",
        GA.shots_left("wall") == 5 and GA.shots_left("wall_damaged") == 1
        and GA.shots_left("pill", { health = 15 }) == 15 and GA.shots_left("pill", { health = 2 }) == 2, "?")
  C.DECOY_GETAWAY_PROX_WEIGHT = 0
  S({ { 22, 25, 1.0 }, { 22, 26, 1.0 }, { 22, 27, 1.0 } })
  -- The line tiles from pill 5 to (22,25), without the pill's and the tank's
  -- squares; only rows 21..23 are used (the decoy square (22,24) stays
  -- clear).
  local line = {}
  local UT = require("util")
  for _, st in ipairs(cpf.simulate_shot(UT.m2w(20), UT.m2w(20), UT.m2w(22), UT.m2w(25))) do
    if st.my >= 21 and st.my <= 23 then line[#line + 1] = st end
  end
  check("blocker rig: the line from pill 5 to (22,25) has >= 3 tiles in rows 21..23",
        #line >= 3, tostring(#line))
  local W1, W2, W3 = line[1], line[2], line[3]
  local function K(w) return w.my * 256 + w.mx end
  local function clear() TMAP[K(W1)], TMAP[K(W2)], TMAP[K(W3)] = nil, nil, nil end
  -- Parked on square 1 after the first hit.  The blockers go up after the
  -- scan (they would shield the decoy square too, and the scan's P would be
  -- empty).
  local function on_sq1(t0)
    clear()
    local b = dbot(); arrive(b, t0); lock(b, t0 + 1)
    b.inf.armour = b.inf.armour - 5; lock(b, t0 + 2)           -- hit 1: the first step
    b.inf.tanky = 25 * 256 + 128; lock(b, t0 + 4)              -- arrives on square 1
    return b, b.st.orders.held
  end
  local function moved(hb, b) return hb.ga.phase == "move" and b.st.goal.my == 26 and hb.ga.hits == 0 end
  local function held(hb) return hb.ga.phase == "wait" and hb.ga.used == 1 end

  -- THE HIT LEDGER: a wall-hit sound on a wall's square is one shell
  -- stopped on it (M.hear).  The test sets the two globals braincore.c
  -- gives a live brain.
  EVENT_SOUND = EVENT_SOUND or 8
  SND_SHOT_BUILDING_NEAR = SND_SHOT_BUILDING_NEAR or 4
  local function hit(b, w, t)
    b.inf.events = { { type = EVENT_SOUND, data = { SND_SHOT_BUILDING_NEAR, w.mx, w.my, 0xFF } } }
    lock(b, t)
    b.inf.events = nil
  end
  local function lastb(hb) return hb.ga.blk and hb.ga.blk.list and hb.ga.blk.list[1] end

  -- A FULL WALL, the last blocker: 5 - 0 = 5 shots, it holds.
  local b, hb = on_sq1(200)
  check("blocker rig: parked on square 1 after the hit, trigger hit",
        held(hb) and hb.ga.trigs[1] == "hit", tostring(hb.ga.phase))
  TMAP[K(W1)] = C.T_BUILDING
  lock(b, 205); lock(b, 206)
  check("a full wall: 5 - 0 = 5 shots > 2, it holds",
        held(hb) and hb.ga.blk and hb.ga.blk.shots == 5 and hb.ga.blk.id == 5 and hb.ga.blk.why == "last"
        and #hb.ga.blk.list == 1 and lastb(hb).kind == "wall" and lastb(hb).start == 5 and lastb(hb).hits == 0,
        tostring(hb.ga.blk and hb.ga.blk.shots))
  if drawn then
    local rb = { text = {} }
    ORD.draw_getaway({ is_on = function() return true end, rect = function() end,
                       line = function() end, circle = function() end,
                       text = function(id, x, y, t) rb.text[#rb.text + 1] = t end,
                       detail = function() end }, b.st)
    local lng = 0
    for _, t in ipairs(rb.text) do if #t > lng then lng = #t end end
    check("overlay: the closest pill, the last blocker's count, the triggers, and its box",
          has(rb.text, "closest p5 (20,20) last blocker 5-0=5 shots, step at <=2 | steps by: hit")
          and has(rb.text, "5-0=5 shots") and lng <= 127,
          table.concat(rb.text, " | "))
  end
  -- The first shell makes it a damaged wall.  No sound reached the brain
  -- here: the full-to-damaged change is hit 1 on its own.
  TMAP[K(W1)] = C.T_HALFBUILD
  lock(b, 207)
  check("a full wall with 1 hit counted (the damaged change): 5 - 1 = 4 shots, it holds",
        held(hb) and hb.ga.blk.shots == 4 and lastb(hb).hits == 1, tostring(hb.ga.blk.shots))
  hit(b, W1, 208)
  check("hit 2 (a wall-hit sound on its square): 5 - 2 = 3 shots, it holds",
        held(hb) and hb.ga.blk.shots == 3 and lastb(hb).hits == 2, tostring(hb.ga.blk.shots))
  hit(b, { mx = W1.mx + 5, my = W1.my }, 209)
  check("a wall-hit sound on another square is not counted",
        held(hb) and hb.ga.blk.shots == 3, tostring(hb.ga.blk.shots))
  hit(b, W1, 210)
  check("a full wall with 3 hits counted: 5 - 3 = 2 shots <= 2, it moves without a hit",
        moved(hb, b) and hb.ga.blk.shots == 2 and hb.ga.trigs[2] == "blk"
        and GA.blk_txt(hb.ga.blk) == string.format("last(%d,%d)5-3=2", W1.mx, W1.my),
        tostring(hb.ga.phase) .. " " .. tostring(hb.ga.blk and hb.ga.blk.shots))
  lock(b, 211)
  check("moving: no blocker count is kept (the overlay shows none)", hb.ga.blk == nil, "?")
  b.inf.tanky = 26 * 256 + 128; lock(b, 212)
  check("the blocker step keeps the arrival rule: parked on square 2 first, a new ledger",
        hb.ga.phase == "wait" and hb.ga.used == 2 and next(hb.ga.led) == nil, tostring(hb.ga.phase))

  -- THE SOUND AND THE DAMAGED CHANGE IN ONE THINK: one hit, not two.
  b, hb = on_sq1(250)
  TMAP[K(W1)] = C.T_BUILDING
  lock(b, 255)
  TMAP[K(W1)] = C.T_HALFBUILD
  hit(b, W1, 256)
  check("the first shell's sound and its damaged change are 1 hit: 4 shots",
        held(hb) and hb.ga.blk.shots == 4 and lastb(hb).hits == 1, tostring(hb.ga.blk.shots))

  -- A HIT IN THE THINK THE WALL BECOMES THE LAST BLOCKER: the ledger entry
  -- is made before the sounds are heard, so the hit counts: 5 - 1 = 4.
  b, hb = on_sq1(270)
  TMAP[K(W1)] = C.T_BUILDING
  hit(b, W1, 275)
  check("a hit in the first think of the last wall counts: 5 - 1 = 4 shots",
        held(hb) and hb.ga.blk.shots == 4 and lastb(hb).hits == 1
        and hb.ga.led[K(W1)] and #hb.ga.led[K(W1)].ticks == 1, tostring(hb.ga.blk.shots))

  -- A WALL ALREADY DAMAGED when the count starts: its life is unknown, so it
  -- keeps 1 (the worst case).  It moves at once.
  b, hb = on_sq1(300)
  TMAP[K(W1)] = C.T_HALFBUILD
  lock(b, 305)
  check("a wall already damaged when the count starts counts 1: it moves",
        moved(hb, b) and hb.ga.blk.shots == 1 and lastb(hb).park == "wall_damaged"
        and GA.blk_txt(hb.ga.blk) == string.format("last(%d,%d)dmg=1", W1.mx, W1.my),
        tostring(hb.ga.blk.shots))

  -- A WALL THAT IS GONE (rubble or grass) counts 0: no blocker left, it moves.
  b, hb = on_sq1(320)
  TMAP[K(W1)] = C.T_BUILDING
  lock(b, 325)
  TMAP[K(W1)] = C.T_RUBBLE
  lock(b, 326)
  check("a wall that is gone counts 0: no blocker left, it moves",
        moved(hb, b) and hb.ga.blk.shots == 0 and hb.ga.blk.why == "open", tostring(hb.ga.blk.shots))

  -- TWO WALLS: only the LAST one is counted (the one nearer the pill takes
  -- the shells first).  While both stand it holds with no count; hits on the
  -- first wall are not the last wall's.
  b, hb = on_sq1(340)
  TMAP[K(W1)], TMAP[K(W2)] = C.T_BUILDING, C.T_BUILDING
  lock(b, 345)
  check("two walls: 2 blockers, waiting, no count, it holds",
        held(hb) and hb.ga.blk.shots == nil and hb.ga.blk.why == "wait" and #hb.ga.blk.list == 2
        and GA.blk_txt(hb.ga.blk) == "2 blockers, waiting", tostring(hb.ga.blk.why))
  TMAP[K(W1)] = C.T_HALFBUILD
  hit(b, W1, 346); hit(b, W1, 347); hit(b, W1, 348); hit(b, W1, 349)
  check("two walls: 4 hits on the first wall, still waiting, nothing in the ledger",
        held(hb) and hb.ga.blk.why == "wait" and next(hb.ga.led) == nil, tostring(hb.ga.blk.why))
  TMAP[K(W1)] = C.T_RUBBLE
  lock(b, 350)
  check("two walls: the first is gone, the last wall is full: 5 - 0 = 5, it holds",
        held(hb) and hb.ga.blk.why == "last" and hb.ga.blk.shots == 5
        and lastb(hb).mx == W2.mx and lastb(hb).my == W2.my, tostring(hb.ga.blk.shots))
  TMAP[K(W2)] = C.T_HALFBUILD
  hit(b, W2, 351)
  hit(b, W2, 352)
  check("two walls: the last wall has 2 hits (the change + a sound, then a sound): 5 - 2 = 3, it holds",
        held(hb) and hb.ga.blk.shots == 3, tostring(hb.ga.blk.shots))
  hit(b, W2, 353)
  check("two walls: the last wall has 3 hits: 5 - 3 = 2, it moves",
        moved(hb, b) and hb.ga.blk.shots == 2 and hb.ga.trigs[2] == "blk", tostring(hb.ga.blk.shots))
  -- A full wall and a damaged wall behind it: when the full one goes, the
  -- damaged one is the last and counts 1.  It moves.
  b, hb = on_sq1(360)
  TMAP[K(W1)], TMAP[K(W2)] = C.T_BUILDING, C.T_HALFBUILD
  lock(b, 365)
  check("a full wall and a damaged wall: waiting, it holds", held(hb) and hb.ga.blk.why == "wait",
        tostring(hb.ga.blk.why))
  TMAP[K(W1)] = nil
  lock(b, 366)
  check("the full wall is gone: the last wall was damaged when the count started, 1, it moves",
        moved(hb, b) and hb.ga.blk.shots == 1, tostring(hb.ga.blk.shots))
  -- A wall and our pill: 2 blockers, waiting.
  b, hb = on_sq1(380)
  TMAP[K(W1)] = C.T_BUILDING
  b.w.pills[8] = { mx = W3.mx, my = W3.my, owner = "friendly", health = 1 }
  b.w.pill_at = { [K(W3)] = { { pill = b.w.pills[8] } } }
  lock(b, 385)
  check("a wall and our pill (armour 1): 2 blockers, waiting, it holds",
        held(hb) and hb.ga.blk.why == "wait", tostring(hb.ga.blk.why))
  b.w.pills[8], b.w.pill_at = nil, nil

  -- NO BLOCKER (a forest only): 0 shots left, it moves.
  b, hb = on_sq1(400)
  TMAP[K(W1)] = C.T_FOREST
  lock(b, 405)
  check("no blocker (a forest only): 0 shots left, it moves",
        moved(hb, b) and hb.ga.blk.shots == 0 and #hb.ga.blk.list == 0, tostring(hb.ga.phase))

  -- OUR PILL on the line: its armour is its shots left.
  b, hb = on_sq1(500)
  b.w.pills[8] = { mx = W3.mx, my = W3.my, owner = "friendly", health = 15 }
  b.w.pill_at = { [K(W3)] = { { pill = b.w.pills[8] } } }
  lock(b, 505)
  check("our pill with armour 15: 15 shots left, it holds",
        held(hb) and hb.ga.blk.shots == 15 and hb.ga.blk.list[1].kind == "pill", tostring(hb.ga.blk.shots))
  b.w.pills[8].health = 3; lock(b, 506)
  check("our pill with armour 3: 3 shots left, it holds", held(hb) and hb.ga.blk.shots == 3,
        tostring(hb.ga.blk.shots))
  C.DECOY_GETAWAY_PILL_SHELL_DAMAGE = 2; lock(b, 507)
  check("PILL_SHELL_DAMAGE 2: armour 3 is ceil(3/2) = 2 shots, it moves",
        moved(hb, b) and hb.ga.blk.shots == 2, tostring(hb.ga.blk.shots))
  C.DECOY_GETAWAY_PILL_SHELL_DAMAGE = 1
  b, hb = on_sq1(520)
  b.w.pills[8] = { mx = W3.mx, my = W3.my, owner = "allied", health = 2 }
  b.w.pill_at = { [K(W3)] = { { pill = b.w.pills[8] } } }
  lock(b, 525)
  check("an ally's pill with armour 2: 2 shots left, it moves",
        moved(hb, b) and hb.ga.blk.shots == 2, tostring(hb.ga.blk.shots))
  b.w.pills[8], b.w.pill_at = nil, nil

  -- WALL_LIFE: a building_life of 1 makes a full wall 2 shots.
  C.DECOY_GETAWAY_WALL_LIFE = 1
  b, hb = on_sq1(540)
  TMAP[K(W1)] = C.T_BUILDING
  lock(b, 545)
  check("WALL_LIFE 1: a full wall is 2 shots, it moves", moved(hb, b) and hb.ga.blk.shots == 2,
        tostring(hb.ga.blk.shots))
  C.DECOY_GETAWAY_WALL_LIFE = 4

  -- THE CLOSEST PILL ONLY.  A full wall on pill 5's line (d = 5.39, 5
  -- shots); pill 6 at (22,32) (d = 7, in range) has an open line.  Only pill
  -- 5 counts: it holds.  With pill 6 at (22,29) (d = 4, now the closest) it
  -- moves.
  b, hb = on_sq1(600)
  TMAP[K(W1)] = C.T_BUILDING
  b.w.pills[6] = { mx = 22, my = 32, owner = "hostile", health = 5 }
  lock(b, 605)
  check("two pills: the far pill's open line is ignored, the closest (p5, 5 shots) holds it",
        held(hb) and hb.ga.blk.id == 5 and hb.ga.blk.shots == 5,
        tostring(hb.ga.blk.id) .. " " .. tostring(hb.ga.blk.shots))
  b.w.pills[6].my = 29
  lock(b, 606)
  check("two pills: pill 6 is now the closest, its line is open (0 shots): it moves",
        moved(hb, b) and hb.ga.blk.id == 6 and hb.ga.blk.shots == 0
        and hb.ga.trigs[2] == "blk", tostring(hb.ga.blk.id))
  b.w.pills[6] = nil
  clear()

  -- KNOCKED OFF THE PARK SQUARE (Andrew, Sep 24: "It should stay counting
  -- even if it got pushed off and head to the next spot").  The line, the
  -- last blocker and the ledger stay the PARK square's (blk.tx/ty, and the
  -- same wall and ledger table), not the knock square's.
  b, hb = on_sq1(650)
  TMAP[K(W3)] = C.T_BUILDING
  lock(b, 655)
  -- (21,25): its own line from pill 5 misses W3, so the old rule (the
  -- count from the tank's square) would see no blocker and move at once.
  local kx, ky = 21, 25
  local _, _, kwhy = GA.blockers(b.w, b.w.pills[5], kx, ky, {})
  check("knock rig: the knock square's own line is open (the old rule would move)",
        kwhy == "open", tostring(kwhy))
  local led0 = hb.ga.led
  b.inf.tankx, b.inf.tanky = kx * 256 + 100, ky * 256 + 140
  lock(b, 656)
  check("knocked off square 1: the count stays on square 1's line (full wall, 5 - 0 = 5), it holds",
        held(hb) and hb.ga.blk.why == "last" and hb.ga.blk.shots == 5
        and hb.ga.blk.tx == 22 and hb.ga.blk.ty == 25 and hb.ga.blk.off == true
        and lastb(hb).mx == W3.mx and lastb(hb).my == W3.my and hb.ga.led == led0
        and b.st.goal.mx == 22 and b.st.goal.my == 25,
        tostring(hb.ga.blk.why) .. " " .. tostring(hb.ga.blk.shots))
  TMAP[K(W3)] = C.T_HALFBUILD
  lock(b, 657)
  hit(b, W3, 658)
  check("knocked off: hits on square 1's wall still count (the change, then a sound = 2): 3 shots, it holds",
        held(hb) and hb.ga.blk.shots == 3 and lastb(hb).hits == 2, tostring(hb.ga.blk.shots))
  if drawn then
    local rb = { text = {}, lines = 0 }
    ORD.draw_getaway({ is_on = function() return true end, rect = function() end,
                       line = function() rb.lines = rb.lines + 1 end, circle = function() end,
                       text = function(id, x, y, t) rb.text[#rb.text + 1] = t end,
                       detail = function() end }, b.st)
    local dx, dy = kx * 256 + 100 - (22 * 256 + 128), ky * 256 + 140 - (25 * 256 + 128)
    check("overlay knocked off: the count is square 1's, plus the tank-off-park label",
          has(rb.text, "closest p5 (20,20) last blocker 5-2=3 shots, step at <=2 | steps by: hit")
          and has(rb.text, string.format("tank off park (%d,%d wu)", dx, dy)),
          table.concat(rb.text, " | "))
  end
  hit(b, W3, 659)
  check("knocked off: hit 3 on square 1's wall: 5 - 3 = 2, it moves to square 2 from where it is",
        moved(hb, b) and hb.ga.trigs[2] == "blk" and hb.ga.blk.shots == 2
        and hb.ga.blk.tx == 22 and hb.ga.blk.ty == 25, tostring(hb.ga.phase))
  lock(b, 660)
  check("moving from the knock square: the goal is square 2, not square 1",
        hb.ga.phase == "move" and b.st.goal.mx == 22 and b.st.goal.my == 26, tostring(b.st.goal.my))
  b.inf.tankx, b.inf.tanky = 22 * 256 + 128, 26 * 256 + 128
  lock(b, 661)
  check("knocked off, then on square 2: parked there, a new ledger",
        hb.ga.phase == "wait" and hb.ga.used == 2 and next(hb.ga.led) == nil, tostring(hb.ga.phase))

  -- Knocked off with no count left to fire: it holds, and the hold goal is
  -- still square 1 (it drives back).  A hit while off still moves it.
  b, hb = on_sq1(670)
  TMAP[K(W3)] = C.T_BUILDING
  lock(b, 675)
  b.inf.tankx, b.inf.tanky = kx * 256 + 128, ky * 256 + 128
  lock(b, 676); lock(b, 677)
  check("knocked off, full wall: holds, goal square 1",
        held(hb) and hb.ga.blk.shots == 5 and b.st.goal.mx == 22 and b.st.goal.my == 25,
        tostring(hb.ga.phase))
  b.inf.tankx, b.inf.tanky = 22 * 256 + 128, 25 * 256 + 128
  lock(b, 678)
  check("back on square 1: the count and the ledger go on, off = false",
        held(hb) and hb.ga.blk.shots == 5 and hb.ga.blk.off == false, tostring(hb.ga.phase))
  b.inf.tankx, b.inf.tanky = kx * 256 + 128, ky * 256 + 128
  lock(b, 679)
  b.inf.armour = b.inf.armour - 5; lock(b, 680)
  check("knocked off: a hit still moves it, trigger hit, goal square 2",
        hb.ga.phase == "move" and hb.ga.trigs[2] == "hit" and b.st.goal.my == 26, tostring(hb.ga.phase))
  -- Knocked onto the NEXT square with no trigger: not an arrival (it only
  -- counts in the move phase), still parked on square 1.
  b, hb = on_sq1(690)
  TMAP[K(W3)] = C.T_BUILDING
  lock(b, 692)
  b.inf.tankx, b.inf.tanky = 22 * 256 + 128, 26 * 256 + 128
  lock(b, 693); lock(b, 694)
  check("knocked onto square 2 with no trigger: still parked on square 1, goal square 1",
        held(hb) and hb.ga.park_my == 25 and b.st.goal.my == 25 and hb.ga.blk.ty == 25,
        tostring(hb.ga.phase))
  clear()

  -- THE DECOY SQUARE: no blocker step there.  0 shots, no hit: it stays.
  b = dbot(); arrive(b, 700); lock(b, 701)
  hb = b.st.orders.held
  lock(b, 702); lock(b, 720); lock(b, 740)
  check("on the decoy square 0 shots left does not move it (the first step waits for a hit)",
        hb.ga.phase == "wait" and hb.ga.used == 0 and hb.ga.blk == nil
        and b.st.goal.my == 24, tostring(hb.ga.phase))
  b.inf.armour = b.inf.armour - 5; lock(b, 741)
  check("and the first hit still moves it, trigger hit",
        hb.ga.phase == "move" and hb.ga.trigs[1] == "hit", tostring(hb.ga.phase))

  -- A SHELL THAT RUNS OUT before the tank's square: no count, no step.
  local sim = cpf.simulate_shot
  b, hb = on_sq1(800)
  cpf.simulate_shot = function() return { { mx = 20, my = 20 }, { mx = 20, my = 21 } } end
  lock(b, 805)
  cpf.simulate_shot = sim
  check("shell short: no count, it holds", held(hb) and hb.ga.blk ~= nil
        and hb.ga.blk.shots == nil, tostring(hb.ga.phase))

  -- KNOB OFF (keel): 0 shots left on square 1, no hit: it holds (hits only).
  C.DECOY_GETAWAY_BLOCKER_STEP = false
  b, hb = on_sq1(900)
  lock(b, 905); lock(b, 930)
  check("DECOY_GETAWAY_BLOCKER_STEP=false: 0 shots left does not move it",
        held(hb) and hb.ga.blk == nil, tostring(hb.ga.phase))
  b.inf.armour = b.inf.armour - 5; lock(b, 931)
  check("DECOY_GETAWAY_BLOCKER_STEP=false: a hit moves it",
        hb.ga.phase == "move" and hb.ga.trigs[2] == "hit", tostring(hb.ga.phase))
  C.DECOY_GETAWAY_BLOCKER_STEP = true

  -- THE DIAGONAL STEP (DECOY_GETAWAY_DIAGONAL): while a getaway moves, a
  -- diagonal next square is driven straight at (steering.cpf_path_to asks
  -- orders.getaway_diagonal); only then, and only past drivable sides.
  ;(function()  -- own function: main is at the 200-local cap
  check("DECOY_GETAWAY_DIAGONAL: live on, keel off",
        C.DECOY_GETAWAY_DIAGONAL == true and C.PRESETS.keel.DECOY_GETAWAY_DIAGONAL == false, "?")
  local hm = { mx = 20, my = 25, decoy = true,
               ga = { phase = "move", idx = 1, path = { { mx = 21, my = 24 } } } }
  local gm = ORD.decoy_goal(hm)
  local nx, ny = ORD.getaway_diagonal(gm, 20, 25, false)
  check("diagonal: a moving getaway goes straight to the diagonal square",
        gm._getaway == true and nx == 21 and ny == 24, tostring(nx) .. "," .. tostring(ny))
  check("diagonal: from an orthogonal neighbour or two away, no change",
        ORD.getaway_diagonal(gm, 21, 25, false) == nil
        and ORD.getaway_diagonal(gm, 19, 26, false) == nil, "?")
  check("diagonal: not in a boat", ORD.getaway_diagonal(gm, 20, 25, true) == nil, "?")
  TMAP[24 * 256 + 20] = C.T_BUILDING
  check("diagonal: a wall on a side square still blocks it (corner)",
        ORD.getaway_diagonal(gm, 20, 25, false) == nil, "?")
  TMAP[24 * 256 + 20] = nil
  TMAP[25 * 256 + 21] = C.T_HALFBUILD
  check("diagonal: a half wall on the other side square blocks it",
        ORD.getaway_diagonal(gm, 20, 25, false) == nil, "?")
  TMAP[25 * 256 + 21] = C.T_DEEPSEA
  check("diagonal: deep sea on a side square blocks it",
        ORD.getaway_diagonal(gm, 20, 25, false) == nil, "?")
  TMAP[25 * 256 + 21] = C.T_FOREST
  check("diagonal: a tree on a side square does not block it",
        ORD.getaway_diagonal(gm, 20, 25, false) == 21, "?")
  TMAP[25 * 256 + 21] = nil
  -- The side squares also pass the chain search's own test (M.passable):
  -- a known mine or a live pill on one blocks the straight drive.
  TMAP[24 * 256 + 20] = C.T_GRASS + _G.TERRAIN_MINE_FLAG
  check("diagonal: a known mine on a side square blocks it",
        ORD.getaway_diagonal(gm, 20, 25, false, { pills = {} }) == nil
        and ORD.getaway_diagonal(gm, 20, 25, false) == nil, "?")
  TMAP[24 * 256 + 20] = nil
  local wp = { pills = { [9] = { mx = 21, my = 25, owner = "friendly", health = 5 } } }
  wp.pill_at = { [25 * 256 + 21] = { { pill = wp.pills[9] } } }
  check("diagonal: a live pill on a side square blocks it (world given)",
        ORD.getaway_diagonal(gm, 20, 25, false, wp) == nil, "?")
  wp.pills[9].health = 0
  check("diagonal: a dead pill there does not",
        ORD.getaway_diagonal(gm, 20, 25, false, wp) == 21, "?")
  local hw = { mx = 20, my = 25, decoy = true,
               ga = { phase = "wait", idx = 1, park_mx = 20, park_my = 25,
                      path = { { mx = 21, my = 24 } } } }
  check("diagonal: a parked getaway (wait) is not changed",
        ORD.getaway_diagonal(ORD.decoy_goal(hw), 20, 25, false) == nil, "?")
  check("diagonal: any other goal is not changed",
        ORD.getaway_diagonal({ kind = "goto_tile", mx = 21, my = 24 }, 20, 25, false) == nil
        and ORD.getaway_diagonal({ kind = "attack_pill", mx = 21, my = 24, _decoy = true },
                                 20, 25, false) == nil, "?")
  C.DECOY_GETAWAY_DIAGONAL = false
  check("DECOY_GETAWAY_DIAGONAL=false (keel): the old route (no change)",
        ORD.getaway_diagonal(gm, 20, 25, false) == nil, "?")
  C.DECOY_GETAWAY_DIAGONAL = true
  end)()
  C.DECOY_GETAWAY_PROX_WEIGHT = 0.5
  end)()

  GA.block = saved.block
  cpf.simulate_shot = saved.sim
  _G.get_terrain, _G.TERRAIN_MASK, _G.TERRAIN_MINE_FLAG = saved.gt, saved.tm, saved.mf
  _G.KEY_TURNLEFT, _G.KEY_TURNRIGHT, _G.KEY_FASTER = saved.kl, saved.kr, saved.kf
  _G.EVENT_PING, _G.PING_KIND_CAUTION, _G.PING_KIND_BOT_COMMAND = nil, nil, nil
  math.atan = atan1
end)()

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
