// Port of the Triangle.js engine (MIT, see THIRD_PARTY_NOTICES.md). Statement
// order and floating point expressions follow src/engine/index.ts on purpose:
// parity depends on them, so don't tidy the arithmetic.
#include "core/game.h"

#include <algorithm>
#include <cmath>

// JavaScript never fuses a*b+c; on arm64 the compiler otherwise would.
#pragma clang fp contract(off)

namespace core {

namespace {

constexpr uint32_t kModulus = 2147483647;
constexpr uint32_t kMaxFloat = 2147483646;

constexpr int kSpawnHighestY = kVisibleHeight + 2;
constexpr double kSpawnY = kVisibleHeight + 2.04;

// Indexed by Piece, then rotation. Each block is {x, rows below falling.y}.
constexpr int8_t kShapes[8][4][4][2] = {
    {},
    // I
    {{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
     {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
     {{3, 2}, {2, 2}, {1, 2}, {0, 2}},
     {{1, 3}, {1, 2}, {1, 1}, {1, 0}}},
    // J
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{2, 0}, {1, 0}, {1, 1}, {1, 2}},
     {{2, 2}, {2, 1}, {1, 1}, {0, 1}},
     {{0, 2}, {1, 2}, {1, 1}, {1, 0}}},
    // L
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{2, 2}, {1, 0}, {1, 1}, {1, 2}},
     {{0, 2}, {2, 1}, {1, 1}, {0, 1}},
     {{0, 0}, {1, 2}, {1, 1}, {1, 0}}},
    // O
    {{{0, 0}, {1, 0}, {0, 1}, {1, 1}},
     {{1, 0}, {1, 1}, {0, 0}, {0, 1}},
     {{1, 1}, {0, 1}, {1, 0}, {0, 0}},
     {{0, 1}, {0, 0}, {1, 1}, {1, 0}}},
    // S
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}},
     {{2, 1}, {2, 2}, {1, 0}, {1, 1}},
     {{1, 2}, {0, 2}, {2, 1}, {1, 1}},
     {{0, 1}, {0, 0}, {1, 2}, {1, 1}}},
    // T
    {{{1, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{2, 1}, {1, 0}, {1, 1}, {1, 2}},
     {{1, 2}, {2, 1}, {1, 1}, {0, 1}},
     {{0, 1}, {1, 2}, {1, 1}, {1, 0}}},
    // Z
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}},
     {{2, 0}, {2, 1}, {1, 1}, {1, 2}},
     {{2, 2}, {1, 2}, {1, 1}, {0, 1}},
     {{0, 2}, {0, 1}, {1, 1}, {1, 0}}},
};

struct Kicks {
    int count;
    int8_t offsets[5][2];  // {dx, dy}, where a positive dy moves the piece down
};

