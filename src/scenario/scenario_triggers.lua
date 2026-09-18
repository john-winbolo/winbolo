-- Copyright (c) 1998-2026 John Morrison.
-- SPDX-License-Identifier: GPL-2.0-or-later
--
-- =========================================================================
-- scenario_triggers.lua — the trigger router
--
-- A scenario's triggers are data: a hook to run on, a list of tests, and a
-- list of actions. This is what turns that data into calls. It ships inside
-- the binary and no operator edits it; scenario_host.c holds the bytes and
-- loads them.
--
-- It loads as a second chunk into the state the author's script has already
-- run in, and never concatenated on to it: the author's file keeps its own
-- line numbers in an error, a top-level return in it stays legal, and the
-- definitions it made are in place by the time the loop below reads them.
--
-- Within a hook the author's own function runs first and the triggers after
-- it. The author's is the specific thing and wins; a trigger cannot stop it.
-- The author can stop the triggers by returning exactly false, which a
-- function that falls off the end does not do -- it returns nil, so a
-- handler that does not care about triggers is unaffected and a stray early
-- return cannot switch a scenario's triggers off by accident.
--
-- Arguments, from scnInstallTriggers:
--
--   triggers  the trigger array in manifest order, each
--             { when = "<hook>",
--               where   = { { field = ..., op = ..., value = ... }, ... },
--               actions = { { op = ..., args = { ... }, text = ... }, ... } }
--
--   fields    a hook name to a table of that hook's field names, each
--             { arg = <1-based argument position>, derive = <how>,
--               kind = <"pill"/"base">, arg2 = <the y beside an x> }.
--             derive is absent for one of the function's own parameters,
--             and otherwise "team", "tag" or "region". A hook with no
--             readable fields is an empty table rather than absent.
--
-- A value is a number, a string, a boolean, or a table { field = "<name>" }
-- naming a payload field to read when the trigger fires.
--
-- A field answers either one value or a set. A parameter and a team are one
-- value, compared with the seven operators. A tag and a region are sets,
-- and answer only whether they hold a name: in for yes, ne for no.
--
-- Nothing here checks that a hook exists, that a field suits its hook, that
-- an operator suits its field or that an op takes the arguments given. That
-- is checked before a round starts, and raising here would spend one of the
-- scenario's errors on something the operator has already been told about.
-- =========================================================================

local triggers, fields = ...

-- The op surface, taken once. This is the table the host installed before
-- either chunk ran, which is what an action's op is dispatched on. A call
-- action is the other way round and reads the author's own globals when it
-- fires, because a script may define a function under a condition.
local game = game
local G    = _G

if type(game) ~= "table" then
    -- A script that took the op surface away leaves every op an op the
    -- table does not carry, which is skipped. Reading through the nil
    -- instead would raise, and cost the scenario an error for each action
    -- it had written.
    game = {}
end

-- The owner value that means nobody, off the op surface rather than written
-- down here. An owner carrying it is on no team, which is the difference
-- between an owner and a seat.
local NEUTRAL = game.NEUTRAL
if type(NEUTRAL) ~= "number" then
    NEUTRAL = nil
end

-- ── Values ───────────────────────────────────────────────────────────────

-- One accessor off the op surface, or nil where the table has not got it.
-- Every game.* call below goes through this: calling a nil would raise, and
-- an error here would count against the scenario for a question it only
-- asked in passing.
local function accessor(name)
    local fn = game[name]
    if type(fn) ~= "function" then
        return nil
    end
    return fn
end

-- What the host said about one field of one hook: which argument it reads,
-- and how the value is worked out from it. nil for a name this hook has no
-- field of.
--
-- The position is checked here rather than at each use, because select is
-- what reads it and select raises on an index below one.
local function entryOf(map, name)
    if name == nil then
        return nil
    end
    local e = map[name]
    if type(e) ~= "table" or type(e.arg) ~= "number" or e.arg < 1 then
        return nil
    end
    return e
end

-- The one value a scalar field reads off the payload, or nil where there is
-- none to read. A row whose field answers nil does not hold, under any
-- operator.
--
-- A team is the seat's, through game.lobby_slot. It is nil for an argument
-- that is not a number, for an owner that is nobody, and for a seat the
-- roster has nothing in — three ways of saying there is no team to test,
-- and none of them an error.
--
-- A set-valued field has no scalar to answer with; membership is asked of
-- it instead, further down.
local function scalar(e, ...)
    local raw = (select(e.arg, ...))

    if e.derive == nil then
        return raw
    end
    if e.derive ~= "team" then
        return nil
    end
    if type(raw) ~= "number" or raw == NEUTRAL then
        return nil
    end
    local fn = accessor("lobby_slot")
    if fn == nil then
        return nil
    end
    local seat = fn(raw)
    if type(seat) ~= "table" or type(seat.team) ~= "number" then
        return nil
    end
    return seat.team
