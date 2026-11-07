// Finds perfect clears: placements of the known pieces that leave the board empty,
// using the move search's real reachability and handling hold and line clears.
// Plain values and its own buffers only, so it can run on a worker thread on a copy.
// It knows only what the player can see. It takes no core::Game, whose queue runs past the previews.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

#include "ai/move_search.h"

namespace ai {

constexpr int kPcMaxRows = 10;
// Kicks move a piece down at most 2, and a piece's box is 4 tall: from this far above the
// stack, it has the same choices as from the spawn, for far fewer states to search.
constexpr int kPcAirRows = 6;

struct PcQuery {
    core::Board board;
    core::Piece current = core::Piece::None;  // taken as being at its spawn
    core::Piece hold = core::Piece::None;
    bool holdUsed = false;
    std::vector<core::Piece> queue;  // the pieces known to come, in order
    bool queueEnds = false;          // nothing comes after them, as in a drill
    // A bit per core::Piece: what's left of the 7-bag after `queue`. 0 means the next piece starts a new bag.
    uint8_t bagLeft = 0;
    core::Ruleset rules;
    // The most lines a perfect clear may take, at most kPcMaxRows. The fewest that work are tried first.
    int maxLines = 4;
};

// What's left of the bag, from every piece the player has seen so far, in the order they came.
uint8_t pcBagLeft(std::span<const core::Piece> seen);

enum class PcStatus {
    Found,           // `steps` ends in a perfect clear
    NeedMorePieces,  // none with the known pieces, but they don't rule out one at `lines`
    Impossible,      // none within `lines` lines with these pieces. With 0, none ever: the cell count can't work
    Unknown,         // the budget ran out, it was cancelled, or the stack is over kPcMaxRows
};

struct PcStep {
    core::Piece piece;
    bool hold;  // press hold first, to get this piece
    std::array<core::Cell, 4> cells;  // on the board as it stands when this piece locks
    std::vector<Move> moves;          // from the spawn; a hard drop follows
};

struct PcResult {
    PcStatus status = PcStatus::Unknown;
    int lines = 0;         // how many lines the answer is about
    int piecesNeeded = 0;  // placements from here to clear `lines` lines
    std::vector<PcStep> steps;
    std::vector<core::Piece> assumed;  // solveWithUnseen: the unseen pieces its steps rely on, in order
    int nodes = 0;
};

constexpr int kPcDefaultNodes = 200'000;

// The placement to make now, when a perfect clear can't be proven yet.
struct PcAdvice {
    PcStatus status = PcStatus::Unknown;  // what solve says about the visible pieces
    bool hasStep = false;
    PcStep step{};
    // The share of sampled futures that a rollout clears after this placement, seeing only what a
    // player would. It's the best of many choices on few samples, so it reads high: rank by it,
    // but don't show it as a measured rate. 1 when it's proven, and 0 when it's impossible.
    double chance = 0;
    int samples = 0;         // how many futures `chance` rests on; 0 when proven, or if none finished
    int unknownSamples = 0;  // of those, how many ran out of nodes and were counted as failures
    bool partial = false;    // stopped early by the time budget or cancel: the best so far
    int lines = 0;
};

struct PcAdviceOptions {
    int maxSamples = 8;        // the most futures for the placement that ends up on top
    int nodesPerSolve = 5'000;
    int timeBudgetMs = 0;      // 0 for none. Stopping on time depends on the machine, so answers can then differ
    uint32_t seed = 1;         // the same state, seed and samples always give the same answer
};

class PcSolver {
public:
    // Found only ever uses the known pieces. NeedMorePieces says what comes next isn't ruled out:
    // with exactly enough known, hold can skip one for an unseen piece, and some piece the bag
    // allows then finishes it. With fewer known, placing them all just passes the simple checks.
    PcResult solve(const PcQuery& query, const std::atomic<bool>* cancel = nullptr, int maxNodes = kPcDefaultNodes);

    // Also tries every order of unseen pieces the bag allows, at the first line count the known
    // pieces can't reach. Found then means some order works, and `assumed` is one: the player
    // may not get it. Impossible means no order does at that line count.
    PcResult solveWithUnseen(const PcQuery& query, const std::atomic<bool>* cancel = nullptr, int maxNodes = kPcDefaultNodes);

    // The placement with the best chance. A placement solve can prove wins outright, at 100%.
    PcAdvice advise(const PcQuery& query, const std::atomic<bool>* cancel = nullptr, const PcAdviceOptions& options = {});

    // The share of `samples` random futures that a rollout clears after this placement.
    double chanceAfter(const PcQuery& query, const PcStep& step, int samples, uint32_t seed, int nodesPerSolve);

private:
    PcStatus rollout(PcQuery state, const std::vector<core::Piece>& future, size_t window, uint32_t seed, const std::atomic<bool>* cancel, int nodes);

    struct State {
        std::array<uint16_t, kPcMaxRows> rows{};  // a bit per column, floor first
        std::array<int8_t, core::kBoardWidth> columns{};  // cells in each column, which clears keep in step
    };
    struct Key {
        std::array<uint16_t, kPcMaxRows> rows;
        uint8_t next, current, hold, bag;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const;
    };
    struct Candidate {
        std::array<core::Cell, 4> cells;
        uint32_t id;
    };

    PcResult run(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes, bool unseen);
    bool search(const State& state, int lines, int placementsLeft, size_t next, core::Piece current, core::Piece hold, bool canHold, uint8_t bag);
    bool draw(const State& state, int lines, int placementsLeft, size_t next, core::Piece hold, uint8_t bag);
    bool place(const State& state, int lines, int placementsLeft, size_t next, core::Piece piece, core::Piece hold, bool pressedHold, uint8_t bag);
    bool finish(const PcQuery& query, PcResult& result);

    const PcQuery* query_ = nullptr;
    const std::atomic<bool>* cancel_ = nullptr;
    int maxNodes_ = 0, nodes_ = 0;
    bool stopped_ = false;   // out of budget or cancelled
    bool capped_ = false;    // a placement went above kPcMaxRows, which only rows left empty on the board allow
    bool hollow_ = false;    // the board has such rows
    bool toFinish_ = false;  // true for a full solve, false to stop once the known pieces are placed
    bool unseen_ = false;    // draw unseen pieces once the known ones run out
    MoveSearch search_;
    core::Board board_;
    std::unordered_set<Key, KeyHash> failed_;
    std::vector<std::vector<Candidate>> candidates_;  // per depth, since the search below reuses search_
    struct PathStep {
        core::Piece piece;
        bool hold;
        std::array<core::Cell, 4> cells;
    };
    std::vector<PathStep> path_;
    std::vector<core::Piece> drawn_;
};

}  // namespace ai
