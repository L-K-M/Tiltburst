# 12 — Audio

Part of the Tiltburst implementation plan. Canon: ../../PLAN.md

Depends on: 05-engine-core.md (threads, SPSC ring template, settings, logging),
09-table-format.md (table pack loading), 10-scripting.md (`tb.play_sound`,
`tb.play_music`), 11-game-framework.md (game states, settings menu).

This document owns everything in `/src/audio` (`tb_audio`): the miniaudio
device, the lock-free mixer, the sfxr-style SFX synthesizer, the tracker music
engine, and the `audio.json` table file. All audio content is authorable as
text (canon R9): no shipped binary audio assets are required; WAV files are an
optional convenience for human authors only.

## 1. Architecture overview

```
 sim thread (1000 Hz)                    main thread
   |  SoundEvent SPSC ring (1024)          |  Command SPSC ring (64)
   v                                       v
 +---------------------------------------------------+
 |               audio thread (miniaudio callback)   |
 |  tick->sample clock   pending events   voices[32] |
 |        |                   |              |       |
 |        v                   v              v       |
 |   tracker (music)      sfx bus         ui bus     |
 |        |                   |              |       |
 |     music bus  --duck-->   +------+-------+       |
 |        +-------------------> master -> limiter    |
 +---------------------------------------------------+
                              |
                              v  f32 stereo 48 kHz
                             DAC
```

The audio callback is the only place samples are produced: it is
allocation-free, lock-free, exception-free, and never logs directly. The sim
emits `SoundEvent`s at tick granularity; the audio thread schedules them at
exact sample positions via the drift-corrected clock (§4). Music is
synthesized live by the tracker (§8); SFX are pre-rendered PCM clips (§5)
played by the 32-voice mixer (§3).

## 2. Device

miniaudio, low-level device API only. The high-level `ma_engine` /
`ma_sound` API is **forbidden** (it owns its own mixer, allocator, and
threading; it violates the lock-free requirement).

Configuration (exact):

```cpp
ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
cfg.playback.format   = ma_format_f32;
cfg.playback.channels = 2;
cfg.sampleRate        = 48000;
cfg.periodSizeInFrames = 128;      // fallback ladder below
cfg.periods            = 2;
cfg.performanceProfile = ma_performance_profile_low_latency;
cfg.dataCallback       = &AudioSystem::dataCallback;
cfg.noPreSilencedOutputBuffer = MA_TRUE;   // callback always fills fully
```

Backend selection is miniaudio's default order per platform (WASAPI,
CoreAudio, ALSA/PulseAudio). CLI tools and CI use the miniaudio **null
backend** (`--audio-null`, and automatically when no device exists).

### 2.1 Period fallback ladder

Try `periodSizeInFrames` in this order: **128 → 256 → 512**. Move down the
ladder when either:

1. `ma_device_init` or `ma_device_start` fails at the current size, or
2. ≥ 5 underruns are detected within the first 10 s of playback. An underrun
   is counted when the wall-clock gap between consecutive callback entries
   exceeds 1.5 × the period duration (measured with the 05-engine-core.md
   monotonic clock inside the callback — reading a timestamp is allowed;
   formatting/logging is not).

The working period size is persisted to `settings.json` key
`audio.period_frames` (see 05-engine-core.md) and used directly on subsequent
launches; the settings menu's "Reset audio" action clears it. 512 always
ships; there is no failure mode below 512 other than the null backend.

### 2.2 Latency budget and startup log

Estimated output latency = `periodSizeInFrames * periods / 48000`:

| period | device buffer | + scheduling lead D (§4.2) | total target |
|---|---|---|---|
| 128 | 5.33 ms | 4.00 ms | **≤ 10 ms** (canon) |
| 256 | 10.67 ms | 6.67 ms | ≤ 18 ms (degraded, logged WARN) |
| 512 | 21.33 ms | 12.00 ms | ≤ 34 ms (degraded, logged WARN) |

At startup, after `ma_device_start`, log exactly one line (INFO at 128, WARN
otherwise):

```
audio: backend=wasapi rate=48000 period=128 periods=2 est_latency_ms=5.33 lead_ms=4.00
```

### 2.3 Callback contract

Inside the callback: no `new`/`malloc`, no mutexes, no C++ exceptions, no
file/console I/O, no `fmt::format`. All communication uses:

- **in:** the sim→audio `SoundEvent` SPSC ring (capacity 1024 events) and the
  main→audio `AudioCommand` SPSC ring (capacity 64). Both are the SPSC
  template from 05-engine-core.md; the capacities and payloads are fixed here.
- **out:** the mixed f32 stereo buffer, plus an `AudioStats` struct of
  relaxed atomics (`underruns`, `late_events`, `dropped_events`,
  `stolen_voices`, `peak_master`, `callback_cpu_pct`) that the main thread
  reads and logs every 10 s when values change.

Patch banks (§5.5) are swapped by atomic pointer publish with an epoch ack:
main thread publishes `{bank*, epoch}`; the callback copies the pointer at
entry and stores `acked_epoch`; main frees the old bank only after observing
`acked_epoch >= publish_epoch`.

```cpp
// tb_audio public surface (main thread unless noted)
class AudioSystem {
public:
  bool init(const Settings& s);              // device + ladder + log
  void shutdown();
  void publishBank(std::unique_ptr<PatchBank>);   // from table load
  bool pushCommand(const AudioCommand&);          // SPSC, non-blocking
  AudioStats stats() const;
  static void dataCallback(ma_device*, void* out, const void*, ma_uint32 frames);
};
```

## 3. Mixer

### 3.1 Voices and buses

- **32 PCM voices**, all dynamic, playing pre-rendered patch/WAV PCM on the
  `sfx` or `ui` bus. Music is *not* a PCM voice: the tracker renders directly
  into the music bus (§8), including a second tracker instance during
  crossfades.
- Buses: `sfx`, `music`, `ui` → `master`. Per-bus volume from settings keys
  `audio.master`, `audio.sfx`, `audio.music`, `audio.ui`, each an integer
  0–100. Amplitude mapping `gain = (v / 100)^2`. Defaults: master 80,
  sfx 100, music 60, ui 80. Volume changes arrive as commands and ramp
  linearly over 960 samples (20 ms) to avoid zipper noise.

Voice state:

```cpp
struct Voice {
  const float* pcm;  uint32_t len;  uint32_t pos;   // mono PCM, 48 kHz
  float gain;  float gl, gr;                        // velocity*patch gain, pan
  uint8_t bus;        // 0 sfx, 1 ui
  uint8_t priority;   // 0-9, from patch
  uint16_t patch;     // interned id
  uint64_t seq;       // global start counter
  uint32_t start_offset;   // frames into current callback buffer
};
```

Mono patch PCM is panned constant-power: `angle = (pan + 1) * π/4`,
`gl = cos(angle)`, `gr = sin(angle)`. Velocity → amplitude:
`gain = (0.25 + 0.75 * velocity) * db_to_amp(patch.volume_db)`.

### 3.2 Voice stealing

On play with no free voice:

1. Candidate = active voice with the **lowest priority**; among equals, the
   **oldest** (smallest `seq`).
2. Steal only if `new.priority >= candidate.priority`; otherwise drop the new
   sound and increment `dropped_events`.
3. Stolen voices are cut with a 64-sample (1.33 ms) linear fade-out rendered
   before reuse (mix the fade into the buffer, then rewrite the slot).

