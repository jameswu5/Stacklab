// Self-checks for core/. Expected values were worked out by hand from Triangle.js's
// source and from the published Park-Miller check value, not by executing Triangle.
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <string>
#include <vector>

#include "core/game.h"

using namespace core;

static int allocations = 0;

void* operator new(std::size_t size) {
    allocations++;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

static void run(Game& g, int ticks) {
    for (int i = 0; i < ticks; i++) g.tick({});
}

static void press(Game& g, Action a, double subframe = 0) {
    const InputEvent e[] = {{a, true, subframe}};
    g.tick(e);
}

// Taps a key within one tick and returns that tick's locks.
static const std::vector<LockResult>& tap(Game& g, Action a) {
    const InputEvent e[] = {{a, true, 0}, {a, false, 0}};
    return g.tick(e);
}

static void place(Game& g, Piece piece, int rot, int x, double y) {
    g.falling.piece = piece;
    g.falling.rot = rot;
    g.falling.x = x;
    g.falling.y = y;
}

// Fills rows [0, rows) except the listed cells.
static void fillExcept(Game& g, int rows, std::initializer_list<Cell> empty) {
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < kBoardWidth; x++) g.board.at(x, y) = Piece::Garbage;
    }
    for (const Cell& c : empty) g.board.at(c.x, c.y) = Piece::None;
}

static char letter(Piece p) { return " IJLOSTZ"[static_cast<int>(p)]; }

static void testRng() {
    Rng r(1);
    assert(r.next() == 16807);
    assert(r.next() == 282475249);
    assert(r.next() == 1622650073);

    // The check value from Park and Miller's paper.
    Rng check(1);
    uint32_t v = 0;
    for (int i = 0; i < 10000; i++) v = check.next();
    assert(v == 1043618065);

    assert(Rng(1).value == 1);
    assert(Rng(0).value == 2147483646);
    assert(Rng(2147483647).value == 2147483646);

    Rng f(1);
    assert(f.nextFloat() == 16806.0 / 2147483646.0);
}

static void testPieceOrder() {
    const struct {
        uint32_t seed;
        const char* order;
    } cases[] = {
        {1, "OJILSTZTOLJSIZLISOTZJ"},
        {12345, "LOSTIJZLOZTISJOSTZJIL"},
        {2147483646, "ZSIOLJTOSIZJLTOSZTIJL"},
    };
    for (const auto& c : cases) {
        Game g(Ruleset{}, Handling{}, c.seed);
        std::string seen(1, letter(g.falling.piece));
        for (int i = 0; i < 20; i++) {
            assert(static_cast<int>(g.queue.size()) >= g.rules.previews);
            press(g, Action::HardDrop);
            g.board = Board{};
            seen += letter(g.falling.piece);
        }
        assert(seen == c.order);
    }
}

static void testNoSzo() {
    Ruleset rules;
    rules.noSzo = true;
    const struct {
        uint32_t seed;
        const char* order;
    } cases[] = {
        {1, "JILSTZOTOLJSIZLISOTZJ"},           // O rotates to the back of its bag
        {12345, "LOSTIJZLOZTISJOSTZJIL"},       // already legal, so unchanged
        {2147483646, "IOLJTZSOSIZJLTOSZTIJL"},  // Z and S rotate back, later bags untouched
    };
    for (const auto& c : cases) {
        Game g(rules, Handling{}, c.seed);
        std::string seen(1, letter(g.falling.piece));
        for (int i = 0; i < 20; i++) {
            press(g, Action::HardDrop);
            g.board = Board{};
            seen += letter(g.falling.piece);
        }
        assert(seen == c.order);
    }
}

static void testSpawnAndGhost() {
    Game g(Ruleset{}, Handling{}, 1);
    assert(g.falling.piece == Piece::O);
    assert(g.falling.x == 4);
    assert(near(g.falling.y, 22.04));
    assert(g.ghost().cells()[0].y == 1 || g.ghost().cells()[0].y == 0);
    assert(static_cast<int>(g.ghost().y) == 1);

    press(g, Action::HardDrop);
    assert(g.board.at(4, 0) == Piece::O && g.board.at(5, 1) == Piece::O);
    assert(g.stats.pieces == 1);
    assert(g.falling.piece == Piece::J && g.falling.x == 3);
    assert(static_cast<int>(g.ghost().y) == 3);  // rests on the O

    Game i(Ruleset{}, Handling{}, 12345);
    while (i.falling.piece != Piece::I) tap(i, Action::Hold), press(i, Action::HardDrop);
    assert(i.falling.x == 3);
}

