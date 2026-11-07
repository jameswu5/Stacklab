// Self-checks for the perfect clear solver. Every answer it gives is played through
// core::Game, and "no" answers are compared with boards known to have a perfect clear and
// with a plain search that has none of the solver's shortcuts. `pc_test --bench` prints timings.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <thread>
#include <vector>

#include "ai/pc_solver.h"
#include "modes/modes.h"
#include "tests/move_driver.h"

namespace {

using ai::Move;
using ai::MoveSearch;
using ai::PcQuery;
using ai::PcResult;
using ai::PcSolver;
using ai::PcStatus;
using core::Piece;

constexpr Piece kPieces[] = {Piece::I, Piece::J, Piece::L, Piece::O, Piece::S, Piece::T, Piece::Z};

core::Ruleset noGravity() {
    core::Ruleset rules;
    rules.gravity = 0;
    return rules;
}

std::array<int, 4> cellKey(std::array<core::Cell, 4> cells) {
    std::array<int, 4> key;
    for (int i = 0; i < 4; i++) key[i] = cells[i].y * core::kBoardWidth + cells[i].x;
    std::sort(key.begin(), key.end());
    return key;
}

// Locks the cells, clears full rows, and says whether the board is now empty.
bool lockCells(core::Board& board, std::array<core::Cell, 4> cells, Piece piece) {
    for (const core::Cell& cell : cells) board.at(cell.x, cell.y) = piece;
    core::Board out;
    int write = 0;
    for (int y = 0; y < core::kBoardHeight; y++) {
        bool full = true;
        for (int x = 0; x < core::kBoardWidth; x++) full = full && board.at(x, y) != Piece::None;
        if (full) continue;
        for (int x = 0; x < core::kBoardWidth; x++) out.at(x, write) = board.at(x, y);
        write++;
    }
    board = out;
    return std::all_of(board.cells.begin(), board.cells.end(), [](Piece cell) { return cell == Piece::None; });
}

// A solution has to work in the real game, by real key presses.
void checkByPlaying(const PcQuery& query, const PcResult& result) {
    assert(result.status == PcStatus::Found);
    assert(!result.steps.empty());
    core::Handling handling;
    handling.sdf = 20;
    core::Game game(query.rules, handling, 1);
    game.board = query.board;
    game.hold = query.hold;
    game.holdUsed = query.holdUsed;
    game.queue = query.queue;
    game.queue.insert(game.queue.end(), result.assumed.begin(), result.assumed.end());
    if (query.queueEnds) game.queue.insert(game.queue.end(), game.rules.previews + 1, Piece::None);
    else for (int i = 0; i < 14; i++) game.queue.push_back(kPieces[i % 7]);  // nothing the answer may rely on
    game.respawn(query.current);
    for (size_t i = 0; i < result.steps.size(); i++) {
        const ai::PcStep& step = result.steps[i];
        Driver driver{game};
        if (step.hold) driver.tap(core::Action::Hold);
        if (game.falling.piece == Piece::None) {  // a drill out of queue gives the held piece back
            assert(query.queueEnds && game.hold == step.piece);
            game.hold = Piece::None;
            game.respawn(step.piece);
        }
        assert(game.falling.piece == step.piece);
        for (Move move : step.moves) {
            assert(!driver.locked);
            driver.move(move);
        }
        driver.hardDrop();
        assert(driver.locked);
        assert(driver.perfectClear == (i + 1 == result.steps.size()));
    }
}

// An exact cover of `height` rows by pieces, in no particular order.
struct Tile {
    Piece piece;
    std::array<core::Cell, 4> cells;
};

// `tries` limits the backtracking, since a bad early choice can take a very long time to undo.
bool coverRows(std::array<std::array<bool, 10>, 12>& filled, int height, std::mt19937& rng, std::vector<Tile>& tiles, int& tries) {
    if (--tries < 0) return false;
    int ex = -1, ey = -1;
    for (int y = 0; y < height && ex < 0; y++)
        for (int x = 0; x < 10 && ex < 0; x++)
            if (!filled[y][x]) ex = x, ey = y;
    if (ex < 0) return true;

    struct Option {
        Piece piece;
        int rot, block;
    };
    std::vector<Option> options;
    for (Piece piece : kPieces)
        for (int rot = 0; rot < 4; rot++)
            for (int block = 0; block < 4; block++) options.push_back({piece, rot, block});
    std::shuffle(options.begin(), options.end(), rng);
    for (const Option& option : options) {
        core::Falling shape;
        shape.piece = option.piece;
        shape.rot = option.rot;
        auto cells = shape.cells();
        const int dx = ex - cells[option.block].x, dy = ey - cells[option.block].y;
        bool fits = true;
        for (core::Cell& cell : cells) {
            cell.x += dx;
            cell.y += dy;
            fits = fits && cell.x >= 0 && cell.x < 10 && cell.y >= 0 && cell.y < height && !filled[cell.y][cell.x];
        }
        if (!fits) continue;
        for (const core::Cell& cell : cells) filled[cell.y][cell.x] = true;
        tiles.push_back({option.piece, cells});
        if (coverRows(filled, height, rng, tiles, tries)) return true;
        tiles.pop_back();
        for (const core::Cell& cell : cells) filled[cell.y][cell.x] = false;
    }
    return false;
}

// A game that really ends in a perfect clear: the boards along the way, and the piece placed on each.
struct Solvable {
    std::vector<core::Board> boards;
    std::vector<Piece> pieces;
    int midClears = 0;
};

// Tiles are placed in a random order that the move search can reach, on the board as it stands.
// Rows clearing partway move the later tiles down, and split ones can't be placed: those fail.
bool makeSolvable(int height, std::mt19937& rng, MoveSearch& search, Solvable& out) {
    std::array<std::array<bool, 10>, 12> filled{};
    std::vector<Tile> tiles;
    int tries = 2000;
    if (!coverRows(filled, height, rng, tiles, tries)) return false;

    out = {};
    core::Board board;
    std::vector<int> rowNow(height);
    for (int y = 0; y < height; y++) rowNow[y] = y;
    core::Ruleset rules = noGravity();
    std::shuffle(tiles.begin(), tiles.end(), rng);
    while (!tiles.empty()) {
        bool placed = false;
        for (size_t i = 0; i < tiles.size() && !placed; i++) {
            auto cells = tiles[i].cells;
            for (core::Cell& cell : cells) cell.y = rowNow[cell.y];
            search.run(board, core::spawnFalling(tiles[i].piece), rules);
            for (const ai::Placement& placement : search.placements) {
                if (cellKey(search.cells(placement)) != cellKey(cells)) continue;
                out.boards.push_back(board);
                out.pieces.push_back(tiles[i].piece);
                for (const core::Cell& cell : cells) board.at(cell.x, cell.y) = tiles[i].piece;
                // Rows that are now full, top down so the numbers stay right.
                for (int y = height - 1; y >= 0; y--) {
                    if (rowNow[y] < 0) continue;
                    bool full = true;
                    for (int x = 0; x < 10; x++) full = full && board.at(x, rowNow[y]) != Piece::None;
                    if (!full) continue;
                    const int gone = rowNow[y];
                    rowNow[y] = -1;
                    for (int& row : rowNow)
                        if (row > gone) row--;
                    for (int yy = gone; yy + 1 < core::kBoardHeight; yy++)
                        for (int x = 0; x < 10; x++) board.at(x, yy) = board.at(x, yy + 1);
                    if (tiles.size() > 1) out.midClears++;
                }
                tiles.erase(tiles.begin() + i);
                placed = true;
                break;
            }
        }
        if (!placed) return false;
    }
    return true;
}

PcQuery queryFrom(const Solvable& game, size_t from, size_t count) {
    PcQuery query;
    query.board = game.boards[from];
    query.current = game.pieces[from];
    query.queue.assign(game.pieces.begin() + from + 1, game.pieces.begin() + from + count);
    query.rules = noGravity();
    query.maxLines = ai::kPcMaxRows;
    return query;
}

// Boards known to have a perfect clear must get one, played out for real.
void testKnownSolutions() {
    std::mt19937 rng(1);
    PcSolver solver;
    MoveSearch search;
    int checked = 0, midClears = 0, usedHold = 0, tooBig = 0;
    for (int round = 0; round < 400; round++) {
        Solvable game;
        const int height = 2 + round % 5;
        if (!makeSolvable(height, rng, search, game)) continue;
        midClears += game.midClears;
        for (size_t count = 1; count <= 7 && count <= game.pieces.size(); count++) {
            const size_t from = game.pieces.size() - count;
            PcQuery query = queryFrom(game, from, count);
            PcResult result = solver.solve(query);
            if (result.status == PcStatus::Unknown) {
                tooBig++;
                continue;
            }
            assert(result.status == PcStatus::Found);
            checkByPlaying(query, result);
            checked++;

            // The same pieces with the first two swapped: hold gets them back in order.
            if (count < 2) continue;
            PcQuery swapped = query;
            std::swap(swapped.current, swapped.queue[0]);
            result = solver.solve(swapped);
            if (result.status == PcStatus::Unknown) {
                tooBig++;
                continue;
            }
            assert(result.status == PcStatus::Found);
            checkByPlaying(swapped, result);
            for (const ai::PcStep& step : result.steps) usedHold += step.hold;
            checked++;

            // And out of hold, with the current piece the one that goes second.
            PcQuery fromHold = query;
            fromHold.hold = query.current;
            fromHold.current = query.queue[0];
            fromHold.queue.erase(fromHold.queue.begin());
            result = solver.solve(fromHold);
            if (result.status == PcStatus::Unknown) {
                tooBig++;
                continue;
            }
            assert(result.status == PcStatus::Found);
            checkByPlaying(fromHold, result);
            checked++;
        }
    }
    std::printf("known solutions: %d found and played, %d rows cleared partway, %d holds, %d over budget\n", checked, midClears, usedHold, tooBig);
    assert(checked > 1000);
    assert(midClears > 100);
    assert(usedHold > 100);
}

// Sealed gaps at random: a board with a known perfect clear gets a row buried in it that an I has to
// clear first. That covers cells below it and splits the empty space, as real stacks do.
void testBuriedRows() {
    std::mt19937 rng(6);
    PcSolver solver;
    MoveSearch search;
    int checked = 0, sealed = 0;
    for (int round = 0; round < 600 && checked < 300; round++) {
        Solvable game;
        if (!makeSolvable(2 + round % 3, rng, search, game)) continue;
        const size_t count = 1 + rng() % std::min<size_t>(game.pieces.size(), 5);
        const size_t from = game.pieces.size() - count;
        PcQuery query = queryFrom(game, from, count);

        int height = 0;
        for (int y = 0; y < ai::kPcMaxRows; y++)
            for (int x = 0; x < 10; x++)
                if (query.board.at(x, y) != Piece::None) height = y + 1;
        const int row = rng() % (height + 1), gap = rng() % 7;
        core::Board buried;
        for (int y = 0; y < core::kBoardHeight; y++)
            for (int x = 0; x < 10; x++) {
                if (y < row) buried.at(x, y) = query.board.at(x, y);
                else if (y == row) buried.at(x, y) = x >= gap && x < gap + 4 ? Piece::None : Piece::Garbage;
                else buried.at(x, y) = query.board.at(x, y - 1);
            }
        search.run(buried, core::spawnFalling(Piece::I), query.rules);
        bool reachable = false;
        const std::array<core::Cell, 4> cells = {{{gap, row}, {gap + 1, row}, {gap + 2, row}, {gap + 3, row}}};
        for (const ai::Placement& placement : search.placements) reachable = reachable || cellKey(search.cells(placement)) == cellKey(cells);
        if (!reachable) continue;
        for (int x = 0; x < 10; x++)
            sealed += row > 0 && buried.at(x, row - 1) == Piece::None && buried.at(x, row) != Piece::None;

        query.queue.insert(query.queue.begin(), query.current);
        query.current = Piece::I;
        query.board = buried;
        PcResult result = solver.solve(query);
        assert(result.status == PcStatus::Found);
        checkByPlaying(query, result);
        checked++;
    }
    std::printf("buried rows: %d found and played, covering %d empty cells\n", checked, sealed);
    assert(checked >= 100 && sealed > 100);
}

// With no pruning or shortcuts: every placement of every order, from the real spawn.
bool plainSearch(const core::Board& board, Piece current, Piece hold, bool canHold, std::span<const Piece> queue, const core::Ruleset& rules, MoveSearch& search) {
    auto tryPiece = [&](Piece piece, Piece nextHold, std::span<const Piece> rest) {
        search.run(board, core::spawnFalling(piece), rules);
        std::vector<std::array<core::Cell, 4>> cells;
        for (const ai::Placement& placement : search.placements) cells.push_back(search.cells(placement));
        for (const auto& placed : cells) {
            core::Board after = board;
            if (lockCells(after, placed, piece)) return true;
            Piece next = rest.empty() ? Piece::None : rest[0];
            if (next == Piece::None && nextHold == Piece::None) continue;
            if (plainSearch(after, next, nextHold, rules.allowHold, rest.empty() ? rest : rest.subspan(1), rules, search)) return true;
        }
        return false;
    };
    if (current == Piece::None) return hold != Piece::None && tryPiece(hold, Piece::None, queue);
    if (tryPiece(current, hold, queue)) return true;
    if (!canHold) return false;
    if (hold != Piece::None) return hold != current && tryPiece(hold, current, queue);
    return !queue.empty() && tryPiece(queue[0], current, queue.subspan(1));
}

int cellCount(const core::Board& board) {
    int cells = 0;
    for (Piece cell : board.cells) cells += cell != Piece::None;
    return cells;
}

// The solver's prunes against the plain search, on boards with and without a perfect clear.
void testAgainstPlainSearch() {
    std::mt19937 rng(2);
    PcSolver solver;
    MoveSearch search, plain;
    int yes = 0, no = 0, withClears = 0;
    for (int round = 0; round < 600 && yes + no < 60; round++) {
        Solvable game;
        if (!makeSolvable(2 + round % 4, rng, search, game)) continue;
        const size_t count = 1 + round % 3;
        if (game.pieces.size() < count) continue;
        PcQuery query = queryFrom(game, game.pieces.size() - count, count);
        // Half the time another piece or a shifted cell, which usually spoils it.
        if (round % 4 == 0) query.current = kPieces[rng() % 7];
        if (round % 8 == 1 && cellCount(query.board) > 0) {
            for (int tries = 0; tries < 100; tries++) {
                core::Piece& cell = query.board.at(rng() % 10, rng() % 2);
                if (cell == Piece::None) continue;
                cell = Piece::None;
                query.board.at(rng() % 10, rng() % 2) = Piece::Garbage;
                break;
            }
        }
        query.hold = round % 5 == 0 ? kPieces[rng() % 7] : Piece::None;
        query.holdUsed = round % 7 == 0;
        withClears += game.midClears > 0;

        PcResult result = solver.solve(query);
        const bool exists = plainSearch(query.board, query.current, query.hold, !query.holdUsed, query.queue, query.rules, plain);
        if (exists) {
            assert(result.status == PcStatus::Found);
            checkByPlaying(query, result);
            yes++;
        } else {
            assert(result.status == PcStatus::NeedMorePieces || result.status == PcStatus::Impossible);
            no++;
        }
    }
    std::printf("against the plain search: %d with a perfect clear, %d without, %d from games with clears partway\n", yes, no, withClears);
    assert(yes > 15 && no > 15);
}

void testStatuses() {
    PcSolver solver;
    core::Ruleset rules = noGravity();

    // An odd cell count can never work, whatever comes.
    PcQuery odd;
    odd.board.at(0, 0) = Piece::Garbage;
    odd.current = Piece::T;
    odd.rules = rules;
    PcResult result = solver.solve(odd);
    assert(result.status == PcStatus::Impossible && result.lines == 0);

    // An empty board: 5 pieces could clear 2 lines, but these can't, and 4 lines is past what's known.
    PcQuery empty;
    empty.current = Piece::T;
    empty.queue = {Piece::S, Piece::Z, Piece::J, Piece::L, Piece::O, Piece::I};
    empty.rules = rules;
    result = solver.solve(empty);
    assert(result.status == PcStatus::NeedMorePieces);
    assert(result.lines == 4 && result.piecesNeeded == 10 && result.steps.empty());

    // The same 2 lines with the right pieces.
    PcQuery two;
    two.current = Piece::I;
    two.queue = {Piece::I, Piece::I, Piece::I, Piece::O};
    two.rules = rules;
    result = solver.solve(two);
    assert(result.status == PcStatus::Found && result.steps.size() == 5 && result.lines == 2);
    checkByPlaying(two, result);

    // A column that needs an odd amount, walled in by columns that are done. The two rows'
    // worth of cells leave a well 1 wide needing 2 cells: nothing fits it at 2 lines.
    PcQuery well;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 9; x++)
            if (x != 4) well.board.at(x, y) = Piece::Garbage;
    well.board.at(9, 1) = Piece::Garbage;
    well.board.at(9, 0) = Piece::None;
    well.current = Piece::I;
    well.queue = {Piece::O, Piece::T};
    well.rules = rules;