Same-patch cap: at most **4 simultaneous voices per patch id**. Starting a
5th steals the oldest voice of that patch (with the same fade). This keeps
spinner/pop machine-gunning from comb-filter buildup while letting rapid
flipper events overlap.

### 3.3 Master soft limiter

Applied to the summed master, per sample-frame, exact algorithm:

```
T = 0.891           // threshold, -1 dBFS
k = 1.5             // tanh drive
inv = 1.0 / tanh(k) // makeup, = 1.105
a_att = exp(-1.0 / (0.002 * 48000))  // 2 ms attack  = 0.98965
a_rel = exp(-1.0 / (0.150 * 48000))  // 150 ms release = 0.999861

peak = max(|L|, |R|)
env  = (peak > env) ? a_att * env + (1 - a_att) * peak
                    : a_rel * env + (1 - a_rel) * peak
g    = (env > T) ? T / env : 1.0
outL = tanh(k * g * L) * inv;   outR = tanh(k * g * R) * inv
```

`env` persists across callbacks. The tanh stage is a safety shaper: at
`|x| ≤ 0.5` its gain error is < 0.9 dB; the gain-reduction stage does the
real limiting. Track `peak_master` (post-limiter absolute peak) in stats.

## 4. Event path: sim tick → sample-accurate playback

### 4.1 SoundEvent

The sim emits one `SoundEvent` per sound, at the tick it happens — both from
engine-automatic purposes (§7.2) and from `tb.play_sound` (scripts run on the
sim thread; see 10-scripting.md). 16-byte POD:

```cpp
struct SoundEvent {
  uint32_t tick;       // sim tick (1000 Hz) the sound occurs on
  uint16_t patch;      // interned patch id (§5.5)
  uint8_t  flags;      // bit0: duck (§10); bits1-2: bus (0 sfx, 1 ui)
  uint8_t  _pad;
  float    velocity;   // 0..1
  float    pan;        // -1..+1, sim computes pan = clamp(2*x/width - 1, -1, 1) * 0.6
};
static_assert(sizeof(SoundEvent) == 16);
```

This ring is the audio leg of the 05-engine-core.md §8.2 ring topology, and
it is authoritative here: sounds cross the sim→audio boundary as
`SoundEvent`, never as the 32-byte `SimEvent` that `render_ring` and
`game_ring` carry. Element type, capacity (1024), and overflow policy are
fixed in this section. Purpose→patch resolution (§7.2, via the table `map`
interned at table load), velocity, and pan are all computed **on the sim
thread at emission** — deterministic, part of the simulation — so the
callback never consults the map or intern table; it only schedules
ready-made events. A `tb.play_sound` emission has no ball behind it, so it
carries `velocity = 1.0` and `pan = 0.0` — impact-scaled velocity and
position-derived pan are what the automatic §7.2 purposes add (§6.2).

Ring overflow (producer side full): drop the *new* event, increment a sim-side
counter mirrored into `dropped_events`. UI sounds from the main thread (menus)
travel in `AudioCommand{PlayUi, patch}` and start at the next buffer start —
no tick mapping.

### 4.2 Drift-corrected tick-to-sample clock

The sim clock (OS monotonic driving the 1000 Hz loop) and the DAC clock drift
relative to each other (typically < 100 ppm, but unbounded across devices).
The audio thread owns this clock; nominal rate is 48 samples per tick.

State and per-callback update (`stream_pos` = total frames written since
device start, `P` = period frames, `T` = atomic load of the newest completed
sim tick, published by the sim each tick):

```
double   spt = 48.0;        // samples per tick, corrected
uint64_t anchor_tick;       // sim tick mapped to anchor_sample
double   anchor_sample;
double   d_avg = 0.0;       // smoothed phase error, samples
bool     init = false;

per callback:
  if (!init) { anchor_tick = T; anchor_sample = stream_pos; init = true; }
  ideal = anchor_sample + (T - anchor_tick) * spt   // where tick T maps
  err   = ideal - stream_pos                        // + : mapping runs ahead
  if (|err| > 480) {                                // 10 ms: sim stall, pause,
    anchor_tick = T; anchor_sample = stream_pos;    // underrun -> hard re-anchor
    d_avg = 0;
  } else {
    d_avg = 0.98 * d_avg + 0.02 * err
    spt   = clamp(48.0 * (1.0 - d_avg / 480000.0), 47.976, 48.024)
  }                                                 // ±500 ppm slew; removes
                                                    // d_avg over ~10 s
```

Mapping with scheduling lead `D = P + 64` frames (4.0 ms at P = 128):

```
sample_for_tick(t) = anchor_sample + (t - anchor_tick) * spt + D
```

`D` exists because an event produced during tick `t` is first seen by the
callback up to one period later; the lead guarantees the mapped position is
normally in the future, so **relative spacing between rapid events (flipper,
sling, spinner at 5 ms apart) is preserved exactly** instead of being
quantized to callback boundaries.

Per callback, after the clock update:

1. Drain the SoundEvent ring. For each event compute
   `s = round(sample_for_tick(tick))`.
2. `s < stream_pos` → **late**: start at offset 0, increment `late_events`.
3. `stream_pos <= s < stream_pos + P` → start this callback at
   `start_offset = s - stream_pos`.
4. `s >= stream_pos + P` → push to the pending queue (fixed array, capacity
   64, insertion-sorted by `s`; if full, treat as late). Re-examined next
   callback.

Because a voice's `start_offset` is honored mid-buffer, two flipper hits 5 ms
apart start exactly `round(5 * spt)` ≈ 240 samples apart regardless of period
size.

## 5. SFX synthesizer (sfxr-style)

Patches are **pre-rendered to mono PCM at table load** on the main thread:
deterministic given the same binary, cheap (< 1 ms per patch), cached in the
`PatchBank`. The generator runs at the classic sfxr rate of **44100 Hz** with
the classic constants below (so well-known sfxr parameter intuitions and
presets transfer directly), then the finished clip is linearly resampled to
48000 Hz (`x = i * 44100 / 48000`, `out[i] = lerp(in[⌊x⌋], in[⌊x⌋+1], frac)`).

### 5.1 Parameters (JSON keys, per patch)

| Key | Range | Default | Meaning |
|---|---|---|---|
| `wave` | `square` `saw` `sine` `noise` `triangle` | `square` | oscillator |
| `duty` | 0.02–0.98 | 0.5 | square duty cycle (0.5 = symmetric) |
| `duty_sweep` | −1–1 | 0 | + narrows the pulse over time |
| `attack` | 0–1 | 0 | envelope attack; seconds = `attack² × 2.268` |
| `sustain` | 0–1 | 0.3 | envelope sustain; same time mapping |
| `punch` | 0–1 | 0 | extra transient level during sustain |
| `decay` | 0–1 | 0.4 | envelope decay; same time mapping |
| `base_freq` | 0–1 | 0.3 | pitch; `f ≈ 3528·(base_freq² + 0.001)` Hz |
| `freq_limit` | 0–1 | 0 | if > 0, sound ends when pitch falls below it |
| `freq_slide` | −1–1 | 0 | + slides pitch up, − down |
| `freq_delta_slide` | −1–1 | 0 | acceleration of the slide |
| `vib_depth` | 0–1 | 0 | vibrato depth |
| `vib_speed` | 0–1 | 0 | vibrato rate |
| `arp_mod` | −1–1 | 0 | one-shot pitch jump; + up, − far down; 0 = off |
| `arp_speed` | 0–1 | 0 | how soon the jump happens (1 = immediate) |
| `repeat_speed` | 0–1 | 0 | re-runs pitch/arp program (0 = off) |
| `flanger_offset` | −1–1 | 0 | flanger delay |
| `flanger_sweep` | −1–1 | 0 | flanger delay sweep |
| `lpf_cutoff` | 0–1 | 1 | low-pass cutoff; 1.0 = bypass |
| `lpf_sweep` | −1–1 | 0 | cutoff sweep |
| `lpf_resonance` | 0–1 | 0 | low-pass resonance |
| `hpf_cutoff` | 0–1 | 0 | high-pass cutoff |
| `hpf_sweep` | −1–1 | 0 | high-pass cutoff sweep |
| `volume_db` | −24–+6 | 0 | applied after normalization (§5.4) |
| `priority` | 0–9 | 5 | voice-stealing priority (§3.2) |