static void testDasArr() {
    Handling h;
    h.das = 6;
    h.arr = 0;
    Game g(Ruleset{}, h, 1);
    press(g, Action::Left);
    assert(g.falling.x == 3);
    run(g, 4);
    assert(g.falling.x == 3);
    run(g, 1);
    assert(g.falling.x == 0);

    h.arr = 2;
    Game a(Ruleset{}, h, 1);
    press(a, Action::Left);
    run(a, 4);
    assert(a.falling.x == 3);
    run(a, 1);
    assert(a.falling.x == 2);
    run(a, 1);
    assert(a.falling.x == 2);
    run(a, 1);
    assert(a.falling.x == 1);
    run(a, 2);
    assert(a.falling.x == 0);

    // Inputs land on their subframe: half a tick of gravity, then half of DAS.
    h.arr = 0;
    Game s(Ruleset{}, h, 1);
    press(s, Action::Left, 0.5);
    assert(near(s.falling.y, 22.02));
    assert(s.falling.x == 3);
    run(s, 5);
    assert(s.falling.x == 3);
    run(s, 1);
    assert(s.falling.x == 0);

    // A hoisted key starts charged.
    Game hoisted(Ruleset{}, h, 1);
    const InputEvent e[] = {{Action::Left, true, 0, true}};
    hoisted.tick(e);
    assert(hoisted.falling.x == 0);
}

static void testDcd() {
    Handling h;
    h.das = 6;
    h.arr = 0;
    h.dcd = 2;
    Game g(Ruleset{}, h, 1);
    press(g, Action::Right);
    run(g, 5);
    assert(g.falling.x == 8);
    press(g, Action::HardDrop);
    assert(g.falling.piece == Piece::J && g.falling.x == 3);
    run(g, 1);
    assert(g.falling.x == 7);

    h.dcd = 0;
    Game n(Ruleset{}, h, 1);
    press(n, Action::Right);
    run(n, 5);
    press(n, Action::HardDrop);
    assert(n.falling.x == 7);
}

static void testGravityAndSoftDrop() {
    Game g(Ruleset{}, Handling{}, 1);
    run(g, 1);
    assert(near(g.falling.y, 22.02));

    Ruleset rising;
    rising.gravityIncrease = 0.6;
    Game r(rising, Handling{}, 1);
    run(r, 2);
    assert(near(r.falling.y, 21.99));

    Handling h;
    h.sdf = 6;
    Game s(Ruleset{}, h, 1);
    press(s, Action::SoftDrop);
    assert(near(s.falling.y, 21.74));

    Game instant(Ruleset{}, Handling{}, 1);
    press(instant, Action::SoftDrop);
    assert(static_cast<int>(std::floor(instant.falling.y)) == 1);
}

static void testLockDelay() {
    Game g(Ruleset{}, Handling{}, 1);
    press(g, Action::SoftDrop);
    run(g, 29);
    assert(g.stats.pieces == 0);
    run(g, 1);
    assert(g.stats.pieces == 1);

    // A move on the ground restarts the delay.
    Game m(Ruleset{}, Handling{}, 1);
    press(m, Action::SoftDrop);
    run(m, 8);
    tap(m, Action::Left);
    run(m, 29);
    assert(m.stats.pieces == 0);
    run(m, 1);
    assert(m.stats.pieces == 1);

    // The 15th reset locks at the end of its tick.
    Game l(Ruleset{}, Handling{}, 1);
    press(l, Action::SoftDrop);
    for (int i = 0; i < 14; i++) tap(l, i % 2 ? Action::Right : Action::Left);
    assert(l.stats.pieces == 0);
    assert(tap(l, Action::Left).size() == 1);
    assert(l.stats.pieces == 1);
}

static void testKicks() {
    // T against the left wall: 1->2 takes the first kick, (+1, 0).
    Game t(Ruleset{}, Handling{}, 1);
    place(t, Piece::T, 1, -1, 10.5);
    press(t, Action::RotateCW);
    assert(t.falling.x == 0 && t.falling.rot == 2);

    // 180 on the floor: 0->2 kicks up one row.
    Game f(Ruleset{}, Handling{}, 1);
    place(f, Piece::T, 0, 4, 1.5);
    press(f, Action::Rotate180);
    assert(f.falling.rot == 2 && f.falling.x == 4);
    assert(std::floor(f.falling.y) == 2);

    // I against the left wall: 1->2 skips (-1, 0) and takes (+2, 0).
    Game i(Ruleset{}, Handling{}, 1);
    place(i, Piece::I, 1, -2, 10.5);
    press(i, Action::RotateCW);
    assert(i.falling.x == 0 && i.falling.rot == 2);

    // A blocked rotation changes nothing.
    Game b(Ruleset{}, Handling{}, 1);
    place(b, Piece::T, 0, 5, 4.5);
    fillExcept(b, 6, {{6, 4}, {5, 3}, {6, 3}, {7, 3}});
    press(b, Action::RotateCW);
    assert(b.falling.rot == 0 && b.falling.x == 5);
}