    // A drill's queue ends: too few pieces is then a no, not a wait.
    PcQuery ends = empty;
    ends.queueEnds = true;
    assert(solver.solve(ends).status == PcStatus::Impossible);

    // A drill's held piece comes back by itself, with no press.
    PcQuery heldBack;
    heldBack.current = Piece::I;
    heldBack.hold = Piece::I;
    heldBack.queue = {Piece::I, Piece::I, Piece::O};
    heldBack.queueEnds = true;
    heldBack.rules = rules;
    result = solver.solve(heldBack);
    assert(result.status == PcStatus::Found);
    checkByPlaying(heldBack, result);

    // Hold that's been used this piece can't be used for the first placement. Only the O in
    // hold finishes this, so it needs hold right away.
    PcQuery used;
    for (int x = 0; x < 8; x++) used.board.at(x, 0) = used.board.at(x, 1) = Piece::Garbage;
    used.current = Piece::T;
    used.hold = Piece::O;
    used.rules = rules;
    assert(solver.solve(used).status == PcStatus::Found);
    used.holdUsed = true;
    assert(solver.solve(used).status != PcStatus::Found);
    used.holdUsed = false;
    used.rules.allowHold = false;
    assert(solver.solve(used).status != PcStatus::Found);

    // A tuck: with hold off the T has to go first, and it can't drop into the slot past the overhang.
    PcQuery tuck;
    for (int x = 0; x < 10; x++) {
        if (x != 4) tuck.board.at(x, 0) = Piece::Garbage;
        if (x < 3 || x > 5) tuck.board.at(x, 1) = Piece::Garbage;
        if (x < 4 || x > 7) tuck.board.at(x, 2) = Piece::Garbage;
    }
    tuck.current = Piece::T;
    tuck.queue = {Piece::I};
    tuck.rules = rules;
    tuck.rules.allowHold = false;
    result = solver.solve(tuck);
    assert(result.status == PcStatus::Found && result.steps.size() == 2 && result.steps[0].piece == Piece::T);
    assert(std::any_of(result.steps[0].moves.begin(), result.steps[0].moves.end(), [](Move move) { return move == Move::SoftDrop || move == Move::Down; }));
    checkByPlaying(tuck, result);

