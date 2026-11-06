// Judges each placement by comparing the keys pressed for it with the move search's
// minimum. A placement costs one per press of move, rotate or soft drop, held or not.
// Hard drop is on neither side. Hold is a choice of piece rather than a way to place
// one, so it never costs anything: the count starts again for the piece it brings out.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "ai/move_search.h"

namespace ai {

struct Judgement {
    core::Piece piece;
    int rot;  // of the cheapest way to these cells, so symmetric pieces count as one
    int presses;
    int minimum;
    int faults;  // presses beyond the minimum, never below 0
};

// Reusable: after the first pieces, ticks don't allocate.
// ponytail: the search doesn't let time pass, so at gravity high enough to pull a piece
// down between presses a minimum can be out of reach and a fault invented. Skip judging
// or search from the floor if finesse is shown in a mode that gets that fast.
class Finesse {
public:
    explicit Finesse(const core::Game& game);

    // After changing the game other than through tick: retry, undo, board edits.
    void restart(const core::Game& game);

    // Runs game.tick and returns its result, setting `judged`.
    const std::vector<core::LockResult>& tick(core::Game& game, std::span<const core::InputEvent> inputs);

    // The piece locked this tick, unless it couldn't be judged fairly: it wasn't among
    // the search's placements, or its keys or board can't be told apart from another
    // piece's in the same tick. Never a guess, so a total of faults is only ever low.
    std::optional<Judgement> judged;

private:
    void startPiece(const core::Game& game);
    void judge(const core::LockResult& lock, int presses);

    MoveSearch search_;
    core::Game spawn_;  // a copy to find the piece's spawn on, whatever it's done since
    int presses_ = 0;
    bool blind_ = false;  // this piece's search or count is unknown, so don't judge it
};

}  // namespace ai
