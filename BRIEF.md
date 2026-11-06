# Stacklab: Project Brief

*Stacklab is a working title. Date: 2026-11-06.*

## 1. Goal

A native macOS block-stacking trainer in C++, built so that practice transfers directly to TETR.IO. The owner is U rank, runs 40L in about 30s, plays at about 3 PPS, and is aiming for X rank. Timing accuracy and input feel therefore come before everything else, and the bot must be able to play at X-rank level.

Success means three things:

1. Stacking here feels the same as stacking in TETR.IO.
2. Given the same seed, handling and inputs, the simulation produces the same result as TETR.IO (section 5).
3. New modes and drills are cheap to add, usually as a table entry or a data file.

### Identity

The game copies mechanics, not identity. It has its own name, visual design and audio. It uses no Tetris or TETR.IO names, logos, art, music or layouts in the product. Piece colours are a user setting with an original default palette. Developer docs may refer to TETR.IO when describing parity.

### Out of scope

Online play, accounts, leaderboards, bundled skins or music, and non-Mac builds. The design avoids blocking them, but nothing is built for them now.

## 2. Tech stack

| Choice | Decision | Reason |
|---|---|---|
| Language | C++20 | Project requirement |
| Build | CMake | Already installed |
| Window, input, 2D, audio | SDL3 | See below |
| Text | SDL3_ttf and a bundled OFL-licensed font | Not installed yet: `brew install sdl3_ttf` |
| Data files | JSON via nlohmann/json | Drills and replays are structured |
| Tests | Plain assert-based executables run by CTest | No framework needed |
| Settings location | `SDL_GetPrefPath` | Standard per-user location on macOS |

### Why SDL3 instead of raylib

raylib was the starting suggestion, with the owner open to alternatives. SDL3 wins on the requirement that matters most here:

- **SDL3** queues key events with the OS event time, in nanoseconds. See `Cocoa_GetEventTimestamp` and its use in `SDL_cocoakeyboard.m`. Inputs can be placed on the correct subframe regardless of the render rate.
- **raylib** exposes per-frame key state and a press queue, with no timestamps. Timing precision is limited to the poll rate, so it needs a very high frame rate to approach the same accuracy.

The cost is more setup and a separate text library. SDL3 3.4 is already installed. Since `core/` has no graphics dependency, the choice only affects `app/`.

## 3. Architecture

```
app/    SDL3: screens, rendering, audio, input capture, settings UI
modes/  Mode implementations and the mode table
ai/     move search, finesse, PC solver, bot
core/   board, pieces, kicks, RNG, player sim, garbage, replays (no SDL)
data/   rulesets, drills
tests/  self-checks, replay parity runner
```

Dependencies only point downward: `app` → `modes` → `ai` → `core`.

- **Deterministic core.** The same seed, ruleset, handling and inputs always produce the same game. Replays are those four things. Undo and instant retry are state copies. The bot and solver work on copies on a worker thread.
- **`Ruleset`.** All rules are data: kick table, gravity, lock settings, bag type, previews, hold, garbage settings. Modes and drills select or override a ruleset.
- **`Handling`.** Per-profile DAS, ARR, SDF, DCD and the IRS, IHS and safelock options. Values are stored in frames, as in TETR.IO, so settings can be copied across unchanged.
- **Keybinds.** Named profiles mapping each action to several keys, stored by scancode. OS key repeat is ignored. Actions: left, right, soft drop, hard drop, rotate CW, CCW and 180, hold, retry, undo.
- **Player interface.** A human and a bot produce the same input stream into the same simulation.

### Timing loop

The simulation runs at 60 ticks per second with subframe input positions, as TETR.IO does. Each render frame does the following:

1. Drain SDL events, converting each timestamp to a frame and subframe.
2. Step the simulation up to the current time, applying inputs at their exact positions.
3. Draw.

Rendering follows the display refresh rate, while simulation accuracy doesn't depend on it.

### Visual design

The owner chose the "Cloud" design: soft rounded cards around the board. All sizes are in points on a 1440×900 window, and should scale with the window.

