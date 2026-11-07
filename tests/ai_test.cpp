// Self-checks that the move search agrees with core::Game, not just with itself.
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "ai/finesse.h"
#include "ai/move_search.h"

namespace {

using ai::Move;
using ai::MoveSearch;
using core::Action;
using core::Piece;
using core::Spin;

constexpr Piece kPieces[] = {Piece::I, Piece::J, Piece::L, Piece::O, Piece::S, Piece::T, Piece::Z};

int rowOf(const core::Game& game) { return static_cast<int>(std::floor(game.falling.y)); }

// The piece's cells as a value that ignores which rotation made them.
std::array<int, 4> cellKey(std::array<core::Cell, 4> cells) {
    std::array<int, 4> key;
    for (int i = 0; i < 4; i++) key[i] = cells[i].y * core::kBoardWidth + cells[i].x;
    std::sort(key.begin(), key.end());
    return key;
}

core::Board withPlaced(core::Board board, std::array<core::Cell, 4> cells, Piece piece) {
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
    return out;
}

// Runs ticks in core until the piece locks or the ticks run out.
struct Driver {
    core::Game& game;
    ai::Finesse* finesse = nullptr;
    bool locked = false;
    Spin spin = Spin::None;

    void tick(std::initializer_list<core::InputEvent> inputs) {
        if (locked) return;
        std::span span(inputs.begin(), inputs.size());
        const auto& locks = finesse ? finesse->tick(game, span) : game.tick(span);
        if (locks.empty()) return;
        locked = true;
        spin = locks[0].spin;
    }

    void tap(Action action) { tick({{action, true, 0}, {action, false, 0}}); }

    void move(Move move) {
        switch (move) {
            case Move::Left: tap(Action::Left); break;
            case Move::Right: tap(Action::Right); break;
            case Move::RotateCW: tap(Action::RotateCW); break;
            case Move::RotateCCW: tap(Action::RotateCCW); break;
            case Move::Rotate180: tap(Action::Rotate180); break;
            case Move::DasLeft:
            case Move::DasRight: {
                Action action = move == Move::DasLeft ? Action::Left : Action::Right;
                tick({{action, true, 0}});
                for (int i = 0; i < game.handling.das + 1; i++) tick({});
                tick({{action, false, 0}});
                break;
            }
            case Move::SoftDrop: {
                tick({{Action::SoftDrop, true, 0}});
                for (int last = -1; !locked && last != rowOf(game);) {
                    last = rowOf(game);
                    tick({});
                }
                tick({{Action::SoftDrop, false, 0}});
                break;
            }
            case Move::Down:
                tick({{Action::SoftDrop, true, 0}});
                tick({{Action::SoftDrop, false, 0}});
                break;
        }
    }

    void hardDrop() { tick({{Action::HardDrop, true, 0}, {Action::HardDrop, false, 0}}); }
};

core::Game gameOn(const core::Board& board, Piece piece, const core::Ruleset& rules, const core::Handling& handling) {
    core::Game game(rules, handling, 1);
    game.board = board;
    game.respawn(piece);
    return game;
}

core::Board messyBoard(std::mt19937& rng) {
    core::Board board;
    int top = 2 + rng() % 13;
    int fill = 45 + rng() % 50;
    for (int x = 0; x < core::kBoardWidth; x++) {
        int height = rng() % top;
        for (int y = 0; y < height; y++)
            if (static_cast<int>(rng() % 100) < fill) board.at(x, y) = Piece::Garbage;
    }
    return board;
}

// A slot for a T-spin double, a roof to tuck under, and a shaft with a side cave.
std::vector<core::Board> handBoards() {
    std::vector<core::Board> boards;
    core::Board tsd;
    for (int x = 0; x < 10; x++) {
        if (x != 4) tsd.at(x, 0) = Piece::Garbage;
        if (x < 3 || x > 5) tsd.at(x, 1) = Piece::Garbage;
    }
    tsd.at(3, 2) = Piece::Garbage;
    boards.push_back(tsd);

    core::Board roof;
    for (int x = 0; x < 4; x++) roof.at(x, 1) = Piece::Garbage;
    boards.push_back(roof);

    core::Board cave;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 10; x++)
            if (x < 6 && y != 5 && y != 6) cave.at(x, y) = Piece::Garbage;
    boards.push_back(cave);
    return boards;
}