    // The height limit: 4 I's and an O clear 2 lines, but not if 1 is all that's allowed.
    two.maxLines = 1;
    result = solver.solve(two);
    assert(result.status == PcStatus::Impossible && result.lines == 1);
    // The fewest lines come first. These pieces could clear 2 lines or go on to 4.
    two.maxLines = 4;
    result = solver.solve(two);
    assert(result.status == PcStatus::Found && result.lines == 2);
    // A stack higher than the limit is a no at that limit, and only that limit.
    PcQuery high;
    for (int y = 0; y < 5; y++) high.board.at(0, y) = Piece::Garbage;
    high.board.at(1, 0) = Piece::Garbage;
    high.current = Piece::I;
    high.rules = rules;
    result = solver.solve(high);
    assert(result.status == PcStatus::Impossible && result.lines == 4);
    high.maxLines = 6;
    assert(solver.solve(high).status == PcStatus::NeedMorePieces);

    // Row 0's gap is sealed off with room for only 2 cells, which no piece could fill. Clearing
    // row 1 first opens it. Checking empty regions for a multiple of 4 would wrongly say no.
    PcQuery sealed;
    for (int x = 0; x < 8; x++) sealed.board.at(x, 0) = Piece::Garbage;
    for (int x = 4; x < 10; x++) sealed.board.at(x, 1) = Piece::Garbage;
    sealed.current = Piece::I;
    sealed.queue = {Piece::O, Piece::I, Piece::I};
    sealed.rules = rules;
    result = solver.solve(sealed);
    assert(result.status == PcStatus::Found && result.lines == 3);
    checkByPlaying(sealed, result);