Validation (tb_validate, see 09-table-format.md): unknown keys are errors;
out-of-range values are errors; `sustain == 0 && decay == 0` is an error.

### 5.2 Parameter → internal mapping (at 44100 Hz)

```
fperiod    = 100.0 / (base_freq² + 0.001)        // period, subsample units
fmaxperiod = 100.0 / (freq_limit² + 0.001)
fslide     = 1.0 - freq_slide³ * 0.01            // per-sample period multiplier
fdslide    = -freq_delta_slide³ * 1e-6           // per-sample fslide increment
square_duty = duty;      duty_slide = -duty_sweep * 5e-5     // per sample
vib_amp    = vib_depth * 0.5
vib_inc    = vib_speed² * 0.01                   // radians per sample
arp_mult   = arp_mod >= 0 ? 1.0 - arp_mod² * 0.9 : 1.0 + arp_mod² * 10.0
arp_limit  = arp_mod == 0 ? 0 : int((1 - arp_speed)² * 20000 + 32)   // samples
rep_limit  = repeat_speed == 0 ? 0 : int((1 - repeat_speed)² * 20000 + 32)
env_len[ATTACK]  = int(attack²  * 100000)        // samples; 1.0 -> 2.268 s
env_len[SUSTAIN] = int(sustain² * 100000)
env_len[DECAY]   = int(decay²   * 100000)
flanger_phase  = sign(flanger_offset) * flanger_offset² * 1020   // subsamples
flanger_dphase = sign(flanger_sweep)  * flanger_sweep²  * 1.0
fltw   = lpf_cutoff³ * 0.1;    fltw_d  = 1.0 + lpf_sweep * 1e-4
fltdmp = clamp(5.0 / (1.0 + lpf_resonance² * 20.0) * (0.01 + fltw), 0.0, 0.8)
flthp  = hpf_cutoff² * 0.1;    flthp_d = 1.0 + hpf_sweep * 3e-4
```

### 5.3 Sample-generation loop (pseudocode)

Waveforms (`fp` = phase/period in [0,1)):

```
square:   fp < square_duty ? 0.5 : -0.5
saw:      1.0 - 2.0 * fp
sine:     sin(2π * fp)
triangle: fp < 0.5 ? 4*fp - 1 : 3 - 4*fp
noise:    noise_buf[int(fp * 32)]     // 32 floats in [-1,1]
```

Noise RNG: a PCG32 (05-engine-core.md) seeded with the FNV-1a hash of the
patch id string, so pre-rendering is reproducible run to run.

```
reset_run():                     // at t=0 and on each repeat trigger
  recompute fperiod, fslide, square_duty, arp_time = 0, arp_done = false
  // envelope, phase, filters, flanger state are NOT reset

for each output sample until envelope finishes (hard cap 4.0 s -> WARN):
  rep_time++
  if rep_limit > 0 and rep_time >= rep_limit: rep_time = 0; reset_run()
  arp_time++
  if arp_limit > 0 and !arp_done and arp_time >= arp_limit:
      arp_done = true; fperiod *= arp_mult
  fslide  += fdslide
  fperiod *= fslide
  if fperiod > fmaxperiod:
      fperiod = fmaxperiod
      if freq_limit > 0: break                    // sound ends
  vib_phase += vib_inc
  rfperiod  = fperiod * (1.0 + vib_amp * sin(vib_phase))
  period    = max(8, int(rfperiod))
  square_duty = clamp(square_duty + duty_slide, 0.02, 0.98)

  env_time++                                       // 3-stage envelope
  while env_time > env_len[stage]: env_time = 0; stage++   // skip len==0
  if stage > DECAY: break
  env_vol = stage == ATTACK  ? env_time / env_len[ATTACK]
          : stage == SUSTAIN ? 1.0 + (1.0 - env_time / env_len[SUSTAIN]) * 2.0 * punch
          :                    1.0 - env_time / env_len[DECAY]

  flanger_phase += flanger_dphase
  iphase = clamp(abs(int(flanger_phase)), 0, 1023)

  ssample = 0
  repeat 8 times:                                  // 8x supersample
      phase++
      if phase >= period:
          phase %= period
          if wave == noise: refill noise_buf[32] from rng
      raw = waveform(wave, phase / period)
      prev_fltp = fltp                             // resonant low-pass
      fltw = clamp(fltw * fltw_d, 0.0, 0.1)
      if lpf_cutoff < 1.0:
          fltdp += (raw - fltp) * fltw
          fltdp -= fltdp * fltdmp
      else:
          fltp = raw; fltdp = 0
      fltp += fltdp
      flthp = clamp(flthp * flthp_d, 1e-5, 0.1)    // high-pass
      fltphp += fltp - prev_fltp
      fltphp -= fltphp * flthp
      sub = fltphp
      flanger_buf[ipos & 1023] = sub               // flanger
      sub += flanger_buf[(ipos - iphase + 1024) & 1023]
      ipos++
      ssample += sub
  emit clamp((ssample / 8.0) * env_vol, -8.0, 8.0)
```

### 5.4 Normalization

After rendering: find `peak = max(|s|)`. If `peak > 1e-4`, scale the clip so
`peak == 0.891` (−1 dBFS), then apply `db_to_amp(volume_db)`. Relative
loudness between patches is authored **only** via `volume_db`, never by the
raw synth level. A clip that is all-zero after rendering is a tb_validate
error.

### 5.5 Patch bank and id interning

The built-in bank (§7) registers ids 0–23 in the order listed there. Table
`patches` and `wav` entries get ids 24+ in JSON key order; a table patch with
a built-in's name **overrides** it (same id, new PCM). `wav` entries are
decoded with miniaudio (`ma_decoder`, any PCM WAV, mono or stereo — stereo is
downmixed `0.5*(L+R)`), resampled to 48 kHz, max 10 s (validator error
beyond). The complete bank is immutable after load and swapped as in §2.3.

## 6. `audio.json` schema

One file per table (canon §5.5). `//` comments allowed. All four top-level
keys optional; missing sections fall back to built-ins.

