-- tables/test-lab/rules.lua
-- Reference rules for the test-lab table. Demonstrates: CONFIG block,
-- scoring, bonus, ball save, per-player state, one timed mode, timers.
-- Rules card: hit both targets to light the top lane; the lit top lane
-- scores 5000 (and starts Lab Frenzy).

-- CONFIG: every tunable number lives here (style guide, 10 §7). ----------
local CONFIG = {
  SCORE_SLING      = 110,    -- per slingshot fire
  SCORE_POP        = 500,    -- per pop bumper hit
  SCORE_TARGET     = 1000,   -- per standup target hit
  SCORE_LANE       = 500,    -- top lane, unlit
  SCORE_LANE_LIT   = 5000,   -- top lane, lit (rules-card award)
  SCORE_FRENZY_HIT = 250,    -- added to every switch during Lab Frenzy
  BONUS_POP        = 100,    -- end-of-ball bonus per pop hit
  BONUS_TARGET     = 500,    -- end-of-ball bonus per target hit
  FRENZY_MS        = 20000,  -- Lab Frenzy duration (live play)
  BALL_SAVE_MS     = 8000,   -- ball save window per ball
}

-- Helpers ----------------------------------------------------------------
local function targets_done()
  return tb.state.targets.left and tb.state.targets.right
end

local function set_lane_light()
  if tb.state.lane_lit then
    tb.light_blink("light_top_lane", "fast_blink")
  else
    tb.light_off("light_top_lane")
  end
end

local function start_frenzy()
  -- Token identity: timers only freeze (never cancel), so a stale timer
  -- from a previous frenzy must not tear down a newer one (§3.6).
  local frenzy_token = {}
  tb.state.frenzy = frenzy_token
  tb.light_blink("light_pop", "strobe")         -- pop light = frenzy tell
  tb.backglass.set_layout("mode")
  tb.backglass.animate("mode_start")
  tb.show_message("LAB FRENZY!", { style = "mode", duration_ms = 3000 })
  tb.timer(CONFIG.FRENZY_MS, function()         -- frozen while not in play
    if tb.state.frenzy ~= frenzy_token then return end
    tb.state.frenzy = false
    tb.light_off("light_pop")
    tb.backglass.set_layout("scores")
    tb.show_message("FRENZY OVER", { style = "info" })
  end)
end

-- Lifecycle --------------------------------------------------------------
function on_init()
  tb.light_off("light_pop")     -- explicit start state; obvious on reload
  tb.light_off("light_top_lane")
end

tb.on("ball_start", function()
  -- Per-player progress persists across balls; init once per player.
  tb.state.targets = tb.state.targets or { left = false, right = false }
  tb.state.lane_lit = tb.state.lane_lit or false
  tb.state.frenzy = false                       -- frenzy never spans balls
  tb.light_off("light_pop")
  -- A frenzy interrupted by a drain leaves the backglass on "mode": the
  -- stale timer's token guard early-returns, so restore it here.
  tb.backglass.set_layout("scores")
  set_lane_light()
  tb.ball_save(CONFIG.BALL_SAVE_MS)
end)

tb.on("game_end", function(ev)
  tb.show_message("PLAYER " .. ev.winner .. " WINS", { style = "jackpot" })
end)

-- Scoring ----------------------------------------------------------------
tb.on("switch_hit", function(ev)
  for _, tag in ipairs(ev.tags or {}) do  -- buttons never score (§4.1)
    if tag == "button" then return end
  end
  -- Frenzy rides on top of all normal scoring below.
  if tb.state.frenzy then tb.score(CONFIG.SCORE_FRENZY_HIT) end

  if ev.id == "slings_left_sling" or ev.id == "slings_right_sling" then
    tb.score(CONFIG.SCORE_SLING)
  elseif ev.id == "pop_main" then
    tb.score(CONFIG.SCORE_POP)
    tb.add_bonus(CONFIG.BONUS_POP)
  elseif ev.id == "target_left" or ev.id == "target_right" then
    -- Guard: a switch can land before the first ball_start initialized
    -- the per-player table (switch testing, attract play).
    tb.state.targets = tb.state.targets or { left = false, right = false }
    local side = (ev.id == "target_left") and "left" or "right"
    tb.score(CONFIG.SCORE_TARGET)
    tb.add_bonus(CONFIG.BONUS_TARGET)
    if not tb.state.targets[side] then
      tb.state.targets[side] = true
      if targets_done() and not tb.state.lane_lit then
        tb.state.lane_lit = true
        set_lane_light()
        tb.show_message("TOP LANE LIT", { style = "info" })
      end
    end
  end
end)

tb.on("rollover", function(ev)
  if ev.id ~= "top_lane" then return end
  if tb.state.lane_lit then
    tb.state.lane_lit = false
    set_lane_light()
    tb.state.targets = { left = false, right = false }  -- re-arm the ladder
    tb.score(CONFIG.SCORE_LANE_LIT)
    if not tb.state.frenzy then start_frenzy() end
  else
    tb.score(CONFIG.SCORE_LANE)
  end
end)

-- Feedback-only handlers -------------------------------------------------
tb.on("tilt_warning", function(ev)
  tb.show_message("WARNING " .. ev.count, { style = "warning" })
end)

tb.on("tilt", function()
  tb.backglass.animate("tilt")   -- framework already killed the flippers
end)
