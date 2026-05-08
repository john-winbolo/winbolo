-- =========================================================================
-- NewAutopilot/bt.lua — lightweight behaviour tree library
--
-- Five node types, all stateless (no internal node state).
-- The goal table IS the blackboard.
--
-- bt.tick(node, ctx)  where  ctx = { goal, state, world, info }
-- Returns "success", "fail", or "running".
-- =========================================================================

local M = {}

-- ── Node constructors ────────────────────────────────────────────────────

-- Run children L→R.  Fail/running on first non-success.
function M.sequence(...)
  return { type = "sequence", children = { ... } }
end

-- Run children L→R.  Success/running on first non-fail.
function M.selector(...)
  return { type = "selector", children = { ... } }
end

-- Call fn(ctx) → returns "success"/"fail"/"running"
function M.action(fn)
  return { type = "action", fn = fn }
end

-- Call predicate(ctx) → maps true/false to "success"/"fail"
function M.condition(fn)
  return { type = "condition", fn = fn }
end

-- If cond fails, return "fail" without evaluating child.
function M.guard(cond, child)
  return { type = "guard", cond = cond, child = child }
end

-- ── Evaluator ────────────────────────────────────────────────────────────

function M.tick(node, ctx)
  local t = node.type

  if t == "action" then
    return node.fn(ctx)

  elseif t == "condition" then
    return node.fn(ctx) and "success" or "fail"

  elseif t == "guard" then
    local r = M.tick(node.cond, ctx)
    if r == "fail" then return "fail" end
    return M.tick(node.child, ctx)

  elseif t == "sequence" then
    for i = 1, #node.children do
      local r = M.tick(node.children[i], ctx)
      if r ~= "success" then return r end
    end
    return "success"

  elseif t == "selector" then
    for i = 1, #node.children do
      local r = M.tick(node.children[i], ctx)
      if r ~= "fail" then return r end
    end
    return "fail"
  end

  return "fail"
end

return M
