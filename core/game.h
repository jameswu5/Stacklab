// Deterministic game simulation. No SDL, no I/O, no clock: the same seed,
// rules, handling and inputs always produce the same game.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace core {

constexpr int kBoardWidth = 10;
constexpr int kVisibleHeight = 20;
constexpr int kBoardHeight = 40;  // visible rows plus the buffer above them
constexpr int kTicksPerSecond = 60;

enum class Piece : uint8_t { None, I, J, L, O, S, T, Z, Garbage };
enum class Spin : uint8_t { None, Mini, Normal };
enum class Action : uint8_t { Left, Right, SoftDrop, HardDrop, RotateCW, RotateCCW, Rotate180, Hold };
enum class SpinBonuses : uint8_t { None, TSpins, AllMiniPlus };

// Applied during the tick it is passed to, at `subframe` in [0, 1).
struct InputEvent {
    Action action;
    bool pressed;
    double subframe;
    bool hoisted = false;  // left/right held since before the game: starts charged
};

// Stored in frames, as in TETR.IO. sdf == 41 means instant soft drop.
struct Handling {
    double arr = 0;
    double das = 6;
    double dcd = 0;
    double sdf = 41;
    bool safelock = false;
    bool cancel = false;
    bool may20g = true;
    bool irsHold = false;  // false is tap
    bool ihsHold = false;  // false is tap
};

// Defaults are the fallbacks Triangle.js uses for options missing from a
// replay. Versus values must come from a real replay (BRIEF.md section 5.2).
struct Ruleset {
    double gravity = 0.02;
    double gravityIncrease = 0;
    int gravityMargin = 0;
    double lockTime = 30;
    int lockResets = 15;
    bool may20g = true;
    bool allow180 = true;
    bool allowHold = true;
    bool clutch = true;
    int previews = 5;
    SpinBonuses spinBonuses = SpinBonuses::AllMiniPlus;

    bool b2bChaining = true;
    bool b2bCharging = false;
    int b2bChargeAt = 4;
    int b2bChargeBase = 3;
    int pcGarbage = 0;
    int pcB2b = 0;
    double garbageMultiplier = 1;
    double garbageIncrease = 0.008;
    int garbageMargin = 10800;
    bool garbageSpecialBonus = false;
    bool roundDown = true;  // false rounds fractions randomly
};

struct Board {
    std::array<Piece, kBoardWidth * kBoardHeight> cells{};

    // y counts up from the floor.
    Piece& at(int x, int y) { return cells[y * kBoardWidth + x]; }
    Piece at(int x, int y) const { return cells[y * kBoardWidth + x]; }
};

struct Cell {
    int x, y;
};

struct Falling {
    Piece piece = Piece::None;
    int rot = 0;  // 0 spawn, 1 CW, 2 180, 3 CCW
    int x = 0;
    double y = 0;  // fractional while falling; the occupied row is floor(y)

    double locking = 0;  // frames spent on the ground since the last reset
    int lockResets = 0;
    int rotResets = 0;
    int highestY = 0;  // lowest row reached; going below it refunds the resets
    int totalRotations = 0;

    std::array<Cell, 4> cells() const;
};

struct LockResult {
    Piece piece = Piece::None;
    int lines = 0;
    Spin spin = Spin::None;
    bool perfectClear = false;
    std::array<int, 5> sends{};  // surge (3), normal attack, PC; unused entries are 0
    int b2b = -1;
    int combo = -1;
};

struct Stats {
    int pieces = 0;
    int lines = 0;
    int keys = 0;
    int attack = 0;
    int b2b = -1;
    int combo = -1;
};

// Park-Miller LCG, matching TETR.IO's piece order for a given seed.
struct Rng {
    uint32_t value;

    explicit Rng(uint32_t seed);
    uint32_t next();
    double nextFloat();
};

// Attack before rounding and multipliers, as in BRIEF.md section 5.3.
double attackValue(int lines, Spin spin, int b2b, int combo, const Ruleset& rules);

// A plain value: copying it is how undo, retry and the bot take snapshots.
struct Game {
    Ruleset rules;
    Handling handling;
    Rng rng;

    Board board;
    Falling falling;
    Piece hold = Piece::None;
    bool holdUsed = false;
    std::vector<Piece> queue;  // front is next; always at least rules.previews long
    Stats stats;
    bool toppedOut = false;
    uint64_t frame = 0;

    Game(const Ruleset& rules, const Handling& handling, uint32_t seed);

    // Advances one tick (1/60 s). The result lists pieces locked during this
    // tick and stays valid until the next call.
    const std::vector<LockResult>& tick(std::span<const InputEvent> inputs);

    // Where the falling piece would land.
    Falling ghost() const;

    double seconds() const { return static_cast<double>(frame) / kTicksPerSecond; }

private:
    struct Shift {
        int dir;
        bool held = false;
        double arr = 0;
        double das = 0;
    };

    bool fits(int rot, int x, int y) const;
    void pushBag();
    void nextPiece(bool isHold);
    void spawn(Piece piece, bool isHold);
    bool blockedOut();
    void holdPiece();
    void rotate(int amount);
    Spin detectSpin(bool finOrTst) const;
    bool shouldLock() const;
    bool is20G() const;
    bool fallBy(double amount);
    void fall(double delta);
    void lock();
    int roundAttack(double amount);
    void dcd();
    void shift();
    void processShift(Shift& s, double delta);
    void processSubframe(double subframe);
    void keyDown(const InputEvent& e);
    void keyUp(const InputEvent& e);

    Rng garbageRng_;  // TETR.IO keeps this separate from the bag; rounding draws from it
    std::vector<LockResult> locks_;
    Shift left_{-1};
    Shift right_{1};
    int lastShift_ = -1;
    bool softDropHeld_ = false;
    bool cwHeld_ = false;
    bool ccwHeld_ = false;
    bool rot180Held_ = false;
    bool holdHeld_ = false;
    bool hitWall_ = false;
    bool forceLock_ = false;
    bool lastWasClear_ = false;
    Spin lastSpin_ = Spin::None;
    int safeLock_ = 0;
    double subframe_ = 0;
    double gravity_;
    double garbageMultiplier_;
};

}  // namespace core
