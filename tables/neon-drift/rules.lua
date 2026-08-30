-- tables/neon-drift/rules.lua
-- Neon Drift rules v1 (M9): scoring, gear-shift drop bank, drift-corner
-- mode, basic multiball. Full design per 15-launch-tables.md §1; the
-- elements this greybox ships are wired, the rest land with the art and
-- table-completion milestones (M13+/M16).

local CONFIG = {
  SCORE_ORBIT      = 750,     -- per orbit entry switch pass
  SCORE_SPINNER    = 120,     -- per spinner revolution
  SCORE_SLING      = 110,     -- per slingshot fire
  SCORE_RAMP       = 2500,    -- per ramp made
  SCORE_GEAR       = 3000,    -- per drop target down
  SCORE_BANK       = 12000,   -- per full gear bank
  SCORE_NOSE       = 5000,    -- per N-O-S standup hit
  SCORE_NOS_ALL    = 25000,   -- N-O-S word complete
  SCORE_DRIFT_SHOT = 5000 * 2, -- drift mode: doubled orbit value
  DRIFT_MODE_MS    = 15000,   -- drift-corner mode duration (live play)
  SCOOP_DWELL_MS   = 1200,    -- pit scoop dwell before the eject
  SCOOP_EJECT_SPD  = 3.0,     -- scoop eject speed (element default)
  SCOOP_EJECT_ANG  = -57,     -- scoop eject angle (element default)
  BALL_SAVE_MS     = 8000,    -- ball save per ball
  LOCK_TO_MB       = 2,       -- locked balls that trigger multiball
  BONUS_SPIN       = 50,      -- end-of-ball bonus per revolution
  BONUS_RAMP       = 400,     -- end-of-ball bonus per ramp
}

-- Gears: the bank completes → shift up. Gear multiplies ramp + orbit.
local function has_tag(ev, want)
  for _, tag in ipairs(ev.tags or {}) do
    if tag == want then return true end
  end
  return false
end

local function gear_mult()
  return 1 + (tb.state.gear or 1) * 0.5 - 0.5   -- 1.0, 1.5, 2.0, ...
end

local function drift_active()
  return tb.state.drift ~= nil
end

-- 12-audio.md §9 state stack (v1: two overlays): the current theme is
-- main, unless multiball (deepest) or the drift mode is active.
local function play_current_theme()
  if tb.state.mb_active then tb.play_music("multiball")
  elseif drift_active()   then tb.play_music("mode")
  else                         tb.play_music("main")
  end
end

local function end_drift_mode()
  if not drift_active() then return end   -- timer/drain double-fire guard
  tb.state.drift = nil
  tb.magnet_off("drift_magnet")
  play_current_theme()                    -- multiball outranks "main"
  tb.show_message("DRIFT OVER", { style = "info" })
end

-- Lifecycle ----------------------------------------------------------------
function on_init()
  tb.light_off("light_rpm_r")
  tb.light_off("light_rpm_p")
  tb.light_off("light_rpm_m")
  tb.magnet_off("drift_magnet")
end

tb.on("game_start", function(ev)
  tb.show_message("NEON DRIFT — " .. ev.player_count .. "P", { style = "mode" })
end)

tb.on("ball_start", function()
  tb.state.gear = 1
  tb.state.spin_count = 0
  tb.state.nos = { left = false, mid = false, right = false }
  tb.state.mb_active = false             -- fresh ball: no multiball carryover
  play_current_theme()                   -- 12-audio.md §9: script selects
  tb.light_blink("light_rpm_r", "slow_blink")
  tb.ball_save(CONFIG.BALL_SAVE_MS)
end)

tb.on("ball_end", function(ev)
  tb.show_message("BONUS " .. ev.bonus .. " x" .. ev.bonus_multiplier,
                  { style = "mode", duration_ms = 3000 })
end)

tb.on("game_end", function(ev)
  tb.show_message("PLAYER " .. ev.winner .. " WINS " .. ev.scores[ev.winner],
                  { style = "jackpot", duration_ms = 5000 })
end)