```json
{
  "patches": {                       // §5.1 params; overrides built-ins by name
    "flipper_clack": { "wave": "noise", "base_freq": 0.26, "punch": 0.4,
                       "sustain": 0.05, "decay": 0.12, "lpf_cutoff": 0.5 },
    "lead_pulse":  { "wave": "square", "duty": 0.25, "attack": 0.05,
                     "sustain": 0.5, "decay": 0.3,  "volume_db": -2 },
    "stab_pulse":  { "wave": "square", "duty": 0.5,  "sustain": 0,
                     "decay": 0.15, "volume_db": -4 },
    "bass_saw":    { "wave": "saw",  "sustain": 0.5, "decay": 0.2 },
    "kit_noise":   { "wave": "noise", "sustain": 0,  "decay": 0.18 }
  },
  "wav": {                           // optional, human authors only
    "crowd_cheer": "assets/cheer.wav"
  },
  "songs": {                         // §8; reserved ids listed in §9
    "main": { /* full example in §8.5 */ }
  },
  "map": {                           // purpose -> patch id (§7.2 key set,
                                     // listed again in §6.2)
    "drain": "crowd_cheer",          // wav ids are valid targets
    "spinner": "spinner_tick",
    "pop_bumper": "none"             // reserved: disable this automatic
                                     // purpose, the script owns it (§6.2c)
  }
}
```

Every id namespace (patches, wav, songs) is flat per table; a `map` value or
script `tb.play_sound` id must resolve to a table patch, a table wav, or a
built-in patch — otherwise tb_validate errors. A `map` **key** must be one of
the §7.2 purpose keys (unknown key = error), and its value is either such an
id or the reserved word `"none"`, which disables that automatic purpose
(§6.2). §6.2 is the rule for deciding which mechanism a
15-launch-tables.md brief row uses.

### 6.1 The complete test-lab `audio.json`

Audio companion to the canonical test-lab table (canon 5.8): table.json in
09-table-format.md §7, rules.lua in 10-scripting.md §6. This exact file
ships at `tables/test-lab/audio.json` and must match this listing
byte-for-byte (comments included); `tb_validate tables/test-lab` exits 0
with zero warnings (CI gate, 16-testing-ci.md). It defines every audio id
rules.lua references — patches `sfx_mode_start`, `sfx_bonus_count`,
`sfx_scoop` and the reserved song `main` (§9); every other sound falls back
to the built-ins (§7).

```json
{
  // test-lab — minimal valid audio file. Canon: PLAN.md 5.8.
  // Companion to table.json (09-table-format.md §7) and rules.lua
  // (10-scripting.md §6); used by tb_audio unit tests and the authoring docs.
  "patches": {
    // SFX referenced by rules.lua
    "sfx_mode_start":  { "wave": "square", "base_freq": 0.45, "punch": 0.4,
                         "sustain": 0.25, "decay": 0.35, "arp_mod": 0.5,
                         "arp_speed": 0.5, "priority": 7 },
    "sfx_bonus_count": { "wave": "triangle", "base_freq": 0.5,
                         "sustain": 0.03, "decay": 0.1, "volume_db": -3,
                         "priority": 4 },
    "sfx_scoop":       { "wave": "noise", "base_freq": 0.14, "punch": 0.4,
                         "sustain": 0.08, "decay": 0.22, "lpf_cutoff": 0.35,
                         "priority": 6 },
    // Tracker instruments for the "main" song (channel rules in §8.1)
    "lab_lead": { "wave": "square", "duty": 0.25, "attack": 0.02,
                  "sustain": 0.4, "decay": 0.25, "volume_db": -2 },
    "lab_bass": { "wave": "triangle", "sustain": 0.5, "decay": 0.2 },
    "lab_kit":  { "wave": "noise", "sustain": 0, "decay": 0.15 }
  },
  "songs": {
    // Reserved id (§9); rules.lua starts it with tb.play_music("main").
    "main": {
      "bpm": 120, "ticks_per_row": 12,
      "patterns": {
        "A": {
          "pulse1": ["A-4 lab_lead 10", "---", "C-5", "---",
                     "E-5", "---", "C-5", "---",
                     "G-4", "---", "B-4", "---",
                     "D-5", "---", "B-4", "OFF"],
          "pulse2": ["---", "A-3 lab_lead 6", "---", "A-3",
                     "---", "A-3", "---", "A-3",
                     "---", "G-3", "---", "G-3",
                     "---", "G-3", "---", "G-3"],
          "wide":   ["A-2 lab_bass 12", "---", "A-2", "---",
                     "A-2", "---", "A-2", "---",
                     "G-2", "---", "G-2", "---",
                     "G-2", "---", "G-2", "---"],
          "noise":  ["C-2 lab_kit 14", "C-6 . 5", "C-2 . 14", "C-6 . 5",
                     "C-2 . 14", "C-6 . 5", "C-2 . 14", "C-6 . 5",
                     "C-2 . 14", "C-6 . 5", "C-2 . 14", "C-6 . 5",
                     "C-2 . 14", "C-6 . 5", "C-2 . 14", "C-6 . 5"]
        }
      },
      "order": ["A"]
    }
  }
}
```

### 6.2 Reading a table sound brief: `map` vs. `tb.play_sound`

Every table in 15-launch-tables.md carries a "Sound / music brief" whose
rows are keyed inconsistently by design — some by a §7.2 purpose key
(`pop_bumper`, `ramp_made`, `drain`), some by a canon event name
(`spinner_spin`, `target_down`, `bank_complete`), some by prose ("magnet
on", "hatch purchase", "letter collect"). Tiltburst has exactly **two**
sound mechanisms, and every brief row lands in one of them:

1. **`map` (§6) — engine-automatic, no Lua.** Available only when the row's
   key *is* one of the §7.2 purpose keys (or aliases one, table below).
   Author `"map": { "<purpose>": "<patch id>" }`. The sim emits the
   `SoundEvent` itself with impact-derived velocity and position-derived
   pan (§7.2, §4.1).
2. **`tb.play_sound(patch_id, opts)` from `rules.lua` (10 §3.3) —
   script-played.** Everything else. The patch is declared in `patches` /
   `wav` exactly the same way; only the trigger differs. Script-played
   events carry `velocity = 1.0` and `pan = 0.0` (the API takes no velocity
   or pan option), so a row that wants impact-scaled loudness or
   table-position panning has to be a `map` row.

**The complete `map` key set** (§7.2 is normative; 19 keys, nothing else is
a legal `map` key):

```
flipper  slingshot  pop_bumper  standup_target  drop_target  spinner
rollover ramp_made  magnet      kicker          launch       drain
tilt_warning  tilt  ball_lock   wall_hit        ball_ball
menu_move     menu_select
```

Brief rows resolve like this:

| Brief row reads | Mechanism | `map` key / call |
|---|---|---|
| any word from the list above | `map` | itself |
| `spinner_spin` (the event) | `map` | `spinner` |
| `target_down` (a drop bank falling) | `map` | `drop_target` |
| "standup hit" | `map` | `standup_target` |
| "magnet on", magnet pulse/grab | `map` | `magnet` |
| "kickback fire", scoop/saucer eject | `map` | `kicker` (the element is a `kicker`) |
| plunger/auto-launch release | `map` | `launch` |
| `bank_complete`, `switch_hit`, `kicker_enter`, `ball_launched`, `captive_full_travel`, `multiball_start` / `multiball_end`, `timer_tick`, and every game-lifecycle event (§4.3 of 10) | script | `tb.play_sound` in that event's handler |
| jackpot / super-jackpot rows | script | `tb.play_sound("jackpot_hit"/…, {duck=true})` — these built-ins have no automatic trigger (§7.2) |
| prose rows ("letter collect", "order delivered", "act start", "streak lost", "conveyor tick", "hatch purchase", "bank at van", "lockdown", "magnet release", "cannon fire") | script | `tb.play_sound` at the point in the rules where the thing happens |

Three rules settle the rows that look like purposes but are not:

- **(a) One purpose = one patch, table-wide.** `map` cannot vary by
  element. Cosmic Carnival's three spinners at 660 / 880 / 1100 Hz need
  three patches played from one `spinner_spin` handler that switches on
  `ev.id` — not three `map` entries, which cannot exist.
- **(b) One purpose = one fixed clip.** Patches are pre-rendered PCM (§5),
  so rows asking for randomized or tracking pitch (Atomic Diner's
  `ad_bubble` ±3 semitones, Cosmic Carnival's `cc_honk` ±2 semitones,
  Tilt-O-Tron's `tt_crane` 600→450→350 Hz, Neon Drift's `nd_tach`
  +4 % per spin capped at 2×) ship **one patch per rung**, picked in Lua
  (`tb.rng_range` for random rows, state for tracking rows). To retune a
  patch by frequency factor `r`, invert §5.1's
  `f ≈ 3528·(base_freq² + 0.001)`:
  `base_freq' = sqrt(r·(base_freq² + 0.001) − 0.001)` — e.g. `base_freq
  0.30` = 321 Hz, and ×2 gives `base_freq' = 0.4254` = 642 Hz. The synth
  ceiling is `base_freq = 1.0` → 3531.5 Hz, so a full 2× ladder must start
  at `base_freq ≤ 0.707` (≤ 1766 Hz). `nd_tach`'s +4 %/spin ladder is
  therefore 18 rungs `k = 0…17` at `r = 1.04ᵏ` (rung 17 = 1.95×, since
  `1.04¹⁸ = 2.03×` would pass the cap) plus one rung pinned at exactly 2× —
  19 patches, selected with `k = min(spins, 18)`.
- **(c) Silence the purpose you took over.** When (a) or (b) moves a
  purpose row into the script, set that purpose to the reserved value
  `"none"` — `"map": { "pop_bumper": "none" }` — or the automatic sound
  doubles with the scripted one. `"none"` is the only non-id value a `map`
  entry may take; tb_validate errors if a table defines a patch or wav
  named `none`.

Worked example — Neon Drift's §1.6 brief (patches declared in its
`patches` block) becomes:

