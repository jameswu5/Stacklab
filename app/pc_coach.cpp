#include "app/pc_coach.h"

#include <algorithm>
#include <bit>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

#include "app/latest.h"

namespace {

struct PcRequest {
    uint64_t id = 0;
    ai::PcQuery query;
    std::span<const PcSetup> setups;  // practice only: those to check
    int residue = 0;
    core::Piece startHold = core::Piece::None;
};

struct SetupAnswer {
    uint64_t id = 0;
    std::bitset<kMaxPcSetups> fits;
};

}  // namespace

class PcWorker {
public:
    PcWorker() {
        // Longer than any preview count, so filling a request doesn't allocate.
        for (PcRequest& request : requests_.slots()) request.query.queue.reserve(32);
        thread_ = std::thread([this] { run(); });
    }
    ~PcWorker() {
        stop_ = true;
        cancel_ = true;
        wake();
        thread_.join();
    }

    PcRequest& next() { return requests_.next(); }

    // Cancels first, so a stale cancel can only ever hit an older request: the
    // worker clears it after taking one, and a request taken before that runs to the end.
    void ask() {
        cancel_ = true;
        requests_.publish();
        wake();
    }

    bool poll(PcAnswer& answer) {
        if (!answers_.take()) return false;
        answer = answers_.latest();
        return true;
    }

    const SetupAnswer* pollSetups() { return setupAnswers_.take() ? &setupAnswers_.latest() : nullptr; }

private:
    void wake() {
        wakes_.fetch_add(1);
        wakes_.notify_one();
    }

    void run() {
        ai::PcSolver solver;
        ai::MoveSearch search;
        uint32_t seenWakes = 0;
        while (true) {
            wakes_.wait(seenWakes);
            seenWakes = wakes_.load();
            if (stop_) return;
            if (!requests_.take()) continue;
            const PcRequest& request = requests_.latest();
            cancel_ = false;
            if (stop_) return;  // its cancel may just have been cleared
            // Setups first: they're quick, and the solver can take a while.
            if (!request.setups.empty()) {
                SetupAnswer& found = setupAnswers_.next();
                found.id = request.id;
                found.fits.reset();
                for (size_t i = 0; i < std::min(request.setups.size(), kMaxPcSetups) && !cancel_; i++) {
                    const PcSetup& setup = request.setups[i];
                    found.fits[i] = pcSetupFits(setup, request.residue, request.startHold, request.query.board) && pcSetupBuildable(setup, request.query, search);
                }
                if (cancel_) continue;
                setupAnswers_.publish();
            }
            const ai::PcResult result = solver.solve(request.query, &cancel_);
            if (cancel_) continue;  // newer request is waiting, and this answer would be dropped
            PcAnswer& answer = answers_.next();
            answer = {request.id, result.status, result.lines};
            if (!result.steps.empty()) {
                answer.hasStep = true;
                answer.piece = result.steps[0].piece;
                answer.hold = result.steps[0].hold;
                answer.cells = result.steps[0].cells;
            }
            answers_.publish();
        }
    }

    Latest<PcRequest> requests_;
    Latest<PcAnswer> answers_;
    Latest<SetupAnswer> setupAnswers_;
    std::atomic<bool> cancel_{false}, stop_{false};
    std::atomic<uint32_t> wakes_{0};
    std::thread thread_;  // last, so everything it uses exists first
};

namespace {

constexpr const char* kNames = " IJLOSTZ";

core::Piece pieceFromLetter(char letter) {
    if (letter == 'X') return core::Piece::Garbage;
    const char* at = letter ? std::strchr(kNames + 1, letter) : nullptr;
    return at ? static_cast<core::Piece>(at - kNames) : core::Piece::None;
}

}  // namespace

std::vector<PcSetup> loadPcSetups(const std::string& path) {
    std::ifstream in(path);
    const nlohmann::json file = nlohmann::json::parse(in, nullptr, false);
    std::vector<PcSetup> setups;
    if (!file.is_array()) return setups;
    try {
        for (const nlohmann::json& entry : file) {
            PcSetup setup;
            setup.residue = entry.at("residue").get<int>();
            setup.name = entry.at("name").get<std::string>();
            setup.chance = entry.at("chance").get<std::string>();
            setup.condition = entry.at("condition").get<std::string>();
            const auto board = entry.at("board").get<std::vector<std::string>>();
            const int rows = static_cast<int>(board.size());
            if (rows < 1 || rows > kSetupRows) return {};
            for (int row = 0; row < rows; row++) {
                if (board[row].size() != 10) return {};
                for (int x = 0; x < 10; x++) setup.cells[(rows - 1 - row) * 10 + x] = pieceFromLetter(board[row][x]);
            }
            for (char letter : entry.at("need").get<std::string>()) setup.need[static_cast<int>(pieceFromLetter(letter))]++;
            for (char letter : entry.at("hold").get<std::string>()) setup.holds |= 1 << static_cast<int>(pieceFromLetter(letter));
            setups.push_back(std::move(setup));
        }
    } catch (const nlohmann::json::exception&) {
        return {};
    }
    return setups;
}