// The search must report what core does for this exact key sequence.
void checkReproduced(const core::Board& board, Piece piece, const core::Ruleset& rules, const core::Handling& handling, MoveSearch& search, int& checked) {
    core::Game start = gameOn(board, piece, rules, handling);
    search.run(start.board, start.falling, rules);
    assert(!search.placements.empty());
    for (const ai::Placement& placement : search.placements) {
        auto moves = search.inputs(placement);
        assert(static_cast<int>(moves.size()) == placement.cost);
        core::Game game = start;
        Driver driver{game};
        for (size_t i = 0; i < moves.size(); i++) {
            assert(!driver.locked);
            driver.move(moves[i]);
        }
        driver.hardDrop();
        assert(driver.locked);
        assert(driver.spin == placement.spin);
        assert(game.board.cells == withPlaced(board, search.cells(placement), piece).cells);
        checked++;
    }
}

void testEveryPlacementReproduces() {
    core::Ruleset slowFall;
    slowFall.gravity = 0;  // core only falls when told to, as in the search
    core::Handling sdf20;
    sdf20.sdf = 20;
    core::Handling das3;
    das3.das = 3;
    das3.sdf = 20;
    core::Ruleset fewResets = slowFall;
    fewResets.lockResets = 2;  // so that running out of resets gets exercised
    core::Ruleset tSpinsOnly = slowFall;
    tSpinsOnly.spinBonuses = core::SpinBonuses::TSpins;
    tSpinsOnly.allow180 = false;

    std::mt19937 rng(1);
    std::vector<core::Board> boards = handBoards();
    for (int i = 0; i < 300; i++) boards.push_back(messyBoard(rng));

    MoveSearch search;
    int checked = 0;
    for (const core::Board& board : boards) {
        for (Piece piece : kPieces) {
            checkReproduced(board, piece, slowFall, sdf20, search, checked);
            checkReproduced(board, piece, fewResets, das3, search, checked);
            checkReproduced(board, piece, tSpinsOnly, sdf20, search, checked);
        }
    }
    std::printf("reproduced %d placements through core\n", checked);
}

// With no way to run out of resets, the placements within a few presses should
// be exactly what trying every key sequence in core finds, at the same cost.
using Found = std::map<std::pair<std::array<int, 4>, Spin>, int>;

void tryEverySequence(const core::Game& game, int depth, int maxDepth, Found& found) {
    core::Game dropped = game;
    auto cells = dropped.ghost().cells();
    Driver dropper{dropped};
    dropper.hardDrop();
    assert(dropper.locked);
    auto key = std::make_pair(cellKey(cells), dropper.spin);
    auto it = found.find(key);
    if (it == found.end() || it->second > depth) found[key] = depth;
    if (depth == maxDepth) return;
    for (Move move : {Move::Left, Move::Right, Move::DasLeft, Move::DasRight, Move::RotateCW, Move::RotateCCW, Move::Rotate180, Move::SoftDrop, Move::Down}) {
        core::Game next = game;
        Driver driver{next};
        driver.move(move);
        assert(!driver.locked);
        tryEverySequence(next, depth + 1, maxDepth, found);
    }
}

