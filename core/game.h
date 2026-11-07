// Deterministic game simulation. No SDL, no I/O, no clock: the same seed,
// rules, handling and inputs always produce the same game.
#pragma once

#include <array>
#include <cstddef>
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
    bool hoisted = false;  // held since before the game: left/right starts charged
};

// Garbage on its way to this player, in the order the round received it.
struct GarbageEvent {
    enum Type : uint8_t {
        Arrive,   // queued but can't rise yet; amount is before passthrough
        Confirm,  // its travel time starts on this frame
        Target,   // gameId is who this player's attacks go to
    };
    Type type;
    int gameId;  // the sender, except for Target
    int iid = 0;
    int ackIid = 0;  // the last of this player's sends the sender had seen
    int amount = 0;
    size_t beforeInput = 0;  // applied ahead of inputs[beforeInput], or after all of them
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
    bool noSzo = false;  // the first bag can't start with S, Z or O

    // Gravity that rises with level instead of with time. Off by default.
    bool levels = false;
    double levelSpeed = 1;
    double levelGBase = 0.8;
    // ponytail: inferred from 3 Blitz replays, which pass from 0.0069 to 0.0075 and fail at 0.0065 and 0.01; correct from a replay that pins it
    double levelGSpeed = 0.007;

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

    int garbageSpeed = 20;  // frames from Confirm until garbage can rise
    double garbageCap = 8;  // rows that can rise per lock
    double garbageCapIncrease = 0;
    int garbageCapMargin = 0;
    int garbageCapMax = 40;
    int openerPhase = 0;  // for this many pieces an attack cancels up to twice its value
    double messinessChange = 1;  // chance the hole moves between batches of garbage
    double messinessInner = 0;   // chance the hole moves between rows of a batch
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
    std::array<Cell, 4> cells{};  // where it locked, before any lines cleared
    int lines = 0;
    Spin spin = Spin::None;
    bool perfectClear = false;
    int garbageCleared = 0;
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
    int level = 1;
    int levelLines = 0;  // lines toward the next level
    int score = 0;
    int garbageCleared = 0;  // cleared lines that held garbage
    int garbageSent = 0;      // attack left after cancelling
    int garbageReceived = 0;  // incoming left when confirmed; TETR.IO's stats fit this
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

// Rule data for ai/, which searches moves without running a Game.
struct Kicks {
    int count;
    int8_t offsets[5][2];  // {dx, dy}, where a positive dy moves the piece down
};
const Kicks& kickTable(Piece piece, int from, int to);
// T corners as {dx, dy from the centre, front when rotation is either of these}.
constexpr int kTCorners[4][4] = {{-1, -1, 3, 0}, {1, -1, 0, 1}, {1, 1, 1, 2}, {-1, 1, 2, 3}};

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
    const std::vector<LockResult>& tick(std::span<const InputEvent> inputs, std::span<const GarbageEvent> garbage = {});

    // Fills the bottom rows of an empty board. A row's hole moves from the row
    // below's with probability `messiness`. `rows` must be at most kVisibleHeight,
    // so the rows stay clear of the spawn.
    void fillGarbage(int rows, double messiness);

    // Puts `piece` at the spawn after outside edits to the board, queue or hold,
    // leaving holdUsed alone. Tops out again if the spawn is still blocked.
    void respawn(Piece piece);

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

    struct Incoming {
        int gameId;
        int iid;
        int amount;
        int64_t frame;  // when it was confirmed; effectively never until then
    };

    struct Outgoing {
        int iid;
        int amount;
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
    void sink(int pointsPerRow);
    void fall(double delta);
    void lock(bool hard);
    void applyGarbage(const GarbageEvent& e);
    int cancelGarbage(int amount);
    void tankGarbage(bool hard);
    void rerollColumn();
    void gainLevels(int lines);
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
    double garbageCap_;

    std::vector<Incoming> incoming_;
    std::vector<Outgoing> outgoing_;  // sends the opponent hasn't acknowledged
    int target_ = -1;  // ponytail: one opponent, as in League; keep sends per player for bigger rooms
    int lastSendIid_ = 0;
    int cancelSent_ = 0;  // what cancels have let through, for the opener phase
    int lastColumn_ = -1;  // -1 until the first roll
    bool columnChanged_ = false;
};

}  // namespace core
