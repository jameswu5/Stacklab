#include "app/play.h"

#include <algorithm>
#include <random>
#include <span>
#include <vector>

namespace {

// When the goal was reached: at the hard drop that caused the lock, so the
// time isn't rounded to a whole tick, or at the end of the tick for a lock
// that no input caused. Each hard drop in a tick causes one of its locks, in order.
double finishSeconds(const core::Game& game, std::span<const core::InputEvent> inputs, const std::vector<core::LockResult>& locks, int progressBefore, const Mode& mode) {
    int progress = progressBefore, locksBefore = 0;
    for (const core::LockResult& lock : locks) {
        progress += mode.garbageRows > 0 ? lock.garbageCleared : lock.lines;
        if (progress >= mode.goalLines) break;
        locksBefore++;
    }
    for (const core::InputEvent& input : inputs) {
        if (input.action != core::Action::HardDrop || !input.pressed) continue;
        if (locksBefore-- == 0) return (static_cast<double>(game.frame - 1) + input.subframe) / core::kTicksPerSecond;
    }
    return game.seconds();
}

}  // namespace

std::vector<core::Piece> dealResidue(core::Game& game, int residue, uint32_t seed) {
    if (residue == 0) return {};
    for (int i = 0; i < 6; i++) game.deal();  // past the first bag, which no_szo reorders, with the falling piece
    std::vector<core::Piece> dealt(residue + 1);
    for (core::Piece& piece : dealt) piece = game.deal();
    game.hold = dealt[std::mt19937(seed)() % dealt.size()];
    game.respawn(game.deal());
    return dealt;
}

void Play::restart(const core::Handling& handling, uint32_t seed, uint64_t runStartNs) {
    core::Ruleset rules = mode->rules;
    if (mode->versus) rules.garbageMultiplier = versusOptions.multiplier;
    game = core::Game(rules, handling, seed);
    if (mode->garbageRows > 0) {
        game.fillGarbage(mode->garbageRows);
        game.cheeseLeft = mode->goalLines - mode->garbageRows;
    }
    if (drill) setUpDrill(game, *drill);
    std::vector<core::Piece> dealt;
    if (mode->pcStyle == PcStyle::Practice) dealt = dealResidue(game, residue, seed);
    filled = 0;
    missed = false;
    finesse.restart(game);
    faults = {};
    if (mode->pcFile) {
        if (!coach) coach = std::make_unique<PcCoach>();
        coach->style = mode->pcStyle;
        coach->start(game, dealt);
    }
    if (mode->versus) {
        if (!versus) versus = std::make_unique<Versus>();
        versus->preset = botPreset;
        versus->options = versusOptions;
        versus->match = {};
        versus->recording = &recording;
        versus->playback = nullptr;
        versus->startRound(seed);
        rounds.clear();
    }
    shown = game;
    inputs.clear();
    playback = nullptr;
    usedUndo = recordingFull = false;
    recording.mode = mode->name;
    recording.residue = residue;
    recording.seed = seed;
    recording.handling = handling;
    recording.ticks.clear();
    recording.inputs.clear();
    recording.ticks.reserve(kReplayInputs);
    recording.inputs.reserve(kReplayInputs);
    recording.versus = versusOptions;
    recording.botTicks.clear();
    recording.botInputs.clear();
    if (mode->versus) {
        recording.botTicks.reserve(kReplayInputs);
        recording.botInputs.reserve(kReplayInputs);
    }
    startNs = runStartNs;
    started = false;
    over = false;
    if (mode->undo) {
        if (undoSlots.empty()) {
            undoSlots.reserve(kUndoDepth);
            for (int i = 0; i < kUndoDepth; i++) undoSlots.push_back({core::Game(core::Ruleset{}, core::Handling{}, 1), 0, {}, {}});
        }
        for (UndoSlot& slot : undoSlots) slot.game.queue.reserve(game.queue.capacity());  // a drill's queue can be long
        undoCount = 0;
        saveUndo();
    }
}

void Play::saveUndo() {
    undoNewest = (undoNewest + 1) % kUndoDepth;
    undoSlots[undoNewest].game = game;
    undoSlots[undoNewest].filled = filled;
    undoSlots[undoNewest].faults = faults;
    if (mode->pcFile) undoSlots[undoNewest].pc = coach->progress;
    undoCount = std::min(undoCount + 1, kUndoDepth);
}