void testMinimumsMatchBruteForce() {
    core::Ruleset rules;
    rules.gravity = 0;
    rules.lockResets = 1000;
    core::Handling handling;
    handling.sdf = 20;
    constexpr int kDepth = 3;

    std::mt19937 rng(2);
    std::vector<core::Board> boards = handBoards();
    for (int i = 0; i < 4; i++) boards.push_back(messyBoard(rng));

    MoveSearch search;
    for (const core::Board& board : boards) {
        for (Piece piece : kPieces) {
            core::Game game = gameOn(board, piece, rules, handling);
            Found brute;
            tryEverySequence(game, 0, kDepth, brute);

            search.run(game.board, game.falling, rules);
            Found searched;
            for (const ai::Placement& placement : search.placements) {
                if (placement.cost > kDepth) continue;
                auto key = std::make_pair(cellKey(search.cells(placement)), placement.spin);
                auto it = searched.find(key);
                if (it == searched.end() || it->second > placement.cost) searched[key] = placement.cost;
            }
            assert(searched == brute);
        }
    }
}

// Random keys at random times, under real gravity and lock delay: wherever a piece
// locks, the search from its spawn must have offered that placement.
void testRealPlayIsFound() {
    core::Handling das10;
    das10.das = 10;
    das10.arr = 2;
    das10.sdf = 20;
    int locks = 0, spins = 0, overCount = 0;
    MoveSearch search;
    for (const core::Handling& handling : {core::Handling{}, das10}) {
        std::mt19937 rng(3);
        for (int seed = 1; seed <= 40; seed++) {
            core::Game game(core::Ruleset{}, handling, seed);
            search.run(game.board, game.falling, game.rules);
            std::array<bool, 8> down{};
            int presses = 0;
            for (int tick = 0; tick < 40000 && !game.toppedOut; tick++) {
                // Mostly nothing, so the piece gets time to fall between keys.
                core::InputEvent input{Action::Left, true, 0};
                bool any = rng() % 4 == 0;
                if (any) {
                    int action = rng() % 8;
                    if (action == static_cast<int>(Action::HardDrop) && rng() % 8) action = rng() % 3;
                    input.action = static_cast<Action>(action);
                    input.pressed = !down[action];
                    down[action] = input.pressed;
                    if (input.pressed && input.action != Action::Hold && input.action != Action::HardDrop) presses++;
                }
                int heldBefore = game.holdUsed;
                bool shifting = down[0] || down[1];
                bool isHardDrop = any && input.action == Action::HardDrop && input.pressed;
                auto cells = cellKey(game.ghost().cells());
                const auto& result = game.tick(std::span(&input, any ? 1 : 0));
                if (!result.empty()) {
                    // Only ticks where the piece can't have moved before it locked.
                    if (isHardDrop || (!any && !shifting)) {
                        bool found = false;
                        for (const ai::Placement& placement : search.placements) {
                            if (placement.spin != result[0].spin || cellKey(search.cells(placement)) != cells) continue;
                            found = true;
                            if (placement.cost > presses) overCount++;
                            break;
                        }
                        assert(found);
                        locks++;
                        spins += result[0].spin != Spin::None;
                    }
                    presses = 0;
                    search.run(game.board, game.falling, game.rules);
                } else if (heldBefore != game.holdUsed) {
                    presses = 0;
                    search.run(game.board, game.falling, game.rules);
                }
            }
        }
    }
    std::printf("found %d placements from real play, %d with spins; %d cost more than the presses used\n", locks, spins, overCount);
    assert(locks > 1000);
    assert(spins > 0);
}