end

-- One value, resolved against the arguments the hook was handed. Answers
-- false where there is nothing to resolve it to: a payload reference naming
-- a field this hook has not got, or naming a set, which is not a value and
-- so cannot sit on the right of a row or in an action's arguments.
--
-- Two returns rather than one, because a field that is genuinely nil and a
-- field there is no reading of are different answers and a single nil
-- cannot tell them apart.
local function value(v, map, ...)
    if type(v) ~= "table" then
        return true, v
    end
    local e = entryOf(map, v.field)
    if e == nil then
        return false
    end
    if e.derive == "tag" or e.derive == "region" then
        return false
    end
    return true, scalar(e, ...)
end

-- ── Sets ─────────────────────────────────────────────────────────────────

-- Whether a set-valued field holds want: the tags on the item the payload
-- names, or the region the square pair it carries is inside.
--
-- Answers nil where the question cannot be put at all — an index the map
-- cannot hold, an argument of the wrong kind, a value that is not a name.
-- That is neither true nor false, and the row it came from does not hold
-- whichever way round its operator asked.
--
-- A region is asked rather than enumerated: game.in_region answers whether
-- one square is inside one named rectangle, so membership is a single call
-- and no list of regions is ever built.
local function contains(e, want, ...)
    if type(want) ~= "string" then
        return nil
    end

    if e.derive == "tag" then
        local n = (select(e.arg, ...))
        if type(n) ~= "number" or type(e.kind) ~= "string" then
            return nil
        end
        local fn = accessor("tags")
        if fn == nil then
            return nil
        end
        local list = fn(e.kind, n)
        if type(list) ~= "table" then
            return nil
        end
        for i = 1, #list do
            if list[i] == want then
                return true
            end
        end
        return false
    end

    if type(e.arg2) ~= "number" or e.arg2 < 1 then
        return nil
    end
    local mx = (select(e.arg, ...))
    local my = (select(e.arg2, ...))
    if type(mx) ~= "number" or type(my) ~= "number" then
        return nil
    end
    local fn = accessor("in_region")
    if fn == nil then
        return nil
    end
    local inside = fn(want, mx, my)
    if type(inside) ~= "boolean" then
        return nil
    end
    return inside
end

-- ── Comparing ────────────────────────────────────────────────────────────

-- One where-row's test, on a field that answers a single value. Two values
-- of different kinds compare false rather than raising, ne included: a row
-- that cannot be evaluated does not hold, and the alternative would have a
-- scenario firing on the rows it got wrong.
local function compare(op, a, b)
    if op == "in" then
        -- in asks whether a set holds a value, and a row stores one value
        -- rather than a list, so there is nothing here for it to look in.
        -- A set-valued field is the only place it means anything, and that
        -- is answered by contains above rather than here.
        return false
    end

    local kind = type(a)
    if kind ~= type(b) then
        return false
    end
    if op == "eq" then
        return a == b
    end
    if op == "ne" then
        return a ~= b
    end

    -- The four orderings. Lua orders numbers and strings and nothing else,
    -- and the two sides are already the same kind.
    if kind ~= "number" and kind ~= "string" then
        return false
    end
    if op == "lt" then
        return a < b
    end
    if op == "lte" then
        return a <= b
    end
    if op == "gt" then
        return a > b
    end
    if op == "gte" then
        return a >= b
    end
    return false
end

-- Whether one row holds.
--
-- A set-valued field takes two operators and no others: in is "holds this"
-- and ne is "does not hold this". eq and the four orderings ask something a
-- set cannot answer — a set is not equal to a name, and no set is less than
-- one — so they do not hold on it whatever the value. Which operator suits
-- which field is refused before a round starts; here the row simply fails.
local function rowHolds(row, map, ...)
    local e = entryOf(map, row.field)
    if e == nil then
        return false
    end

    local ok, want = value(row.value, map, ...)
    if not ok then
        return false
    end

    if e.derive == "tag" or e.derive == "region" then
        if row.op ~= "in" and row.op ~= "ne" then
            return false
        end
        local has = contains(e, want, ...)
        if has == nil then
            return false
        end
        if row.op == "ne" then
            return not has
        end
        return has
    end

    local got = scalar(e, ...)
    if got == nil then
        return false
    end
    return compare(row.op, got, want)