-- Scoring ------------------------------------------------------------------
tb.on("switch_hit", function(ev)
  if has_tag(ev, "button") then return end   -- buttons never score (§4.1)

  if ev.id == "loop_left_switch" or ev.id == "loop_right_switch" then
    local base = CONFIG.SCORE_ORBIT
    local value = math.floor(base * gear_mult())
    if drift_active() then value = value * 2 end
    tb.score(value)
    tb.state.orbits = (tb.state.orbits or 0) + 1

  elseif ev.id == "speedo_spinner" then
    tb.score(CONFIG.SCORE_SPINNER)
    tb.add_bonus(CONFIG.BONUS_SPIN)
    tb.state.spin_count = (tb.state.spin_count or 0) + 1

  elseif ev.id == "slings_left_sling" or ev.id == "slings_right_sling" then
    tb.score(CONFIG.SCORE_SLING)

  elseif has_tag(ev, "nos") then
    -- Guard: a switch can land before the first ball_start initialized
    -- the per-player table (switch testing, attract play).
    tb.state.nos = tb.state.nos or { left = false, mid = false, right = false }
    tb.score(CONFIG.SCORE_NOSE)
    local key = (ev.id == "nos_left" and "left")
        or (ev.id == "nos_mid" and "mid") or "right"
    if not tb.state.nos[key] then
      tb.state.nos[key] = true
      local all = tb.state.nos.left and tb.state.nos.mid and tb.state.nos.right
      if all then
        tb.score(CONFIG.SCORE_NOS_ALL)
        tb.show_message("NITRO SAVE", { style = "jackpot" })
        tb.state.nos = { left = false, mid = false, right = false }
      end
    end
  end
end)

tb.on("target_down", function(ev)
  if ev.bank_id ~= "gear_bank" then return end
  tb.score(CONFIG.SCORE_GEAR)
  tb.state.gear_hits = (tb.state.gear_hits or 0) + 1
end)

tb.on("bank_complete", function(ev)
  if ev.bank_id ~= "gear_bank" then return end
  tb.score(CONFIG.SCORE_BANK)
  tb.state.gear = math.min((tb.state.gear or 1) + 1, 9)
  tb.play_sound("nd_gearshift", { duck = true })
  tb.show_message("GEAR " .. tb.state.gear, { style = "mode" })
  tb.timer(600, function() tb.drop_bank_reset("gear_bank") end)
end)

tb.on("ramp_made", function(ev)
  local value = math.floor(CONFIG.SCORE_RAMP * gear_mult())
  tb.score(value)
  tb.add_bonus(CONFIG.BONUS_RAMP)
  if ev.id == "right_ramp" and not drift_active() then
    -- Drop-ramp into the drift corner: mode start (15 §1.3). Token
    -- identity guards the stale timer (timers freeze, never cancel).
    local drift_token = {}
    tb.state.drift = drift_token
    tb.magnet_on("drift_magnet")
    play_current_theme()                   -- drift-corner theme (mb still wins)
    tb.show_message("DRIFT CORNER", { style = "mode" })
    tb.timer(CONFIG.DRIFT_MODE_MS, function()
      if tb.state.drift == drift_token then end_drift_mode() end
    end)
  end
end)

-- Pit scoop: hold, then eject (the capture_ms auto-eject would fire at
-- 800 ms on its own; the timer owns the pacing instead).
tb.on("kicker_enter", function(ev)
  if ev.id ~= "pit_scoop" then return end
  tb.kick_hold("pit_scoop")
  tb.score(1000)
  tb.timer(CONFIG.SCOOP_DWELL_MS, function()
    tb.kick("pit_scoop", CONFIG.SCOOP_EJECT_SPD, CONFIG.SCOOP_EJECT_ANG)
  end)
end)

-- Locks: unlit → mandatory immediate release (10 §3.4). Lit → lock; the
-- second ball starts a basic multiball.
tb.on("ball_lock", function(ev)
  if ev.lock_id ~= "drift_lock" then return end
  if not drift_active() then
    tb.release_lock("drift_lock", 1)
    return
  end
  tb.show_message("BALL " .. ev.count .. " LOCKED", { style = "mode" })
  if ev.count >= CONFIG.LOCK_TO_MB then
    tb.release_lock("drift_lock", CONFIG.LOCK_TO_MB)
    tb.play_sound("nd_nitro_hit")         -- the table's jackpot stab
    tb.show_message("MULTIBALL", { style = "jackpot" })
  end
end)

-- Multiball edges (framework events): theme swap per 12-audio.md §9.
-- mb_active rides the framework events so drift-end/multiball-end
-- restore whichever theme is actually still running.
tb.on("multiball_start", function()
  tb.state.mb_active = true
  play_current_theme()
end)

tb.on("multiball_end", function()
  tb.state.mb_active = false
  play_current_theme()
end)

tb.on("drain", function(ev)
  tb.state.drains = (tb.state.drains or 0) + 1
  if drift_active() and (ev.balls_remaining or 0) == 0 then
    end_drift_mode()
  end
end)