void testHandCases() {
    core::Ruleset rules;
    core::Handling handling;
    auto boards = handBoards();
    MoveSearch search;

    auto has = [&](std::array<core::Cell, 4> cells, Spin spin) {
        for (const ai::Placement& placement : search.placements)
            if (placement.spin == spin && cellKey(search.cells(placement)) == cellKey(cells)) return placement.cost;
        return -1;
    };

    // The T-spin double slot, reached by rotating in.
    core::Game tsd = gameOn(boards[0], Piece::T, rules, handling);
    search.run(tsd.board, tsd.falling, rules);
    assert(has({{{3, 1}, {4, 1}, {5, 1}, {4, 0}}}, Spin::Normal) > 0);
    // A plain hard drop can't get there.
    assert(has({{{3, 1}, {4, 1}, {5, 1}, {4, 0}}}, Spin::None) == -1);

    // A flat I under the roof: right, soft drop, then slide to the wall.
    core::Game roof = gameOn(boards[1], Piece::I, rules, handling);
    search.run(roof.board, roof.falling, rules);
    assert(has({{{0, 0}, {1, 0}, {2, 0}, {3, 0}}}, Spin::None) == 3);

    // A side cave needs the piece to stop falling partway down the shaft, and can
    // be reached by nothing but a row at a time.
    core::Game cave = gameOn(boards[2], Piece::I, rules, handling);
    search.run(cave.board, cave.falling, rules);
    assert(has({{{2, 5}, {3, 5}, {4, 5}, {5, 5}}}, Spin::None) > 0);

    // No piece, as when a drill's queue runs out: nowhere to place, and after a real search too.
    search.run(core::Board{}, core::Falling{}, rules);
    assert(search.placements.empty());

    // No 180 means no 180 in any path.
    core::Ruleset no180;
    no180.allow180 = false;
    core::Game flat = gameOn(core::Board{}, Piece::T, no180, handling);
    search.run(flat.board, flat.falling, no180);
    for (const ai::Placement& placement : search.placements)
        for (Move move : search.inputs(placement)) assert(move != Move::Rotate180);
    // Rotated twice flat on an empty floor, then: two presses instead of one.
    assert(has({{{4, 1}, {3, 1}, {5, 1}, {4, 0}}}, Spin::None) == 2);
}

using ai::Finesse;

// Taps through Finesse, as the app will, and returns what the last lock was judged.
struct Player {
    core::Game game;
    Finesse finesse{game};
    Driver driver{game, &finesse};

    Player(const core::Board& board, Piece piece, const core::Ruleset& rules, const core::Handling& handling = {}) : game(gameOn(board, piece, rules, handling)) {}

    void play(std::initializer_list<Move> moves) {
        for (Move move : moves) driver.move(move);
    }
    void press(std::initializer_list<core::InputEvent> inputs) { finesse.tick(game, std::span(inputs.begin(), inputs.size())); }
    void tap(Action action) { press({{action, true, 0}, {action, false, 0}}); }
    void wait(int ticks) {
        for (int i = 0; i < ticks; i++) press({});
    }
    std::optional<ai::Judgement> drop() {
        tap(Action::HardDrop);
        return finesse.judged;
    }
};

void checkJudged(const std::optional<ai::Judgement>& judged, int presses, int minimum, int faults) {
    assert(judged);
    assert(judged->presses == presses);
    assert(judged->minimum == minimum);
    assert(judged->faults == faults);
}

