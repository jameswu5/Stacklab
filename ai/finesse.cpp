#include "ai/finesse.h"

#include <algorithm>

namespace ai {

namespace {

// The cells as a value that ignores which rotation made them.
std::array<int, 4> cellKey(std::array<core::Cell, 4> cells) {
    std::array<int, 4> key;
    for (int i = 0; i < 4; i++) key[i] = cells[i].y * core::kBoardWidth + cells[i].x;
    std::sort(key.begin(), key.end());
    return key;
}

}  // namespace

// Constructed, not copied, so spawn_ has the capacity that copying into it relies on.
Finesse::Finesse(const core::Game& game) : spawn_(core::Ruleset{}, core::Handling{}, 1) { restart(game); }

void Finesse::restart(const core::Game& game) {
    judged.reset();
    presses_ = 0;
    startPiece(game);
}

void Finesse::startPiece(const core::Game& game) {
    spawn_ = game;
    spawn_.respawn(game.falling.piece);
    blind_ = spawn_.toppedOut;
    if (!blind_) search_.run(spawn_.board, spawn_.falling, spawn_.rules);
}

void Finesse::judge(const core::LockResult& lock, int presses) {
    auto key = cellKey(lock.cells);
    for (const Placement& placement : search_.placements) {  // cheapest first
        if (placement.spin != lock.spin || cellKey(search_.cells(placement)) != key) continue;
        judged = Judgement{lock.piece, placement.rot, presses, placement.cost, std::max(0, presses - placement.cost)};
        return;
    }
}

const std::vector<core::LockResult>& Finesse::tick(core::Game& game, std::span<const core::InputEvent> inputs) {
    judged.reset();
    bool holdUsed = game.holdUsed;
    const auto& locks = game.tick(inputs);

    // A tick's events are in order, so walk them to see which piece each key was for.
    int presses = presses_, pressesAtLock = -1;
    bool held = false;
    for (const core::InputEvent& input : inputs) {
        if (!input.pressed) continue;
        if (input.action == core::Action::HardDrop) {
            if (locks.empty()) continue;  // safelock ignored it
            if (pressesAtLock < 0) pressesAtLock = presses;
            presses = 0;
            holdUsed = false;
        } else if (input.action == core::Action::Hold) {
            if (holdUsed || !game.rules.allowHold) continue;
            presses = 0;
            held = true;
            holdUsed = true;
        } else if (!input.hoisted) {
            presses++;
        }
    }

    // A lock without a hard drop came from the ground, at a time inside the tick that
    // isn't known, so a key in the same tick could be for either piece.
    bool unclear = false;
    if (!locks.empty() && pressesAtLock < 0) {
        unclear = presses != presses_;
        pressesAtLock = presses;
        presses = 0;
    }

    if (locks.size() == 1 && !blind_ && !unclear) judge(locks[0], pressesAtLock);
    presses_ = presses;
    if (!locks.empty() || held) startPiece(game);
    blind_ = blind_ || unclear;
    return locks;
}

}  // namespace ai
