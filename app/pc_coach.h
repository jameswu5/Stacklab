// The PC coach: asks the solver about each piece on a worker thread, and keeps a run's
// progress towards 10 perfect clears. The frame loop never waits for the worker or
// allocates because of it (BRIEF.md sections 6 and 7).
#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "ai/move_search.h"
#include "ai/pc_solver.h"
#include "app/settings.h"
#include "core/game.h"
#include "modes/modes.h"

constexpr int kPcGoal = 10;  // in one run of 40 lines
// The owner trains 4-line PCs, with 2-line ones allowed, so a 6-line PC reads as "No PC" and ends the streak.
constexpr int kPcMaxLines = 4;
// An opener's TSDs and its PC: Black Tea Cannon's 8-height PC takes 8.
constexpr int kOpenerMaxLines = 10;

// The shortest PC is 2 lines, so 10 PCs can take fewer than 40.
inline bool pcGoalOutOfReach(int pcs, int linesLeft) { return pcs + linesLeft / 2 < kPcGoal; }

// A run's progress. A plain value, so undo can put it back.
struct PcProgress {
    int pcs = 0;
    int streak = 0;           // ends when an attempt spends more than kPcMaxLines lines
    int phase = 0;            // the bag residue: pieces placed before this attempt, modulo 7
    int linesInAttempt = 0;
    int stage = 0;            // locks that cleared lines: an opener's stage
    bool attempted = false;   // a piece has been placed in this attempt
    bool residueShown = false;  // Bag count: the player asked to see this attempt's phase
    PcTally tally;

    // placed counts the pieces placed so far, this one included.
    void note(const core::LockResult& lock, int placed);
};

// A four.lol perfect-clear setup, from data/four-lol/pc-setups.json, which
// tools/four_lol_setups.py makes. The data is for personal use, so it isn't in git.
constexpr int kSetupRows = 6;  // an opener's second TSD stands 6 high; PC setups use 4
struct PcSetup {
    int residue = 0;  // an opener's: its stage, the TSDs or other line clears before it
    std::string name, chance, condition;  // condition is four.lol's text, shown as it is
    std::array<core::Piece, kSetupRows * 10> cells{};  // y up from the floor; Garbage where four.lol draws gray
    std::array<int, 8> need{};    // by core::Piece: pieces the condition says to keep on hand besides the board's
    uint8_t holds = 0;            // a bit per piece the attempt may start with in hold; 0 is any
};

std::vector<PcSetup> loadPcSetups(const std::string& path);  // empty if the file is missing or damaged

// An opener's setups, stage by stage, from data/four-lol/openers/<name>.json (tools/four_lol_openers.py).
struct PcOpener {
    std::string name;
    std::vector<PcSetup> setups;
};

constexpr size_t kMaxPcSetups = 512;  // ponytail: more in the file are ignored; the mirrored file has 361

// The setup is for this residue and start hold, the board's blocks in its rows are its own
// pieces or on its gray, and its gray is all there.
bool pcSetupFits(const PcSetup& setup, int residue, core::Piece startHold, const core::Board& board);

// The setup can still be built from the query's position, in its queue's order with one hold:
// every piece goes to its own cells and can reach them (ai::MoveSearch), or waits in hold,
// and the pieces the condition keeps are on hand at the end. Pieces past the previews are
// unordered: the rest of the bag, or the whole next bag. Line clears aren't played out.
bool pcSetupBuildable(const PcSetup& setup, const ai::PcQuery& query, ai::MoveSearch& search);

// What the stats card shows. A plain value, so drawing can't touch the worker's data.
struct PcView {
    const char* status = "";
    char detail[48] = "";
    const char* alert = nullptr;  // that a placement or a hold killed the PC
    char hintLine[48] = "";
    bool showHint = false;  // the overlay
    core::Piece hintPiece = core::Piece::None;
    std::array<core::Cell, 4> hintCells{};
    char setupLine[48] = "";  // practice: how to ask for a setup, why there's none, or the shown one's chance
    const PcSetup* setup = nullptr;  // drawn on the board, with its name and condition on the card
    bool setupGhost = false;  // until it's built: line clears after that would leave the ghost behind
    int pcs = 0, streak = 0, bestPcs = 0;
    int goal = kPcGoal;
    char residue[2] = "";  // takes the streak's row: the bag residue, or ? until asked for
    const char* outcome = nullptr;  // heads the results in place of "Finished"
    bool outOfReach = false;
    bool nullified = false;  // undo was used, so the run won't be saved
    PcTally tally;  // saved over all runs, for the results
};

