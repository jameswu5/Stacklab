#include "app/play.h"

#include <algorithm>
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

void Play::restart(const core::Handling& handling, uint32_t seed, uint64_t runStartNs) {
    game = core::Game(mode->rules, handling, seed);
    if (mode->garbageRows > 0) game.fillGarbage(mode->garbageRows, mode->messiness);
    shown = game;
    inputs.clear();
    startNs = runStartNs;
    started = false;
    over = false;
    if (mode->undo) {
        if (undoSlots.empty()) {
            undoSlots.reserve(kUndoDepth);
            for (int i = 0; i < kUndoDepth; i++) undoSlots.emplace_back(core::Ruleset{}, core::Handling{}, 1);
        }
        undoCount = 0;
        saveUndo();
    }
}

void Play::saveUndo() {
    undoNewest = (undoNewest + 1) % kUndoDepth;
    undoSlots[undoNewest] = game;
    undoCount = std::min(undoCount + 1, kUndoDepth);
}

void Play::undo(uint64_t timeNs) {
    if (!mode->undo || !started || over) return;
    if (undoCount > 1) {
        undoNewest = (undoNewest + kUndoDepth - 1) % kUndoDepth;
        undoCount--;
    }
    game = undoSlots[undoNewest];
    // Later inputs in the tick that saved the slot may have moved the piece.
    game.respawn(game.falling.piece);
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
    // order. CW and CCW are assumed to match 180, and soft drop to carry unhoisted.
    if (held[BindSoftDrop] > 0) inputs.push(core::Action::SoftDrop, true, startNs, startNs, 0);
    Bind firstMove = lastMove == BindLeft ? BindRight : BindLeft;
    for (Bind bind : {BindHold, BindCW, BindCCW, Bind180, firstMove, lastMove})
        if (held[bind] > 0) inputs.push(static_cast<core::Action>(bind), true, startNs, startNs, 0, true);
}

void Play::key(Bind action, bool down, uint64_t timeNs) {
    if (!started && timeNs >= startNs) begin();
    int& count = held[action];
    if (down ? count++ > 0 : count == 0 || --count > 0) return;
    if (down && (action == BindLeft || action == BindRight)) lastMove = action;
    if (started && !over) inputs.push(static_cast<core::Action>(action), down, timeNs, startNs, game.frame);
}

void Play::stepTo(uint64_t nowNs) {
    if (!started && nowNs >= startNs) begin();
    uint64_t target = tickAt(nowNs > startNs ? nowNs - startNs : 0).tick;
    while (!over && game.frame < target) {
        auto due = inputs.due(game.frame);
        int progressBefore = mode->goalProgress(game.stats);
        const auto& locks = game.tick(due);

        if (game.toppedOut && mode->editable) {
            game.board = core::Board{};
            game.respawn(game.falling.piece);
        } else if (game.toppedOut) {
            over = true;
            endSeconds = game.seconds();
        }
        // The run has ended in each of these, so file work can't disturb play.
        if (mode->goalLines > 0 && mode->goalProgress(game.stats) >= mode->goalLines) {
            over = true;
            endSeconds = finishSeconds(game, due, locks, progressBefore, *mode);
            if (best == 0 || endSeconds < best) {
                best = endSeconds;
                saveBest(best, bestPath);
            }
        }
        if (mode->bestIsScore() && !game.toppedOut && game.frame >= static_cast<uint64_t>(mode->timeLimitSeconds) * core::kTicksPerSecond) {
            over = true;
            endSeconds = game.seconds();
            if (game.stats.score > best) {
                best = game.stats.score;
                saveBestScore(game.stats.score, bestPath);
            }
        }
        inputs.consume(due.size());
        if (mode->undo && !locks.empty()) saveUndo();
    }
}

void Play::paint(int x, int y, core::Piece piece) {
    for (const core::Cell& cell : game.falling.cells())
        if (cell.x == x && cell.y == y) return;
    game.board.at(x, y) = piece;
}

void Play::cycleQueue(int slot) {
    core::Piece& piece = game.queue[slot];
    piece = static_cast<core::Piece>(static_cast<int>(piece) % 7 + 1);
}

// Goes through no piece as well.
void Play::cycleHold() {
    game.hold = static_cast<core::Piece>((static_cast<int>(game.hold) + 1) % 8);
}

// The tick in progress is run on a copy with the inputs received so far, so
// they reach the screen without waiting for the tick to complete. It runs
// again on the real game once complete, so this doesn't affect the result.
void Play::stepShown(uint64_t nowNs) {
    shown = game;
    if (over || !started) return;
    auto due = inputs.due(game.frame);
    shown.tick(due);

    for (size_t reached = inputs.head + due.size(); inputs.measured < reached; inputs.measured++) {
        lastInputNs = nowNs - std::min(nowNs, inputs.times[inputs.measured]);
        worstInputNs = std::max(worstInputNs, lastInputNs);
    }
}
