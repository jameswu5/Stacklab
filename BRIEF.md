# Stacklab: Project Brief

*Stacklab is a working title. Date: 2026-11-06.*

## 1. Goal

A native macOS block-stacking trainer in C++, built so that practice transfers directly to TETR.IO. The owner is U rank, runs 40L in about 30s, plays at about 3 PPS, and is aiming for X rank. Timing accuracy and input feel therefore come before everything else, and the bot must be able to play at X-rank level.

Success means three things:

1. Stacking here feels the same as stacking in TETR.IO.
2. Given the same seed, handling and inputs, the simulation produces the same result as TETR.IO (section 5).
3. New modes and drills are cheap to add, usually as a table entry or a data file. A technique coach (section 7) is the exception: it's code and tuning.

The owner's first concrete target is TETR.IO's PC badge: 10 perfect clears in one 40L run. The project began as a way to learn those solves, so the perfect clear trainer is built around it.

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
- **Keybinds.** Named profiles mapping each action to several keys, stored by scancode. OS key repeat is ignored. Actions: left, right, soft drop, hard drop, rotate CW, CCW and 180, hold, retry, menu, undo, PC hint, bag residue, setup hint.
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
- **Top bar (68 high, 32 side padding):** "Stacklab" wordmark on the left (20, bold), mode name in a pill beside it (14, semibold), light/dark toggle on the right. The centre stays clear because pieces spawn there.
- **Main row, centred, 20 gap:** left column, board card, right column. Both columns are 230 wide with cards stacked 20 apart.
- **Left column:** Hold card, then a stats card with the time (44, bold) above rows for Lines, Pieces, PPS, KPP and Finesse, separated by 1pt lines.
- **Right column:** a full-height Next card with 5 previews, large because the owner reads the queue in peripheral vision.
- **Cards:** radius 24, padding 20, flat with no shadows. Labels are 12, semibold, uppercase, in the muted colour.
- **Board card:** radius 28, padding 14, holding a 10×20 board at 38 per cell, with a radius 8 board background in a faint checkerboard.
- **Blocks:** radius 7 with a 2 gap between cells. Next previews use 44 per cell and hold uses 24, with corner radius scaled to match.
- **Lines left:** in a mode with a lines goal and no garbage, the lines left as a number inside the top of the board (140, bold, tabular), in the muted colour at 20% opacity in light mode and 16% in dark mode, under the ghost, the stack and the piece, and hidden on the results.
- **Ghost:** the piece colour at 42% opacity in light mode and 30% in dark mode.
- **Toggle:** a pill with sun and moon icons, and a circular knob behind the active one.
- **Menu:** the screen the app opens on. A centred card, 560 wide, with a row for each mode and one for Settings. Rows show the name (20, bold), a line of description and any personal best. Fixed keys: up, down and Enter.
- **Results:** when a run ends, a card over the board (300 wide) shows the outcome, the time, PPS, KPP, APM, APP, the best and, where the mode lists it, finesse faults with the piece and orientation that had the most, and a table of faults for each piece and orientation. Retry works from it directly. The Next card becomes a Session card, listing this mode's ended runs this session, newest first. Retried or abandoned runs aren't listed, and history isn't saved.

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

Piece colours are saturated rather than pastel, because the owner tracks pieces in peripheral vision.

| Piece | Light | Dark |
|---|---|---|
| I | `#00AFB3` | `#2BD9D9` |
| O | `#F0B400` | `#FFD52E` |
| T | `#8F45DE` | `#C07EFF` |
| S | `#2FB344` | `#4EDB4A` |
| Z | `#E5344B` | `#FF5F70` |
| J | `#2B6BE8` | `#5A9BFF` |
| L | `#F27A17` | `#FF8C2B` |

## 4. Modes

### Extension model

```cpp
struct Mode {
    const char* name;      // on the menu and in the top bar
    const char* blurb;     // under the name on the menu
    core::Ruleset rules;
    int goalLines = 0;               // reaching this many lines ends the run; 0 is no goal
    const char* bestFile = nullptr;  // holds the personal best, under the pref path; null keeps none
    int stats = StatLines | StatPieces | StatPPS | StatKPP | StatFinesse;  // the stats card's rows
    int garbageRows = 0;             // a cheese race: on the board at once, refilled until goalLines are placed; only garbage lines count
    bool undo = false;               // the undo key takes back the last piece, clock included
    const char* pcFile = nullptr;    // turns on the PC coach, and holds its record under the pref path
    PcStyle pcStyle = PcStyle::Coach;  // Coach, Practice from a chosen bag residue, or Count with no solver
};
```