    // A stack over the rows it can hold is an honest don't know.
    PcQuery tall;
    tall.board.at(0, ai::kPcMaxRows) = Piece::Garbage;
    tall.board.at(1, ai::kPcMaxRows) = Piece::Garbage;
    tall.current = Piece::I;
    tall.rules = rules;
    assert(solver.solve(tall).status == PcStatus::Unknown);

    // Out of budget or cancelled is a don't know, never a no.
    assert(solver.solve(empty, nullptr, 1).status == PcStatus::Unknown);
    std::atomic<bool> cancel{true};
    assert(solver.solve(empty, &cancel).status == PcStatus::Unknown);
    assert(solver.solve(empty).status == PcStatus::NeedMorePieces);  // and nothing's left over from those
}

// What the player can see of a game, and no more: `seen` is every piece shown so far.
PcQuery visibleQuery(const core::Game& game, std::span<const Piece> seen) {
    PcQuery query;
    query.board = game.board;
    query.current = game.falling.piece;
    query.hold = game.hold;
    query.holdUsed = game.holdUsed;
    query.queue.assign(game.queue.begin(), game.queue.begin() + game.rules.previews);
    query.bagLeft = ai::pcBagLeft(seen);
    query.rules = game.rules;
    return query;
}

void testVisibleOnly() {
    core::Game game(core::Ruleset{}, core::Handling{}, 3);
    std::vector<Piece> seen = {game.falling.piece};
    seen.insert(seen.end(), game.queue.begin(), game.queue.begin() + game.rules.previews);
    const core::InputEvent drop[] = {{core::Action::HardDrop, true, 0}, {core::Action::HardDrop, false, 0}};
    // What the player works out about the bag is what's really left of it, hidden in the queue.
    for (int i = 0; i < 40; i++) {
        uint8_t hidden = 0;
        for (size_t q = game.rules.previews; q < game.queue.size(); q++) hidden |= 1 << static_cast<int>(game.queue[q]);
        assert(ai::pcBagLeft(seen) == hidden);
        game.tick(drop);
        game.board = {};
        seen.push_back(game.queue[game.rules.previews - 1]);
    }

    // Two games that look the same but go on differently get the same answers.
    PcSolver solver;
    for (uint32_t seed = 1; seed <= 5; seed++) {
        core::Game one(core::Ruleset{}, core::Handling{}, seed);
        core::Game two = one;
        for (size_t q = two.rules.previews; q < two.queue.size(); q++) two.queue[q] = kPieces[q % 7];
        two.queue.insert(two.queue.end(), {Piece::I, Piece::I, Piece::I, Piece::I});
        std::vector<Piece> shown = {one.falling.piece};
        shown.insert(shown.end(), one.queue.begin(), one.queue.begin() + one.rules.previews);
        const PcQuery a = visibleQuery(one, shown), b = visibleQuery(two, shown);
        for (const PcResult& result : {solver.solve(a), solver.solve(b)}) assert(result.status == PcStatus::NeedMorePieces && result.lines == 4 && result.steps.empty());
        const PcResult first = solver.solveWithUnseen(a, nullptr, 20'000), second = solver.solveWithUnseen(b, nullptr, 20'000);
        assert(first.status == second.status && first.nodes == second.nodes && first.assumed == second.assumed && first.steps.size() == second.steps.size());

        const ai::PcAdviceOptions few{.maxSamples = 4, .nodesPerSample = 300};
        const ai::PcAdvice adviceA = solver.advise(a, nullptr, few), adviceB = solver.advise(b, nullptr, few);
        assert(adviceA.hasStep && adviceB.hasStep && adviceA.samples == 4);
        assert(adviceA.step.piece == adviceB.step.piece && adviceA.step.hold == adviceB.step.hold && cellKey(adviceA.step.cells) == cellKey(adviceB.step.cells));
        assert(adviceA.chance == adviceB.chance && adviceA.unknownSamples == adviceB.unknownSamples);
    }
}

// Starting low in the air for speed must find exactly what starting at the spawn does, at the walls too.
void testAirStart() {
    std::mt19937 rng(5);
    core::Ruleset rules = noGravity();
    MoveSearch high, low;
    int checked = 0;
    for (int round = 0; round < 300; round++) {
        core::Board board;
        int top = 0;
        const int rows = 1 + round % ai::kPcMaxRows;
        for (int y = 0; y < rows; y++) {
            const int fill = 20 + rng() % 70;
            for (int x = 0; x < core::kBoardWidth; x++) {
                if (static_cast<int>(rng() % 100) >= fill) continue;
                board.at(x, y) = Piece::Garbage;
                top = y + 1;
            }
        }
        for (Piece piece : kPieces) {
            high.run(board, core::spawnFalling(piece), rules);
            core::Falling start = core::spawnFalling(piece);
            start.y = std::min(start.y, static_cast<double>(top + ai::kPcAirRows));
            start.highestY = static_cast<int>(start.y);
            low.run(board, start, rules);
            std::map<std::array<int, 4>, bool> fromHigh, fromLow;
            for (const ai::Placement& placement : high.placements) fromHigh[cellKey(high.cells(placement))] = true;
            for (const ai::Placement& placement : low.placements) fromLow[cellKey(low.cells(placement))] = true;
            assert(fromHigh == fromLow);
            checked++;
        }
    }
    std::printf("air start: %d searches found the same placements as the spawn\n", checked);
}

