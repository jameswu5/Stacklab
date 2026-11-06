#include "app/play.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    // Keep movement that's still held, so DAS can be charged across a retry.
    for (Bind bind : {BindLeft, BindRight, BindSoftDrop})
        if (held[bind] > 0) inputs.push(static_cast<core::Action>(bind), true, timeNs, startNs, 0);
}

void Play::key(Bind action, bool down, uint64_t timeNs) {
    int& count = held[action];
    if (down ? count++ > 0 : count == 0 || --count > 0) return;
    if (!over) inputs.push(static_cast<core::Action>(action), down, timeNs, startNs, game.frame);
}

void Play::stepTo(uint64_t nowNs) {
    uint64_t target = tickAt(nowNs > startNs ? nowNs - startNs : 0).tick;
    while (!over && game.frame < target) {
        auto due = inputs.due(game.frame);
        game.tick(due);
        inputs.consume(due.size());

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

        if (game.toppedOut) over = true;
        if (lines == kGoalLines) {
            over = true;
            // The run has ended, so file work can't disturb play.
            if (best[kGoalLines] == 0 || splits[kGoalLines] < best[kGoalLines]) {
                best = splits;
                saveBest(best, bestPath);
            }
        }
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