void testFaultCounting() {
    core::Ruleset rules;
    rules.gravity = 0;
    core::Handling handling;
    handling.sdf = 20;

    Player straight(core::Board{}, Piece::T, rules, handling);
    checkJudged(straight.drop(), 0, 0, 0);

    Player wiggle(core::Board{}, Piece::T, rules, handling);
    wiggle.play({Move::Left, Move::Right, Move::Left});
    checkJudged(wiggle.drop(), 3, 1, 2);

    // Tapping to the wall when DAS does it in one press.
    Player taps(core::Board{}, Piece::T, rules, handling);
    taps.play({Move::Left, Move::Left, Move::Left});
    checkJudged(taps.drop(), 3, 1, 2);
    Player das(core::Board{}, Piece::T, rules, handling);
    das.play({Move::DasLeft});
    checkJudged(das.drop(), 1, 1, 0);

    // A soft drop that a hard drop made pointless is a wasted press.
    Player pointless(core::Board{}, Piece::T, rules, handling);
    pointless.play({Move::SoftDrop});
    checkJudged(pointless.drop(), 1, 0, 1);

    // 180 is one press when the rules have it, and two rotations are then a fault.
    Player twice(core::Board{}, Piece::T, rules, handling);
    twice.play({Move::RotateCW, Move::RotateCW});
    checkJudged(twice.drop(), 2, 1, 1);
    core::Ruleset no180 = rules;
    no180.allow180 = false;
    Player noKey(core::Board{}, Piece::T, no180, handling);
    noKey.play({Move::RotateCW, Move::RotateCW});
    checkJudged(noKey.drop(), 2, 2, 0);

    // The same cells from a symmetric rotation are the same placement, so turning
    // a Z upside down to land where it would have anyway is a fault.
    Player zFlat(core::Board{}, Piece::Z, rules, handling);
    zFlat.play({Move::Rotate180});
    checkJudged(zFlat.drop(), 1, 0, 1);

    // A tuck is judged by the search: nothing cheaper exists, so no fault.
    Player tuck(handBoards()[1], Piece::I, rules, handling);
    tuck.play({Move::Right, Move::SoftDrop, Move::DasLeft});
    checkJudged(tuck.drop(), 3, 3, 0);
}

// Every path the search reports costs exactly its presses, and the cheapest path
// to a placement is never a fault.
void testSearchPathsAreClean() {
    core::Ruleset rules;
    rules.gravity = 0;
    core::Handling handling;
    handling.sdf = 20;
    std::mt19937 rng(7);
    std::vector<core::Board> boards = handBoards();
    for (int i = 0; i < 30; i++) boards.push_back(messyBoard(rng));

    MoveSearch search;
    int judged = 0, spinPairs = 0;
    for (const core::Board& board : boards) {
        for (Piece piece : kPieces) {
            core::Game start = gameOn(board, piece, rules, handling);
            search.run(start.board, start.falling, rules);
            std::map<std::pair<std::array<int, 4>, Spin>, int> cheapest;
            for (const ai::Placement& placement : search.placements) {
                Player player(board, piece, rules, handling);
                for (Move move : search.inputs(placement)) player.driver.move(move);
                auto result = player.drop();
                assert(result);
                assert(result->presses == placement.cost);
                assert(result->faults == placement.cost - result->minimum);
                auto key = std::make_pair(cellKey(search.cells(placement)), placement.spin);
                if (cheapest.emplace(key, placement.cost).second) assert(result->faults == 0);
                for (const ai::Placement& other : search.placements)
                    if (other.spin != placement.spin && other.cost != placement.cost && cellKey(search.cells(other)) == key.first) spinPairs++;
                judged++;
            }
        }
    }
    std::printf("judged %d search paths, %d with a differently priced spin to the same cells\n", judged, spinPairs);
}

// Gravity carries a piece for free, where the search charges a press for each way down.
void testFaultsNeverGoNegative() {
    core::Ruleset rules;
    rules.gravity = 0.5;
    core::Handling handling;
    handling.sdf = 20;
    Player tuck(handBoards()[1], Piece::I, rules, handling);
    tuck.play({Move::Right});
    tuck.wait(60);  // onto the floor, level with the space under the roof
    assert(std::floor(tuck.game.falling.y) == 1);
    tuck.play({Move::DasLeft});
    checkJudged(tuck.drop(), 2, 3, 0);
}

void testNotFoundIsNoJudgement() {
    core::Ruleset rules;
    rules.gravity = 0;
    Player player(core::Board{}, Piece::T, rules);
    // The search ran on an empty board, and this floor makes the lock one it never saw.
    for (int x = 0; x < 9; x++) player.game.board.at(x, 0) = Piece::Garbage;
    assert(!player.drop());
    // The next piece is judged as usual.
    checkJudged(player.drop(), 0, 0, 0);
}

