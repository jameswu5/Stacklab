// Where a piece can end up from where it is, and the fewest key presses to get
// there. Time doesn't pass in the search: gravity and lock delay can't act, so
// it answers "what can be reached", not "what can be reached at this gravity".
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/game.h"

namespace ai {

// Each is one key press. A Das move holds the key until the piece stops, and
// SoftDrop reaches the floor and lets go.
// ponytail: holding soft drop across a later move isn't modelled, so a drop, move,
// drop tuck counts two presses of soft drop where a player can use one. Minimums can
// only come out high, which misses a fault rather than inventing one.
// Down is one row, from a slow soft drop or from waiting for gravity. Waiting is no
// press at all, but costing it one keeps hard drops the cheapest way down.
enum class Move : uint8_t { Left, Right, DasLeft, DasRight, RotateCW, RotateCCW, Rotate180, SoftDrop, Down };

// Where a piece can lock, with hard drop as the implied last press: `cost` leaves
// it out. The same cells can appear twice from symmetric rotations of I, O, S and
// Z, and with different spins.
struct Placement {
    int rot, x, y;
    core::Spin spin;
    int cost;
    uint16_t state;  // for MoveSearch::inputs
};

// Reusable: after the first run, later runs don't allocate.
class MoveSearch {
public:
    MoveSearch();

    std::vector<Placement> placements;  // cheapest first

    // `start` is taken as having no spin yet, as at a spawn.
    void run(const core::Board& board, const core::Falling& start, const core::Ruleset& rules);

    std::array<core::Cell, 4> cells(const Placement& placement) const;
    std::vector<Move> inputs(const Placement& placement) const;

private:
    struct Node {
        uint16_t parent = 0;
        uint8_t cost = 0;
        Move move = Move::Left;
        uint8_t resets = 0;  // Falling::lockResets on the way here
        int8_t lowest = 0;   // Falling::highestY on the way here
        bool seen = false;
        bool placed = false;
    };

    bool fits(int rot, int x, int y) const;
    int landing(int rot, int x, int y) const;
    core::Spin spinAt(int rot, int x, int y, bool finOrTst, const core::Ruleset& rules) const;

    core::Piece piece_ = core::Piece::None;
    std::array<uint64_t, core::kBoardWidth> open_{};  // per column, bit y set when the cell is empty
    std::array<std::array<uint64_t, 13>, 4> free_{};  // per rotation and x, bit y set when the piece fits
    std::vector<Node> nodes_;
    std::vector<uint16_t> queue_;
};

}  // namespace ai