// SRS+, indexed by [from][to].
constexpr Kicks kKicks[4][4] = {
    {{},
     {4, {{-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}},
     {5, {{0, -1}, {1, -1}, {-1, -1}, {1, 0}, {-1, 0}}},
     {4, {{1, 0}, {1, -1}, {0, 2}, {1, 2}}}},
    {{4, {{1, 0}, {1, 1}, {0, -2}, {1, -2}}},
     {},
     {4, {{1, 0}, {1, 1}, {0, -2}, {1, -2}}},
     {5, {{1, 0}, {1, -2}, {1, -1}, {0, -2}, {0, -1}}}},
    {{5, {{0, 1}, {-1, 1}, {1, 1}, {-1, 0}, {1, 0}}},
     {4, {{-1, 0}, {-1, -1}, {0, 2}, {-1, 2}}},
     {},
     {4, {{1, 0}, {1, -1}, {0, 2}, {1, 2}}}},
    {{4, {{-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}},
     {5, {{-1, 0}, {-1, -2}, {-1, -1}, {0, -2}, {0, -1}}},
     {4, {{-1, 0}, {-1, 1}, {0, -2}, {-1, -2}}},
     {}},
};

constexpr Kicks kIKicks[4][4] = {
    {{},
     {4, {{1, 0}, {-2, 0}, {-2, 1}, {1, -2}}},
     {1, {{0, -1}}},
     {4, {{-1, 0}, {2, 0}, {2, 1}, {-1, -2}}}},
    {{4, {{-1, 0}, {2, 0}, {-1, 2}, {2, -1}}},
     {},
     {4, {{-1, 0}, {2, 0}, {-1, -2}, {2, 1}}},
     {1, {{1, 0}}}},
    {{1, {{0, 1}}},
     {4, {{-2, 0}, {1, 0}, {-2, -1}, {1, 2}}},
     {},
     {4, {{2, 0}, {-1, 0}, {2, -1}, {-1, 2}}}},
    {{4, {{1, 0}, {-2, 0}, {1, 2}, {-2, -1}}},
     {1, {{-1, 0}}},
     {4, {{1, 0}, {-2, 0}, {1, -2}, {-2, 1}}},
     {}},
};

// T corners as {dx, dy from the centre, front when rotation is either of these}.
constexpr int kTCorners[4][4] = {{-1, -1, 3, 0}, {1, -1, 0, 1}, {1, 1, 1, 2}, {-1, 1, 2, 3}};

int row(double y) { return static_cast<int>(std::floor(y)); }

}  // namespace

std::array<Cell, 4> Falling::cells() const {
    std::array<Cell, 4> out;
    const auto& shape = kShapes[static_cast<int>(piece)][rot];
    for (int i = 0; i < 4; i++) out[i] = {x + shape[i][0], row(y) - shape[i][1]};
    return out;
}

Rng::Rng(uint32_t seed) : value(seed % kModulus) {
    if (value == 0) value = kMaxFloat;
}

uint32_t Rng::next() {
    value = static_cast<uint32_t>(uint64_t{16807} * value % kModulus);
    return value;
}

double Rng::nextFloat() { return (next() - 1.0) / kMaxFloat; }

double attackValue(int lines, Spin spin, int b2b, int combo, const Ruleset& rules) {
    const bool anySpin = spin != Spin::None;
    double garbage = 0;
    switch (lines) {
        case 0: break;
        case 1: garbage = spin == Spin::Normal ? 2 : 0; break;
        case 2: garbage = spin == Spin::Normal ? 4 : 1; break;
        case 3: garbage = spin == Spin::Normal ? 6 : 2; break;
        case 4: garbage = anySpin ? 10 : 4; break;
        case 5: garbage = anySpin ? 12 : 5; break;
        default: garbage = anySpin ? 12 + 2 * (lines - 5) : 5 + (lines - 5); break;
    }

    if (lines > 0 && b2b > 0) {
        if (rules.b2bChaining) {
            // ponytail: libm log1p, V8 uses fdlibm; port fdlibm's if a replay ever differs by 1 ulp
            const double log = std::log1p(b2b * 0.8);
            garbage += std::floor(1 + log) + (b2b == 1 ? 0 : (1 + std::fmod(log, 1)) / 3);
        } else {
            garbage += 1;
        }
    }

    if (combo > 0) {
        garbage *= 1 + 0.25 * combo;
        if (combo > 1) garbage = std::max(std::log1p(combo * 1.25), garbage);
    }
    return garbage;
}

Game::Game(const Ruleset& rules, const Handling& handling, uint32_t seed)
    : rules(rules),
      handling(handling),
      rng(seed),
      garbageRng_(seed),
      gravity_(rules.gravity),
      garbageMultiplier_(rules.garbageMultiplier) {
    queue.reserve(rules.previews + 8);
    locks_.reserve(8);
    nextPiece(false);
}

bool Game::fits(int rot, int x, int y) const {
    const auto& shape = kShapes[static_cast<int>(falling.piece)][rot];
    for (const auto& block : shape) {
        const int cx = x + block[0];
        const int cy = y - block[1];
        if (cx < 0 || cx >= kBoardWidth || cy < 0 || cy >= kBoardHeight) return false;
        if (board.at(cx, cy) != Piece::None) return false;
    }
    return true;
}

void Game::pushBag() {
    Piece bag[] = {Piece::Z, Piece::L, Piece::O, Piece::S, Piece::I, Piece::J, Piece::T};
    for (int i = 6; i != 0; i--) {
        const int r = static_cast<int>(std::floor(rng.nextFloat() * (i + 1)));
        std::swap(bag[i], bag[r]);
    }
    queue.insert(queue.end(), bag, bag + 7);
}

void Game::nextPiece(bool isHold) {
    while (static_cast<int>(queue.size()) <= rules.previews) pushBag();
    const Piece piece = queue.front();
    queue.erase(queue.begin());
    spawn(piece, isHold);
}

// Without ARE a piece is never asleep, so IRS and IHS only act in hold mode.
void Game::spawn(Piece piece, bool isHold) {
    int irs = 0;
    if (handling.irsHold && falling.piece != Piece::None) {
        if (ccwHeld_) irs -= 1;
        if (cwHeld_) irs += 1;
        if (rot180Held_) irs += 2;
        irs = ((irs % 4) + 4) % 4;
    }
    const bool ihs = handling.ihsHold && holdHeld_ && !isHold;

    dcd();
    hitWall_ = false;
    forceLock_ = false;
    if (!isHold) holdUsed = false;

    falling = Falling{};
    falling.piece = piece;
    falling.x = piece == Piece::O ? 4 : 3;
    falling.y = kSpawnY;
    falling.highestY = kSpawnHighestY;

    if (blockedOut()) {
        toppedOut = true;
        return;
    }
    if (ihs) {
        holdPiece();
        return;
    }
    if (irs != 0) rotate(irs);
    if (is20G()) {
        while (fallBy(1)) {
        }
    }
}

// True when the spawn is blocked. After a clear, clutch may lift the piece free.
bool Game::blockedOut() {
    if (fits(falling.rot, falling.x, row(falling.y))) return false;
    if (lastWasClear_ && rules.clutch) {
        const double originalY = falling.y;
        const int originalHighest = falling.highestY;
        while (row(falling.y) < kBoardHeight) {
            falling.y += 1;
            falling.highestY += 1;
            if (fits(falling.rot, falling.x, row(falling.y))) return false;
        }
        falling.y = originalY;
        falling.highestY = originalHighest;
    }
    return true;
}

void Game::holdPiece() {
    if (holdUsed || !rules.allowHold) return;
    holdUsed = true;
    const Piece held = hold;
    hold = falling.piece;
    if (held != Piece::None) {
        spawn(held, true);
    } else {
        nextPiece(true);
    }
}

void Game::rotate(int amount) {
    const int from = falling.rot;
    const int to = (((from + amount) % 4) + 4) % 4;
    int newX = falling.x;
    double newY = falling.y;
    bool finOrTst = false;

    if (!fits(to, falling.x, row(falling.y))) {
        const bool maxMovement = falling.totalRotations > rules.lockResets + 15;
        const Kicks& kicks = (falling.piece == Piece::I ? kIKicks : kKicks)[from][to];
        bool kicked = false;
        for (int i = 0; i < kicks.count && !kicked; i++) {
            const int dx = kicks.offsets[i][0];
            const int dy = kicks.offsets[i][1];
            const double y = maxMovement ? falling.y - dy : std::ceil(falling.y) - 0.1 - dy;
            if (!fits(to, falling.x + dx, row(y))) continue;
            newX = falling.x + dx;
            newY = y;
            kicked = true;
            finOrTst = (from == 2 || from == 0) && dy == 2 && ((to == 3 && dx == 1) || (to == 1 && dx == -1));
        }
        if (!kicked) return;
    }

    falling.x = newX;
    falling.y = newY;
    falling.rot = to;
    if (falling.lockResets < 31) falling.lockResets++;
    if (falling.rotResets < 63) falling.rotResets++;
    lastSpin_ = detectSpin(finOrTst);
    falling.totalRotations++;
    dcd();
    if (!shouldLock()) falling.locking = 0;
}

Spin Game::detectSpin(bool finOrTst) const {
    if (rules.spinBonuses == SpinBonuses::None) return Spin::None;
    const int x = falling.x;
    const int y = row(falling.y);

    Spin tSpin = Spin::None;
    if (falling.piece == Piece::T && !fits(falling.rot, x, y - 1)) {
        int corners = 0;
        int front = 0;
        for (const auto& corner : kTCorners) {
            const int cx = x + corner[0] + 1;
            const int cy = y - corner[1] - 1;
            const bool occupied =
                cx < 0 || cy < 0 || cx >= kBoardWidth || cy >= kBoardHeight || board.at(cx, cy) != Piece::None;
            if (!occupied) continue;
            corners++;
            if (falling.rot == corner[2] || falling.rot == corner[3]) front++;
        }
        if (corners >= 3) tSpin = front != 2 && !finOrTst ? Spin::Mini : Spin::Normal;
    }
    if (rules.spinBonuses == SpinBonuses::TSpins) return tSpin;

    const bool immobile = !fits(falling.rot, x - 1, y) && !fits(falling.rot, x + 1, y) &&
                          !fits(falling.rot, x, y + 1) && !fits(falling.rot, x, y - 1);
    return std::max(tSpin, immobile ? Spin::Mini : Spin::None);
}

bool Game::shouldLock() const { return falling.lockResets >= rules.lockResets; }

bool Game::is20G() const {
    const bool high = gravity_ > kVisibleHeight;
    if (softDropHeld_) {
        const bool prefer = handling.may20g || (high && rules.may20g);
        return (handling.sdf == 41 || gravity_ * handling.sdf > kVisibleHeight) && prefer;
    }
    return high && rules.may20g;
}

bool Game::fallBy(double amount) {
    double y1 = std::round(1e6 * (falling.y - amount)) / 1e6;
    double y2 = falling.y - 1;
    if (std::fmod(y1, 1) == 0) y1 -= 1e-6;
    if (std::fmod(y2, 1) == 0) y2 += 2e-6;

    if (!fits(falling.rot, falling.x, row(y1)) || !fits(falling.rot, falling.x, row(y2))) return false;

    const int highest = falling.highestY;
    if (highest > y1) falling.highestY = row(y1);
    falling.y = y1;
    lastSpin_ = Spin::None;
    if (y1 < highest) {
        falling.lockResets = 0;
        falling.rotResets = 0;
    }
    return true;
}

void Game::fall(double delta) {
    if (safeLock_ > 0) safeLock_--;

    double drop = gravity_ * delta;
    if (softDropHeld_) {
        if (handling.sdf == 41) {
            drop = 400 * delta;
        } else {
            drop *= handling.sdf;
            drop = std::max(drop, 0.05 * handling.sdf);
        }
    }

    if (shouldLock() && !fits(falling.rot, falling.x, row(falling.y - 1))) {
        drop = 20;
        forceLock_ = true;
    }

    if (falling.rotResets > rules.lockResets + 15) {
        drop += 0.5 * delta * (falling.rotResets - (rules.lockResets + 15));
    }

    for (double left = drop; left > 0; left -= std::min(1.0, left)) {
        if (fallBy(std::min(1.0, left))) continue;
        falling.locking += delta;
        if (falling.locking > rules.lockTime || forceLock_ || shouldLock()) {
            if (handling.safelock) safeLock_ = 7;
            lock();
        }
        return;
    }
}

int Game::roundAttack(double amount) {
    const double floored = std::floor(amount);
    if (rules.roundDown || floored == amount) return static_cast<int>(floored);
    return static_cast<int>(floored) + (garbageRng_.nextFloat() < amount - floored ? 1 : 0);
}

void Game::lock() {
    holdUsed = false;
    for (const Cell& cell : falling.cells()) board.at(cell.x, cell.y) = falling.piece;

    int lines = 0;
    int garbageCleared = 0;
    int write = 0;
    for (int y = 0; y < kBoardHeight; y++) {
        bool full = true;
        bool hasGarbage = false;
        for (int x = 0; x < kBoardWidth; x++) {
            full = full && board.at(x, y) != Piece::None;
            hasGarbage = hasGarbage || board.at(x, y) == Piece::Garbage;
        }
        if (full) {
            lines++;
            if (hasGarbage) garbageCleared++;
            continue;
        }
        if (write != y) std::copy_n(&board.at(0, y), kBoardWidth, &board.at(0, write));
        write++;
    }
    std::fill(board.cells.begin() + write * kBoardWidth, board.cells.end(), Piece::None);
    const bool pc = std::all_of(board.cells.begin(), board.cells.end(), [](Piece p) { return p == Piece::None; });

    const bool spun = lastSpin_ != Spin::None;
    const bool pcB2b = pc && rules.pcB2b != 0;
    int brokeB2b = -2;  // the chain length if this clear broke it, otherwise -2
    if (lines > 0) {
        stats.combo++;
        if (spun || lines >= 4 || pcB2b) {
            stats.b2b += pcB2b ? rules.pcB2b : 1;
        } else {
            brokeB2b = stats.b2b;
            stats.b2b = -1;
        }
    } else {
        stats.combo = -1;
    }

    const int specialBonus = rules.garbageSpecialBonus && garbageCleared > 0 && (spun || lines >= 4) ? 1 : 0;
    const double garbage = attackValue(lines, lastSpin_, std::max(stats.b2b, 0), std::max(stats.combo, 0), rules);

    LockResult result;
    result.piece = falling.piece;
    result.lines = lines;
    result.spin = lastSpin_;
    result.perfectClear = pc;

    if (garbage > 0 || specialBonus > 0) {
        result.sends[3] = roundAttack(garbage * garbageMultiplier_ + specialBonus);
    }
    if (brokeB2b != -2 && rules.b2bCharging && brokeB2b + 1 > rules.b2bChargeAt) {
        const int surge = static_cast<int>(
            std::floor((brokeB2b - rules.b2bChargeAt + rules.b2bChargeBase + 1) * garbageMultiplier_));
        const int part = static_cast<int>(std::round(surge / 3.0));
        result.sends[0] = part;
        result.sends[1] = part;
        result.sends[2] = surge - 2 * part;
    }
    if (pc) result.sends[4] = roundAttack(rules.pcGarbage * garbageMultiplier_);

    for (int send : result.sends) stats.attack += send;
    lastWasClear_ = lines > 0;

    nextPiece(false);
    lastSpin_ = Spin::None;

    stats.pieces++;
    stats.lines += lines;
    result.b2b = stats.b2b;
    result.combo = stats.combo;
    locks_.push_back(result);
}

void Game::dcd() {
    if (!hitWall_ || handling.dcd == 0) return;
    left_.das = std::min(left_.das, handling.das - handling.dcd);
    left_.arr = handling.arr;
    right_.das = std::min(right_.das, handling.das - handling.dcd);
    right_.arr = handling.arr;
}

void Game::shift() {
    if (fits(falling.rot, falling.x + lastShift_, row(falling.y))) {
        falling.x += lastShift_;
        if (falling.lockResets < 31) falling.lockResets++;
        hitWall_ = false;
        if (is20G()) {
            while (fallBy(1)) {
            }
        }
        if (!shouldLock()) falling.locking = 0;
    } else {
        hitWall_ = true;
    }
}

void Game::processShift(Shift& s, double delta) {
    if (!s.held || lastShift_ != s.dir) return;
    const double arrDelta = std::max(0.0, delta - std::max(0.0, handling.das - s.das));
    s.das = std::min(s.das + delta, handling.das);
    if (s.das < handling.das) return;
    s.arr += arrDelta;
    if (s.arr < handling.arr) return;
    const int steps = handling.arr == 0 ? kBoardWidth : static_cast<int>(std::floor(s.arr / handling.arr));
    s.arr -= handling.arr * steps;
    for (int i = 0; i < steps; i++) shift();
}

void Game::processSubframe(double subframe) {
    if (subframe <= subframe_) return;
    const double delta = subframe - subframe_;
    processShift(left_, delta);
    processShift(right_, delta);
    fall(delta);
    subframe_ = subframe;
}

void Game::keyDown(const InputEvent& e) {
    processSubframe(e.subframe);
    if (toppedOut) return;
    stats.keys++;

    const auto activate = [&](Shift& s) {
        s.held = true;
        s.das = e.hoisted ? handling.das - handling.dcd : 0;
        s.arr = handling.arr;
        lastShift_ = s.dir;
        shift();
    };

    switch (e.action) {
        case Action::Left: activate(left_); break;
        case Action::Right: activate(right_); break;
        case Action::SoftDrop: softDropHeld_ = true; break;
        case Action::RotateCW:
            cwHeld_ = true;
            rotate(1);
            break;
        case Action::RotateCCW:
            ccwHeld_ = true;
            rotate(-1);
            break;
        case Action::Rotate180:
            rot180Held_ = true;
            if (rules.allow180) rotate(2);
            break;
        case Action::HardDrop:
            if (safeLock_ != 0) break;
            while (fallBy(1)) {
            }
            lock();
            break;
        case Action::Hold:
            holdHeld_ = true;
            holdPiece();
            break;
    }
}

void Game::keyUp(const InputEvent& e) {
    processSubframe(e.subframe);

    const auto release = [&](Shift& s, Shift& other) {
        s.held = false;
        s.das = 0;
        if (other.held) lastShift_ = other.dir;
        if (handling.cancel) {
            other.arr = handling.arr;
            other.das = 0;
        }
    };

    switch (e.action) {
        case Action::Left: release(left_, right_); break;
        case Action::Right: release(right_, left_); break;
        case Action::SoftDrop: softDropHeld_ = false; break;
        case Action::RotateCW: cwHeld_ = false; break;
        case Action::RotateCCW: ccwHeld_ = false; break;
        case Action::Rotate180: rot180Held_ = false; break;
        case Action::Hold: holdHeld_ = false; break;
        case Action::HardDrop: break;
    }
}

const std::vector<LockResult>& Game::tick(std::span<const InputEvent> inputs) {
    locks_.clear();
    if (toppedOut) return locks_;

    subframe_ = 0;
    for (const InputEvent& e : inputs) {
        if (e.pressed) {
            keyDown(e);
        } else {
            keyUp(e);
        }
        if (toppedOut) return locks_;
    }

    frame++;
    processShift(left_, 1 - subframe_);
    processShift(right_, 1 - subframe_);
    fall(1 - subframe_);

    if (static_cast<int64_t>(frame) > rules.gravityMargin) gravity_ += rules.gravityIncrease / 60;
    if (static_cast<int64_t>(frame) > rules.garbageMargin) garbageMultiplier_ += rules.garbageIncrease / 60;
    return locks_;
}

Falling Game::ghost() const {
    Falling out = falling;
    int y = row(out.y);
    while (fits(out.rot, out.x, y - 1)) y--;
    out.y = y;
    return out;
}

}  // namespace core