`kModes` in `modes/modes.h` is the single table of menu entries, and `app/play.cpp` acts on a row's values. A **submode** is an existing mode with different values, so adding one means adding a row. A new kind of mode adds the fields it needs here, and `Play` acts on them. This replaces the earlier sketch of a virtual `Mode` class: nothing needs one until a mode can't be described as data. A **new drill** is a data file and needs no code.

### Standard modes

| Mode | Notes |
|---|---|
| 40L | Sprint with personal bests |
| Dig | A cheese race of 10, 18 or 100 garbage lines, at most 10 rows on the board, each cleared one rising from the floor until all are placed. Each row's hole is random among the 9 columns other than its neighbour's |
| Versus | Against the bot (section 8) |

### Trainers

| Trainer | Function |
|---|---|
| Finesse | Live feedback comparing inputs against the optimal count, with fault stats per piece and orientation |
| Perfect clear | A coach on random 7-bag queues, under 40L's rules: a "PC still possible" indicator, a solution overlay on demand, feedback on each placement, and success rates by bag residue, aimed at the badge. PC practice plays one PC from a chosen residue, and Bag count is a 40L run where the player keeps count |
| LST | A coach on random queues, built alongside the bot, since it shares the bot's search and evaluation |
| Setups | The drill runner, for fumens the owner imports |

**Random queues, not memorised placements.** Learning to place pieces in one fixed queue teaches little, so trainers use random queues and a coach that knows the technique (section 7). The fumen runner stays, for the owner's own setups and possibly for a coach's starting boards, but no fixed drill sets are authored. Named setups such as TKI, DT Cannon and MKO get a coach only if the owner wants to learn them.

### Common to all modes

- Instant retry
- Undo in trainers
- Session history
- Stats: PPS, KPP, APM, APP, finesse faults
- Replay save and playback

### Drill format

A drill is a JSON file in `data/drills/`, or under the pref path in `drills/` for the owner's own. Its name, without `.json`, is its id. No menu row starts a drill yet: the Setups sub-menu will, and until then the runner is kept and covered by `tests/app_test.cpp`, with samples in `tests/fixtures/drills/`.

**Adding a fumen drill:** create `~/Library/Application Support/Stacklab/Stacklab/drills/` if it isn't there, and put a `.json` file in it containing `{"name": "...", "board": "v115@..."}`. The fumen's locked pieces become the queue and targets, in order.

```json
{
  "name": "First steps 1",
  "blurb": "A line under the name in the list",
  "board": ["XXXXXXXX..", "XXXXXXXX.."],
  "queue": { "type": "fixed", "pieces": "IOTSZJL" },
  "targets": [{ "piece": "I", "x": 0, "y": 1, "rot": 0 }],
  "onMiss": "retry",
  "next": "another-drill-id"
}
```

| Field | Required | Meaning |
|---|---|---|
| `name` | Yes | In the list and the top bar |
| `blurb` | No | Under the name in the list |
| `board` | No, empty by default | Rows, top first and the last on the floor, 10 wide and at most 20 of them: `.` empty, `X` garbage, or a piece letter. Or a fumen string or URL, using its first page |
| `queue` | With rows, yes | `type` must be `fixed`. Previews past the end are empty |
| `targets` | With rows, yes | At most 63. `x`, `y` and `rot` are `core::Falling`'s: `x` is the left of the piece's box, `y` is the row of the box's top, counted up from the floor, and `rot` is 0 spawn, 1 CW, 2 180, 3 CCW |
| `onMiss` | No | Only `retry`, the default |
| `next` | No | The id of the drill to go to on success |

**From a fumen,** a page's piece that locks becomes a target and joins the queue, in page order, unless the file gives its own `queue` or `targets`. Quiz comments aren't read. So a whole drill can be `{"name": "...", "board": "v115@..."}`.