void Play::startPlayback(const Replay& replay, uint64_t runStartNs) {
    residue = replay.residue;
    if (mode->versus) versusOptions = replay.versus;
    restart(replay.handling, replay.seed, runStartNs);
    if (mode->versus) versus->playback = &replay;
    if (mode->pcFile) coach->discard();
    playback = &replay;
    playbackAt = 0;
}

std::span<const core::InputEvent> Play::due() const {
    if (!playback) return inputs.due(game.frame);
    size_t end = playbackAt;
    while (end < playback->inputs.size() && playback->ticks[end] <= game.frame) end++;
    return std::span(playback->inputs).subspan(playbackAt, end - playbackAt);
}

void Play::undo(uint64_t timeNs) {
    if (!mode->undo || !started || over || playback) return;
    usedUndo = true;
    // A miss saved no slot, so the newest is where the missed piece started.
    if (undoCount > 1 && !missed) {
        undoNewest = (undoNewest + kUndoDepth - 1) % kUndoDepth;
        undoCount--;
    }
    game = undoSlots[undoNewest].game;
    filled = undoSlots[undoNewest].filled;
    faults = undoSlots[undoNewest].faults;
    missed = false;
    // Later inputs in the tick that saved the slot may have moved the piece.
    game.respawn(game.falling.piece);
    finesse.restart(game);
    if (mode->pcFile) coach->rollback(undoSlots[undoNewest].pc, game);
    startNs = timeNs - (game.frame * 1'000'000'000 + core::kTicksPerSecond - 1) / core::kTicksPerSecond;
    // The slot remembers the keys held back then. At subframe 0 these release
    // them before a charged move can act.
    inputs.clear();
    for (int action = 0; action < BindRetry; action++) inputs.push(static_cast<core::Action>(action), false, startNs, startNs, game.frame);
    inputs.measured = inputs.events.size();  // not real key presses, so not for the latency readout
}

void Play::begin() {
    started = true;
    // Keys held when the run starts carry over, with the most recently pressed move
    // last so it sets direction. Replays show hoisted hold, 180 and moves, in that
    // order. CW and CCW are assumed to match 180. Soft drop goes first, and being
    // hoisted only tells finesse that it wasn't pressed for this piece.
    if (held[BindSoftDrop] > 0) inputs.push(core::Action::SoftDrop, true, startNs, startNs, 0, true);
    Bind firstMove = lastMove == BindLeft ? BindRight : BindLeft;
    for (Bind bind : {BindHold, BindCW, BindCCW, Bind180, firstMove, lastMove})
        if (held[bind] > 0) inputs.push(static_cast<core::Action>(bind), true, startNs, startNs, 0, true);
}

void Play::key(Bind action, bool down, uint64_t timeNs) {
    if (playback) return;
    if (!started && timeNs >= startNs) begin();
    int& count = held[action];
    if (down ? count++ > 0 : count == 0 || --count > 0) return;
    if (down && (action == BindLeft || action == BindRight)) lastMove = action;
    if (started && !over && !missed) inputs.push(static_cast<core::Action>(action), down, timeNs, startNs, game.frame);
}

void Play::nextRound(uint64_t nowNs) {
    const Match match = versus->match;
    std::vector<Replay> played = std::move(rounds);
    played.push_back(recording);  // between rounds, so allocating can't disturb play
    restart(game.handling, static_cast<uint32_t>(nowNs), nowNs + kCountdownNs);
    versus->match = match;
    rounds = std::move(played);
}

void Play::stepTo(uint64_t nowNs) {
    if (mode->versus && versus->roundOver && !over && nowNs >= versus->roundEndNs + kRoundCardNs) nextRound(nowNs);
    if (!started && nowNs >= startNs) begin();
    if (mode->pcFile) coach->poll();
    uint64_t target = tickAt(nowNs > startNs ? nowNs - startNs : 0).tick;
    while (!over && !missed && !(mode->versus && versus->roundOver) && game.frame < target) {
        auto due = this->due();
        // Into space reserved at restart, so recording never allocates.
        for (size_t i = 0; i < due.size() && !playback && !recordingFull; i++) {
            recordingFull = recording.inputs.size() == kReplayInputs;
            if (recordingFull) break;
            recording.ticks.push_back(game.frame);
            recording.inputs.push_back(due[i]);
        }
        auto consume = [&] {
            if (playback) playbackAt += due.size();
            else inputs.consume(due.size());
        };
        int progressBefore = mode->goalProgress(game.stats);
        const auto& locks = mode->versus ? game.tick(due, versus->toYou) : finesse.tick(game, due);
        if (mode->versus) {
            versus->afterYourTick(locks, game.frame);
            consume();
            recordingFull = recordingFull || recording.botInputs.size() == kReplayInputs;
            if (game.toppedOut || versus->bot.toppedOut) {
                over = versus->endRound(game, nowNs) || playback;  // a replay is one round
                endSeconds = game.seconds();
            }
            continue;
        }
        if (!locks.empty()) faults.note(finesse.judged);

        if (game.toppedOut && !drill) {
            over = true;
            endSeconds = game.seconds();
        }
        // The run has ended in each of these, so file work can't disturb play.
        if (mode->goalLines > 0 && mode->goalProgress(game.stats) >= mode->goalLines) {
            over = true;
            endSeconds = finishSeconds(game, due, locks, progressBefore, *mode);
            if (mode->bestFile && !playback && (best == 0 || endSeconds < best)) {
                best = endSeconds;
                saveBest(best, bestPath);
                if (replayable() && !bestPath.empty()) saveReplay(recording, bestReplayPath(bestPath));
            }
        }
        consume();
        // A new attempt has its own clock, so the next call picks it up. A miss waits for a key.
        if (drill && judgeDrill(locks, nowNs)) return;
        if (mode->pcFile) {
            coach->afterTick(locks, game);
            if (!over && coach->runDone()) {
                over = true;
                endSeconds = game.seconds();
            }
            if (over) coach->save();  // the run has ended, so file work can't disturb play
        }
        if (mode->undo && !locks.empty()) saveUndo();
    }
}

// A piece can fill any target still open, so hold can change the order. Cells are
// compared, not rotations, since I, S, Z and O fill the same cells in two of theirs.
// ponytail: a target's y assumes the listed order when an earlier piece clears lines; other orders miss there.
bool Play::judgeDrill(std::span<const core::LockResult> locks, uint64_t nowNs) {
    const uint64_t all = (uint64_t{1} << drill->targets.size()) - 1;
    bool wrong = false;
    for (const core::LockResult& lock : locks) {
        if (filled == all) break;
        bool hit = false;
        for (size_t i = 0; i < drill->targets.size() && !hit; i++) {
            const DrillTarget& target = drill->targets[i];
            hit = !(filled >> i & 1) && target.piece == lock.piece && std::is_permutation(target.cells.begin(), target.cells.end(), lock.cells.begin(), [](const core::Cell& a, const core::Cell& b) { return a.x == b.x && a.y == b.y; });
            if (hit) filled |= uint64_t{1} << i;
        }
        wrong = wrong || !hit;
    }
    if (!wrong && filled == all) {
        if (drill->next) {
            drill = drill->next;
            restart(game.handling, 1, nowNs + kCountdownNs);
        } else {
            over = true;
            endSeconds = game.seconds();
        }
        return true;
    }
    // Out of queue: a held piece still gets its turn.
    if (!wrong && !game.toppedOut && game.falling.piece == core::Piece::None) {
        if (game.hold == core::Piece::None) {
            wrong = true;
        } else {
            const core::Piece last = game.hold;
            game.hold = core::Piece::None;
            game.respawn(last);
            finesse.restart(game);
        }
    }
    if (!wrong && !game.toppedOut) return false;
    missed = true;
    return true;
}

// The tick in progress is run on a copy with the inputs received so far, so
// they reach the screen without waiting for the tick to complete. It runs
// again on the real game once complete, so this doesn't affect the result.
void Play::stepShown(uint64_t nowNs) {
    shown = game;
    if (over || !started || missed) return;
    auto due = this->due();
    shown.tick(due);
    if (playback) return;

    for (size_t reached = inputs.head + due.size(); inputs.measured < reached; inputs.measured++) {
        lastInputNs = nowNs - std::min(nowNs, inputs.times[inputs.measured]);
        worstInputNs = std::max(worstInputNs, lastInputNs);
    }
}
