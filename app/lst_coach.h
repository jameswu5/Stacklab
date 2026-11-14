// The LST coach: the LST bot (data/bot/lst.txt) thinks about each piece on a worker
// thread, gives its placement as a hint, and judges each of the player's placements
// against its own first choices. The frame loop never waits for the worker or allocates
// because of it, as with the PC coach.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "core/game.h"

// A placement is fine when the bot's best line from it is this close to its best line
// from any placement. ponytail: hand-picked from self-play, where same-depth roots split
// into near-ties (under about 3) and real differences (8 and up); tune by feel.
constexpr float kLstFineMargin = 3;

enum class LstVerdict { None, Fine, Worse, Fatal };

// A run's counts. A plain value, so undo can put it back.
struct LstProgress {
    int tsds = 0;
    int loops = 0;  // T-spin doubles into the well since the loop last broke
};

// What the stats card shows. A plain value, so drawing can't touch the worker's data.
struct LstView {
    const char* status = "";  // Thinking, Alive or Broken
    char callout[48] = "";  // the verdict on the last placement
    LstVerdict verdict = LstVerdict::None;
    char hintLine[48] = "";
    bool showHint = false;
    core::Piece hintPiece = core::Piece::None;
    std::array<core::Cell, 4> hintCells{};
    int tsds = 0, loops = 0, b2b = 0;
};

// The worker's answer. Plain, so passing it back allocates nothing.
struct LstAnswer {
    uint64_t id = 0;
    bool ok = false;  // there's a hint
    bool hold = false;
    core::Piece piece = core::Piece::None;
    std::array<core::Cell, 4> cells{};
    LstVerdict verdict = LstVerdict::None;  // on the lock the question asked to judge
};

class LstWorker;

class LstCoach {
public:
    LstCoach();
    ~LstCoach();  // joins the worker

    LstProgress progress;
    std::string hintKey, undoKey;  // the keys' names, to say how to ask

    void start(const core::Game& game);
    void rollback(const LstProgress& saved, const core::Game& game);  // undo
    void afterTick(std::span<const core::LockResult> locks, const core::Game& game);
    void poll();
    void take(const LstAnswer& answer);  // one that isn't about the latest question is dropped
    void toggleHint() { hintWanted_ = !hintWanted_; }
    bool answered() const { return answered_; }
    LstView view(const core::Game& game, bool over) const;

private:
    void ask(const core::Game& game);

    std::unique_ptr<LstWorker> worker_;
    uint64_t askId_ = 0;
    bool answered_ = false;
    LstAnswer answer_;
    bool hintWanted_ = false;
    core::Piece askedHold_ = core::Piece::None;
    // The position the falling piece spawned in, and the one the last placed piece did, which
    // the worker judges that placement from until it has answered.
    core::Game spawn_{core::Ruleset{}, core::Handling{}, 1}, judged_{core::Ruleset{}, core::Handling{}, 1};
    uint64_t spawnId_ = 0, judgedId_ = 0;  // the questions asked about those positions
    bool judging_ = false;
    core::LockResult lock_;
    LstVerdict verdict_ = LstVerdict::None;
};