// Unseen pieces: a known shape with its last pieces left unknown has an order that works.
void testUnseenPieces() {
    std::mt19937 rng(3);
    PcSolver solver;
    MoveSearch search;
    int found = 0, played = 0;
    for (int round = 0; round < 200 && found < 30; round++) {
        Solvable game;
        if (!makeSolvable(2 + round % 3, rng, search, game)) continue;
        if (game.pieces.size() < 3) continue;
        // All but the last piece known, and the last one anywhere in the unseen bag.
        PcQuery query = queryFrom(game, 0, game.pieces.size() - 1);
        query.bagLeft = 0;
        PcResult known = solver.solve(query);
        if (known.status == PcStatus::Found) continue;
        assert(known.status == PcStatus::NeedMorePieces);
        PcResult result = solver.solveWithUnseen(query, nullptr, 2'000'000);
        if (result.status == PcStatus::Unknown) continue;
        assert(result.status == PcStatus::Found);
        assert(!result.assumed.empty());
        checkByPlaying(query, result);
        found++;
        played++;

        // Limited to a bag without that piece, it can't rely on it.
        query.bagLeft = static_cast<uint8_t>(0xFE & ~(1 << static_cast<int>(game.pieces.back())));
        result = solver.solveWithUnseen(query, nullptr, 2'000'000);
        if (result.status == PcStatus::Found) {
            assert(std::find(result.assumed.begin(), result.assumed.end(), game.pieces.back()) == result.assumed.end());
            checkByPlaying(query, result);
        }
    }
    std::printf("unseen pieces: %d orders found and played\n", played);
    assert(found >= 20);

    // With hold off, 2 rows 4 wide take an O and another O. The bag's last piece being an I rules that out.
    PcQuery almost;
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 6; x++) almost.board.at(x, y) = Piece::Garbage;
    almost.current = Piece::O;
    almost.bagLeft = 1 << static_cast<int>(Piece::I);
    almost.rules = noGravity();
    almost.rules.allowHold = false;
    assert(solver.solve(almost).status == PcStatus::NeedMorePieces);
    PcResult result = solver.solveWithUnseen(almost, nullptr, 2'000'000);
    assert(result.status == PcStatus::Impossible);
    almost.bagLeft = 1 << static_cast<int>(Piece::O);
    assert(solver.solveWithUnseen(almost, nullptr, 2'000'000).status == PcStatus::Found);
}

// Every order the unseen pieces can come in: the rest of this bag, then whole bags.
void allFutures(uint8_t bag, int length, std::vector<Piece>& pieces, std::vector<std::vector<Piece>>& out) {
    if (length == 0) {
        out.push_back(pieces);
        return;
    }
    const uint8_t from = bag ? bag : 0xFE;
    for (int piece = 1; piece <= 7; piece++) {
        if (!(from >> piece & 1)) continue;
        pieces.push_back(static_cast<Piece>(piece));
        allFutures(from & ~(1 << piece), length - 1, pieces, out);
        pieces.pop_back();
    }
}

// The exact share of futures with a perfect clear after this placement.
double exactChance(PcSolver& solver, const PcQuery& query, Piece piece, bool pressedHold, std::array<core::Cell, 4> cells, const std::vector<std::vector<Piece>>& futures) {
    PcQuery after = query;
    lockCells(after.board, cells, piece);
    after.holdUsed = false;
    after.queueEnds = true;
    if (pressedHold) {
        after.hold = query.current;
        if (query.hold == Piece::None) after.queue.erase(after.queue.begin());
    }
    int hits = 0;
    for (const std::vector<Piece>& future : futures) {
        PcQuery trial = after;
        trial.queue.insert(trial.queue.end(), future.begin(), future.end());
        trial.current = trial.queue.front();
        trial.queue.erase(trial.queue.begin());
        hits += solver.solve(trial, nullptr, 2'000'000).status == PcStatus::Found;
    }
    return static_cast<double>(hits) / futures.size();
}