**How a run is judged:**

- Every locked piece must fill a target still open, compared by cells, so a rotation that fills the same cells counts. Hold can change the order. Anything else is a miss, as are topping out and running out of pieces with targets left.
- A target's coordinates are on the board as it stands when that piece locks, after earlier clears. If an earlier piece clears lines, placing out of the listed order misses.
- A miss pauses on the missed board, clock included, and says so. Retry starts the drill again with a countdown, and undo takes back the missed piece. Otherwise, undo gives back the last piece and its target.
- When the queue runs out, a held piece comes back to be placed.
- Success goes to `next`, with a countdown, or shows the results. Only the last drill of a chain is listed in the session.

`ruleset` isn't supported yet and is rejected, so it can't silently do nothing: every drill uses the Drills row's rules until `versus-standard` exists (section 5.2). A file that can't be used is left out and named at the bottom of the list.

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

**Cancelling.** Each attack cancels the oldest incoming garbage first, and any remainder is sent. During the opener phase (`pieces + 1 <= openerPhase`, while incoming is at least the amount sent so far), an attack can cancel up to twice its value. The first portion reduces what's sent as normal; the extra portion only removes incoming garbage. Replays from before 2025-02-16 use an opener limit one lower; none of ours are that old, so core doesn't implement it.

**Garbage columns.** Hole position rerolls using `messiness_change` and `messiness_inner`, consuming RNG in a fixed order, since RNG drift breaks every later row. Before 2025-05-06 15:00 UTC-4, a cancel that finishes a batch draws nothing; 85.6% of older rounds fail without this.

**Where TETR.IO differs from Triangle.** Each of these comes from recorded TETR.IO results, and core follows TETR.IO:

- Garbage rises in arrival order. Triangle sorts by confirm frame, which gives rows the wrong holes when confirms come out of order.
- A successful shift or a hold clears spin status. Triangle keeps it, which counts false minis and T-spin singles.
- A hold that tops out leaves the hold slot as it was.
- TETR.IO's `received` stat counts what's left of a batch when it's confirmed, risen or not.

The `end` event records `garbagemultiplier` as it stood at the end of the round, so the runner backs out the starting value.

**Spins.** Default is `all-mini+`: the higher of normal T-spin detection and all-spin detection, where an all-spin counts as a mini. Rotation uses the SRS+ kick data, including the 180 kicks.

**Numeric care.** Use `double` to match JavaScript, and don't use `-ffast-math`.

### 5.4 Replay parity test

`tests/replay_parity` loads a replay, applies its config, handling and inputs, then compares the final result with what TETR.IO recorded. For JSON fixtures that record every lock, it compares after each lock instead and reports the first mismatch with the frame, the board diff and the input history.

What each source can check:

| Source | Compared | Agrees with | Can't check |
|---|---|---|---|
| Solo `.ttr` | Pieces, lines, every clear type, B2B, combo, level, score, and the 40L finish frame | TETR.IO | Boards, because none is recorded; attack, because it's recorded as 0 |
| League `.ttrm` and `.ttrmx`, the owner's and Triangle's archive | Final board, hold, next 5, falling piece, stats, score, attack, sent, received, garbage cleared and cause of death from the `end` event, plus every send against the opponent's recorded arrivals | TETR.IO | Boards mid-round; garbage that's still queued at the end; sends after the opponent's game ended |
| `.ttrm` rounds replayed through Triangle (`gen_fixtures.mjs --replay`, stays local) | Every lock: piece, lines, spin, perfect clear, each send, B2B, combo, board | Triangle only | Anything Triangle gets wrong; `no_szo`; levels |
| `tests/fixtures/*.json` from `gen_fixtures.mjs` | Every lock, the end state, and each tick's piece position where traced | Triangle only | Same as above |
| Hand-derived fixtures | Every lock | Our reading of Triangle's source | Anything about TETR.IO |

The archive isn't part of `ctest`, so run it by hand: `tools/archive_parity.py <replay_parity> replay-archive/replays.tar.gz <temp folder> <output file>`. It streams the tarball through the runner in batches, so about 300 MB is on disk at a time, and it doesn't run any of Triangle's code. `replay-archive/unpacked` has its garbage rounds stripped, so it only checks the 1,053 rounds without garbage.