// The worker's answer about one request. Plain, so passing it back allocates nothing.
struct PcAnswer {
    uint64_t id = 0;
    ai::PcStatus status = ai::PcStatus::Unknown;
    int lines = 0;
    bool hasStep = false;
    core::Piece piece = core::Piece::None;
    bool hold = false;  // press hold first, to get this piece
    std::array<core::Cell, 4> cells{};
};

class PcWorker;

class PcCoach {
public:
    PcCoach();
    ~PcCoach();  // cancels whatever is running and joins the thread

    PcProgress progress;
    PcRecord record;       // saved over all runs
    std::string path;      // where record is saved; empty saves nothing
    std::string hintKey;   // the hint key's name, to say how to ask
    std::string bagKey;    // likewise for the residue
    std::string setupKey;  // and for a setup
    std::span<const PcSetup> setups;  // empty without the setups file
    PcStyle style = PcStyle::Coach;
    std::vector<core::Piece> seen;  // every piece shown so far, in the order they came

    // A new run: saves the last one. dealtBefore lists pieces the run starts as having
    // already dealt, the one in hold among them, for a run that starts mid-bag.
    void start(const core::Game& game, std::span<const core::Piece> dealtBefore = {});
    void rollback(const PcProgress& saved, const core::Game& game);  // undo
    void afterTick(std::span<const core::LockResult> locks, const core::Game& game);
    void poll();  // takes the worker's latest answer, if any
    void take(const PcAnswer& answer);  // an answer that isn't about the latest question is dropped
    void toggleHint() { hintWanted_ = !hintWanted_; }
    // Practice and Opener: shows the next setup that fits, and none after the last. Allocates nothing.
    void nextSetup();
    void save();  // adds this run to the record, once
    void discard() { saved_ = true; }  // a replay of a run already counted: saves nothing

    // Only what the player can see, as the solver's answers must be fair to that.
    void fillQuery(const core::Game& game, ai::PcQuery& query) const;
    bool answered() const { return answered_; }
    uint64_t askId() const { return askId_; }
    PcView view(const core::Game& game, int goalLines, bool over) const;
    bool runDone() const;  // the badge, or practice's one attempt is spent

private:
    void noteSeen(const core::Game& game);
    void ask(const core::Game& game);
    void takeSetups(const std::bitset<kMaxPcSetups>& fits);
    bool setupBuilt(const core::Board& board) const;
    bool pcPhase() const { return style != PcStyle::Opener || progress.stage >= stages_ || setupDone_; }

    std::unique_ptr<PcWorker> worker_;
    uint64_t askId_ = 0;
    bool answered_ = false;
    PcAnswer answer_;
    int askedPieces_ = -1;  // what the question was about, to know when to ask again
    core::Piece askedCurrent_ = core::Piece::None, askedHold_ = core::Piece::None;
    bool askedHoldUsed_ = false;
    int lastPieces_ = -1;  // the last piece count with an answer, and that answer
    ai::PcStatus lastStatus_ = ai::PcStatus::Unknown;
    const char* killedBy_ = nullptr;  // what to say about the move that killed the PC
    bool hintWanted_ = false;
    bool saved_ = true;
    bool nullified_ = false;  // undo was used in this run
    int placedBefore_ = 0;    // pieces the run starts as having placed
    core::Piece startHold_ = core::Piece::None;
    size_t attemptStart_ = 0;  // into seen: the attempt's first piece dealt
    bool setupAsked_ = false;
    int setupShown_ = -1;  // into setups, or none
    int setupNumber_ = 0, setupCount_ = 0;  // "1 of 3"
    std::bitset<kMaxPcSetups> setupFits_;  // the worker's latest answer
    bool setupBroken_ = false;  // a placement broke the shown setup, and none other fits
    bool setupDone_ = false;    // the shown setup is built, so pieces past it can't break it
    int stages_ = 0;  // Opener: how many stages of setups there are; the PC follows
};