end

-- Whether every row of a trigger's where holds. No rows is no test, so a
-- trigger without them runs on every occurrence of its hook.
local function holds(trig, map, ...)
    local rows = trig.where
    if rows == nil then
        return true
    end
    for i = 1, #rows do
        if not rowHolds(rows[i], map, ...) then
            return false
        end
    end
    return true
end

-- ── Acting ───────────────────────────────────────────────────────────────

-- One action. The op names a row of the game table, except for call, which
-- names a top-level function of the author's own script: it reaches game.*
-- and nothing else, exactly as the rest of the script does, and an error in
-- it counts against the scenario like any other.
--
-- An op the game table does not carry, and a call naming a function the
-- script never defined, are both skipped.
local function act(action, map, ...)
    local args  = action.args
    local count = args ~= nil and #args or 0
    local first = 1
    local fn

    if action.op == "call" then
        if args == nil then
            return
        end
        local ok, name = value(args[1], map, ...)
        if not ok or type(name) ~= "string" then
            return
        end
        -- rawget, for the reason the host reads its globals the same way: a
        -- metatable on the globals table would otherwise turn this lookup
        -- into a call into script code from inside a hook.
        fn    = rawget(G, name)
        first = 2
    else
        fn = game[action.op]
    end
    if type(fn) ~= "function" then
        return
    end

    local a1, a2, a3, a4, a5, a6
    local n = 0
    for i = first, count do
        local ok, v = value(args[i], map, ...)
        if not ok or v == nil then
            -- An argument naming a field this hook has not got, or one that
            -- read nothing — an owner who is nobody has no team. The action
            -- cannot be made as written, so it is not made; handing an op a
            -- nil where it wants a number is an error the scenario would
            -- pay for.
            return
        end
        n = n + 1
        if n == 1 then
            a1 = v
        elseif n == 2 then
            a2 = v
        elseif n == 3 then
            a3 = v
        elseif n == 4 then
            a4 = v
        elseif n == 5 then
            a5 = v
        elseif n == 6 then
            a6 = v
        else
            -- Both readers cap an action at six arguments, so there is no
            -- seventh to reach here. One would be dropped rather than put
            -- somewhere it does not belong.
            return
        end
    end

    if n == 0 then
        fn()
    elseif n == 1 then
        fn(a1)
    elseif n == 2 then
        fn(a1, a2)
    elseif n == 3 then
        fn(a1, a2, a3)
    elseif n == 4 then
        fn(a1, a2, a3, a4)
    elseif n == 5 then
        fn(a1, a2, a3, a4, a5)
    else
        fn(a1, a2, a3, a4, a5, a6)
    end
end

-- ── Installing ───────────────────────────────────────────────────────────

-- The triggers of one hook, grouped, and the order the hooks were first
-- named in so the install below is the same every time.
local byHook = {}
local named  = {}

for i = 1, #triggers do
    local trig = triggers[i]
    local name = trig.when
    if type(name) == "string" and name ~= "" then
        local list = byHook[name]
        if list == nil then
            list   = {}
            byHook[name] = list
            named[#named + 1] = name
        end
        list[#list + 1] = trig
    end
end

for i = 1, #named do
    local name  = named[i]
    local list  = byHook[name]
    local map   = fields[name] or {}
    -- Captured before the function below is installed, and not after: this
    -- is the author's own handler, and the one installed here is what the
    -- host will resolve in its place.
    --
    -- Read and written as a script reads and writes a global, because this
    -- runs at load in the same breath as the author's own top level. The
    -- lookup a call action makes is the other case and is raw: that one
    -- happens inside a hook, where running a script's metamethod from the
    -- router is the thing the host's own reads take care to avoid.
    local prior = G[name]

    if type(prior) ~= "function" then
        prior = nil
    end

    G[name] = function(...)
        if prior ~= nil and prior(...) == false then
            return
        end
        for j = 1, #list do
            local trig = list[j]
            if holds(trig, map, ...) then
                local actions = trig.actions
                if actions ~= nil then
                    for k = 1, #actions do
                        act(actions[k], map, ...)
                    end
                end
            end
        end
    end
end