static void testSpins() {
    // T-spin double: rotating into a slot with three corners, two in front.
    Game d(Ruleset{}, Handling{}, 1);
    fillExcept(d, 2, {{1, 0}, {0, 1}, {1, 1}, {2, 1}});
    d.board.at(0, 2) = Piece::Garbage;
    place(d, Piece::T, 1, 0, 2.5);
    {
        const InputEvent e[] = {{Action::RotateCW, true, 0}, {Action::HardDrop, true, 0}};
        const auto& locks = d.tick(e);
        assert(locks.size() == 1);
        assert(locks[0].piece == Piece::T);
        assert(locks[0].spin == Spin::Normal);
        assert(locks[0].lines == 2);
        assert(locks[0].sends[3] == 4);
        assert(locks[0].b2b == 0 && locks[0].combo == 0);
    }
    assert(d.board.at(0, 0) == Piece::Garbage);

    // The 0->1 (-1, +2) kick is promoted from mini to a full spin.
    Game fin(Ruleset{}, Handling{}, 1);
    fillExcept(fin, 6, {{6, 4}, {5, 3}, {6, 3}, {7, 3}, {6, 1}, {5, 2}, {5, 1}, {5, 0}, {6, 0}, {0, 5}});
    place(fin, Piece::T, 0, 5, 4.5);
    {
        const InputEvent e[] = {{Action::RotateCW, true, 0}, {Action::HardDrop, true, 0}};
        const auto& locks = fin.tick(e);
        assert(locks.size() == 1);
        assert(locks[0].spin == Spin::Normal);
        assert(locks[0].lines == 2);
    }

    // An immobile J counts as a mini under all-mini+, and not under T-spins.
    const auto immobileJ = [](SpinBonuses bonuses) {
        Ruleset rules;
        rules.spinBonuses = bonuses;
        Game g(rules, Handling{}, 1);
        fillExcept(g, 8, {{4, 5}, {4, 4}, {5, 4}, {6, 4}, {6, 5}, {5, 5}, {5, 3}, {0, 0}, {0, 1}, {0, 2}, {0, 6}, {0, 7}});
        place(g, Piece::J, 0, 4, 5.5);
        const InputEvent e[] = {{Action::RotateCW, true, 0}, {Action::HardDrop, true, 0}};
        const LockResult result = g.tick(e)[0];
        assert(result.lines == 1);
        return result.spin;
    };
    assert(immobileJ(SpinBonuses::AllMiniPlus) == Spin::Mini);
    assert(immobileJ(SpinBonuses::TSpins) == Spin::None);
}

static void testAttackValue() {
    const Ruleset r;
    assert(attackValue(0, Spin::None, 0, 0, r) == 0);
    assert(attackValue(1, Spin::None, 0, 0, r) == 0);
    assert(attackValue(2, Spin::None, 0, 0, r) == 1);
    assert(attackValue(3, Spin::None, 0, 0, r) == 2);
    assert(attackValue(4, Spin::None, 0, 0, r) == 4);
    assert(attackValue(5, Spin::None, 0, 0, r) == 5);
    assert(attackValue(7, Spin::None, 0, 0, r) == 7);
    assert(attackValue(1, Spin::Mini, 0, 0, r) == 0);
    assert(attackValue(2, Spin::Mini, 0, 0, r) == 1);
    assert(attackValue(3, Spin::Mini, 0, 0, r) == 2);
    assert(attackValue(4, Spin::Mini, 0, 0, r) == 10);
    assert(attackValue(1, Spin::Normal, 0, 0, r) == 2);
    assert(attackValue(2, Spin::Normal, 0, 0, r) == 4);
    assert(attackValue(3, Spin::Normal, 0, 0, r) == 6);
    assert(attackValue(4, Spin::Normal, 0, 0, r) == 10);
    assert(attackValue(5, Spin::Normal, 0, 0, r) == 12);
    assert(attackValue(7, Spin::Normal, 0, 0, r) == 16);

    // B2B: chain 1 adds a flat 1, chain 2 adds the logarithmic part as well.
    assert(attackValue(4, Spin::None, 1, 0, r) == 5);
    assert(near(attackValue(4, Spin::None, 2, 0, r), 5.651837148342478));
    assert(attackValue(0, Spin::None, 5, 0, r) == 0);
    Ruleset flat;
    flat.b2bChaining = false;
    assert(attackValue(4, Spin::None, 9, 0, flat) == 5);

    // Combo: a multiplier, then a floor for long combos.
    assert(attackValue(2, Spin::None, 0, 1, r) == 1.25);
    assert(attackValue(2, Spin::None, 0, 2, r) == 1.5);
    assert(near(attackValue(1, Spin::None, 0, 2, r), 1.252762968495368));
}

// Clears four lines with a vertical I in column 0.
static LockResult quad(Game& g, int extraRows = 0) {
    for (int y = 0; y < 4; y++) {
        for (int x = 1; x < kBoardWidth; x++) g.board.at(x, y) = Piece::Garbage;
    }
    for (int y = 4; y < 4 + extraRows; y++) g.board.at(1, y) = Piece::Garbage;
    place(g, Piece::I, 1, -2, 10.5);
    return tap(g, Action::HardDrop)[0];
}