```jsonc
"map": {
  "flipper":    "nd_shift_clack",   // brief row "flipper (both)"
  "pop_bumper": "nd_horn",
  "magnet":     "nd_drift_skid",    // brief row "magnet on"
  "ramp_made":  "nd_boost",
  "spinner":    "none"              // rule (b): the tach ladder is scripted
}
```

```lua
tb.on("bank_complete", function() tb.play_sound("nd_gearshift") end)
-- rule (b): 19-rung tach ladder, "nd_tach_00" … "nd_tach_18"
tb.on("spinner_spin", function()
  local k = math.min(tb.state.spins or 0, 18)
  tb.state.spins = (tb.state.spins or 0) + 1
  tb.play_sound(string.format("nd_tach_%02d", k))
end)

local function award_jackpot()      -- jackpot fires no event: play the
  tb.score(300000)                  -- sound where the rules award it
  tb.play_sound("nd_nitro_hit", { duck = true })
end
```

**Captive balls have no purpose of their own.** The striking ball's contact
with a `captive_ball` resolves through the ball–ball path (08-physics.md
§8, §6.13), so the automatic `ball_ball` purpose already covers the thunk
with impact-derived velocity, and the impact additionally fires the standard
`switch_hit{id, ball_id, speed, tags}`. The specialized
`captive_full_travel{id}` event (the captive reaching the far end `b` at
≥ 0.3 m/s — canon §5.7) has **no** automatic purpose and never will: that
bounce is a 1-D slot clamp, not a collider contact, so it produces no
`wall_hit` either. Play it from the script:

```lua
tb.on("captive_full_travel", function(ev) tb.play_sound("ad_shake") end)
```

A table wanting a distinct captive thunk scripts it the same way rather
than overriding `ball_ball` in `map`, which would also retexture every
ball-to-ball click during multiball.

## 7. Built-in patch bank

### 7.1 The 24 built-in patches

Compiled into `tb_audio` as data (also mirrored to `/assets/patches.json` for
reference). Unlisted params take §5.1 defaults. Ids 0–23 in this order:

```json
{
"flipper_clack":     {"wave":"noise","base_freq":0.22,"sustain":0.05,"punch":0.35,"decay":0.13,"lpf_cutoff":0.42,"lpf_resonance":0.3,"hpf_cutoff":0.05,"volume_db":-2,"priority":8},
"sling_thwack":      {"wave":"noise","base_freq":0.30,"sustain":0.06,"punch":0.5,"decay":0.18,"freq_slide":-0.25,"lpf_cutoff":0.5,"priority":7},
"pop_bumper_ding":   {"wave":"square","base_freq":0.62,"sustain":0.08,"punch":0.4,"decay":0.25,"freq_slide":-0.1,"priority":7},
"target_thud":       {"wave":"noise","base_freq":0.15,"sustain":0.04,"punch":0.3,"decay":0.12,"lpf_cutoff":0.3,"priority":4},
"drop_target_clunk": {"wave":"noise","base_freq":0.12,"sustain":0.06,"punch":0.45,"decay":0.2,"freq_slide":-0.2,"lpf_cutoff":0.35,"lpf_resonance":0.4,"priority":5},
"spinner_tick":      {"wave":"square","base_freq":0.75,"sustain":0.02,"punch":0.2,"decay":0.05,"hpf_cutoff":0.2,"volume_db":-4,"priority":3},
"rollover_chime":    {"wave":"triangle","base_freq":0.55,"sustain":0.1,"decay":0.35,"arp_mod":0.35,"arp_speed":0.6,"priority":4},
"ramp_whoosh":       {"wave":"noise","base_freq":0.2,"sustain":0.25,"decay":0.3,"freq_slide":0.3,"lpf_cutoff":0.6,"lpf_sweep":0.2,"hpf_cutoff":0.1,"volume_db":-3,"priority":5},
"magnet_hum":        {"wave":"square","duty":0.25,"base_freq":0.09,"sustain":0.5,"decay":0.2,"vib_depth":0.3,"vib_speed":0.4,"lpf_cutoff":0.25,"volume_db":-6,"priority":4},
"kicker_pop":        {"wave":"square","base_freq":0.45,"sustain":0.04,"punch":0.6,"decay":0.2,"freq_slide":-0.4,"priority":6},
"launch_spring":     {"wave":"noise","base_freq":0.12,"sustain":0.15,"decay":0.25,"freq_slide":0.55,"lpf_cutoff":0.5,"lpf_sweep":0.3,"priority":6},
"drain_womp":        {"wave":"saw","base_freq":0.28,"sustain":0.2,"punch":0.2,"decay":0.45,"freq_slide":-0.3,"lpf_cutoff":0.4,"lpf_sweep":-0.2,"priority":8},
"tilt_warning_buzz": {"wave":"square","duty":0.2,"base_freq":0.18,"sustain":0.3,"decay":0.1,"vib_depth":0.6,"vib_speed":0.7,"priority":8},
"tilt_alarm":        {"wave":"square","duty":0.3,"base_freq":0.35,"sustain":0.6,"decay":0.15,"vib_depth":1.0,"vib_speed":0.15,"priority":9},
"jackpot_hit":       {"wave":"square","base_freq":0.7,"sustain":0.3,"punch":0.5,"decay":0.45,"arp_mod":0.45,"arp_speed":0.55,"flanger_offset":0.25,"flanger_sweep":0.1,"priority":8},
"lock_clunk":        {"wave":"noise","base_freq":0.1,"sustain":0.08,"punch":0.5,"decay":0.25,"lpf_cutoff":0.3,"lpf_resonance":0.5,"priority":6},
"multiball_riser":   {"wave":"saw","base_freq":0.15,"sustain":0.7,"decay":0.3,"freq_slide":0.35,"freq_delta_slide":0.1,"vib_depth":0.2,"vib_speed":0.5,"flanger_offset":0.3,"flanger_sweep":0.15,"lpf_cutoff":0.6,"lpf_sweep":0.25,"priority":9},
"extra_ball_fanfare":{"wave":"square","base_freq":0.5,"sustain":0.5,"punch":0.3,"decay":0.4,"arp_mod":0.6,"arp_speed":0.35,"repeat_speed":0.55,"priority":9},
"menu_move":         {"wave":"square","base_freq":0.55,"sustain":0.03,"decay":0.09,"hpf_cutoff":0.15,"volume_db":-6,"priority":2},
"menu_select":       {"wave":"square","base_freq":0.5,"sustain":0.08,"punch":0.2,"decay":0.18,"freq_slide":0.25,"volume_db":-4,"priority":2},
"add_player":        {"wave":"square","base_freq":0.42,"sustain":0.1,"punch":0.2,"decay":0.22,"arp_mod":0.4,"arp_speed":0.7,"volume_db":-3,"priority":5},
"knocker":           {"wave":"noise","base_freq":0.32,"sustain":0.03,"punch":0.9,"decay":0.16,"freq_slide":-0.3,"lpf_cutoff":0.6,"lpf_resonance":0.3,"volume_db":2,"priority":9},
"timer_low":         {"wave":"square","duty":0.3,"base_freq":0.58,"sustain":0.03,"decay":0.08,"hpf_cutoff":0.15,"volume_db":-4,"priority":6},
"bonus_tick":        {"wave":"triangle","base_freq":0.5,"sustain":0.02,"decay":0.07,"freq_slide":0.15,"volume_db":-5,"priority":4}
}
```