bool pcSetupFits(const PcSetup& setup, int residue, core::Piece startHold, const core::Board& board) {
    if (setup.residue != residue) return false;
    if (setup.holds && !(setup.holds >> static_cast<int>(startHold) & 1)) return false;
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (board.at(x, y) != core::Piece::None && (y >= kSetupRows || (setup.cells[y * 10 + x] != board.at(x, y) && setup.cells[y * 10 + x] != core::Piece::Garbage)))
                return false;
    // ponytail: gray must be on the board already, so setups drawn on carried-over blocks never show from practice's empty start
    for (size_t i = 0; i < setup.cells.size(); i++)
        if (setup.cells[i] == core::Piece::Garbage && board.cells[i] == core::Piece::None) return false;
    return true;
}

namespace {

// A depth-first search over the queue: each piece either takes cells of its own letter that
// it can reach, or swaps with hold.
struct SetupBuild {
    const PcSetup& setup;
    const ai::PcQuery& query;
    ai::MoveSearch& search;
    core::Board board;
    std::array<core::Piece, 16> order{};  // the previews, and the bag's last piece when it's the only one left
    int known = 0;
    uint8_t pool = 0;  // bits by core::Piece: may come after order, in any order

    core::Piece at(int next) const { return next < known ? order[next] : core::Piece::None; }

    // `next` indexes order: the piece after `current`. A current of None is past what's known.
    bool from(core::Piece current, int next, core::Piece hold, bool canHold) {
        std::array<int, 8> cells{};
        for (size_t i = 0; i < setup.cells.size(); i++)
            if (setup.cells[i] != core::Piece::Garbage && board.cells[i] == core::Piece::None) cells[static_cast<int>(setup.cells[i])]++;
        cells[0] = 0;
        if (current == core::Piece::None || std::all_of(cells.begin(), cells.end(), [](int n) { return n == 0; })) {
            // What's still wanted, the setup's pieces and the ones to keep, must be on hand or to come.
            std::array<int, 8> hand{};
            hand[static_cast<int>(hold)]++;
            hand[static_cast<int>(current)]++;
            for (int k = next; k < known; k++) hand[static_cast<int>(order[k])]++;
            for (int piece = 1; piece < 8; piece++)
                if ((cells[piece] + 3) / 4 + setup.need[piece] > hand[piece] + (pool >> piece & 1)) return false;
            return true;
        }
        if (cells[static_cast<int>(current)] > 0) {
            search.run(board, core::spawnFalling(current), query.rules);
            std::array<uint64_t, 8> masks{};  // distinct cell sets, as symmetric rotations repeat them
            int count = 0;
            for (const ai::Placement& placement : search.placements) {
                uint64_t mask = 0;
                bool own = true;
                for (const core::Cell& cell : search.cells(placement)) {
                    const int i = cell.y * 10 + cell.x;
                    own = own && cell.y < kSetupRows && setup.cells[i] == current && board.cells[i] == core::Piece::None;
                    if (own) mask |= uint64_t{1} << i;
                }
                if (own && count < 8 && std::find(masks.begin(), masks.begin() + count, mask) == masks.begin() + count) masks[count++] = mask;
            }
            for (int k = 0; k < count; k++) {
                for (size_t i = 0; i < setup.cells.size(); i++)
                    if (masks[k] >> i & 1) board.cells[i] = current;
                if (from(at(next), next + 1, hold, true)) return true;
                for (size_t i = 0; i < setup.cells.size(); i++)
                    if (masks[k] >> i & 1) board.cells[i] = core::Piece::None;
            }
        }
        if (!canHold) return false;
        if (hold == core::Piece::None) return from(at(next), next + 1, current, false);
        return from(hold, next, current, false);
    }
};

}  // namespace