static void testSends() {
    // B2B and combo counters.
    Game g(Ruleset{}, Handling{}, 1);
    assert(g.stats.b2b == -1 && g.stats.combo == -1);
    LockResult first = quad(g, 1);
    assert(first.lines == 4 && !first.perfectClear);
    assert(first.b2b == 0 && first.combo == 0 && first.sends[3] == 4);
    LockResult second = quad(g, 1);
    assert(second.b2b == 1 && second.combo == 1);
    assert(second.sends[3] == 6);  // (4 + 1) * 1.25, rounded down
    assert(g.stats.attack == 10 && g.stats.lines == 8 && g.stats.pieces == 2);

    place(g, Piece::T, 0, 4, 10.5);
    tap(g, Action::HardDrop);
    assert(g.stats.combo == -1 && g.stats.b2b == 1);

    // Surge: a plain clear breaks a chain of 5, charged at 4 with base 3.
    Ruleset surge;
    surge.b2bCharging = true;
    Game s(surge, Handling{}, 1);
    s.stats.b2b = 5;
    fillExcept(s, 1, {{0, 0}, {1, 0}});
    place(s, Piece::O, 0, 0, 10.5);
    const LockResult broke = tap(s, Action::HardDrop)[0];
    assert(broke.lines == 1 && broke.b2b == -1);
    assert(broke.sends[0] == 2 && broke.sends[1] == 2 && broke.sends[2] == 1);
    assert(broke.sends[3] == 0 && broke.sends[4] == 0);
    assert(s.stats.attack == 5);

    // Below the charge threshold nothing surges.
    Game low(surge, Handling{}, 1);
    low.stats.b2b = 3;
    fillExcept(low, 1, {{0, 0}, {1, 0}});
    place(low, Piece::O, 0, 0, 10.5);
    const LockResult small = tap(low, Action::HardDrop)[0];
    assert(small.sends[0] == 0 && small.sends[2] == 0);

    // Perfect clear: appended after the normal attack, with an optional B2B bonus.
    Ruleset pc;
    pc.pcGarbage = 5;
    pc.pcB2b = 2;
    Game p(pc, Handling{}, 1);
    const LockResult clear = quad(p);
    assert(clear.perfectClear);
    assert(clear.b2b == 1);  // -1 + 2, no normal increment on top
    assert(clear.sends[3] == 5 && clear.sends[4] == 5);  // the new b2b already counts for this attack

    // Multiplier applies to the attack before rounding.
    Ruleset mult;
    mult.garbageMultiplier = 1.5;
    Game m(mult, Handling{}, 1);
    assert(quad(m, 1).sends[3] == 6);

    // Random rounding draws from its own RNG. Seed 1's first float is tiny, so 5.65 rounds up.
    Ruleset rounding;
    rounding.roundDown = false;
    Game up(rounding, Handling{}, 1);
    up.stats.b2b = 1;
    const uint32_t bagBefore = up.rng.value;
    assert(quad(up, 1).sends[3] == 6);
    assert(up.rng.value == bagBefore);

    // Special bonus needs a spin or quad that removed garbage.
    Ruleset bonus;
    bonus.garbageSpecialBonus = true;
    Game b(bonus, Handling{}, 1);
    assert(quad(b, 1).sends[3] == 5);
}

static void testLevels() {
    Ruleset rules;
    rules.levels = true;
    rules.levelSpeed = 0.42;
    rules.levelGBase = 0.65;

    // Levels 1, 2 and 3 need ceil(2.1) = 3, ceil(4.2) = 5 and ceil(6.3) = 7 lines.
    Game g(rules, Handling{}, 1);
    assert(g.stats.level == 1);
    run(g, 1);
    assert(near(g.falling.y, 22.02));  // level 1 keeps rules.gravity

    quad(g, 1);
    assert(g.stats.level == 2 && g.stats.levelLines == 1);
    const double y = g.falling.y;  // that tick already fell once, at the new gravity
    assert(std::abs(y - (22.04 - 1 / (60 * (0.65 - 0.007)))) < 1e-6);

    quad(g, 1);
    assert(g.stats.level == 3 && g.stats.levelLines == 0);
    assert(std::abs(g.falling.y - (22.04 - 1 / (60 * std::pow(0.65 - 2 * 0.007, 2)))) < 1e-6);

    // One big clear can cross several levels.
    Game big(rules, Handling{}, 1);
    big.stats.levelLines = 8;
    quad(big, 1);
    assert(big.stats.level == 3 && big.stats.levelLines == 4);

    // Off by default: gravity and the counters stay put.
    Game off(Ruleset{}, Handling{}, 1);
    quad(off, 1);
    assert(off.stats.level == 1 && off.stats.levelLines == 0);
    assert(near(off.falling.y, 22.02));
}

