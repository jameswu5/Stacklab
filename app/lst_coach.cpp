#include "app/lst_coach.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <utility>

#include "ai/bot.h"
#include "ai/lst.h"
#include "app/latest.h"
#include "modes/modes.h"

namespace {

struct LstRequest {
    uint64_t id = 0;
    core::Game game{k40LRules, core::Handling{}, 1};  // constructed, so copying into it doesn't allocate
    // A placement to judge: made from `before`, the position asked about as beforeId.
    bool judge = false;
    uint64_t beforeId = 0;
    core::Game before{k40LRules, core::Handling{}, 1};
    core::Piece piece = core::Piece::None;
    std::array<core::Cell, 4> cells{};
};

bool sameCells(const std::array<core::Cell, 4>& a, const std::array<core::Cell, 4>& b) {
    return std::is_permutation(a.begin(), a.end(), b.begin(), [](const core::Cell& p, const core::Cell& q) { return p.x == q.x && p.y == q.y; });
}

bool alive(const core::Board& board) { return ai::lstAlive(ai::rowsOf(board)); }

}  // namespace

class LstWorker {
public:
    LstWorker() {
        ai::loadBot(std::string(SDL_GetBasePath()) + "data/bot/lst.txt", bot_);
        for (LstRequest& request : requests_.slots()) {
            request.game.queue.reserve(64);
            request.before.queue.reserve(64);
        }
        thread_ = std::thread([this] { run(); });
    }
    ~LstWorker() {
        stop_ = true;
        wake();
        thread_.join();
    }

    LstRequest& next() { return requests_.next(); }
    void ask() {
        requests_.publish();
        wake();
    }
    bool poll(LstAnswer& answer) {
        if (!answers_.take()) return false;
        answer = answers_.latest();
        return true;
    }

private:
    void wake() {
        wakes_.fetch_add(1);
        wakes_.notify_one();
    }

    // Fine within kLstFineMargin of the best first placement, ranked as the bot ranks them:
    // how deep its line went, then how well. The roots are the last choose's, on `before`.
    LstVerdict judge(const LstRequest& request) const {
        const ai::Bot::Root* best = nullptr;
        const ai::Bot::Root* mine = nullptr;
        auto better = [](const ai::Bot::Root* a, const ai::Bot::Root& b) { return !a || b.depth > a->depth || (b.depth == a->depth && b.score > a->score); };
        const core::Game& before = request.before;
        for (const ai::Bot::Root& root : bot_.roots()) {
            if (better(best, root)) best = &root;
            core::Falling falling;
            falling.piece = root.hold ? (before.hold != core::Piece::None ? before.hold : before.queue[0]) : before.falling.piece;
            falling.rot = root.placement.rot;
            falling.x = root.placement.x;
            falling.y = root.placement.y;
            if (falling.piece == request.piece && sameCells(falling.cells(), request.cells) && better(mine, root)) mine = &root;
        }
        if (!best) return LstVerdict::None;
        return mine && mine->depth == best->depth && mine->score >= best->score - kLstFineMargin ? LstVerdict::Fine : LstVerdict::Worse;
    }

    void run() {
        uint32_t seenWakes = 0;
        uint64_t chosen = 0;  // the request whose game the bot's roots are about
        ai::BotChoice choice;
        while (true) {
            wakes_.wait(seenWakes);
            seenWakes = wakes_.load();
            if (stop_) return;
            if (!requests_.take()) continue;
            const LstRequest& request = requests_.latest();
            LstAnswer& answer = answers_.next();
            answer = {request.id};
            if (request.judge) {
                if (chosen != request.beforeId) bot_.choose(request.before, choice, &stop_);
                answer.verdict = judge(request);
            }
            answer.ok = bot_.choose(request.game, choice, &stop_);
            chosen = request.id;
            if (stop_) return;
            answer.hold = choice.hold;
            answer.piece = choice.hold ? (request.game.hold != core::Piece::None ? request.game.hold : request.game.queue[0]) : request.game.falling.piece;
            answer.cells = choice.cells;
            answers_.publish();
        }
    }