bool pcSetupBuildable(const PcSetup& setup, const ai::PcQuery& query, ai::MoveSearch& search) {
    SetupBuild build{setup, query, search, query.board};
    for (core::Piece piece : query.queue)
        if (build.known < 15) build.order[build.known++] = piece;
    if (std::popcount(query.bagLeft) == 1) build.order[build.known++] = static_cast<core::Piece>(std::countr_zero(query.bagLeft));
    // ponytail: unordered past what's known, so a setup can fit that some orders of the pool can't build
    build.pool = std::popcount(query.bagLeft) >= 2 ? query.bagLeft : 0xFE;
    return build.from(query.current, 0, query.hold, !query.holdUsed);
}

void PcProgress::note(const core::LockResult& lock, int placed) {
    if (!attempted) tally.attempts[phase]++;
    attempted = true;
    linesInAttempt += lock.lines;
    stage += lock.lines > 0;
    const bool withinLimit = linesInAttempt <= kPcMaxLines;
    if (!withinLimit) streak = 0;
    if (!lock.perfectClear) return;
    pcs++;
    streak++;
    if (withinLimit) tally.successes[phase]++;
    phase = placed % 7;
    linesInAttempt = 0;
    attempted = false;
    residueShown = false;
}

PcCoach::PcCoach() : worker_(std::make_unique<PcWorker>()) {
    seen.reserve(1024);  // 40 lines can't take this many pieces, so recording never allocates
}

PcCoach::~PcCoach() = default;

void PcCoach::save() {
    if (saved_) return;
    saved_ = true;
    if (nullified_) return;
    record.best = std::max(record.best, progress.pcs);
    for (int i = 0; i < 7; i++) {
        record.tally.attempts[i] += progress.tally.attempts[i];
        record.tally.successes[i] += progress.tally.successes[i];
    }
    if (!path.empty()) savePcRecord(record, path);
}

void PcCoach::start(const core::Game& game, std::span<const core::Piece> dealtBefore) {
    save();
    saved_ = false;
    nullified_ = false;
    progress = {};
    placedBefore_ = static_cast<int>(dealtBefore.size()) - (game.hold != core::Piece::None);
    progress.phase = placedBefore_ % 7;
    seen.assign(dealtBefore.begin(), dealtBefore.end());
    seen.push_back(game.falling.piece);
    lastPieces_ = -1;
    lastStatus_ = ai::PcStatus::Unknown;
    killedBy_ = nullptr;
    hintWanted_ = false;
    startHold_ = game.hold;
    attemptStart_ = dealtBefore.size();
    setupAsked_ = false;
    setupShown_ = -1;
    setupFits_.reset();
    setupBroken_ = setupDone_ = false;
    stages_ = 0;
    for (const PcSetup& setup : setups) stages_ = std::max(stages_, setup.residue + 1);
    ask(game);
}

void PcCoach::rollback(const PcProgress& saved, const core::Game& game) {
    progress = saved;
    nullified_ = true;
    killedBy_ = nullptr;
    hintWanted_ = false;
    lastPieces_ = -1;  // the next answer is a new starting point, so it blames nothing
    setupDone_ = setupShown_ >= 0 && setupBuilt(game.board);
    ask(game);
}

// Everything dealt so far, plus the previews on show. Undo shortens it, and the same
// pieces come again because the queue is restored with the game.
void PcCoach::noteSeen(const core::Game& game) {
    const size_t previews = std::min<size_t>(game.rules.previews, game.queue.size());
    const size_t total = placedBefore_ + game.stats.pieces + 1 + (game.hold != core::Piece::None) + previews;
    if (seen.size() > total) seen.resize(total);
    while (seen.size() < total) seen.push_back(game.queue[previews - (total - seen.size())]);
}

void PcCoach::fillQuery(const core::Game& game, ai::PcQuery& query) const {
    query.board = game.board;
    query.current = game.falling.piece;
    query.hold = game.hold;
    query.holdUsed = game.holdUsed;
    const size_t previews = std::min<size_t>(game.rules.previews, game.queue.size());
    query.queue.assign(game.queue.begin(), game.queue.begin() + previews);
    query.bagLeft = ai::pcBagLeft(seen);
    query.rules = game.rules;
    query.maxLines = kPcMaxLines;
}