static void testScore() {
    // Soft drop is 1 per row, including a sink at 20G: 21 down, then 5 off the ledge.
    Game s(Ruleset{}, Handling{}, 1);
    press(s, Action::SoftDrop);
    assert(s.stats.score == 21);
    for (int y = 0; y < 5; y++) s.board.at(3, y) = Piece::Garbage;
    place(s, Piece::O, 0, 3, 6.5);
    tap(s, Action::Right);
    assert(s.falling.x == 4 && s.stats.score == 26);

    // A quad drops 7 rows at 2 each. The second has B2B on the clear but not the combo.
    Game g(Ruleset{}, Handling{}, 1);
    quad(g, 1);
    assert(g.stats.score == 800 + 14);
    quad(g, 1);
    assert(g.stats.score == 814 + 1200 + 50 + 14);

    // A T-spin double with no drop.
    Game t(Ruleset{}, Handling{}, 1);
    fillExcept(t, 2, {{1, 0}, {0, 1}, {1, 1}, {2, 1}});
    t.board.at(0, 2) = Piece::Garbage;
    place(t, Piece::T, 1, 0, 2.5);
    const InputEvent e[] = {{Action::RotateCW, true, 0}, {Action::HardDrop, true, 0}};
    t.tick(e);
    assert(t.stats.score == 1200);

    // A perfect clear adds its bonus, and level is the one after this clear's lines.
    Game pc(Ruleset{}, Handling{}, 1);
    quad(pc);
    assert(pc.stats.score == 800 + 3500 + 14);

    // A plain single that perfect clears takes the B2B bonus when pcB2b keeps the chain
    // going above 0, and not otherwise. A flat I drops 9 rows.
    const auto pcSingle = [](int pcB2b, int b2b) {
        Ruleset allClear;
        allClear.pcB2b = pcB2b;
        Game g(allClear, Handling{}, 1);
        g.stats.b2b = b2b;
        fillExcept(g, 1, {{0, 0}, {1, 0}, {2, 0}, {3, 0}});
        place(g, Piece::I, 0, 0, 10.5);
        assert(tap(g, Action::HardDrop)[0].perfectClear);
        return g.stats.score;
    };
    assert(pcSingle(1, 0) == 150 + 3500 + 18);
    assert(pcSingle(1, -1) == 100 + 3500 + 18);
    assert(pcSingle(0, 2) == 100 + 3500 + 18);

    Ruleset rules;
    rules.levels = true;
    rules.levelSpeed = 0.42;
    Game level(rules, Handling{}, 1);
    quad(level, 1);
    assert(level.stats.level == 2 && level.stats.score == 800 * 2 + 14);
}

static void testFillGarbage() {
    const auto holeIn = [](const Game& g, int y) {
        int hole = -1;
        for (int x = 0; x < kBoardWidth; x++) {
            if (g.board.at(x, y) != Piece::None) continue;
            assert(hole == -1);
            hole = x;
        }
        assert(hole != -1);
        return hole;
    };

    // A hole never repeats the row below's, and every column gets used.
    Game moving(Ruleset{}, Handling{}, 5);
    moving.fillGarbage(kVisibleHeight);
    assert(moving.board.at(0, kVisibleHeight) == Piece::None);
    for (const Cell& c : moving.falling.cells()) assert(moving.board.at(c.x, c.y) == Piece::None);
    std::array<bool, kBoardWidth> used{};
    for (uint32_t seed = 1; seed <= 200; seed++) {
        Game g(Ruleset{}, Handling{}, seed);
        g.fillGarbage(kVisibleHeight);
        for (int y = 0; y < kVisibleHeight; y++) {
            used[holeIn(g, y)] = true;
            if (y > 0) assert(holeIn(g, y) != holeIn(g, y - 1));
        }
    }
    for (bool column : used) assert(column);

    // The seed decides the board, and the bag is what it would have been anyway.
    Game copy(Ruleset{}, Handling{}, 5);
    copy.fillGarbage(kVisibleHeight);
    assert(copy.board.cells == moving.board.cells);
    Game other(Ruleset{}, Handling{}, 6);
    other.fillGarbage(kVisibleHeight);
    assert(other.board.cells != moving.board.cells);
    assert(moving.queue == Game(Ruleset{}, Handling{}, 5).queue);

    // Only rows holding garbage count toward a dig goal.
    Game dig(Ruleset{}, Handling{}, 5);
    dig.fillGarbage(2);
    dig.board.at(holeIn(dig, 1), 1) = Piece::Garbage;  // lines the holes up, for the I
    dig.board.at(holeIn(dig, 0), 1) = Piece::None;
    place(dig, Piece::I, 1, holeIn(dig, 0) - 2, 10.5);
    const int digLocks = tap(dig, Action::HardDrop)[0].garbageCleared;
    assert(dig.stats.lines == 2 && dig.stats.garbageCleared == 2);
    assert(digLocks == 2);
    for (int x = 0; x < kBoardWidth; x++) dig.board.at(x, 8) = Piece::J;
    place(dig, Piece::O, 0, 0, 12.5);
    tap(dig, Action::HardDrop);
    assert(dig.stats.lines == 3 && dig.stats.garbageCleared == 2);
}

static void testRespawn() {
    Game g(Ruleset{}, Handling{}, 1);
    g.queue[0] = Piece::T;
    g.queue.resize(1);  // the bag tops the edited queue back up behind the T
    g.board.at(4, 21) = Piece::Garbage;
    g.respawn(Piece::I);
    assert(g.toppedOut && g.falling.piece == Piece::I);

    g.board = Board{};
    g.respawn(Piece::I);
    assert(!g.toppedOut && g.falling.x == 3 && near(g.falling.y, 22.04));
    tap(g, Action::HardDrop);
    assert(g.falling.piece == Piece::T);
    assert(static_cast<int>(g.queue.size()) >= g.rules.previews);
}

static void testHold() {
    Game g(Ruleset{}, Handling{}, 1);
    tap(g, Action::Hold);
    assert(g.hold == Piece::O && g.falling.piece == Piece::J && g.holdUsed);
    tap(g, Action::Hold);
    assert(g.hold == Piece::O && g.falling.piece == Piece::J);
    tap(g, Action::HardDrop);
    assert(!g.holdUsed && g.falling.piece == Piece::I);
    tap(g, Action::Hold);
    assert(g.hold == Piece::I && g.falling.piece == Piece::O && g.falling.x == 4);

    Ruleset off;
    off.allowHold = false;
    Game n(off, Handling{}, 1);
    tap(n, Action::Hold);
    assert(n.hold == Piece::None && n.falling.piece == Piece::O);
}