- **Font:** Plus Jakarta Sans (OFL), bundled in `assets/fonts/` with its licence. Numbers use tabular figures.
- **Top bar (68 high, 32 side padding):** "Stacklab" wordmark on the left (20, bold), mode name in a pill in the centre (14, semibold), light/dark toggle on the right.
- **Main row, centred, 20 gap:** left column, board card, right column. Both columns are 230 wide with cards stacked 20 apart.
- **Left column:** Hold card, then a stats card with the time (44, bold) above rows for Lines, Pieces, PPS, KPP and Finesse, separated by 1pt lines.
- **Right column:** Next card with 5 previews, then a "Pace vs best" card (32, bold, in the good colour).
- **Cards:** radius 24, padding 20, flat with no shadows. Labels are 12, semibold, uppercase, in the muted colour.
- **Board card:** radius 28, padding 14, holding a 10×20 board at 38 per cell, with a radius 8 board background in a faint checkerboard.
- **Blocks:** radius 7 with a 2 gap between cells. Previews use 24 per cell, with corner radius scaled to match.
- **Ghost:** the piece colour at 42% opacity in light mode and 30% in dark mode.
- **Toggle:** a pill with sun and moon icons, and a circular knob behind the active one.

| Colour | Light | Dark |
|---|---|---|
| Background | `#FBF6EF` | `#22212B` |
| Card | `#FFFFFF` | `#2C2B37` |
| Text | `#3A3440` | `#F2EEF2` |
| Muted text | `#8F8791` | `#9C98A6` |
| Lines, toggle track | `#F1EBE3` | `#383744` |
| Toggle knob | `#FFFFFF` | `#4B4A5A` |
| Board background | `#F9F5EE` | `#25242E` |
| Board checker | `#F4EFE7` | `#2A2933` |
| Good | `#4FA88F` | `#93DED3` |

| Piece | Light | Dark |
|---|---|---|
| I | `#7CCFC4` | `#93DED3` |
| O | `#F2D06B` | `#F6DD8A` |
| T | `#B9A0E6` | `#C9B4F0` |
| S | `#9BD48A` | `#B0E09F` |
| Z | `#F19A9E` | `#F5AEB1` |
| J | `#86AEE8` | `#A0C2F0` |
| L | `#F4B076` | `#F7C394` |

## 4. Modes

### Extension model

```cpp
struct Mode {
    virtual ~Mode() = default;
    virtual void start(Match&) = 0;
    virtual void onEvent(Match&, const GameEvent&) = 0;  // lock, clear, garbage, topout
    virtual Outcome check(const Match&) = 0;
    virtual void drawOverlay(const Match&, Canvas&) {}
    virtual StatsLayout stats() const = 0;
};
```

`Match` owns one or more players. A single table lists each menu entry as a name, a mode factory and parameters. A **submode** is an existing mode with different parameters, so adding one means adding a row. A **new drill** is a data file and needs no code.

### Standard modes

| Mode | Notes |
|---|---|
| 40L | Sprint with personal bests and split comparison |
| Blitz | 2-minute score attack |
| Dig | Clear N garbage lines, with configurable messiness |
| Sandbox | Editable board and queue, no end condition |
| Versus | Against the bot (section 8) |

### Trainers

| Trainer | Function |
|---|---|
| Finesse | Live feedback comparing inputs against the optimal count, with fault stats per piece and orientation |
| Perfect clear | Opener and continuation drills, a "PC still possible" indicator, a solution overlay, and success-rate stats |
| LST | Scripted stages for building and continuing the stack, then adaptive drills where justified |
| Setups | Generic drill runner for the starter set and imported fumens |

**Starter drills:** PC opener, LST, TKI, DT Cannon, MKO, a dig set and 4-wide. Set definitions are drafted by the implementer and checked by the owner.

### Common to all modes

- Instant retry
- Undo in trainers
- Session history
- Stats: PPS, KPP, APM, APP, finesse faults
- Replay save and playback

### Drill format

A drill is a JSON file:

```json
{
  "name": "PC opener: example",
  "ruleset": "versus-standard",
  "board": "fumen-or-rows",
  "queue": { "type": "fixed", "pieces": "IOTSZJL" },
  "targets": [{ "piece": "I", "x": 0, "y": 0, "rot": 0 }],
  "onMiss": "retry",
  "next": "another-drill-id"
}
```

The exact fields get settled when the runner is built. Fumen import is required, since it's how the community shares setups.

## 5. TETR.IO parity