void PcCoach::ask(const core::Game& game) {
    noteSeen(game);
    askId_++;
    answered_ = false;
    askedPieces_ = game.stats.pieces;
    askedCurrent_ = game.falling.piece;
    askedHold_ = game.hold;
    askedHoldUsed_ = game.holdUsed;
    if (style == PcStyle::Count) return;
    PcRequest& request = worker_->next();
    request.id = askId_;
    fillQuery(game, request.query);
    request.setups = style == PcStyle::Practice || style == PcStyle::Opener ? setups : std::span<const PcSetup>{};
    request.residue = style == PcStyle::Opener ? progress.stage : progress.phase;
    request.startHold = startHold_;
    worker_->ask();
}

// Pressing hold changes the question as well as placing a piece does.
void PcCoach::afterTick(std::span<const core::LockResult> locks, const core::Game& game) {
    const int placedAfter = placedBefore_ + game.stats.pieces;
    for (size_t i = 0; i < locks.size(); i++) progress.note(locks[i], placedAfter - static_cast<int>(locks.size() - 1 - i));
    if (!locks.empty()) {
        killedBy_ = nullptr;
        hintWanted_ = false;
        // An opener's TSD clears the rows its setup stood on: on to the next stage's, or the PC after the last.
        // ponytail: any clear counts, even one that isn't the setup's; the next stage's setups then won't fit
        const bool cleared = std::any_of(locks.begin(), locks.end(), [](const core::LockResult& lock) { return lock.lines > 0; });
        if (style == PcStyle::Opener && cleared && setupShown_ >= 0 && !setupDone_) {
            if (progress.stage < stages_) setupShown_ = -1;
            else setupDone_ = true;
        }
        if (setupShown_ >= 0 && setupBuilt(game.board)) setupDone_ = true;
    }
    if (game.stats.pieces != askedPieces_ || game.falling.piece != askedCurrent_ || game.hold != askedHold_ || game.holdUsed != askedHoldUsed_) ask(game);
}

bool PcCoach::setupBuilt(const core::Board& board) const {
    const PcSetup& setup = setups[setupShown_];
    for (size_t i = 0; i < setup.cells.size(); i++)
        if (setup.cells[i] != core::Piece::None && setup.cells[i] != core::Piece::Garbage && board.cells[i] != setup.cells[i]) return false;
    return true;
}

// The worker checks every setup after each lock or hold. The shown one stays while it can
// still be built; else the next that can, or "Setup broken". Once built, pieces past it
// (and the lines they clear) can't break it.
void PcCoach::takeSetups(const std::bitset<kMaxPcSetups>& fits) {
    setupFits_ = fits;
    const int count = static_cast<int>(std::min(setups.size(), kMaxPcSetups));
    if (setupShown_ < 0 ? setupAsked_ && !setupBroken_ : !setupDone_ && !fits[setupShown_]) {
        const int from = setupShown_;
        setupShown_ = -1;
        for (int k = 1; k <= count && setupShown_ < 0; k++)
            if (fits[(from + k + count) % count]) setupShown_ = (from + k + count) % count;
        setupBroken_ = from >= 0 && setupShown_ < 0;
    }
    setupCount_ = setupNumber_ = 0;
    for (int i = 0; i < count; i++) {
        setupCount_ += fits[i];
        if (i <= setupShown_) setupNumber_ += fits[i];
    }
}

void PcCoach::nextSetup() {
    const int count = static_cast<int>(std::min(setups.size(), kMaxPcSetups));
    const int shown = setupShown_;
    setupShown_ = -1;
    setupAsked_ = true;
    setupBroken_ = setupDone_ = false;
    for (int i = shown + 1; i < count && setupShown_ < 0; i++)
        if (setupFits_[i]) setupShown_ = i;
    if (setupShown_ < 0 && setupFits_.any()) setupAsked_ = false;  // past the last: hidden again
    takeSetups(setupFits_);
}

void PcCoach::poll() {
    PcAnswer answer;
    if (worker_->poll(answer)) take(answer);
    if (const SetupAnswer* found = worker_->pollSetups(); found && found->id == askId_) takeSetups(found->fits);
}

void PcCoach::take(const PcAnswer& answer) {
    if (answer.id != askId_) return;
    answered_ = true;
    answer_ = answer;
    // A placement or a hold, since undo clears lastPieces_ and nothing else asks.
    if (answer.status == ai::PcStatus::Impossible && lastStatus_ != ai::PcStatus::Impossible && lastPieces_ >= 0) {
        killedBy_ = askedPieces_ == lastPieces_ ? "That hold killed it: undo" : askedPieces_ == lastPieces_ + 1 ? "That piece killed it: undo" : "Recent piece killed it: undo";
    }
    lastPieces_ = askedPieces_;
    lastStatus_ = answer.status;
}

