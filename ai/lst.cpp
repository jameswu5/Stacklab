#include "ai/lst.h"

namespace ai {

Rows rowsOf(const core::Board& board) {
    Rows rows{};
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (board.at(x, y) != core::Piece::None) rows[y] |= 1 << x;
    return rows;
}

bool lstAlive(const Rows& rows) {
    auto filled = [&](int x, int y) { return x < 0 || x >= core::kBoardWidth || y < 0 || (y < core::kBoardHeight && (rows[y] >> x & 1)); };
    for (uint16_t row : rows)
        if (row >> kLstWell & 1) return false;
    int tops[core::kBoardWidth] = {};
    for (int x = 0; x < core::kBoardWidth; x++) {
        for (int y = 0; y < core::kBoardHeight; y++)
            if (filled(x, y)) tops[x] = y + 1;
        for (int y = 0; y < tops[x]; y++) {
            if (filled(x, y)) continue;
            // The only holes are T-slot cells beside the well, one deep, with row y open across columns 1-3.
            if (x != 1 && x != 3) return false;
            if (!filled(x, y + 1) || !filled(x, y - 1) || filled(4 - x, y)) return false;
            const bool left = filled(1, y + 1), right = filled(3, y + 1);
            if (left && right) {
                // A fractal: only a TSD in the slot just above clears the cover.
                if (filled(1, y + 2) || filled(3, y + 2)) return false;
            } else if (right && !filled(3, y + 2) && filled(4, y + 2)) {
                return false;
            }
        }
    }
    // The slot's walls keep up with the overhang side and the stack, or the next slot can't form.
    return tops[1] >= tops[0] - 2 && tops[3] >= tops[4] - 2;
}

}  // namespace ai
