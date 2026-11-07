#include "ai/pc_solver.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ai {

namespace {

constexpr uint16_t kFullRow = (1 << core::kBoardWidth) - 1;
constexpr uint8_t kFullBag = 0xFE;  // a bit per core::Piece from I to Z

void fillBoard(core::Board& board, const std::array<uint16_t, kPcMaxRows>& rows) {
    board = core::Board{};
    for (int y = 0; y < kPcMaxRows; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (rows[y] >> x & 1) board.at(x, y) = core::Piece::Garbage;
}

// Every occupied row has to clear, so there can't be more of them than lines left. And each
// column gets exactly `lines` cells before the end, whatever order rows clear in, so a piece only
// ever sits across columns that still need cells. Columns between two full ones therefore need a
// multiple of 4 between them. Empty regions of the board can't be checked that way: a clear
// joins the regions above and below it.
bool viable(const std::array<uint16_t, kPcMaxRows>& rows, const std::array<int8_t, core::kBoardWidth>& columns, int lines) {
    int occupied = 0;
    for (uint16_t row : rows) occupied += row != 0;
    if (occupied > lines) return false;
    int run = 0;
    for (int8_t column : columns) {
        int need = lines - column;
        if (need < 0) return false;
        if (need == 0 && run % 4 != 0) return false;
        run = need == 0 ? 0 : run + need;
    }
    return run % 4 == 0;
}

}  // namespace

size_t PcSolver::KeyHash::operator()(const Key& key) const {
    size_t hash = 0xcbf29ce484222325;
    auto mix = [&](size_t value) { hash = (hash ^ value) * 0x100000001b3; };
    for (uint16_t row : key.rows) mix(row);
    mix(key.next);
    mix(key.current);
    mix(key.hold);
    mix(key.bag);
    return hash;
}

uint8_t pcBagLeft(std::span<const core::Piece> seen) {
    const size_t inBag = seen.size() % 7;
    if (inBag == 0) return 0;
    uint8_t left = kFullBag;
    for (core::Piece piece : seen.last(inBag)) left &= ~(1 << static_cast<int>(piece));
    return left;
}

// Place `piece`, then carry on with the board it leaves.
bool PcSolver::place(const State& state, int lines, int placementsLeft, size_t next, core::Piece piece, core::Piece hold, bool pressedHold, uint8_t bag) {
    int top = 0;
    for (int y = 0; y < kPcMaxRows; y++)
        if (state.rows[y]) top = y + 1;
    core::Falling start = core::spawnFalling(piece);
    start.y = std::min(start.y, static_cast<double>(top + kPcAirRows));
    start.highestY = static_cast<int>(start.y);
    fillBoard(board_, state.rows);
    search_.run(board_, start, query_->rules);

    size_t depth = path_.size();
    if (candidates_.size() <= depth) candidates_.resize(depth + 1);
    std::vector<Candidate>& candidates = candidates_[depth];
    candidates.clear();
    for (const Placement& placement : search_.placements) {
        Candidate candidate{search_.cells(placement), 0};
        std::array<int, 4> ids;
        for (int i = 0; i < 4; i++) ids[i] = candidate.cells[i].y * core::kBoardWidth + candidate.cells[i].x;
        std::sort(ids.begin(), ids.end());
        for (int id : ids) candidate.id = candidate.id << 8 | id;
        // Spins don't matter here, and symmetric rotations are the same cells.
        if (std::none_of(candidates.begin(), candidates.end(), [&](const Candidate& seen) { return seen.id == candidate.id; })) candidates.push_back(candidate);
    }

    for (const Candidate& candidate : candidates) {
        State after = state;
        bool fits = true;
        for (const core::Cell& cell : candidate.cells) {
            if (cell.y >= kPcMaxRows) {
                capped_ = capped_ || hollow_;
                fits = false;
                break;
            }
            after.rows[cell.y] |= 1 << cell.x;
            after.columns[cell.x]++;
        }
        if (!fits) continue;

        int write = 0;
        for (int y = 0; y < kPcMaxRows; y++)
            if (after.rows[y] != kFullRow) after.rows[write++] = after.rows[y];
        const int cleared = kPcMaxRows - write;
        while (write < kPcMaxRows) after.rows[write++] = 0;
        for (int8_t& column : after.columns) column -= cleared;

        path_.push_back({piece, pressedHold, candidate.cells});
        const bool empty = std::all_of(after.rows.begin(), after.rows.end(), [](uint16_t row) { return row == 0; });
        if (empty) return true;
        if (placementsLeft > 1 && viable(after.rows, after.columns, lines - cleared) && draw(after, lines - cleared, placementsLeft - 1, next, hold, bag)) return true;
        path_.pop_back();
        if (stopped_) return false;
    }
    return false;
}

// Bring out the next piece, which may be an unseen one.
bool PcSolver::draw(const State& state, int lines, int placementsLeft, size_t next, core::Piece hold, uint8_t bag) {
    if (next < query_->queue.size()) return search(state, lines, placementsLeft, next + 1, query_->queue[next], hold, query_->rules.allowHold, bag);
    if (!unseen_) return search(state, lines, placementsLeft, next, core::Piece::None, hold, query_->rules.allowHold, bag);
    const uint8_t from = bag ? bag : kFullBag;
    for (int piece = 1; piece <= 7; piece++) {
        if (!(from >> piece & 1)) continue;
        drawn_.push_back(static_cast<core::Piece>(piece));
        if (search(state, lines, placementsLeft, next, static_cast<core::Piece>(piece), hold, query_->rules.allowHold, from & ~(1 << piece))) return true;
        drawn_.pop_back();
        if (stopped_) return false;
    }
    return false;
}

bool PcSolver::search(const State& state, int lines, int placementsLeft, size_t next, core::Piece current, core::Piece hold, bool canHold, uint8_t bag) {
    if (current == core::Piece::None && !toFinish_) return true;  // the known pieces are all placed

    // After the root, canHold only depends on the rules, so it needn't be in the key.
    Key key{state.rows, static_cast<uint8_t>(next), static_cast<uint8_t>(current), static_cast<uint8_t>(hold), bag};
    if (!failed_.insert(key).second) return false;

    if (++nodes_ > maxNodes_ || (cancel_ && cancel_->load(std::memory_order_relaxed))) {
        stopped_ = true;
        return false;
    }

    // Out of known pieces with one held: hold swaps it for whatever comes, and if
    // nothing comes, as in a drill, it comes back by itself.
    if (current == core::Piece::None) return hold != core::Piece::None && place(state, lines, placementsLeft, next, hold, core::Piece::None, !query_->queueEnds, bag);

    if (place(state, lines, placementsLeft, next, current, hold, false, bag)) return true;
    if (stopped_ || !canHold || hold == current) return false;
    if (hold != core::Piece::None) return place(state, lines, placementsLeft, next, hold, current, true, bag);

    // An empty hold brings out the next piece, so that's the one to place.
    if (next < query_->queue.size()) return place(state, lines, placementsLeft, next + 1, query_->queue[next], current, true, bag);
    if (!unseen_) return !toFinish_;  // holding the last known piece, waiting for what comes
    const uint8_t from = bag ? bag : kFullBag;
    for (int piece = 1; piece <= 7; piece++) {
        if (!(from >> piece & 1)) continue;
        drawn_.push_back(static_cast<core::Piece>(piece));
        if (place(state, lines, placementsLeft, next, static_cast<core::Piece>(piece), current, true, from & ~(1 << piece))) return true;
        drawn_.pop_back();
        if (stopped_) return false;
    }
    return false;
}

// The answer's inputs, from the real spawn. False if a step can't be reached from there,
// which would make the search's shortcut wrong, so it's then not an answer at all.
bool PcSolver::finish(const PcQuery& query, PcResult& result) {
    core::Board board = query.board;
    for (const PathStep& step : path_) {
        search_.run(board, core::spawnFalling(step.piece), query.rules);
        std::array<int, 4> want;
        for (int i = 0; i < 4; i++) want[i] = step.cells[i].y * core::kBoardWidth + step.cells[i].x;
        std::sort(want.begin(), want.end());
        bool found = false;
        for (const Placement& placement : search_.placements) {
            auto cells = search_.cells(placement);
            std::array<int, 4> have;
            for (int i = 0; i < 4; i++) have[i] = cells[i].y * core::kBoardWidth + cells[i].x;
            std::sort(have.begin(), have.end());
            if (have != want) continue;
            result.steps.push_back({step.piece, step.hold, step.cells, search_.inputs(placement)});
            found = true;
            break;
        }
        if (!found) return false;
        for (const core::Cell& cell : step.cells) board.at(cell.x, cell.y) = step.piece;
        int write = 0;
        for (int y = 0; y < core::kBoardHeight; y++) {
            bool full = true;
            for (int x = 0; x < core::kBoardWidth; x++) full = full && board.at(x, y) != core::Piece::None;
            if (full) continue;
            for (int x = 0; x < core::kBoardWidth; x++) board.at(x, write) = board.at(x, y);
            write++;
        }
        for (; write < core::kBoardHeight; write++)
            for (int x = 0; x < core::kBoardWidth; x++) board.at(x, write) = core::Piece::None;
    }
    return true;
}

PcResult PcSolver::run(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes, bool unseen) {
    PcResult result;
    query_ = &query;
    cancel_ = cancel;
    maxNodes_ = maxNodes;
    nodes_ = 0;
    stopped_ = capped_ = false;
    path_.clear();
    drawn_.clear();

    State start;
    int cells = 0, occupied = 0;
    for (int y = 0; y < core::kBoardHeight; y++) {
        bool any = false;
        for (int x = 0; x < core::kBoardWidth; x++) {
            if (query.board.at(x, y) == core::Piece::None) continue;
            any = true;
            cells++;
            if (y >= kPcMaxRows) continue;
            start.rows[y] |= 1 << x;
            start.columns[x]++;
        }
        occupied += any;
        if (any && y >= kPcMaxRows) return result;  // Unknown: too tall to hold
    }
    hollow_ = false;
    for (int y = 0; y + 1 < kPcMaxRows; y++) hollow_ = hollow_ || (start.rows[y] == 0 && start.rows[y + 1] != 0);
    if (cells % 2 != 0) {
        result.status = PcStatus::Impossible;  // pieces add 4 cells and clears take 10
        return result;
    }

    const int known = (query.current != core::Piece::None) + (query.hold != core::Piece::None) + static_cast<int>(query.queue.size());
    const bool canHold = query.rules.allowHold && !query.holdUsed;
    auto attempt = [&](int lines, int placements, bool toFinish) {
        toFinish_ = toFinish;
        failed_.clear();
        if (!viable(start.rows, start.columns, lines)) return false;
        return search(start, lines, placements, 0, query.current, query.hold, canHold, query.bagLeft);
    };
    auto answer = [&](PcStatus status) {
        result.status = status;
        result.nodes = nodes_;
        return result;
    };

    const int maxLines = std::min(query.maxLines, kPcMaxRows);
    for (int lines = std::max(occupied, 1); lines <= maxLines; lines++) {
        const int spare = 10 * lines - cells;
        if (spare % 4 != 0 || spare == 0) continue;
        const int placements = spare / 4;

        // Past what's known, a queue that ends rules this out, and longer ones too.
        if (placements > known && query.queueEnds) break;
        result.lines = lines;
        result.piecesNeeded = placements;

        if (placements <= known) {
            unseen_ = false;
            if (attempt(lines, placements, true)) {
                if (!finish(query, result)) return answer(PcStatus::Unknown);
                return answer(PcStatus::Found);
            }
            if (stopped_ || capped_) return answer(PcStatus::Unknown);
            continue;
        }

        unseen_ = unseen;
        const bool possible = attempt(lines, placements, unseen);
        if (stopped_) return answer(PcStatus::Unknown);
        if (possible && unseen) {
            if (!finish(query, result)) return answer(PcStatus::Unknown);
            result.assumed = drawn_;
            return answer(PcStatus::Found);
        }
        if (!possible && capped_) return answer(PcStatus::Unknown);
        return answer(possible ? PcStatus::NeedMorePieces : PcStatus::Impossible);
    }
    result.lines = maxLines;
    return answer(PcStatus::Impossible);
}

PcResult PcSolver::solve(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes) { return run(query, cancel, maxNodes, false); }

PcResult PcSolver::solveWithUnseen(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes) { return run(query, cancel, maxNodes, true); }

}  // namespace ai