### 7.2 Automatic purposes (the default `map`)

The engine emits these SoundEvents itself — no script code required. This
table is the **complete purpose key set**: a `map` key that is not listed
here is a tb_validate error, and a sound whose trigger is not listed here is
script-played (§6.2). A table overrides any subset via `map`, or disables a
purpose with `"none"` (§6.2c). Velocity for impact purposes is
`clamp(impact_speed / 8.0, 0, 1)` (m/s); for state purposes it is 1.0.

| Purpose key | Fired when | Default patch |
|---|---|---|
| `flipper` | flipper button press (per flipper) | `flipper_clack` |
| `slingshot` | sling fires | `sling_thwack` |
| `pop_bumper` | pop bumper fires | `pop_bumper_ding` |
| `standup_target` | standup hit | `target_thud` |
| `drop_target` | drop target falls | `drop_target_clunk` |
| `spinner` | each spinner step (`spinner_spin`) | `spinner_tick` |
| `rollover` | rollover triggered | `rollover_chime` |
| `ramp_made` | `ramp_made` event | `ramp_whoosh` |
| `magnet` | magnet on/pulse, retriggered every 500 ms while held | `magnet_hum` |
| `kicker` | kicker fires | `kicker_pop` |
| `launch` | plunger release | `launch_spring` |
| `drain` | `drain` event | `drain_womp` |
| `tilt_warning` | `tilt_warning` event | `tilt_warning_buzz` |
| `tilt` | `tilt` event | `tilt_alarm` |
| `ball_lock` | `ball_lock` event | `lock_clunk` |
| `wall_hit` | ball–wall/post impact > 1.5 m/s, rate-limited 30 ms/ball | `target_thud` |
| `ball_ball` | ball–ball impact, rate-limited 30 ms | `lock_clunk` |
| `menu_move` / `menu_select` | UI navigation (main thread) | respective patches |

`jackpot_hit` and `multiball_riser` have no automatic trigger; scripts play
them via `tb.play_sound` (10-scripting.md). There is likewise **no captive
ball purpose**: the strike is covered by `ball_ball` (plus its
`switch_hit`), and `captive_full_travel` is script-played — see §6.2. The
game framework
(11-game-framework.md) plays `add_player` (player joins), `extra_ball_fanfare`
(extra ball awarded), `knocker` (replay/match award), `timer_low` (final-
seconds timer tick, once per second), and `bonus_tick` (each bonus-count
step) — emitted by the framework on the sim thread, with no §7.2 purpose
entry and no script code required.

## 8. Tracker music

Music is synthesized live on the audio thread by a 4-channel tracker — cheap
(4 oscillators) and fully text-authorable.

### 8.1 Channels

| Channel | Allowed instrument `wave` | Typical role |
|---|---|---|
| `pulse1` | `square` | lead |
| `pulse2` | `square` | chords / arps |
| `wide` | `saw`, `triangle`, `sine` | bass / pads |
| `noise` | `noise` | drums |

An instrument is a patch id from `patches` (or built-ins). Tracker mode uses
**only** these patch fields: `wave`, `duty`, `attack`, `sustain`, `decay`,
`volume_db`. `sustain > 0` means *gated*: attack → hold at 1.0 until note
end. `sustain == 0` means *one-shot*: attack → immediate release. Attack and
release seconds use the §5.1 mapping (`x² × 2.268 s`). All other patch
params are ignored in tracker mode. Wrong `wave` for the channel is a
tb_validate error. Channels are monophonic: a new note hard-retriggers
(phase reset) with a 64-sample linear fade-out of the previous signal mixed
under the new attack to avoid clicks.

Oscillators run at 48 kHz with a float phase accumulator
(`phase += f / 48000`, wrap at 1.0) using the §5.3 waveform formulas — naive,
aliasing, intentionally chip-flavored; no supersampling, no filters. The
noise channel is sample-and-hold white noise: a new PCG32 value (seed
`0x5EEDCAFE` per tracker instance at song start) every `48000 / (8 * f)`
samples via a fractional accumulator, so note pitch controls noise color.

### 8.2 Note names and frequency

Note = letter (`A`–`G`), optional `#` (sharps only, no flats), `-`, octave
digit 0–8. Semitone index: C 0, C# 1, D 2, D# 3, E 4, F 5, F# 6, G 7, G# 8,
A 9, A# 10, B 11. Then:

```
midi = (octave + 1) * 12 + semitone        // "C-4" = 60, "A-4" = 69
f    = 440.0 * 2^((midi - 69) / 12)        // Hz
```

### 8.3 Song and pattern format

```json
{ "bpm": 112, "ticks_per_row": 12,
  "patterns": { "<name>": { "pulse1": ["<cell>", ...], "pulse2": [...],
                             "wide": [...], "noise": [...] } },
  "order": ["<name>", ...] }
```

- `bpm` 40–260; `ticks_per_row` 1–31. Classic tempo math:
  `tick_seconds = 2.5 / bpm`, `row_seconds = ticks_per_row * tick_seconds`.
  With `ticks_per_row = 6` a row is a 16th note at `bpm`; with 12, an 8th.
- Every pattern has all four channels with exactly **16 or 32 rows** (all
  patterns in one song must match). `order` is 1–128 pattern names; the song
  loops from the end of `order` back to index 0 (looping is inherent; a
  non-looping song simply gets stopped by script).