**"Exact" means:** given the seed, config, handling and inputs from a real TETR.IO replay, our engine ends the game in the same state as TETR.IO recorded, and produces the same attack and garbage values. Replays hold no mid-game state, so a divergence on any piece only shows up at the end. The automated replay test in section 5.4 decides this.

**What it can't cover:** TETR.IO is closed source and its rules change over time. Parity is measured against a pinned set of replays, and rulesets are versioned so they can be updated. Network effects in online matches have no equivalent in a local game. In replays, recorded garbage arrival times are fed in directly.

### 5.1 Reference implementation

[Triangle.js](https://github.com/halp1/triangle) (MIT) is an open-source TypeScript engine whose test suite runs against real replays. Its README says it was last known to work with TETR.IO Beta 1.7.8. Use it as the rules reference and as a comparison when debugging mismatches. Anything ported must keep its licence notice in `THIRD_PARTY_NOTICES.md`.

### 5.2 Rule values come from replays

Replay files embed the room config, including kick set, spin bonuses, combo table, garbage speed, cap, messiness, opener phase, B2B charging and PC values. These are not reliably documented anywhere public. **The `versus-standard` ruleset is extracted from a recent Tetra League replay, not written from memory.** Fallback defaults in other code aren't a source for the current values.

### 5.3 Rules as read from Triangle's source

Confirm all of this through the replay test.

**RNG.** Park-Miller LCG: `v = 16807 * v mod 2147483647`, with `float = (v - 1) / 2147483646`. Bags shuffle `Z L O S I J T` with Fisher-Yates from the last index down.

**Base attack.**

| Clear | Normal | T-spin / spin | Mini |
|---|---|---|---|
| 0 lines | 0 | 0 | 0 |
| Single | 0 | 2 | 0 |
| Double | 1 | 4 | 1 |
| Triple | 2 | 6 | 2 |
| Quad | 4 | 10 | 10 |
| Penta | 5 | 12 | 12 |

Larger clears add 1 per line, or 2 per line with a spin.

**Attack order for a locked piece:**

1. Start from the base value.
2. **B2B**, if `lines > 0` and `b2b > 0`, with `L = log1p(b2b * 0.8)`, when chaining is on:
   `+= floor(1 + L) + (b2b == 1 ? 0 : (1 + frac(L)) / 3)`. Otherwise `+= 1`.
3. **Combo**, if `combo > 0`, using the multiplier table:
   `*= 1 + 0.25 * combo`, then if `combo > 1`: `= max(log1p(combo * 1.25), value)`.
4. **Result:** `round(value * garbageMultiplier + specialBonus)`, using the configured rounding mode. `specialBonus` is 1 when enabled and the clear removed garbage and was a spin or 4+ lines.

**Counters.** B2B and combo start at -1. Any clear increments combo; a non-clear resets it to -1. A spin or 4+ line clear increments B2B; any other clear resets it to -1. A perfect clear adds the configured PC B2B amount instead of the normal increment, when one is set.

**Surge.** When a clear breaks B2B and `b2b + 1 > chargeAt`:
`s = floor((b2b - chargeAt + chargeBase + 1) * multiplier)`. Send it as three attacks ahead of the normal attack: `round(s/3)`, `round(s/3)`, `s - 2*round(s/3)`.

**Perfect clear.** Appends `round(pcGarbage * multiplier)` after the normal attack.

**Cancelling.** Each attack cancels the oldest incoming garbage first, and any remainder is sent. During the opener phase (`pieces + 1 <= openerPhase`, while incoming is at least the amount sent so far), an attack can cancel up to twice its value. The first portion reduces what's sent as normal; the extra portion only removes incoming garbage. Replays from before 2025-02-16 use an opener limit one lower, so fixtures should be newer than that.

**Garbage columns.** Hole position rerolls using `messiness_change` and `messiness_inner`, consuming RNG in a fixed order. Port this path carefully, since RNG drift breaks every later row. Also port rounding, cap, delay and blocking behaviour.

**Spins.** Default is `all-mini+`: the higher of normal T-spin detection and all-spin detection, where an all-spin counts as a mini. Rotation uses the SRS+ kick data, including the 180 kicks.

**Numeric care.** Use `double` to match JavaScript, and don't use `-ffast-math`.

### 5.4 Replay parity test

`tests/replay_parity` loads a replay, applies its config, handling and inputs, then compares the final result with what TETR.IO recorded. For JSON fixtures that record every lock, it compares after each lock instead and reports the first mismatch with the frame, the board diff and the input history.

What each source can check:

| Source | Compared | Can't check |
|---|---|---|
| Solo `.ttr` | Pieces, lines, every clear type, B2B, combo, level, and the 40L finish frame | Boards, because none is recorded; attack, because it's recorded as 0 |
| League `.ttrm` | The final board, hold, queue and stats from the `end` event | Where in the game a mismatch began |
| Hand-derived fixtures | Every lock | Anything about TETR.IO; they only show core agrees with our reading of Triangle's source |

Files live in `replays/`, which is gitignored because they include usernames.

Two rules found by real replays aren't in Triangle's engine: `no_szo` and level gravity. The level gravity formula, level 1 gravity and `levelgspeed` are inferred from 3 Blitz games, and are marked with `ponytail:` comments in the code.

**Required fixtures, supplied by the owner:**

- 3 or more 40L replays
- 3 or more Blitz replays
- 5 or more recent Tetra League rounds, including B2B chains, surge, perfect clears and heavy garbage

Triangle's replay archive may provide more, if it's accessible and its terms allow. Its test loader reads `.ttrmx` files, so confirm the format of the owner's exported replays before writing ours.

**Pass condition:** no mismatches across all fixtures. Any exception is written down with its cause.

## 6. Input feel

This is the go/no-go risk for the project, and it's tested first.

- Measure input-to-photon latency against TETR.IO in the owner's browser, using a phone's slow-motion camera. Target: no worse than TETR.IO.
- Default to the lowest-latency present mode that doesn't tear noticeably, and offer a setting.
- Keep frame time stable, since allocation or file work must not happen during play.
- Treat the owner's judgement as the final test: 40L times should match within normal variance after a short adjustment.

## 7. Trainer engines

**Move search.** A breadth-first search over position, rotation and spin state, using the real kick and movement rules. It records reachable placements and the minimum inputs for each. Finesse, the PC solver and the bot all share it. Performance matters, so use bitboards.

**Finesse.** Compare the player's inputs for a placement against the search's minimum. It's computed rather than tabled, so it automatically respects the active rules and tucks.

**PC solver.** Depth-first search over placements with bitboards, pruning, and memoisation. Handles hold and runs on a worker thread. There are two questions with different costs:

- *Is there a solution with the visible queue?* Should be fast enough to run after every piece.
- *What's the chance of success across unseen bags?* Much more expensive, so precompute or sample it, and only show it in the UI once it's fast enough.

**Solution overlay.** Show the next placement as a ghost, on demand. Hidden by default so it doesn't undercut training.

**LST.** No simple rule defines a "valid LST" stack, so it starts as authored stages. Adaptive drills use a table of accepted states, added only if the scripted version proves too limiting.

## 8. Bot

Fully our own C++ implementation. The owner will supply open-source repositories for ideas. **Check each licence before copying code**, since ideas are fine but code carries obligations.

**Design:**

- Bag-aware beam search over the shared move search.
- Evaluation includes board shape, holes, wells, T-slots, attack, B2B and surge state, and incoming garbage.
- Weights live in data files and can be tuned with self-play.
- The bot submits real inputs through the same engine, so it can't break the rules.
- It thinks on a worker thread, using a state copy, and never blocks rendering.

**Tunable by the player:**

| Setting | Effect |
|---|---|
| PPS cap | Hard speed limit, from far below beginner level to the bot's maximum |
| Search width and depth | Quality of decisions |
| Mistake rate | Occasional deliberately weaker placements |
| Reaction delay | Slower response to incoming garbage |
| Style | Weights favouring T-spins, quads, combos or downstacking |

**Toggles in versus:** garbage on or off in each direction, ruleset choice, attack multiplier, and starting board.

**Presets** run from new player (well under 1 PPS) through to X rank, with an uncapped mode above that. Each is calibrated against measured stats, not named by feel.

### X-rank target

Speed alone can beat almost anyone, so it doesn't count. **The X-rank preset must win through decision quality at human speed.** It is capped to a PPS typical of X-rank players and must meet all of these:

1. **Attack efficiency:** its APP and VS in bot matches at that cap meet or exceed X-rank reference values. Those values come from TETRA CHANNEL data and X-rank replays, not from memory.
2. **Against the owner:** it wins a clear majority of FT7 sets while the owner is below X rank.
3. **Human validation:** it stays competitive once the owner reaches X, or in sets against another X-rank player. Stats and self-play can't fully confirm strength against humans, so this is the final test.

These stats come from different opponents, so they're evidence, not proof. Bots are never run on TETR.IO itself, so rank can't be measured directly.

### Tuning

Strength at a capped PPS depends more on evaluation quality than on raw search speed, since each piece has hundreds of milliseconds to think. Reaching it needs a large volume of test games:

- **Headless self-play:** many parallel games with no window, using the deterministic core.
- **Weight optimisation:** an automated search over evaluation weights using self-play results.
- **Regression ladder:** a new bot version replaces the old one only after beating it by a statistically significant margin over many games.
- **Coverage:** openers, B2B and surge management, and defending against heavy garbage, each with benchmark positions.

A learned evaluation is the next step if hand-tuned weights plateau below target.

**Risk:** X-rank decision quality at human speed is by far the hardest goal in the project, and it isn't guaranteed. Lower presets are useful much earlier, so each is shipped as it becomes good enough.

## 9. Build order

Each phase ends with something the owner can play.

| Phase | Scope | Done when |
|---|---|---|
| 1. Feel | Core sim, SDL3 app, handling, keybinds, 40L, solo replay parity | Solo replays pass, and the owner confirms it feels right. **Go/no-go.** |
| 2. Framework | Menus, settings UI, mode table, stats, history, Blitz, Dig, Sandbox | A submode is added with one table row |
| 3. Training tools | Move search, finesse, drill runner, fumen import, undo | Owner completes an imported drill with live finesse feedback |
| 4. Perfect clear | Solver, indicator, overlay, PC drills | "Possible" indicator stays correct and doesn't cause frame drops |
| 5. Pattern library | LST and the starter drills | Owner signs off each set |
| 6. Versus rules | Garbage, surge, multi-player match, versus replay parity | Tetra League replays pass with no mismatches |
| 7. Bot | Search, evaluation, presets, toggles, self-play tuning | Presets span new player to the owner's level |
| 8. X-rank bot | Sustained tuning against the targets in section 8 | The X-rank preset meets all three targets; likely the longest phase |

Attack calculation is written and unit-tested in phase 1, even though versus arrives later, since scoring depends on it. Phases 3 to 5 come before the bot because trainers are the priority and they build the search that the bot reuses.

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Input latency worse than TETR.IO | Project fails its main goal | Tested in phase 1 before anything else is built on top |
| A rule change in TETR.IO | Parity drifts | Versioned rulesets; re-run with fresh replays |
| Undocumented parity edge cases | Delays phases 1 and 6 | Replay test finds the first mismatch; compare against Triangle |
| Bot doesn't reach X-rank strength at human speed | Top preset falls short of the owner's goal | Lower presets ship first; self-play tuning, then a learned evaluation if needed |
| PC success-rate query too slow | Feature limited | Precompute or sample, or leave it out |
| Drill authoring takes time | Slower content growth | Fumen import; owner reviews sets |

## 11. Owner inputs

- **TETR.IO replays** per section 5.4, needed from phase 1 for solo and phase 6 for versus. Include one recent Tetra League replay for the ruleset.
- **Current handling settings**, to use as the default profile.
- **Reference repositories** for the bot, before phase 7.
- **X-rank replays** for benchmarks, before phase 8, and ideally access to an X-rank player for validation.
- **Review of drill sets** in phase 5.
- **Final name**, whenever convenient. It must be an original name, not "Tetris" or similar.

## 12. Sources

- [Triangle.js](https://github.com/halp1/triangle): `src/engine/utils/damageCalc`, `src/engine/garbage`, `src/engine/index.ts`, `src/engine/utils/rng`, `test/engine/replay.ts`
- [SDL3 keyboard event docs](https://wiki.libsdl.org/SDL3/SDL_KeyboardEvent)
- [SDL `SDL_cocoaevents.m`](https://github.com/libsdl-org/SDL/blob/main/src/video/cocoa/SDL_cocoaevents.m) and [`SDL_cocoakeyboard.m`](https://github.com/libsdl-org/SDL/blob/main/src/video/cocoa/SDL_cocoakeyboard.m)
- [TETR.IO community wiki](https://tetris.wiki/TETR.IO) (background only)