A skipped round is not a pass. Results, using Triangle `@haelp/teto@4.2.8`:

| Set | Checked | Passed | Failed | Skipped |
|---|---|---|---|---|
| The owner's replays against TETR.IO, 1,717 sends | 84 | 84 | 0 | None |
| Archive against TETR.IO, 5,220 files, 2,497,056 sends | 110,701 | 110,693 | 8 | 61 never started |
| The 1,053 garbage-free archive rounds per lock against Triangle, 36,390 locks | 1,053 | 1,053 | 0 | None |
| Generated fixtures against Triangle | 787 | 787 | 0 | None |

**The 8 exceptions.** All are on the final tick of a round, and in all 8 every send up to there matches. The cause of each isn't established, and no rule was fitted to them:

- **4 score only**, by 1 to 18 points, with the same board. These look like drop points on a tick TETR.IO stopped partway through.
- **3 garbage smash deaths where core stays alive.** On the lock that killed the player, TETR.IO has more rows on the board than core: in the clearest case, 13 rows rise where the cap of 8 should hold 5 of them back. Core has no garbage smash rule beyond a blocked spawn. It gets the other 49,225 smash deaths right.
- **1 garbage smash death with the same row count** but different holes in one older block of rows, and 6 extra drop points.

Still unchecked:

- **Options no replay uses:** `core` has no support for garbage hole size, absolute cap, `messiness_timeout`, `messiness_nosame`, `messiness_center`, or passthrough other than `zero`, and `unsupported()` rejects replays that use them. The cap never grows and `messiness_inner` is always 0 in these replays, so those paths have only a hand-worked check or none.
- **Against TETR.IO:** mini quad and spin quad, score or attack; RNG rounding.
- **Against anything:** a spin quad. `tests/fixtures/` has no I-piece spin and no mini double, though the archive replays contain 173 mini doubles.
- **More than two players:** garbage targets one opponent, and sends are only compared in two-player rounds.

Files live in `replays/` and `replay-archive/`, which are gitignored because they include usernames or players' inputs.

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

- A fault is a press beyond the minimum for the same cells and spin, never below 0. Move, rotate and soft drop each cost one per press, held or not. Hard drop is on neither side, and a key held since before the run costs nothing.
- Hold never costs anything, and restarts the count for the piece it brings out.
- A 180 is one press when the rules allow it, so two rotations are then a fault. Waiting for gravity is free for the player, so it can hide a fault but not invent one.
- A lock that can't be judged fairly gets no judgement: it isn't among the search's placements, it landed on the ground in the same tick as a key, or it shares a tick with another lock.
- The Finesse row shows the run's total. It turns red with the last placement's count, as in `12 (+2)`, until the next piece locks. Undo takes faults back with the piece, and a drill's miss or next drill starts them over.
- **Finesse is off in modes with `rules.levels`, such as Blitz, and shows "-".** The search doesn't let time pass, so at high gravity it could invent faults.

**PC solver.** Depth-first search over placements with bitboards, pruning, and memoisation. Handles hold and runs on a worker thread. It takes only what the player can see, as a `PcQuery`: the board, falling piece, hold, previews and what's left of the bag. It never takes a `core::Game`, whose queue runs past the previews.

- It uses the move search's reachability, so tucks and spins count, and lines may clear partway. Every answer it gives comes with real inputs.
- `maxLines` caps a clear's height, 4 by default. The fewest lines that work are tried first.
- Its answer is one of: found, from the visible pieces alone; not enough pieces known; impossible within that height, whatever comes; or unknown, when the node budget runs out, it's cancelled, or the stack is over 10 rows. An odd cell count is the only "never".
- With exactly as many pieces visible as a clear needs, hold can skip one and use the next, unseen piece. So failing with the visible pieces is only impossible if no piece the bag allows would do it. With hold empty and one more visible than needed, every reachable choice is already visible.
- **Pruning must stay sound across line clears.** It only uses what clears can't change: each column gets exactly one cell per line cleared, so columns between two finished ones must need a multiple of 4 between them, and occupied rows can't outnumber the lines left. Checking empty regions of the board for a multiple of 4 gives wrong "impossible" answers, because a clear joins the regions above and below it.