static void testIrsIhs() {
    Handling h;
    h.irsHold = true;
    Game r(Ruleset{}, h, 1);
    {
        const InputEvent e[] = {{Action::RotateCW, true, 0}, {Action::HardDrop, true, 0}};
        r.tick(e);
    }
    assert(r.falling.piece == Piece::J && r.falling.rot == 1);

    // Tap mode has nothing to buffer without ARE.
    Game tapIrs(Ruleset{}, Handling{}, 1);
    {
        const InputEvent e[] = {{Action::RotateCW, true, 0}, {Action::HardDrop, true, 0}};
        tapIrs.tick(e);
    }
    assert(tapIrs.falling.rot == 0);

    Handling ih;
    ih.ihsHold = true;
    Game i(Ruleset{}, ih, 1);
    {
        const InputEvent e[] = {{Action::Hold, true, 0}};
        i.tick(e);
    }
    assert(i.hold == Piece::O && i.falling.piece == Piece::J);
    press(i, Action::HardDrop);
    assert(i.hold == Piece::I && i.falling.piece == Piece::O);  // the spawning I swaps with the held O
    assert(i.holdUsed);
}

static void testSafelock() {
    Handling h;
    h.safelock = true;
    Game g(Ruleset{}, h, 1);
    press(g, Action::SoftDrop);
    run(g, 30);
    assert(g.stats.pieces == 1);
    press(g, Action::HardDrop);
    assert(g.stats.pieces == 1);
    run(g, 6);
    press(g, Action::HardDrop);
    assert(g.stats.pieces == 2);
}

static void testTopOut() {
    Game g(Ruleset{}, Handling{}, 1);
    place(g, Piece::O, 0, 0, 10.5);
    g.board.at(4, 21) = Piece::Garbage;
    assert(tap(g, Action::HardDrop).size() == 1);
    assert(g.toppedOut);
    const uint64_t frame = g.frame;
    run(g, 5);
    assert(g.frame == frame);

    // Clutch: straight after a clear the blocked spawn lifts free.
    Game c(Ruleset{}, Handling{}, 1);
    fillExcept(c, 1, {{0, 0}, {1, 0}});
    c.board.at(4, 22) = Piece::Garbage;
    place(c, Piece::O, 0, 0, 10.5);
    tap(c, Action::HardDrop);
    assert(!c.toppedOut);
    assert(near(c.falling.y, 23.04));  // resting on the garbage, so gravity can't move it

    Ruleset noClutch;
    noClutch.clutch = false;
    Game n(noClutch, Handling{}, 1);
    fillExcept(n, 1, {{0, 0}, {1, 0}});
    n.board.at(4, 22) = Piece::Garbage;
    place(n, Piece::O, 0, 0, 10.5);
    tap(n, Action::HardDrop);
    assert(n.toppedOut);
}

static bool sameState(const Game& a, const Game& b) {
    return a.board.cells == b.board.cells && a.falling.piece == b.falling.piece && a.falling.x == b.falling.x &&
           a.falling.y == b.falling.y && a.falling.rot == b.falling.rot && a.hold == b.hold &&
           a.queue == b.queue && a.rng.value == b.rng.value && a.frame == b.frame && a.toppedOut == b.toppedOut &&
           a.stats.attack == b.stats.attack && a.stats.pieces == b.stats.pieces;
}

// Pseudo-random play: never illegal, deterministic across copies, no allocation.
static void testRandomPlay() {
    Handling h;
    h.das = 3;
    h.arr = 1;
    h.dcd = 1;
    h.sdf = 20;
    uint32_t x = 2463534242;
    const auto random = [&x] {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };

    Game g(Ruleset{}, h, 7);
    Game copy = g;
    Game midCopy = g;
    bool copied = false;
    bool held[8] = {};
    int during = 0;

    int pieces = 0;
    for (int tick = 0; tick < 200000; tick++) {
        InputEvent inputs[3];
        int count = 0;
        double subframe = 0;
        for (int i = 0; i < 3; i++) {
            if (random() % 4 != 0) continue;
            const auto action = static_cast<Action>(random() % 8);
            subframe += (random() % 10) / 40.0;
            const bool pressed = !held[static_cast<int>(action)];
            held[static_cast<int>(action)] = pressed;
            inputs[count++] = {action, pressed, subframe};
        }
        const std::span<const InputEvent> span(inputs, count);

        const int start = allocations;
        g.tick(span);
        during += allocations - start;
        copy.tick(span);
        if (copied) midCopy.tick(span);

        if (tick == 2000) {
            midCopy = g;
            copied = true;
        }

        if (!g.toppedOut) {
            for (const Cell& c : g.falling.cells()) {
                assert(c.x >= 0 && c.x < kBoardWidth && c.y >= 0 && c.y < kBoardHeight);
                assert(g.board.at(c.x, c.y) == Piece::None);
            }
            for (const Cell& c : g.ghost().cells()) assert(g.board.at(c.x, c.y) == Piece::None);
        }
        assert(sameState(g, copy));
        if (copied) assert(sameState(g, midCopy));

        if (g.toppedOut) {
            pieces += g.stats.pieces;
            g = Game(Ruleset{}, h, 7 + tick);
            copy = g;
            midCopy = g;
            for (bool& b : held) b = false;
        }
    }
    assert(pieces > 1000);
    assert(during == 0);
}

