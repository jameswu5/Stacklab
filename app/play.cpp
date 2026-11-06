#include "app/play.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace {

// When the goal was reached: at the hard drop that caused the lock, so the
// time isn't rounded to a whole tick, or at the end of the tick for a lock
// that no input caused. Each hard drop in a tick causes one of its locks, in order.
double finishSeconds(const core::Game& game, std::span<const core::InputEvent> inputs, const std::vector<core::LockResult>& locks, int linesBefore) {
    int lines = linesBefore, locksBefore = 0;
    for (const core::LockResult& lock : locks) {
        lines += lock.lines;
        if (lines >= kGoalLines) break;
        locksBefore++;
    }
    for (const core::InputEvent& input : inputs) {
        if (input.action != core::Action::HardDrop || !input.pressed) continue;
        if (locksBefore-- == 0) return (static_cast<double>(game.frame - 1) + input.subframe) / core::kTicksPerSecond;
    }
    return game.seconds();
}

}  // namespace

void Play::restart(const core::Handling& handling, uint32_t seed, uint64_t timeNs) {
    game = core::Game(core::Ruleset{}, handling, seed);
    shown = game;
    inputs.clear();
    startNs = timeNs;
    over = false;
    splits = {};
    splitLines = 0;
    std::snprintf(pace, sizeof pace, "-");
    paceAhead = false;
    // Keys held through a retry carry over. Left and right start charged, with
    // the most recently pressed one last. This is inferred from Triangle.js, not
    // documented, so check it here if real replays disagree.
    Bind firstMove = lastMove == BindLeft ? BindRight : BindLeft;
    for (Bind bind : {firstMove, lastMove})
        if (held[bind] > 0) inputs.push(static_cast<core::Action>(bind), true, timeNs, startNs, 0, true);
    if (held[BindSoftDrop] > 0) inputs.push(core::Action::SoftDrop, true, timeNs, startNs, 0);
}

void Play::key(Bind action, bool down, uint64_t timeNs) {
    int& count = held[action];
    if (down ? count++ > 0 : count == 0 || --count > 0) return;
    if (down && (action == BindLeft || action == BindRight)) lastMove = action;
    if (!over) inputs.push(static_cast<core::Action>(action), down, timeNs, startNs, game.frame);
}

void Play::stepTo(uint64_t nowNs) {
    uint64_t target = tickAt(nowNs > startNs ? nowNs - startNs : 0).tick;
    while (!over && game.frame < target) {
        auto due = inputs.due(game.frame);
        int linesBefore = game.stats.lines;
        const auto& locks = game.tick(due);

        int lines = std::min(game.stats.lines, kGoalLines);
        if (lines > splitLines) {
            double now = game.seconds();
            for (int line = splitLines + 1; line <= lines; line++) splits[line] = now;
            splitLines = lines;
            if (best[kGoalLines] > 0) {
                double delta = now - best[lines];
                std::snprintf(pace, sizeof pace, "%s%.2fs", delta < 0 ? "-" : "+", std::abs(delta));
                paceAhead = delta < 0;
            }
        }

        if (game.toppedOut) {
            over = true;
            endSeconds = game.seconds();
        }
        if (lines == kGoalLines) {
            over = true;
            endSeconds = finishSeconds(game, due, locks, linesBefore);
            splits[kGoalLines] = endSeconds;
            // The run has ended, so file work can't disturb play.
            if (best[kGoalLines] == 0 || splits[kGoalLines] < best[kGoalLines]) {
                best = splits;
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
    if (over) return;
    auto due = inputs.due(game.frame);
    shown.tick(due);

    for (size_t reached = inputs.head + due.size(); inputs.measured < reached; inputs.measured++) {
        lastInputNs = nowNs - std::min(nowNs, inputs.times[inputs.measured]);
        worstInputNs = std::max(worstInputNs, lastInputNs);
    }
}