void testAdvice() {
    std::mt19937 rng(9);
    PcSolver solver;
    MoveSearch search;

    // Nothing to place, or nothing to gain.
    PcQuery odd;
    odd.board.at(0, 0) = Piece::Garbage;
    odd.current = Piece::T;
    odd.rules = noGravity();
    const ai::PcAdvice none = solver.advise(odd);
    assert(none.status == PcStatus::Impossible && !none.hasStep && none.chance == 0 && none.samples == 0);

    // Provable from what's visible: that placement, at 100%, with no sampling.
    Solvable game;
    while (!makeSolvable(4, rng, search, game)) {}
    const PcQuery provable = queryFrom(game, 4, 6);
    const PcResult proof = solver.solve(provable);
    const ai::PcAdvice proven = solver.advise(provable);
    assert(proof.status == PcStatus::Found);
    assert(proven.status == PcStatus::Found && proven.hasStep && proven.chance == 1 && proven.samples == 0 && !proven.partial);
    assert(proven.step.piece == proof.steps[0].piece && cellKey(proven.step.cells) == cellKey(proof.steps[0].cells));

    // Stopped by either limit: the best so far, marked partial.
    core::Game opening(core::Ruleset{}, core::Handling{}, 12);
    std::vector<Piece> shown = {opening.falling.piece};
    shown.insert(shown.end(), opening.queue.begin(), opening.queue.begin() + opening.rules.previews);
    const PcQuery empty = visibleQuery(opening, shown);
    const ai::PcAdvice timedOut = solver.advise(empty, nullptr, {.timeBudgetMs = 30});
    assert(timedOut.status == PcStatus::NeedMorePieces && timedOut.hasStep && timedOut.partial);
    std::atomic<bool> cancel{false};
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cancel = true;
    });
    const ai::PcAdvice cancelled = solver.advise(empty, &cancel);
    canceller.join();
    assert(cancelled.hasStep && cancelled.partial);
    const ai::PcAdvice stopped = solver.advise(empty, &cancel);
    assert(!stopped.hasStep && stopped.partial);

    // The same state and seed give the same answer, and a whole run isn't partial.
    const ai::PcAdviceOptions few{.maxSamples = 4, .nodesPerSample = 300};
    const ai::PcAdvice one = solver.advise(empty, nullptr, few), two = solver.advise(empty, nullptr, few);
    assert(!one.partial && one.samples == 4 && one.hasStep && cellKey(one.step.cells) == cellKey(two.step.cells) && one.chance == two.chance);

    // Against a count of every future, on boards small enough to count. Each stays near the truth,
    // and what's chosen is about as good as the best choice there is.
    int cases = 0, spread = 0;
    double totalError = 0, totalRegret = 0, worstError = 0;
    for (int round = 0; round < 400 && cases < 40; round++) {
        Solvable small;
        if (!makeSolvable(4, rng, search, small)) continue;
        const size_t from = small.pieces.size() - 4;
        if (small.pieces[from + 2] == small.pieces[from + 3]) continue;
        PcQuery query = queryFrom(small, from, 2);
        query.maxLines = 4;
        uint8_t bag = 1 << static_cast<int>(small.pieces[from + 2]) | 1 << static_cast<int>(small.pieces[from + 3]);
        if (round % 2 == 0) bag |= 1 << static_cast<int>(kPieces[rng() % 7]);
        query.bagLeft = bag;

        // Two unseen pieces are needed, and one more can end up in hold.
        std::vector<std::vector<Piece>> futures;
        std::vector<Piece> pieces;
        allFutures(bag, 3, pieces, futures);

        const ai::PcAdvice advice = solver.advise(query, nullptr, {.maxSamples = 2048, .nodesPerSample = 2'000'000});
        if (advice.status != PcStatus::NeedMorePieces) continue;
        assert(advice.hasStep && advice.samples == 2048 && advice.unknownSamples == 0);
        const double exact = exactChance(solver, query, advice.step.piece, advice.step.hold, advice.step.cells, futures);
        const double sigma = std::sqrt(exact * (1 - exact) / advice.samples);
        const double error = std::abs(advice.chance - exact);
        assert(error <= 5 * sigma + 1e-9);

        double best = 0, worst = 1;
        const std::pair<Piece, bool> options[] = {{query.current, false}, {query.queue[0], true}};
        for (const auto& [piece, hold] : options) {
            search.run(query.board, core::spawnFalling(piece), query.rules);
            std::vector<std::array<core::Cell, 4>> placements;
            for (const ai::Placement& placement : search.placements) placements.push_back(search.cells(placement));
            for (const auto& cells : placements) {
                const double chance = exactChance(solver, query, piece, hold, cells, futures);
                best = std::max(best, chance);
                worst = std::min(worst, chance);
            }
        }
        totalError += error;
        worstError = std::max(worstError, error);
        totalRegret += best - exact;
        spread += best > worst;
        assert(exact <= best + 1e-9);
        cases++;
    }
    std::printf("advice against a count of every future: %d cases, %d with choices that differ, mean error %.3f, worst %.3f, mean shortfall from the best choice %.3f\n", cases, spread, totalError / cases, worstError, totalRegret / cases);
    assert(cases >= 20 && spread >= 15 && totalRegret / cases < 0.03);
}

// --bench
struct Times {
    std::vector<double> ms;
    std::map<PcStatus, int> statuses;
    int mostNodes = 0;

    void add(double value, const PcResult& result) {
        ms.push_back(value);
        statuses[result.status]++;
        mostNodes = std::max(mostNodes, result.nodes);
    }
    void print(const char* name) {
        std::sort(ms.begin(), ms.end());
        std::printf("%-46s n=%4zu median %8.2f ms  p95 %8.2f ms  worst %8.2f ms  nodes<=%7d  found %d, need more %d, impossible %d, unknown %d\n", name, ms.size(), ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back(), mostNodes, statuses[PcStatus::Found], statuses[PcStatus::NeedMorePieces], statuses[PcStatus::Impossible], statuses[PcStatus::Unknown]);
    }
};

template <typename Solve>
void timed(Times& times, Solve solve) {
    auto begin = std::chrono::steady_clock::now();
    PcResult result = solve();
    times.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count(), result);
}

// The pieces a run of the 40L mode really gets: its rules, its queue.
std::vector<Piece> fortyLinesPieces(uint32_t seed, int count) {
    core::Game game(kModes[0].rules, core::Handling{}, seed);
    std::vector<Piece> pieces;
    const core::InputEvent drop[] = {{core::Action::HardDrop, true, 0}, {core::Action::HardDrop, false, 0}};
    while (static_cast<int>(pieces.size()) < count) {
        pieces.push_back(game.falling.piece);
        game.tick(drop);
        game.board = {};
    }
    return pieces;
}

// From an empty board at each bag position, with every piece needed known: how often does a 4-line
// perfect clear exist? This is a ceiling, since a player sees 5 previews, not the whole way.
void benchPhases(int samples, int maxNodes) {
    PcSolver solver;
    std::printf("empty board, 4 lines, all pieces known: %d samples per phase, %d node budget\n", samples, maxNodes);
    for (bool carried : {false, true}) {
        std::printf("%s\n", carried ? "a random piece carried in hold:" : "hold empty:");
        for (int phase = 0; phase < 7; phase++) {
            int found = 0, none = 0, unknown = 0, two = 0;
            Times times;
            for (int sample = 0; sample < samples; sample++) {
                // Past the first bag, which no_szo changes.
                const std::vector<Piece> pieces = fortyLinesPieces(1000 + sample * 7 + phase, 7 + phase + 11);
                PcQuery query;
                query.current = pieces[7 + phase];
                query.queue.assign(pieces.begin() + 7 + phase + 1, pieces.begin() + 7 + phase + 10);
                if (carried) query.hold = kPieces[(sample * 3 + phase) % 7];
                query.rules = kModes[0].rules;
                query.queueEnds = true;
                query.maxLines = 4;
                PcResult result;
                timed(times, [&] { return result = solver.solve(query, nullptr, maxNodes); });
                if (result.status == PcStatus::Found) {
                    found++;
                    two += result.lines == 2;
                } else if (result.status == PcStatus::Unknown) {
                    unknown++;
                } else {
                    none++;
                }
            }
            std::sort(times.ms.begin(), times.ms.end());
            std::printf("  phase %d: %5.1f%% found (%d of them on 2 lines), %d none, %d unknown; median %.1f ms, worst %.1f ms\n", phase, 100.0 * found / samples, two, none, unknown, times.ms[samples / 2], times.ms.back());
        }
    }
}