void testHoldRestartsTheCount() {
    core::Ruleset rules;
    rules.gravity = 0;

    // Presses on the held piece are forgotten, and hold itself never counts.
    Player player(core::Board{}, Piece::T, rules);
    Piece next = player.game.queue.front();
    player.play({Move::Left, Move::Left});
    player.tap(Action::Hold);
    assert(player.game.falling.piece == next);
    assert(!player.finesse.judged);
    player.play({Move::Right});
    // Hold can't be used twice: this one does nothing and restarts nothing.
    player.tap(Action::Hold);
    auto judged = player.drop();
    assert(judged && judged->piece == next);
    assert(judged->presses == 1 && judged->minimum == 1 && judged->faults == 0);

    // A piece that came from hold is judged from the spawn, with hold free again.
    player.tap(Action::Hold);
    assert(player.game.falling.piece == Piece::T);
    judged = player.drop();
    assert(judged && judged->piece == Piece::T);
    checkJudged(judged, 0, 0, 0);

    // A key held since before the run isn't a press for this piece.
    Player carried(core::Board{}, Piece::T, rules);
    carried.press({{Action::Left, true, 0, true}});
    carried.press({{Action::Left, false, 0}});
    auto carriedJudged = carried.drop();
    assert(carriedJudged && carriedJudged->presses == 0 && carriedJudged->faults == 0);

    core::Ruleset noHold = rules;
    noHold.allowHold = false;
    Player held(core::Board{}, Piece::T, noHold);
    held.play({Move::Left});
    held.tap(Action::Hold);
    checkJudged(held.drop(), 1, 1, 0);
}

// Several key events can land in one tick, on either side of a lock.
void testBusyTicks() {
    core::Ruleset rules;
    rules.gravity = 0;
    using core::InputEvent;

    // A key right after a hard drop belongs to the next piece.
    Player roll(core::Board{}, Piece::T, rules);
    roll.press({{Action::Left, true, 0}, {Action::Left, false, 0}, {Action::HardDrop, true, 0.2}, {Action::HardDrop, false, 0.2}, {Action::Right, true, 0.4}, {Action::Right, false, 0.4}});
    checkJudged(roll.finesse.judged, 1, 1, 0);
    checkJudged(roll.drop(), 1, 1, 0);

    // Two locks in a tick: the board between them is gone, so neither is judged.
    Player both(core::Board{}, Piece::T, rules);
    both.press({{Action::HardDrop, true, 0}, {Action::HardDrop, false, 0}, {Action::HardDrop, true, 0.5}, {Action::HardDrop, false, 0.5}});
    assert(!both.finesse.judged);
    checkJudged(both.drop(), 0, 0, 0);

    // Hold then hard drop in a tick: the search is still the held piece's, so nothing matches.
    Player swap(core::Board{}, Piece::T, rules);
    swap.press({{Action::Hold, true, 0}, {Action::Hold, false, 0}, {Action::HardDrop, true, 0.5}, {Action::HardDrop, false, 0.5}});
    assert(!swap.finesse.judged);
    checkJudged(swap.drop(), 0, 0, 0);

    // Hard drop then hold in a tick: the first piece is judged and the count restarts.
    Player drop(core::Board{}, Piece::T, rules);
    drop.press({{Action::HardDrop, true, 0}, {Action::HardDrop, false, 0}, {Action::Hold, true, 0.5}, {Action::Hold, false, 0.5}, {Action::Left, true, 0.7}, {Action::Left, false, 0.7}});
    checkJudged(drop.finesse.judged, 0, 0, 0);
    checkJudged(drop.drop(), 1, 1, 0);

    // A hard drop that safelock ignores ends nothing, so the count carries on.
    core::Ruleset quick;
    quick.gravity = 1;
    quick.lockTime = 1;
    core::Handling safelock;
    safelock.safelock = true;
    Player safe(core::Board{}, Piece::T, quick, safelock);
    while (safe.game.stats.pieces == 0) safe.wait(1);
    safe.tap(Action::Left);
    safe.tap(Action::HardDrop);
    assert(safe.game.stats.pieces == 1);
    safe.wait(7);
    safe.tap(Action::Right);
    checkJudged(safe.drop(), 2, 0, 2);

    // Natural lock on a quiet tick is judged. With a key in the same tick there's
    // no telling which piece the key was for, so neither piece is judged.
    core::Ruleset falling;
    falling.gravity = 20;
    falling.lockTime = 1;
    Player quiet(core::Board{}, Piece::T, falling);
    bool judged = false;
    for (int i = 0; i < 10 && !judged; i++) {
        quiet.wait(1);
        judged = quiet.finesse.judged.has_value();
    }
    assert(judged);
    assert(quiet.finesse.judged->presses == 0);

    Player busy(core::Board{}, Piece::T, falling);
    int pieces = busy.game.stats.pieces;
    while (busy.game.stats.pieces == pieces) {
        busy.press({{Action::RotateCW, true, 0.5}, {Action::RotateCW, false, 0.5}});
        assert(!busy.finesse.judged);
    }
    assert(!busy.drop());
    checkJudged(busy.drop(), 0, 0, 0);
}