// A hold whose spawn is blocked ends the game and leaves the hold slot as it was.
static void testHoldTopOut() {
    Game g(Ruleset{}, Handling{}, 1);
    g.hold = Piece::T;
    g.falling.piece = Piece::I;
    place(g, Piece::I, 0, 3, 5);
    for (int y = 18; y < 24; y++) {
        for (int x = 0; x < kBoardWidth; x++) g.board.at(x, y) = Piece::Garbage;
    }
    tap(g, Action::Hold);
    assert(g.toppedOut);
    assert(g.hold == Piece::T);
}

// Delivers garbage from sender 9 and confirms it on this tick.
static void receive(Game& g, int iid, int amount, int ackIid = 0) {
    const GarbageEvent e[] = {{GarbageEvent::Arrive, 9, iid, ackIid, amount}, {GarbageEvent::Confirm, 9, iid}};
    g.tick({}, e);
}

static int holeIn(const Game& g, int y) {
    int hole = -1;
    for (int x = 0; x < kBoardWidth; x++) {
        if (g.board.at(x, y) == Piece::Garbage) continue;
        assert(hole == -1);
        hole = x;
    }
    return hole;
}

static int garbageRows(const Game& g) {
    int rows = 0;
    while (g.board.at(0, rows) == Piece::Garbage || g.board.at(1, rows) == Piece::Garbage) rows++;
    return rows;
}

// Sets up a quad, which attacks for 4 when it locks.
static void dropQuad(Game& g) {
    fillExcept(g, 4, {{9, 0}, {9, 1}, {9, 2}, {9, 3}});
    place(g, Piece::I, 1, 7, 3);
    tap(g, Action::HardDrop);
}