void bench() {
    PcSolver solver;
    MoveSearch search;
    Times empty, emptyUnseen, opener, openerSpoiled, openerUnseen, stack, stackUnseen;
    std::mt19937 rng(4);

    for (int seed = 1; seed <= 200; seed++) {
        core::Game game(kModes[0].rules, core::Handling{}, seed);
        std::vector<Piece> shown = {game.falling.piece};
        shown.insert(shown.end(), game.queue.begin(), game.queue.begin() + game.rules.previews);
        PcQuery query = visibleQuery(game, shown);
        timed(empty, [&] { return solver.solve(query); });
        if (seed <= 40) timed(emptyUnseen, [&] { return solver.solveWithUnseen(query); });
    }

    // Mid-opener: 4 rows, some pieces down, up to 7 known. Spoiled ones get other pieces, which usually fails.
    for (int round = 0; round < 400; round++) {
        Solvable game;
        if (!makeSolvable(4, rng, search, game)) continue;
        const size_t placed = 3 + round % 5;
        if (game.pieces.size() <= placed) continue;
        const size_t count = std::min<size_t>(7, game.pieces.size() - placed);
        PcQuery query = queryFrom(game, placed, count);
        timed(opener, [&] { return solver.solve(query); });
        std::shuffle(query.queue.begin(), query.queue.end(), rng);
        query.current = kPieces[rng() % 7];
        for (Piece& piece : query.queue) piece = kPieces[rng() % 7];
        timed(openerSpoiled, [&] { return solver.solve(query); });
        if (round % 8 == 0) {
            query.queue.resize(std::max<size_t>(query.queue.size(), 1) - 1);
            timed(openerUnseen, [&] { return solver.solveWithUnseen(query); });
        }
    }

    // Awkward stacks: random placements, usually with holes, and a random 7 known.
    for (int round = 0; round < 300; round++) {
        core::Board board;
        const int pieces = 2 + round % 7;
        for (int i = 0; i < pieces; i++) {
            search.run(board, core::spawnFalling(kPieces[rng() % 7]), noGravity());
            const ai::Placement placement = search.placements[rng() % search.placements.size()];
            for (const core::Cell& cell : search.cells(placement)) board.at(cell.x, cell.y) = Piece::Garbage;
        }
        PcQuery query;
        query.board = board;
        query.current = kPieces[rng() % 7];
        for (int i = 0; i < 5; i++) query.queue.push_back(kPieces[rng() % 7]);
        query.hold = kPieces[rng() % 7];
        query.rules = noGravity();
        timed(stack, [&] { return solver.solve(query); });
        if (round % 6 == 0) timed(stackUnseen, [&] { return solver.solveWithUnseen(query); });
    }

    empty.print("empty board, 7 known: solve");
    emptyUnseen.print("empty board, 7 known: solveWithUnseen");
    opener.print("mid-opener, right pieces: solve");
    openerSpoiled.print("mid-opener, random pieces: solve");
    openerUnseen.print("mid-opener, random, one short: solveWithUnseen");
    stack.print("awkward stack, 7 known: solve");
    stackUnseen.print("awkward stack, 7 known: solveWithUnseen");
}

// Plays a whole attempt at a perfect clear, by real key presses, seeing only what a player sees.
struct Attempt {
    bool cleared = false;
    bool offPlan = false;  // a placement landed somewhere other than planned
    double firstChance = -1;
};

// `randomFirst` placements are random; the rest take the advice. All random if it's more than 10.
Attempt playAttempt(PcSolver& solver, MoveSearch& search, core::Ruleset rules, uint32_t seed, int phase, int randomFirst, const ai::PcAdviceOptions& options) {
    core::Handling handling;
    handling.sdf = 20;
    core::Game game(rules, handling, seed);
    std::mt19937 rng(seed);
    std::vector<Piece> seen = {game.falling.piece};
    seen.insert(seen.end(), game.queue.begin(), game.queue.begin() + game.rules.previews);
    auto reveal = [&](int count) {
        for (int i = 0; i < count; i++) seen.push_back(game.queue[game.rules.previews - count + i]);
    };
    // Past the first bag, which no_szo changes.
    const core::InputEvent drop[] = {{core::Action::HardDrop, true, 0}, {core::Action::HardDrop, false, 0}};
    for (int i = 0; i < 7 + phase; i++) {
        game.tick(drop);
        game.board = {};
        reveal(1);
    }

    Attempt attempt;
    for (int placed = 0; placed < 10; placed++) {
        PcQuery query = visibleQuery(game, seen);
        ai::PcStep step;
        if (placed < randomFirst) {
            search.run(query.board, core::spawnFalling(query.current), query.rules);
            if (search.placements.empty()) return attempt;
            const ai::Placement placement = search.placements[rng() % search.placements.size()];
            step = {query.current, false, search.cells(placement), search.inputs(placement)};
        } else {
            const ai::PcAdvice advice = solver.advise(query, nullptr, options);
            if (!advice.hasStep) return attempt;
            if (attempt.firstChance < 0) attempt.firstChance = advice.chance;
            step = advice.step;
        }
        core::Board expected = game.board;
        lockCells(expected, step.cells, step.piece);
        const bool drewFromQueue = step.hold && game.hold == Piece::None;
        Driver driver{game};
        if (step.hold) driver.tap(core::Action::Hold);
        for (Move move : step.moves) driver.move(move);
        driver.hardDrop();
        if (!driver.locked) return attempt;
        reveal(drewFromQueue ? 2 : 1);
        if (game.board.cells != expected.cells) {
            attempt.offPlan = true;
            return attempt;
        }
        if (driver.perfectClear) {
            attempt.cleared = true;
            return attempt;
        }
    }
    return attempt;
}