// Random play through Finesse under real gravity and lock delay.
void testRealPlayIsJudged() {
    int judgedCount = 0, unjudged = 0, faults = 0;
    std::mt19937 rng(11);
    for (int seed = 1; seed <= 20; seed++) {
        core::Game game(core::Ruleset{}, core::Handling{}, seed);
        Finesse finesse(game);
        std::array<bool, 8> down{};
        for (int tick = 0; tick < 20000 && !game.toppedOut; tick++) {
            core::InputEvent input{Action::Left, true, 0};
            bool any = rng() % 4 == 0;
            if (any) {
                int action = rng() % 8;
                if (action == static_cast<int>(Action::HardDrop) && rng() % 8) action = rng() % 3;
                input.action = static_cast<Action>(action);
                input.pressed = !down[action];
                down[action] = input.pressed;
            }
            const auto& locks = finesse.tick(game, std::span(&input, any ? 1 : 0));
            if (locks.empty()) {
                assert(!finesse.judged);
            } else if (finesse.judged) {
                assert(finesse.judged->piece == locks[0].piece);
                assert(finesse.judged->minimum >= 0);
                assert(finesse.judged->faults == std::max(0, finesse.judged->presses - finesse.judged->minimum));
                judgedCount++;
                faults += finesse.judged->faults;
            } else {
                unjudged++;
            }
        }
    }
    std::printf("judged %d locks from random play with %d faults; %d not judged\n", judgedCount, faults, unjudged);
    assert(judgedCount > 500);
    assert(faults > 0);
}

void reportSpeed() {
    std::mt19937 rng(5);
    std::vector<core::Board> boards;
    for (int i = 0; i < 64; i++) boards.push_back(messyBoard(rng));
    MoveSearch search;
    core::Ruleset rules;
    core::Falling start;
    start.x = 3;
    start.y = 22.04;
    start.highestY = 22;

    constexpr int kRuns = 70000;
    size_t placements = 0;
    auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < kRuns; i++) {
        start.piece = kPieces[i % 7];
        start.x = start.piece == Piece::O ? 4 : 3;
        search.run(boards[i % boards.size()], start, rules);
        placements += search.placements.size();
    }
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    std::printf("search: %.1f us per piece on messy boards, %.0f placements\n", ns / 1e3 / kRuns, static_cast<double>(placements) / kRuns);
}

}  // namespace

int main() {
    testHandCases();
    testEveryPlacementReproduces();
    testMinimumsMatchBruteForce();
    testRealPlayIsFound();
    testFaultCounting();
    testSearchPathsAreClean();
    testFaultsNeverGoNegative();
    testNotFoundIsNoJudgement();
    testHoldRestartsTheCount();
    testBusyTicks();
    testRealPlayIsJudged();
    reportSpeed();
    return 0;
}
