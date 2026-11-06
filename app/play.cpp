#include "app/play.h"

#include <algorithm>
#include <span>
#include <vector>

namespace {

// When the goal was reached: at the hard drop that caused the lock, so the
// time isn't rounded to a whole tick, or at the end of the tick for a lock
// that no input caused. Each hard drop in a tick causes one of its locks, in order.
double finishSeconds(const core::Game& game, std::span<const core::InputEvent> inputs, const std::vector<core::LockResult>& locks, int linesBefore, int goalLines) {
    int lines = linesBefore, locksBefore = 0;
    for (const core::LockResult& lock : locks) {
        lines += lock.lines;
        if (lines >= goalLines) break;
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
    shown = game;
    inputs.clear();
    startNs = runStartNs;
    started = false;
    over = false;
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
        int linesBefore = game.stats.lines;
        const auto& locks = game.tick(due);

        if (game.toppedOut) {
            over = true;
            endSeconds = game.seconds();
        }
        if (game.stats.lines >= mode->goalLines) {
            over = true;
            endSeconds = finishSeconds(game, due, locks, linesBefore, mode->goalLines);
            // The run has ended, so file work can't disturb play.
            if (best == 0 || endSeconds < best) {
                best = endSeconds;
                saveBest(best, bestPath);
            }
        }
        inputs.consume(due.size());
    }
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