bool PcCoach::runDone() const {
    if (progress.pcs >= kPcGoal) return true;
    if (style == PcStyle::Opener) return progress.pcs > 0 || progress.linesInAttempt > kOpenerMaxLines;
    return style == PcStyle::Practice && (progress.pcs > 0 || progress.linesInAttempt > kPcMaxLines);
}

PcView PcCoach::view(const core::Game& game, int goalLines, bool over) const {
    PcView out;
    out.pcs = progress.pcs;
    out.streak = progress.streak;
    out.bestPcs = nullified_ ? record.best : std::max(record.best, progress.pcs);
    out.nullified = nullified_;
    out.outOfReach = goalLines > 0 && pcGoalOutOfReach(progress.pcs, std::max(goalLines - game.stats.lines, 0));
    out.tally = record.tally;
    if (style == PcStyle::Practice || style == PcStyle::Opener) {
        out.goal = 1;
        if (setupShown_ >= 0) {
            out.setup = &setups[setupShown_];
            out.setupGhost = !setupDone_;
            std::snprintf(out.setupLine, sizeof out.setupLine, "%s%s%d of %d", out.setup->chance.c_str(), out.setup->chance.empty() ? "" : ", ", setupNumber_, setupCount_);
        } else if (!setupAsked_) {
            std::snprintf(out.setupLine, sizeof out.setupLine, "%s for a setup", setupKey.c_str());
        } else if (setupBroken_) {
            std::snprintf(out.setupLine, sizeof out.setupLine, "Setup broken");
        } else {
            std::snprintf(out.setupLine, sizeof out.setupLine, setups.empty() ? "No setups file" : "No setup fits");
        }
        if (style == PcStyle::Practice) std::snprintf(out.residue, sizeof out.residue, "%d", placedBefore_ % 7);  // the one chosen, even after its PC
        if (!game.toppedOut) out.outcome = progress.pcs > 0 ? "Perfect clear" : style == PcStyle::Opener ? "No perfect clear" : "Over 4 lines";
    } else if (progress.pcs >= kPcGoal) {
        out.outcome = "10 perfect clears";
    }
    if (style == PcStyle::Count) {
        if (progress.residueShown) std::snprintf(out.residue, sizeof out.residue, "%d", progress.phase);
        else std::snprintf(out.residue, sizeof out.residue, "?");
    }
    if (over) return out;
    if (style == PcStyle::Count) {
        out.status = "Keep count";
        std::snprintf(out.detail, sizeof out.detail, "%s shows the bag residue", bagKey.c_str());
        return out;
    }
    // The solver judges the PC only, so it waits until the opener's setups are built.
    if (!pcPhase()) {
        out.status = "Building";
        std::snprintf(out.detail, sizeof out.detail, "stage %d of %d, then the PC", progress.stage + 1, stages_);
        return out;
    }

    if (!answered_) {
        out.status = "Thinking";
    } else {
        switch (answer_.status) {
            case ai::PcStatus::Found: out.status = "PC ready"; break;
            case ai::PcStatus::NeedMorePieces:
                out.status = "Possible";
                std::snprintf(out.detail, sizeof out.detail, "need more pieces");
                break;
            case ai::PcStatus::Impossible:
                out.status = "No PC";
                if (answer_.lines > 0) std::snprintf(out.detail, sizeof out.detail, "in %d lines with these pieces", answer_.lines);
                else std::snprintf(out.detail, sizeof out.detail, "on this board");
                break;
            case ai::PcStatus::Unknown: out.status = "Can't tell"; break;
        }
    }
    out.alert = killedBy_;

    if (!hintWanted_) {
        std::snprintf(out.hintLine, sizeof out.hintLine, "%s for a hint", hintKey.c_str());
    } else if (!answered_) {
        std::snprintf(out.hintLine, sizeof out.hintLine, "Hint coming");
    } else if (answer_.hasStep) {
        const char name = kNames[static_cast<int>(answer_.piece)];
        if (answer_.hold) std::snprintf(out.hintLine, sizeof out.hintLine, "Hold first, then place the %c", name);
        else std::snprintf(out.hintLine, sizeof out.hintLine, "Place the %c", name);
        out.showHint = true;
        out.hintPiece = answer_.piece;
        out.hintCells = answer_.cells;
    } else {
        std::snprintf(out.hintLine, sizeof out.hintLine, "No hint for this one");
    }
    return out;
}
