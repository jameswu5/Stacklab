#include "app/pc_coach.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>

namespace {

// Hands the newest value from one thread to another without either ever waiting: three
// slots, so the writer always has one free and the reader keeps the one it's reading.
template <typename T>
class Latest {
public:
    T& next() { return slots_[write_]; }
    void publish() { write_ = state_.exchange(write_ | kFresh) & kIndex; }
    bool take() {
        if (!(state_.load() & kFresh)) return false;
        read_ = state_.exchange(read_) & kIndex;
        return true;
    }
    T& latest() { return slots_[read_]; }
    std::array<T, 3>& slots() { return slots_; }

private:
    static constexpr int kIndex = 3, kFresh = 4;
    std::array<T, 3> slots_{};
    std::atomic<int> state_{1};
    int write_ = 0, read_ = 2;
};

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

void PcProgress::note(const core::LockResult& lock, int dealt) {
    if (!attempted) tally.attempts[phase]++;
    attempted = true;
    linesInAttempt += lock.lines;
    const bool withinLimit = linesInAttempt <= kPcMaxLines;
    if (!withinLimit) streak = 0;
    if (!lock.perfectClear) return;
    pcs++;
    streak++;
    if (withinLimit) tally.successes[phase]++;
    phase = dealt % 7;
    linesInAttempt = 0;
    attempted = false;
}

PcCoach::PcCoach() : worker_(std::make_unique<PcWorker>()) {
    seen.reserve(1024);  // 40 lines can't take this many pieces, so recording never allocates
}

PcCoach::~PcCoach() = default;

void PcCoach::save() {
    if (saved_) return;
    saved_ = true;
    record.best = std::max(record.best, progress.pcs);
    for (int i = 0; i < 7; i++) {
        record.tally.attempts[i] += progress.tally.attempts[i];
        record.tally.successes[i] += progress.tally.successes[i];
    }
    if (!path.empty()) savePcRecord(record, path);
}

void PcCoach::start(const core::Game& game) {
    save();
    saved_ = false;
    progress = {};
    seen.clear();
    seen.push_back(game.falling.piece);
    lastPieces_ = -1;
    lastStatus_ = ai::PcStatus::Unknown;
    killed_ = hintWanted_ = false;
    ask(game);
}

void PcCoach::rollback(const PcProgress& saved, const core::Game& game) {
    progress = saved;
    killed_ = hintWanted_ = false;
    ask(game);
}

// Everything dealt so far, plus the previews on show. Undo shortens it, and the same
// pieces come again because the queue is restored with the game.
void PcCoach::noteSeen(const core::Game& game) {
    const size_t previews = std::min<size_t>(game.rules.previews, game.queue.size());
    const size_t total = game.stats.pieces + 1 + (game.hold != core::Piece::None) + previews;
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
    PcRequest& request = worker_->next();
    request.id = askId_;
    fillQuery(game, request.query);
    worker_->ask();
}

// Pressing hold changes the question as well as placing a piece does.
void PcCoach::afterTick(std::span<const core::LockResult> locks, const core::Game& game) {
    const int dealtAfter = game.stats.pieces + (game.hold != core::Piece::None);
    for (size_t i = 0; i < locks.size(); i++) progress.note(locks[i], dealtAfter - static_cast<int>(locks.size() - 1 - i));
    if (!locks.empty()) killed_ = hintWanted_ = false;
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
    // Only a placement counts, so not pressing hold, and not going back with undo.
    if (answer.status == ai::PcStatus::Impossible && lastStatus_ != ai::PcStatus::Impossible && lastPieces_ >= 0 && askedPieces_ > lastPieces_) {
        killed_ = true;
        killedLast_ = askedPieces_ == lastPieces_ + 1;
    }
    lastPieces_ = askedPieces_;
    lastStatus_ = answer.status;
}

PcView PcCoach::view(const core::Game& game, int goalLines, bool over) const {
    PcView out;
    out.pcs = progress.pcs;
    out.streak = progress.streak;
    out.bestPcs = std::max(record.best, progress.pcs);
    out.outOfReach = pcGoalOutOfReach(progress.pcs, std::max(goalLines - game.stats.lines, 0));
    out.tally = record.tally;
    if (over) return out;

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
    if (killed_) out.alert = killedLast_ ? "That piece killed it: undo" : "Recent piece killed it: undo";

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
