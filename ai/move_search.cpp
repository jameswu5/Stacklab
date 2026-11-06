#include "ai/move_search.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ai {

namespace {

constexpr int kXMargin = 3;  // the lowest x that fits is -2, and neighbours of it get checked
constexpr int kMaxResets = 31;  // Falling::lockResets stops counting here

constexpr int stateId(int rot, int x, int y, core::Spin spin) {
    return rot | (x + kXMargin) << 2 | y << 6 | static_cast<int>(spin) << 12;
}

}  // namespace

MoveSearch::MoveSearch() : nodes_(1 << 14) {
    queue_.reserve(nodes_.size());
    placements.reserve(nodes_.size());
}

bool MoveSearch::fits(int rot, int x, int y) const { return y >= 0 && (free_[rot][x + kXMargin] >> y & 1); }

// The row a piece stops at when it falls from y.
int MoveSearch::landing(int rot, int x, int y) const {
    uint64_t blocked = ~free_[rot][x + kXMargin] & ((uint64_t{1} << y) - 1);
    return 64 - std::countl_zero(blocked);
}

// Game::detectSpin, on the masks.
core::Spin MoveSearch::spinAt(int rot, int x, int y, bool finOrTst, const core::Ruleset& rules) const {
    if (rules.spinBonuses == core::SpinBonuses::None) return core::Spin::None;

    core::Spin tSpin = core::Spin::None;
    if (piece_ == core::Piece::T && !fits(rot, x, y - 1)) {
        int corners = 0, front = 0;
        for (const auto& corner : core::kTCorners) {
            int cx = x + corner[0] + 1, cy = y - corner[1] - 1;
            bool occupied = cx < 0 || cy < 0 || cx >= core::kBoardWidth || cy >= core::kBoardHeight || !(open_[cx] >> cy & 1);
            if (!occupied) continue;
            corners++;
            if (rot == corner[2] || rot == corner[3]) front++;
        }
        if (corners >= 3) tSpin = front != 2 && !finOrTst ? core::Spin::Mini : core::Spin::Normal;
    }
    if (rules.spinBonuses == core::SpinBonuses::TSpins) return tSpin;

    bool immobile = !fits(rot, x - 1, y) && !fits(rot, x + 1, y) && !fits(rot, x, y + 1) && !fits(rot, x, y - 1);
    return std::max(tSpin, immobile ? core::Spin::Mini : core::Spin::None);
}