static void testGarbageQueue() {
    const auto locksClean = [](Game& g) { tap(g, Action::HardDrop); };
    Ruleset rules;

    // Rises 20 frames after its confirm on a hard drop, one later on a natural lock.
    Game early(rules, Handling{}, 1);
    receive(early, 1, 3);
    run(early, 18);
    assert(early.frame == 19);
    locksClean(early);
    assert(garbageRows(early) == 0);
    Game ontime(rules, Handling{}, 1);
    receive(ontime, 1, 3);
    run(ontime, 19);
    assert(ontime.frame == 20);
    locksClean(ontime);
    assert(garbageRows(ontime) == 3);
    assert(ontime.stats.garbageReceived == 3);

    // The cap holds the rest for the next lock, which needs no new delay.
    Ruleset capped;
    capped.garbageCap = 2;
    Game cap(capped, Handling{}, 1);
    receive(cap, 1, 3);
    run(cap, 19);
    locksClean(cap);
    assert(garbageRows(cap) == 2);
    locksClean(cap);
    assert(garbageRows(cap) == 3);

    // Without a confirm it stays in the queue however long it waits.
    Game waiting(rules, Handling{}, 1);
    const GarbageEvent arrive[] = {{GarbageEvent::Arrive, 9, 1, 0, 3}};
    waiting.tick({}, arrive);
    run(waiting, 100);
    locksClean(waiting);
    assert(garbageRows(waiting) == 0);
    assert(waiting.stats.garbageReceived == 0);

    // Holes use the garbage RNG in this order: the first roll, a within-batch draw for
    // each later row, then a draw and a reroll between batches.
    const uint32_t seed = 12345;
    Rng rng(seed);
    const auto roll = [&] { return static_cast<int>(std::floor(rng.nextFloat() * kBoardWidth)); };
    const int first = roll();
    rng.next();  // second row of the first batch
    rng.next();  // batch finished, and messinessChange is 1
    const int second = roll();
    Game holes(rules, Handling{}, seed);
    receive(holes, 1, 2);
    receive(holes, 2, 1);
    run(holes, 19);
    locksClean(holes);
    assert(garbageRows(holes) == 3);
    assert(holeIn(holes, 2) == first);
    assert(holeIn(holes, 1) == first);
    assert(holeIn(holes, 0) == second);

    // Garbage rises in arrival order, even if the later batch was confirmed first:
    // the first batch still travelling holds it back.
    Game order(rules, Handling{}, 1);
    const GarbageEvent both[] = {{GarbageEvent::Arrive, 9, 1, 0, 1}, {GarbageEvent::Arrive, 9, 2, 0, 1}, {GarbageEvent::Confirm, 9, 2}};
    order.tick({}, both);
    run(order, 30);
    locksClean(order);
    assert(garbageRows(order) == 0);
    const GarbageEvent confirmFirst[] = {{GarbageEvent::Confirm, 9, 1}};
    order.tick({}, confirmFirst);
    run(order, 19);
    locksClean(order);
    assert(garbageRows(order) == 2);

    // Older rounds draw nothing when a cancel finishes a batch.
    Ruleset older;
    older.cancelMovesHole = false;
    Rng olderRng(seed);
    const int olderFirst = static_cast<int>(std::floor(olderRng.nextFloat() * kBoardWidth));
    Game legacy(older, Handling{}, seed);
    receive(legacy, 1, 1);
    receive(legacy, 2, 1);
    dropQuad(legacy);
    run(legacy, 30);
    tap(legacy, Action::HardDrop);
    assert(garbageRows(legacy) == 0);
    receive(legacy, 3, 1);
    run(legacy, 30);
    tap(legacy, Action::HardDrop);
    assert(garbageRows(legacy) == 1);
    assert(holeIn(legacy, 0) == olderFirst);

    // A copy carries the queue and the column, and plays on identically.
    Game original(rules, Handling{}, seed);
    receive(original, 1, 2);
    run(original, 19);
    Game copy = original;
    locksClean(original);
    locksClean(copy);
    assert(original.board.cells == copy.board.cells);
    assert(holeIn(copy, 0) == holeIn(original, 0));

    // A clear cancels the oldest first, sends the rest and raises nothing.
    Game cancel(rules, Handling{}, 1);
    receive(cancel, 1, 1);
    receive(cancel, 2, 2);
    run(cancel, 30);
    dropQuad(cancel);
    assert(cancel.stats.attack == 4);
    assert(cancel.stats.garbageSent == 1);
    Game sentCheck(rules, Handling{}, 1);
    receive(sentCheck, 1, 3);
    fillExcept(sentCheck, 4, {{9, 0}, {9, 1}, {9, 2}, {9, 3}});
    place(sentCheck, Piece::I, 1, 7, 3);
    const LockResult sentLock = tap(sentCheck, Action::HardDrop)[0];
    assert(sentLock.sends[3] == 4);
    assert(sentLock.sent[3] == 1);
    assert(garbageRows(cancel) == 0);
    locksClean(cancel);
    assert(garbageRows(cancel) == 0);

    // Partly cancelled, so the newest two rows are left, and a clear still raises nothing.
    Game partial(rules, Handling{}, 1);
    receive(partial, 1, 1);
    receive(partial, 2, 5);
    run(partial, 30);
    dropQuad(partial);
    assert(partial.stats.garbageSent == 0);
    assert(garbageRows(partial) == 0);
    locksClean(partial);
    assert(garbageRows(partial) == 2);
    assert(partial.stats.garbageReceived == 6);

    // In the opener an attack cancels up to twice its value, sending nothing extra.
    Ruleset opener;
    opener.openerPhase = 14;
    Game open(opener, Handling{}, 1);
    receive(open, 1, 6);
    run(open, 30);
    dropQuad(open);
    assert(open.stats.garbageSent == 0);
    locksClean(open);
    assert(garbageRows(open) == 0);
    Game late(opener, Handling{}, 1);
    late.stats.pieces = 14;
    receive(late, 1, 6);
    run(late, 30);
    dropQuad(late);
    locksClean(late);
    assert(garbageRows(late) == 2);

    // Passthrough: sends the sender hadn't seen cancel against what it sent back.
    Game crossing(rules, Handling{}, 1);
    const GarbageEvent target[] = {{GarbageEvent::Target, 9}};
    crossing.tick({}, target);
    dropQuad(crossing);
    assert(crossing.stats.garbageSent == 4);
    receive(crossing, 1, 3);  // crosses 3 of the 4 sent
    receive(crossing, 2, 3);  // crosses the last, so 2 are left
    assert(crossing.stats.garbageReceived == 2);
    receive(crossing, 3, 3, 1);  // acknowledged, so nothing is left to cross
    assert(crossing.stats.garbageReceived == 5);

    // Inputs on the same tick run in order around the garbage.
    Game before(rules, Handling{}, 1);
    fillExcept(before, 4, {{9, 0}, {9, 1}, {9, 2}, {9, 3}});
    place(before, Piece::I, 1, 7, 3);
    const InputEvent drop[] = {{Action::HardDrop, true, 0}};
    const GarbageEvent atStart[] = {{GarbageEvent::Arrive, 9, 1, 0, 3, 0}};
    before.tick(drop, atStart);
    assert(before.stats.garbageSent == 1);
    Game after(rules, Handling{}, 1);
    fillExcept(after, 4, {{9, 0}, {9, 1}, {9, 2}, {9, 3}});
    place(after, Piece::I, 1, 7, 3);
    const GarbageEvent atEnd[] = {{GarbageEvent::Arrive, 9, 1, 0, 3, 1}};
    after.tick(drop, atEnd);
    assert(after.stats.garbageSent == 4);
}

int main() {
    testRng();
    testPieceOrder();
    testNoSzo();
    testSpawnAndGhost();
    testDasArr();
    testDcd();
    testGravityAndSoftDrop();
    testLockDelay();
    testKicks();
    testSpins();
    testAttackValue();
    testSends();
    testLevels();
    testScore();
    testFillGarbage();
    testGarbageQueue();
    testRespawn();
    testHoldTopOut();
    testHold();
    testIrsIhs();
    testSafelock();
    testTopOut();
    testRandomPlay();
}