    ai::Bot bot_;
    Latest<LstRequest> requests_;
    Latest<LstAnswer> answers_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> wakes_{0};
    std::thread thread_;  // last, so everything it uses exists first
};

LstCoach::LstCoach() : worker_(std::make_unique<LstWorker>()) {}

LstCoach::~LstCoach() = default;

void LstCoach::start(const core::Game& game) {
    progress = {};
    hintWanted_ = false;
    verdict_ = LstVerdict::None;
    judging_ = false;
    spawn_.queue.reserve(game.queue.capacity());
    judged_.queue.reserve(game.queue.capacity());
    spawn_ = game;
    ask(game);
    spawnId_ = askId_;
}

void LstCoach::rollback(const LstProgress& saved, const core::Game& game) {
    progress = saved;
    hintWanted_ = false;
    verdict_ = LstVerdict::None;
    judging_ = false;
    spawn_ = game;
    ask(game);
    spawnId_ = askId_;
}

void LstCoach::afterTick(std::span<const core::LockResult> locks, const core::Game& game) {
    if (locks.empty()) {
        if (game.hold != askedHold_) ask(game);  // a hold: a new hint, but placements are judged from the spawn
        return;
    }
    const bool broken = !alive(game.board);
    for (const core::LockResult& lock : locks) {
        const bool tsd = lock.piece == core::Piece::T && lock.lines == 2 && lock.spin == core::Spin::Normal;
        progress.tsds += tsd;
        if (tsd && std::any_of(lock.cells.begin(), lock.cells.end(), [](const core::Cell& c) { return c.x == ai::kLstWell; })) progress.loops++;
    }
    if (broken) progress.loops = 0;
    verdict_ = alive(spawn_.board) && broken ? LstVerdict::Fatal : LstVerdict::None;
    judging_ = verdict_ == LstVerdict::None && locks.size() == 1;  // ponytail: two locks in one tick go unjudged
    lock_ = locks.back();
    std::swap(judged_, spawn_);
    judgedId_ = spawnId_;
    spawn_ = game;
    hintWanted_ = false;
    ask(game);
    spawnId_ = askId_;
}

void LstCoach::ask(const core::Game& game) {
    askId_++;
    answered_ = false;
    askedHold_ = game.hold;
    LstRequest& request = worker_->next();
    request.id = askId_;
    request.game = game;
    request.judge = judging_;
    if (judging_) {
        request.beforeId = judgedId_;
        request.before = judged_;
        request.piece = lock_.piece;
        request.cells = lock_.cells;
    }
    worker_->ask();
}

void LstCoach::poll() {
    LstAnswer answer;
    if (worker_->poll(answer)) take(answer);
}

void LstCoach::take(const LstAnswer& answer) {
    if (answer.id != askId_) return;
    answered_ = true;
    answer_ = answer;
    if (judging_) {
        verdict_ = answer.verdict;
        judging_ = false;
    }
}

LstView LstCoach::view(const core::Game& game, bool over) const {
    constexpr const char* kNames = " IJLOSTZ";
    LstView out;
    out.tsds = progress.tsds;
    out.loops = progress.loops;
    out.b2b = std::max(game.stats.b2b, 0);
    if (over) return out;
    out.status = !alive(game.board) ? "Broken" : answered_ ? "Alive" : "Thinking";
    out.verdict = verdict_;
    switch (verdict_) {
        case LstVerdict::None: break;
        case LstVerdict::Fine: std::snprintf(out.callout, sizeof out.callout, "Fine"); break;
        case LstVerdict::Worse: std::snprintf(out.callout, sizeof out.callout, "Worse than the best: %s to undo", undoKey.c_str()); break;
        case LstVerdict::Fatal: std::snprintf(out.callout, sizeof out.callout, "Fatal, the loop broke: %s to undo", undoKey.c_str()); break;
    }
    if (!hintWanted_) {
        std::snprintf(out.hintLine, sizeof out.hintLine, "%s for a hint", hintKey.c_str());
    } else if (!answered_) {
        std::snprintf(out.hintLine, sizeof out.hintLine, "Hint coming");
    } else if (answer_.ok) {
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