void MoveSearch::run(const core::Board& board, const core::Falling& start, const core::Ruleset& rules) {
    for (uint16_t id : queue_) nodes_[id] = {};
    queue_.clear();
    placements.clear();
    piece_ = start.piece;

    for (int x = 0; x < core::kBoardWidth; x++) {
        uint64_t open = 0;
        for (int y = 0; y < core::kBoardHeight; y++)
            if (board.at(x, y) == core::Piece::None) open |= uint64_t{1} << y;
        open_[x] = open;
    }
    core::Falling shape;
    shape.piece = piece_;
    for (shape.rot = 0; shape.rot < 4; shape.rot++) {
        auto blocks = shape.cells();
        for (int x = -kXMargin; x < 13 - kXMargin; x++) {
            uint64_t bits = ~uint64_t{0};
            for (const core::Cell& block : blocks) {
                int column = x + block.x;
                bits &= column < 0 || column >= core::kBoardWidth ? 0 : open_[column] << -block.y;
            }
            free_[shape.rot][x + kXMargin] = bits;
        }
    }

    auto visit = [&](int rot, int x, int y, core::Spin spin, int cost, int resets, int lowest, int parent, Move move) {
        int id = stateId(rot, x, y, spin);
        Node& node = nodes_[id];
        if (node.seen) return;
        node.seen = true;
        node.parent = static_cast<uint16_t>(parent);
        node.cost = static_cast<uint8_t>(cost);
        node.move = move;
        node.resets = static_cast<uint8_t>(resets);
        node.lowest = static_cast<int8_t>(lowest);
        queue_.push_back(static_cast<uint16_t>(id));
    };

    int startY = static_cast<int>(std::floor(start.y));
    int startId = stateId(start.rot, start.x, startY, core::Spin::None);
    visit(start.rot, start.x, startY, core::Spin::None, 0, start.lockResets, start.highestY, startId, Move::Left);

    for (size_t head = 0; head < queue_.size(); head++) {
        int id = queue_[head];
        const Node node = nodes_[id];
        int rot = id & 3, x = (id >> 2 & 15) - kXMargin, y = id >> 6 & 63;
        core::Spin spin = static_cast<core::Spin>(id >> 12);
        int cost = node.cost + 1;
        int moved = std::min(node.resets + 1, kMaxResets);

        // Falling clears a spin, so a hard drop only keeps one from the ground.
        int floor = landing(rot, x, y);
        Node& placed = nodes_[stateId(rot, x, floor, floor == y ? spin : core::Spin::None)];
        if (!placed.placed) {
            placed.placed = true;
            placements.push_back({rot, x, floor, floor == y ? spin : core::Spin::None, node.cost, static_cast<uint16_t>(id)});
        }
        // Out of lock resets on the ground, the next tick locks the piece here.
        if (floor == y && node.resets >= rules.lockResets) continue;

        for (int dir : {-1, 1}) {
            if (!fits(rot, x + dir, y)) continue;
            visit(rot, x + dir, y, spin, cost, moved, node.lowest, id, dir < 0 ? Move::Left : Move::Right);
            // The first cell moves on the press and the rest once DAS charges, ticks
            // later, so running out of resets on the first cell ends the slide there.
            if (landing(rot, x + dir, y) == y && moved >= rules.lockResets) continue;
            int to = x + dir, resets = moved;
            while (fits(rot, to + dir, y)) {
                to += dir;
                resets = std::min(resets + 1, kMaxResets);
            }
            visit(rot, to, y, spin, cost, resets, node.lowest, id, dir < 0 ? Move::DasLeft : Move::DasRight);
        }

        for (int amount : {1, -1, 2}) {
            if (amount == 2 && !rules.allow180) continue;
            int to = (rot + amount + 4) % 4;
            Move move = amount == 1 ? Move::RotateCW : amount == -1 ? Move::RotateCCW : Move::Rotate180;
            if (fits(to, x, y)) {
                visit(to, x, y, spinAt(to, x, y, false, rules), cost, moved, node.lowest, id, move);
                continue;
            }
            const core::Kicks& kicks = core::kickTable(piece_, rot, to);
            for (int i = 0; i < kicks.count; i++) {
                int dx = kicks.offsets[i][0], dy = kicks.offsets[i][1];
                if (!fits(to, x + dx, y - dy)) continue;
                bool finOrTst = (rot == 2 || rot == 0) && dy == 2 && ((to == 3 && dx == 1) || (to == 1 && dx == -1));
                visit(to, x + dx, y - dy, spinAt(to, x + dx, y - dy, finOrTst, rules), cost, moved, node.lowest, id, move);
                break;
            }
        }

        if (floor < y) {
            auto fallTo = [&](int row, Move move) {
                bool lower = row < node.lowest;
                visit(rot, x, row, core::Spin::None, cost, lower ? 0 : node.resets, lower ? row : node.lowest, id, move);
            };
            fallTo(floor, Move::SoftDrop);
            fallTo(y - 1, Move::Down);
        }
    }
}

std::array<core::Cell, 4> MoveSearch::cells(const Placement& placement) const {
    core::Falling falling;
    falling.piece = piece_;
    falling.rot = placement.rot;
    falling.x = placement.x;
    falling.y = placement.y;
    return falling.cells();
}

std::vector<Move> MoveSearch::inputs(const Placement& placement) const {
    std::vector<Move> moves(placement.cost);
    int id = placement.state;
    for (int i = placement.cost - 1; i >= 0; i--) {
        moves[i] = nodes_[id].move;
        id = nodes_[id].parent;
    }
    return moves;
}

}  // namespace ai