Cell = string of 1–4 whitespace-separated tokens: `note [inst] [vol] [fx]`,
with `.` as an explicit skip for a middle token.

| Token | Values | Semantics |
|---|---|---|
| note | `C-4` etc., `---`, `OFF` | `---`: nothing new this row; `OFF`: release |
| inst | patch id | sticky per channel; required on a channel's first note |
| vol | 0–15 | sticky; initial 12; channel gain = `(vol/15)²` |
| fx | below | scope as listed |

Effects (applied on **music ticks**, not per row and not per sample):

| fx | Example | Semantics | Scope |
|---|---|---|---|
| `A<hex digits>` | `A037` | arpeggio: on tick `k`, `f = f_note * 2^(d[k mod n]/12)`, digits are hex semitone offsets | its row only |
| `S+n` / `S-n` | `S-2` | slide `n` semitones (integer 1–12) spread over the row: each tick `f *= 2^(±n/(12*ticks_per_row))`; the change persists | its row only |
| `V<d>,<s>` | `V3,5` | vibrato: depth `d` 0–15 in eighth-semitones, speed `s` 0–15; per tick `phase += s*2π/64`, `f_used = f * 2^((d/8)*sin(phase)/12)` | until note end or replaced |
| `.` / absent | | no effect | |

### 8.4 Scheduler (audio-thread pseudocode)

```
samples_per_tick = 48000 * 2.5 / bpm          // float, e.g. 1071.43 at 112
state: order_idx, row_idx, tick_in_row, accum = 0.0

render(frames):
  done = 0
  while done < frames:
      if accum < 1.0:
          if tick_in_row == 0: process_row(...)      // note/inst/vol/fx triggers
          apply_tick_effects()                        // arp/slide/vib, envelopes
          tick_in_row++
          if tick_in_row == ticks_per_row:
              tick_in_row = 0; row_idx++
              if row_idx == rows_per_pattern:
                  row_idx = 0; order_idx = (order_idx + 1) % len(order)
          accum += samples_per_tick
      n = min(frames - done, int(accum))
      synthesize n samples of all 4 channels into the music accumulator
      accum -= n; done += n
```

Music is rendered mono per channel, summed with fixed static pans
(pulse1 −0.2, pulse2 +0.2, wide 0.0, noise +0.1, constant-power as §3.1)
into the music bus. Cost budget: < 3 % of one core at 48 kHz.

### 8.5 Complete example song (8 bars, synthwave loop)

16 rows per pattern = 2 bars of 4/4 at 8th-note rows (`ticks_per_row` 12).
Pattern `A` = Am → F, pattern `B` = C → G; order plays 8 bars total.
Instruments are the §6 example patches.

```json
{
  "bpm": 112, "ticks_per_row": 12,
  "patterns": {
    "A": {
      "pulse1": ["E-5 lead_pulse 10", "---", "C-5", "---",
                 "D-5", "E-5", "C-5", "---",
                 "F-5 . . V3,5", "---", "E-5", "---",
                 "C-5", "---", "A-4", "OFF"],
      "pulse2": ["---", "A-4 stab_pulse 8 A037", "---", "A-4 . . A037",
                 "---", "A-4 . . A037", "---", "A-4 . . A037",
                 "---", "F-4 . . A047", "---", "F-4 . . A047",
                 "---", "F-4 . . A047", "---", "F-4 . . A047"],
      "wide":   ["A-2 bass_saw 12", "A-2", "A-3", "A-2", "A-2", "A-3", "A-2", "A-3",
                 "F-2", "F-2", "F-3", "F-2", "F-2", "F-3", "F-2", "F-3"],
      "noise":  ["C-2 kit_noise 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4"]
    },
    "B": {
      "pulse1": ["G-5 lead_pulse 10", "---", "E-5", "---",
                 "D-5", "E-5", "G-5", "---",
                 "D-5 . . V3,5", "---", "B-4", "---",
                 "G-4", "---", "A-4", "OFF"],
      "pulse2": ["---", "C-5 stab_pulse 8 A047", "---", "C-5 . . A047",
                 "---", "C-5 . . A047", "---", "C-5 . . A047",
                 "---", "G-4 . . A047", "---", "G-4 . . A047",
                 "---", "G-4 . . A047", "---", "G-4 . . A047"],
      "wide":   ["C-3 bass_saw 12", "C-3", "C-4", "C-3", "C-3", "C-4", "C-3", "C-4",
                 "G-2", "G-2", "G-3", "G-2", "G-2", "G-3", "G-2", "G-3"],
      "noise":  ["C-2 kit_noise 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4",
                 "C-2 . 15", "C-6 . 4", "A-4 . 12", "C-6 . 4"]
    }
  },
  "order": ["A", "A", "B", "B"]
}
```

## 9. Music states

Reserved song ids, one per music state (the game states of
11-game-framework.md plus the timed-mode state of 14-authoring-guide.md §7):
`attract`, `main`, `mode`, `multiball`, `wizard`, `game_over`. This list is
the single song-id vocabulary; table briefs and doc examples use exactly
these names. A table may define any subset; a missing id means silence in
that state. The game framework auto-plays `attract` when entering attract
mode and `game_over` on game end; scripts select the rest via
`tb.play_music("<song_id>")` (typically `main` on `ball_start`, `mode` when
a timed mode starts, `multiball` on `multiball_start`, back to `main` when
either ends). Additional non-reserved song ids are allowed and playable by
scripts — e.g. a table-flavored mode song such as voltage-vandals' `heist`
(15-launch-tables.md), played by script *instead of* `mode`.

The framework plays `attract` with a **−12 dB offset** (gain ×0.251) on the
music bus, so attract music sits quieter than game music
(01-product.md §4.8). The offset is in effect whenever the `attract` song
is the active song and is removed on game start, applied both ways through
the standard 20 ms gain ramp (§3.1).

Transition rule: `tb.play_music` on an already-playing id is a no-op.
Otherwise the mixer **crossfades over 100 ms** (4800 samples, equal-power:
`g_out = cos(x·π/2)`, `g_in = sin(x·π/2)`) by running two tracker instances;
the new song always starts at `order[0]`, row 0. `tb.stop_music` fades out
over the same 100 ms. A third request during a crossfade hard-drops the
oldest instance (it is already quietest).

## 10. Ducking

Big moments push music down so the SFX reads. Duck = music-bus gain envelope:
ramp **−6 dB (×0.501)** linearly over **50 ms**, hold until **200 ms** after
the most recent trigger, ramp back to unity over **50 ms**. Retrigger during
hold/release restarts the hold window (no deeper stacking).

Triggers: any SoundEvent whose resolved patch id is one of
`jackpot_hit`, `multiball_riser`, `extra_ball_fanfare`, `tilt_alarm`,
`drain_womp`, plus any `tb.play_sound(id, {duck=true})` (sets flags bit0,
§4.1). The trigger list is exactly this; tables extend it only via the
`duck` option.

## 11. Mix guidance

- Bus defaults (§3.1): master 80, sfx 100, music 60, ui 80. With these, a
  single normalized patch at velocity 1.0 peaks at ≈ −5 dBFS post-master.
- Headroom: normalized patches peak at −1 dBFS pre-gain; 3–4 simultaneous
  SFX plus music can sum past 1.0 — that is the limiter's job (§3.3), and
  brief gain reduction ≤ 3 dB during multiball is normal. Sustained gain
  reduction means a `volume_db` or bus trim is wrong.
