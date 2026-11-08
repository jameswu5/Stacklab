#include "app/pc_coach.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

#include "app/latest.h"

namespace {

struct PcRequest {
    uint64_t id = 0;
    ai::PcQuery query;
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

private:
    void wake() {
        wakes_.fetch_add(1);
        wakes_.notify_one();
    }

    void run() {
        ai::PcSolver solver;
        uint32_t seenWakes = 0;
        while (true) {
            wakes_.wait(seenWakes);
            seenWakes = wakes_.load();
            if (stop_) return;
            if (!requests_.take()) continue;
            const PcRequest& request = requests_.latest();
            cancel_ = false;
            if (stop_) return;  // its cancel may just have been cleared
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
    std::atomic<bool> cancel_{false}, stop_{false};
    std::atomic<uint32_t> wakes_{0};
    std::thread thread_;  // last, so everything it uses exists first
};

void PcProgress::note(const core::LockResult& lock, int placed) {
    if (!attempted) tally.attempts[phase]++;
    attempted = true;
    linesInAttempt += lock.lines;
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
    ask(game);
}

void PcCoach::rollback(const PcProgress& saved, const core::Game& game) {
    progress = saved;
    nullified_ = true;
    killedBy_ = nullptr;
    hintWanted_ = false;
    lastPieces_ = -1;  // the next answer is a new starting point, so it blames nothing
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
    worker_->ask();
}

// Pressing hold changes the question as well as placing a piece does.
void PcCoach::afterTick(std::span<const core::LockResult> locks, const core::Game& game) {
    const int placedAfter = placedBefore_ + game.stats.pieces;
    for (size_t i = 0; i < locks.size(); i++) progress.note(locks[i], placedAfter - static_cast<int>(locks.size() - 1 - i));
    if (!locks.empty()) {
        killedBy_ = nullptr;
        hintWanted_ = false;
    }
    if (game.stats.pieces != askedPieces_ || game.falling.piece != askedCurrent_ || game.hold != askedHold_ || game.holdUsed != askedHoldUsed_) ask(game);
}

void PcCoach::poll() {
    PcAnswer answer;
    if (worker_->poll(answer)) take(answer);
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
    if (style == PcStyle::Practice) {
        out.goal = 1;
        std::snprintf(out.residue, sizeof out.residue, "%d", placedBefore_ % 7);  // the one chosen, even after its PC
        if (!game.toppedOut) out.outcome = progress.pcs > 0 ? "Perfect clear" : "Over 4 lines";
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
        constexpr const char* kNames = " IJLOSTZ";
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
