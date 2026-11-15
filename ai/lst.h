// LST stacking, as four.lol's /stacking/lst teaches it, on the left of the board: L and S
// make overhangs at columns 0-1, J and Z rebuild columns 3-4, and each T-spin double goes
// into column 2, which stays empty. The rest of the board is a flat stack beside it.
#pragma once

#include <array>
#include <cstdint>

#include "core/game.h"

namespace ai {

using Rows = std::array<uint16_t, core::kBoardHeight>;  // a bit per column, floor first

constexpr int kLstWell = 2;

Rows rowsOf(const core::Board& board);

// Whether the loop can go on from this board, lines already cleared. False when column 2
// has a block, a hole isn't under an overhang beside column 2, or a T-slot is covered on
// the right without the cell above the cover filled (four.lol's "common mistakes": after
// that TSD, column 3 is a one-wide gap nothing but an I can fill).
bool lstAlive(const Rows& rows);

// How ready the next T-spin double over the well is, 0 to 1: at the lowest row y with
// columns 1 and 3 filled and columns 1-3 open in row y + 1, the share of the 14 other cells
// the TSD clears that are filled, halved without an overhang over column 1 or 3. 0 without one.
double lstSlot(const Rows& rows);

}  // namespace ai