There are two questions with different costs:

- *Is there a solution with the visible queue?* Should be fast enough to run after every piece.
- *What's the chance of success across unseen bags?* Much more expensive, so precompute or sample it, and only show it in the UI once it's fast enough.

**Solution overlay.** Show the next placement as a ghost, on demand. Hidden by default so it doesn't undercut training.

**Technique coaches.** Each technique gets a coach that answers three things after every placement: is the technique still alive, what's a good placement here (hidden until asked), and how was the one just made: fine, worse than the best, or fatal to the technique.

- **Fair to what was visible:** a placement isn't marked wrong because of pieces the player hadn't seen. Telling a mistake from bad luck needs the success-across-unseen-bags question above, sampled if necessary.
- **Mistakes teach:** after a fatal placement, the player can undo and retry on the same queue, then see a solution. A new attempt gets a new queue.

**PC coach.** It's the solver, so its answers are exact and tests can check them. For the badge, 10 PCs must fit within 40 lines, so every clear is part of a PC and PC heights are even. The owner trains 4-line PCs. A 2-line PC is allowed, so 10 PCs can take 38 lines, and that's the badge: the run ends in success at the tenth PC. A taller PC reads as "No PC".

**Bag residue** is the number of pieces placed before an attempt, modulo 7. Each PC starts empty with hold carried over, and a 4-line PC takes 10 pieces, so it moves the residue by 3, and a 2-line PC takes 5, so it moves it by 5. Success is measured per residue and as a streak.

**A run that used undo is nullified:** it saves no best and no tallies, and says so on the stats card. No mode has both undo and a best file, and a test holds that.

When no PC is provable from the visible pieces, `PcSolver::advise` suggests a placement. It takes only a `PcQuery`, and samples futures as uniformly random bags consistent with what's been seen, from a fixed seed, so the same state gets the same answer. Each choice is scored by rollouts: the previews come into view one at a time, and each placement comes from a plan for a guessed future. Scoring by whether a clear exists once the whole future is known doesn't work: that reads about 100% for nearly every placement, and its choices did no better than a random first placement in real play. A provable PC always wins, at 100%. Show the suggested placement, not the percentage: it's the best of many choices on 8 futures, so it's noisy and tends to read high. It runs on a worker thread with a cancel flag and a time budget, and says when an answer is partial. An answer takes about a minute for each of the first two placements from an empty board, seconds for the third and about a second from the fourth, so it isn't for live use on an empty board. A precomputed table of first placements would be about 160,000 entries and weeks of machine time, so it isn't planned.

**PC practice** is one PC from an empty board at a residue chosen with left and right on the menu. Residues above 0 start with a random piece from those already dealt in hold, past the first bag, so `seen` and `bagLeft` are true to a mid-run state. Residue 0 starts a plain game with hold empty. The run ends on the first PC or on spending over 4 lines, retry deals a new queue at the same residue, and tallies share `pc-coach.json` with the coach. The setup hint key (G) shows a four.lol setup for the residue as a ghost on the board, with its name, chance and condition: one whose pieces are among the attempt's first n + 1 (so one hold can build it), whose hold condition holds, and whose shape covers every block placed. Pressing again cycles through the others, then hides it. The setups come from a gitignored `data/four-lol/` (personal use, credited in `THIRD_PARTY_NOTICES.md`); without it the key says "No setups file".

**Bag count** is 40L's rules and goal with no undo, no indicator and no hint: the solver isn't asked. The residue row reads "?" until the bag residue key (B) shows it, and it hides again at the next PC. The count isn't scored. Its best and tallies are saved to `pc-count.json`.

The PC coach mode is 40L's rules and goal with undo on. A worker thread answers about each piece from what's visible, `maxLines` 4, and the indicator shows "Thinking" until that piece's answer arrives; a step's `hold` means press hold first. A streak ends when an attempt spends more than 4 lines, an attempt counts once its first piece is placed, PCs, streak and tally roll back with undo, and a callout names a placement or a hold that killed the PC. Best and tallies are saved to `pc-coach.json`.