- Loudness sanity target (LUFS-ish proxy, no full BS.1770 required): over a
  60 s `tb_autoplay` run of normal play with music, the 3 s-window stereo RMS
  must stay within **−22…−14 dBFS**, and `peak_master` must not sit pinned at
  the limiter (post-limiter peak ≥ 0.88 less than 5 % of windows).
- Tuning procedure when out of range: adjust song note volumes and instrument
  `volume_db` first, patch `volume_db` second, bus defaults never (they are
  user-facing). Acceptance: the RMS check above passes on all five shipped
  tables via `tb_autoplay --wav` (see 16-testing-ci.md).

## 12. Latency verification

Software-measured gate (automated, runs on reference hardware; see
16-testing-ci.md for where it hooks in):

1. Enable with `tiltburst --audio-latency-test`. The raw-input path
   (05-engine-core.md) stamps each flipper edge `t0` (monotonic ns). The sim
   forwards `{event_seq, t0}` through a diagnostic SPSC beside the sound ring.
2. When the audio thread starts the matching `flipper_clack` voice at buffer
   offset `o` in the callback entered at wall time `t_cb`, it computes
   `t_dac = t_cb + (o + periodSizeInFrames * periods) / 48000`.
3. Report per 100 flips: `p50/p95(t_dac - t0)` plus the §2.2 breakdown, via
   stats → main-thread log:
   `audio-latency: n=100 p50_ms=7.1 p95_ms=8.9 late=0`.
4. Gate: **p95 ≤ 10 ms at period 128** on reference hardware; ≤ 18 ms at
   256; ≤ 34 ms at 512. `late_events` must be 0 during the test. CI with the
   null backend asserts only the scheduling math (mapped offsets, spacing of
   two events 5 ticks apart = `round(5*spt)` samples), never wall time.

Hardware loopback (manual, optional, documents the true number): 3.5 mm cable
from line-out to line-in; `tiltburst --latency-loopback` opens a duplex
stream, plays `flipper_clack` on each flipper press, records input,
cross-correlates the recorded clack against the rendered PCM, and prints
`button→ear` including OS/DAC buffering. Expected: software p95 + 1–4 ms.

## Common pitfalls

- **Triggering sounds from render frames.** Schedule from sim ticks through
  the §4 clock; frame-triggered audio quantizes 5 ms flipper double-hits to
  16.7 ms and the table sounds mushy (ARCHITECTURE.md §8).
- **Using `ma_engine`/`ma_sound`.** Only the raw device callback is allowed;
  the high-level API allocates and locks internally.
- **Allocating, locking, or logging in the callback** — includes
  `fmt::format` and `std::vector` growth. Use preallocated buffers and the
  atomic stats struct.
- **Re-anchoring the tick clock every callback.** That converts callback
  jitter into event-timing jitter. Re-anchor only when `|err| > 480` samples;
  otherwise correct through `spt` within ±500 ppm (§4.2).
- **Forgetting the scheduling lead `D`.** Without it most events map into the
  past and relative spacing collapses to buffer boundaries.
- **Inventing a `map` key from a brief row.** `map` keys are exactly the 19
  §7.2 purposes; `bank_complete`, `switch_hit`, "hatch purchase" and every
  other brief row that is not a purpose is played from `rules.lua` with
  `tb.play_sound` (§6.2). Writing them into `map` is a validator error, not
  a silent no-op.
- **Doubling a sound you took over in Lua.** Scripting a purpose (per-element
  or per-rung pitch, §6.2 a/b) without setting that purpose to `"none"`
  leaves the automatic patch firing underneath the scripted one — two clips,
  1 ms apart, on every hit.
- **Normalizing away authored loudness.** Patches are peak-normalized to
  −1 dBFS *then* `volume_db` is applied; skipping `volume_db` makes the
  spinner as loud as the jackpot.
- **Wrong sfxr fidelity.** Phase, filters, and flanger advance *per
  subsample* (8×); `repeat_speed` resets only the frequency/arp program,
  never envelope/phase/filter state. Deviating changes every known recipe.
- **Running the generator at 48 kHz with 44.1 kHz constants.** Detunes
  everything ~9 %. Generate at 44100 Hz, then resample (§5).
- **Tracker effects at the wrong rate.** Arp/slide/vibrato advance per music
  tick (`2.5/bpm` s), not per row, not per sample. Slides persist after
  their row; arps do not.
- **Gap crossfades.** `tb.play_music` overlaps two tracker instances for
  100 ms equal-power; stop-then-start leaves an audible hole.
- **Linear pan.** Use constant-power (§3.1); linear pan dips −6 dB at center.
- **Stepped gains.** Bus volume, duck, steal, and retrigger transitions all
  ramp (20 ms / 50 ms / 1.33 ms as specified); instant steps click.

## Done when

- [ ] Device opens at 48 kHz stereo f32; ladder 128→256→512 implemented,
      persisted, and the §2.2 startup line is logged.
- [ ] Callback verified allocation- and lock-free (debug allocator hook +
      code review); stats visible from the main thread.
- [ ] Limiter matches §3.3 constants; a +6 dB overdriven test signal shows
      gain reduction with 2 ms/150 ms envelope and no hard clipping.
- [ ] 32 voices; stealing = lowest priority then oldest, with fade; per-patch
      cap of 4 enforced; unit tests cover all three rules.
- [ ] Unit test: two SoundEvents 5 ticks apart start `round(5*spt)` samples
      apart across arbitrary callback boundaries (null backend).
- [ ] Clock test: simulated ±300 ppm drift converges with `spt` in bounds and
      zero late events after warmup; a 1 s sim stall triggers exactly one
      re-anchor.
- [ ] Synth renders all 24 built-in patches; golden-master hash test (same
      binary, same PCM); each patch ends within 4 s; peak = −1 dBFS
      pre-`volume_db`.
- [ ] `audio.json` loading with override, `wav`, `map`, and validation errors
      per §5.1/§6 wired into tb_validate — including: a `map` key outside the
      §7.2 purpose set errors, `"none"` disables its purpose (no SoundEvent
      emitted), and a patch/wav named `none` errors (§6.2c).
- [ ] Every row of all five §6-style sound briefs in 15-launch-tables.md is
      implemented through exactly one of the two §6.2 mechanisms, with no
      purpose both mapped and scripted.
- [ ] `tables/test-lab/audio.json` matches §6.1 byte-for-byte and
      `tb_validate tables/test-lab` exits 0 with zero warnings.
- [ ] All §7.2 automatic purposes fire from the sim with impact-derived
      velocity and position-derived pan; `captive_full_travel` fires **no**
      automatic sound (script-played, §6.2) while the captive strike still
      sounds through `ball_ball`.
- [ ] Tracker plays the §8.5 example song: correct tempo math (row =
      267.9 ms at bpm 112 / tpr 12), loops seamlessly, all four effects
      audible and unit-tested at tick rate.
- [ ] `tb.play_music`/`tb.stop_music` crossfade 100 ms equal-power; reserved
      state songs auto-trigger per §9, `attract` at its −12 dB offset.
- [ ] Ducking: −6 dB, 50/200/50 ms envelope, exact trigger list, retrigger
      extends hold; covered by a mixdown unit test.
- [ ] `--audio-latency-test` reports p50/p95; p95 ≤ 10 ms at period 128 on
      reference hardware; `--latency-loopback` implemented.
- [ ] 60 s autoplay RMS within −22…−14 dBFS on every shipped table.
