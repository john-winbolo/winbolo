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
check("defend base 3",         shape(P("defend base 3")) == "defend who=auto tgt=base:3", shape(P("defend base 3")))
check("all attack 7",          shape(P("all attack 7")) == "attack who=all tgt=pill:7", shape(P("all attack 7")))
check("nearby defend base 3",  shape(P("nearby defend base 3")) == "defend who=nearby tgt=base:3", shape(P("nearby defend base 3")))
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

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