**LST coach.** No simple rule defines a "valid LST" stack, so this is a search with a hand-written evaluation, and it can give confident bad advice. No test can settle it: the owner validates it by playing. Built alongside the bot, as a bot with a different evaluation.

## 8. Bot

Fully our own C++ implementation, using ideas from [Cold Clear 2](https://github.com/MinusKelvin/cold-clear-2) (MIT or Apache-2.0) and [Zetris](https://github.com/ZetrisAI/Zetris) (BSD-3-Clause, based on MisaMino). **Check each licence before copying code**, since ideas are fine but code carries obligations.

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

**Players by TR** follow the presets in versus' picker, from TR 1k to 25k, defaulting to 20.5k (the owner's). Each round draws a player at that TR from the League ladder's stats (`data/bot/tr-field.json`: per TR band, the log-space mean and covariance of PPS, APP and VS/APM, interpolated between bands, plus one game's noise) and sets the bot to match: the player's PPS as the cap, width 48 and full depth, the base weights jittered ±20% for a style, and `mistakes` read from the bot's mirror APP table (the style's cost measured as nil) (`app/tr_field.*`). Known limits: the bot's APP tops out near 0.95, so the field ends around X/X+; and its VS/APM runs about 2.3 against humans' 2.0-2.15, so its VS reads about 15% high for the same PPS and APP.

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
| 4. Perfect clear | Solver, PC coach, indicator, overlay, success rates by phase | "Possible" indicator stays correct and doesn't cause frame drops |
| 5. Technique coaches | The LST coach, then others the owner asks for. Built during phase 7 | Owner signs off each coach by playing it |
| 6. Versus rules | Garbage, surge, multi-player match, versus replay parity | Tetra League replays pass with no mismatches |
| 7. Bot | Search, evaluation, presets, toggles, self-play tuning | Presets span new player to the owner's level |
| 8. X-rank bot | Sustained tuning against the targets in section 8 | The X-rank preset meets all three targets; likely the longest phase |

Attack calculation is written and unit-tested in phase 1, even though versus arrives later, since scoring depends on it. Phases 3 and 4 come before the bot because trainers are the priority and they build the search that the bot reuses. The order from here is 4, 6, then 7 with 5 alongside it, since coaches beyond PC share the bot's evaluation, and the bot needs phase 6's garbage for versus.

## 10. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Input latency worse than TETR.IO | Project fails its main goal | Tested in phase 1 before anything else is built on top |
| A rule change in TETR.IO | Parity drifts | Versioned rulesets; re-run with fresh replays |
| Undocumented parity edge cases | Delays phases 1 and 6 | Replay test finds the first mismatch; compare against Triangle |
| Bot doesn't reach X-rank strength at human speed | Top preset falls short of the owner's goal | Lower presets ship first; self-play tuning, then a learned evaluation if needed |
| PC success-rate query too slow | Feature limited | Precompute or sample, or leave it out |
| A coach gives confident bad advice | Teaches the wrong habit | PC answers are exact and tested; the owner validates the LST coach by playing |

## 11. Owner inputs

- **TETR.IO replays** per section 5.4, needed from phase 1 for solo and phase 6 for versus. Include one recent Tetra League replay for the ruleset.
- **Current handling settings**, to use as the default profile.
- **X-rank replays** for benchmarks, before phase 8, and ideally access to an X-rank player for validation.
- **Playing each technique coach** to validate it, in phase 5.
- **Final name**, whenever convenient. It must be an original name, not "Tetris" or similar.

## 12. Sources

- [Triangle.js](https://github.com/halp1/triangle): `src/engine/utils/damageCalc`, `src/engine/garbage`, `src/engine/index.ts`, `src/engine/utils/rng`, `test/engine/replay.ts`
- [SDL3 keyboard event docs](https://wiki.libsdl.org/SDL3/SDL_KeyboardEvent)
- [SDL `SDL_cocoaevents.m`](https://github.com/libsdl-org/SDL/blob/main/src/video/cocoa/SDL_cocoaevents.m) and [`SDL_cocoakeyboard.m`](https://github.com/libsdl-org/SDL/blob/main/src/video/cocoa/SDL_cocoakeyboard.m)
- [TETR.IO community wiki](https://tetris.wiki/TETR.IO) (background only)