// The board and view of a player following the advice for a few placements.
PcQuery visibleAfter(PcSolver& solver, uint32_t seed, int placements) {
    core::Game game(kModes[0].rules, core::Handling{}, seed);
    std::vector<Piece> seen = {game.falling.piece};
    seen.insert(seen.end(), game.queue.begin(), game.queue.begin() + game.rules.previews);
    const core::InputEvent drop[] = {{core::Action::HardDrop, true, 0}, {core::Action::HardDrop, false, 0}};
    for (int i = 0; i < 7 + static_cast<int>(seed % 7); i++) {
        game.tick(drop);
        game.board = {};
        seen.push_back(game.queue[game.rules.previews - 1]);
    }
    for (int i = 0; i < placements; i++) {
        const ai::PcAdvice advice = solver.advise(visibleQuery(game, seen), nullptr, {.maxSamples = 16, .nodesPerSample = 5'000});
        if (!advice.hasStep) break;
        const bool drewFromQueue = advice.step.hold && game.hold == Piece::None;
        Driver driver{game};
        if (advice.step.hold) driver.tap(core::Action::Hold);
        for (Move move : advice.step.moves) driver.move(move);
        driver.hardDrop();
        for (int k = drewFromQueue ? 2 : 1; k > 0; k--) seen.push_back(game.queue[game.rules.previews - k]);
    }
    return visibleQuery(game, seen);
}

void benchAdviceTime(int samples) {
    PcSolver solver;
    std::printf("time for one answer, %d futures at most, one thread:\n", samples);
    for (int placements = 0; placements <= 3; placements++) {
        std::vector<double> ms;
        double chances = 0;
        int unknown = 0, total = 0, proven = 0;
        for (uint32_t seed = 1; seed <= 6; seed++) {
            const PcQuery query = visibleAfter(solver, seed, placements);
            auto begin = std::chrono::steady_clock::now();
            const ai::PcAdvice advice = solver.advise(query, nullptr, {.maxSamples = samples});
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
            chances += advice.chance;
            unknown += advice.unknownSamples;
            total += advice.samples;
            proven += advice.status == PcStatus::Found;
            std::printf("    seed %u: %.0f ms, %.0f%%, %d futures\n", seed, ms.back(), 100 * advice.chance, advice.samples);
            std::fflush(stdout);
        }
        std::sort(ms.begin(), ms.end());
        std::printf("  after %d placements: median %8.0f ms, worst %8.0f ms; mean chance %3.0f%%, %d proven of %zu, %d of %d futures out of nodes\n", placements, ms[ms.size() / 2], ms.back(), 100 * chances / ms.size(), proven, ms.size(), unknown, total);
        std::fflush(stdout);
    }

}

void benchAdviceStability() {
    PcSolver solver;
    std::printf("the top choice as futures grow, against 1024 futures, 8 boards each after 0 and 1 placements:\n");
    const int counts[] = {16, 32, 64, 128, 256, 512};
    int same[6] = {};
    double loss[6] = {};
    double ms[6] = {};
    int boards = 0;
    for (int placements = 0; placements <= 1; placements++) {
        for (uint32_t seed = 21; seed <= 28; seed++) {
            const PcQuery query = visibleAfter(solver, seed, placements);
            const ai::PcAdvice top = solver.advise(query, nullptr, {.maxSamples = 1024});
            if (!top.hasStep || top.status != PcStatus::NeedMorePieces) continue;
            boards++;
            // What each choice is worth, judged on other futures than it was chosen on.
            auto worth = [&](const ai::PcStep& step) {
                PcQuery after = query;
                lockCells(after.board, step.cells, step.piece);
                after.holdUsed = false;
                if (step.hold) {
                    after.hold = query.current;
                    if (query.hold == Piece::None) after.queue.erase(after.queue.begin());
                }
                after.queueEnds = true;
                std::mt19937 rng(777);
                int hits = 0;
                const int trials = 512;
                for (int t = 0; t < trials; t++) {
                    std::vector<Piece> future;
                    uint8_t bag = query.bagLeft ? query.bagLeft : 0xFE;
                    while (future.size() < 12) {
                        std::vector<Piece> shuffled;
                        for (int piece = 1; piece <= 7; piece++)
                            if (bag >> piece & 1) shuffled.push_back(static_cast<Piece>(piece));
                        std::shuffle(shuffled.begin(), shuffled.end(), rng);
                        future.insert(future.end(), shuffled.begin(), shuffled.end());
                        bag = 0xFE;
                    }
                    PcQuery trial = after;
                    trial.queue.insert(trial.queue.end(), future.begin(), future.end());
                    trial.current = trial.queue.front();
                    trial.queue.erase(trial.queue.begin());
                    hits += solver.solve(trial, nullptr, 20'000).status == PcStatus::Found;
                }
                return static_cast<double>(hits) / trials;
            };
            const double topWorth = worth(top.step);
            for (int i = 0; i < 6; i++) {
                auto begin = std::chrono::steady_clock::now();
                const ai::PcAdvice advice = solver.advise(query, nullptr, {.maxSamples = counts[i]});
                ms[i] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                const bool match = advice.step.piece == top.step.piece && cellKey(advice.step.cells) == cellKey(top.step.cells);
                same[i] += match;
                loss[i] += match ? 0 : topWorth - worth(advice.step);
            }
        }
    }
    for (int i = 0; i < 6; i++) std::printf("  %4d futures: same as 1024 on %d of %d, mean loss in worth %+.1f points, mean %.0f ms\n", counts[i], same[i], boards, 100 * loss[i] / boards, ms[i] / boards);
    std::fflush(stdout);

}

void benchAdvicePlay(int games, int samples) {
    PcSolver solver;
    MoveSearch search;
    const core::Ruleset rules = kModes[0].rules;
    std::printf("whole attempts from an empty board, %d each, %d futures an answer, 40L rules with real gravity and key presses:\n", games, samples);
    const ai::PcAdviceOptions options{.maxSamples = samples};
    for (int randomFirst : {0, 1, 2, 3, 11}) {
        int cleared = 0, offPlan = 0, answered = 0;
        double first = 0;
        auto begin = std::chrono::steady_clock::now();
        for (int game = 0; game < games; game++) {
            const Attempt attempt = playAttempt(solver, search, rules, 5000 + game, game % 7, randomFirst, options);
            cleared += attempt.cleared;
            offPlan += attempt.offPlan;
            if (attempt.firstChance >= 0) {
                first += attempt.firstChance;
                answered++;
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        std::printf("  %2d random placements first: %3d of %d cleared (%4.1f%%); mean first estimate %4.1f%%; %d off plan; %.1f s an attempt\n", randomFirst, cleared, games, 100.0 * cleared / games, answered ? 100 * first / answered : 0.0, offPlan, seconds / games);
        std::fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--advise") == 0) {
        const char* part = argc > 2 ? argv[2] : "";
        const int samples = argc > 3 ? std::atoi(argv[3]) : 64;
        if (std::strcmp(part, "time") == 0) benchAdviceTime(samples);
        if (std::strcmp(part, "stable") == 0) benchAdviceStability();
        if (std::strcmp(part, "play") == 0) benchAdvicePlay(argc > 4 ? std::atoi(argv[4]) : 100, samples);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) {
        bench();
        benchPhases(argc > 2 ? std::atoi(argv[2]) : 50, argc > 3 ? std::atoi(argv[3]) : 500'000);
        return 0;
    }
    auto stage = [&](const char* name, void (*test)()) {
        if (argc > 2 && std::strcmp(argv[2], name) != 0) return;
        auto begin = std::chrono::steady_clock::now();
        test();
        std::printf("%s %.1f s\n", name, std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
        std::fflush(stdout);
    };
    stage("statuses", testStatuses);
    stage("visible", testVisibleOnly);
    stage("air", testAirStart);
    stage("known", testKnownSolutions);
    stage("buried", testBuriedRows);
    stage("plain", testAgainstPlainSearch);
    stage("unseen", testUnseenPieces);
    stage("advice", testAdvice);
    return 0;
}
