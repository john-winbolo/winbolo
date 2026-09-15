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
local ALL = {
  { pn = 0, name = "Andrew" },
  { pn = 1, name = "Socrates" },
  { pn = 2, name = "Seneca" },
  { pn = 3, name = "Bruce Lee" },
  { pn = 4, name = "Plato" },
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


print("orders.lua — help")
check("help",             shape(P("help")) == "help", shape(P("help")))
check("help with junk",   shape(P("help me")) == "reply:didn't understand", shape(P("help me")))
check("help: 4 lines",    #ORD.HELP == 4, tostring(#ORD.HELP))
check("help line 1 lists the last who-word",
      ORD.HELP[1]:find("all|nearby|last|bot name", 1, true) ~= nil, ORD.HELP[1])
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
check("ring: a neighbour of the pill",
      (function()
         local h = ORD.resolve_ping(st, w, inf, 21, 20)
         return h and h.class == "pill" and h.id == 5 and h.exact == false
       end)(), "?")
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

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
