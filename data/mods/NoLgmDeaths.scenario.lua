-- =========================================================================
-- No LGM Deaths — a mod that keeps the man alive.
--
-- A builder sent out to lay a road or build a pill is worth a shell to
-- anyone who sees him, and losing him costs the trip back to the tank as
-- well as the work. This mod refuses that one death: a builder caught in
-- the open walks away from whatever hit him and carries on. Everything
-- else — tanks, pills, bases — dies as it always did.
--
-- kind = "mod", so it leaves the win condition alone: it can be added to
-- any round, beside a scenario or on its own, and the round still ends the
-- way the map and the lobby say it does.
--
-- bound = false, so it is not tied to one map. A host picks it from the mod
-- list and it runs over whatever is loaded.
-- =========================================================================

scenario = {
  name = "No LGM Deaths",
  description = "Builders survive what would have killed them.",
  api = 1,
  kind = "mod",
  bound = false,
}

-- Asked before anything dies. Answering false for a builder leaves him
-- standing; every other kind is left to the answer it would have had.
function can_die(kind, n, killer, cause)
  if kind == "builder" then
    return false
  end
  return true
end
