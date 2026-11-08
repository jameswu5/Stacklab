#include "app/versus.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

#include "ai/bot.h"
#include "app/latest.h"

namespace {

struct BotRequest {
    uint64_t id = 0;
    double pps = 1;
    core::Game game{kVersusRules, core::Handling{}, 1};  // constructed, so copying into it doesn't allocate
};

// The bot's choice as plain arrays, so passing it back allocates nothing.
struct BotAnswer {
    uint64_t id = 0;
    bool ok = false;  // false: nothing fits, so the piece just drops
    int ticks = 0;
    std::array<uint8_t, kBotPlanTicks> counts{};
    std::array<std::array<core::InputEvent, kBotTickEvents>, kBotPlanTicks> events{};
};

}  // namespace

class BotWorker {
public:
    BotWorker() {
        // y3's settings from the bundled file, or the bot's own fallbacks if it can't be read.
        if (!ai::loadBot(std::string(SDL_GetBasePath()) + "data/bot/default.txt", bot_)) bot_ = {};
        for (BotRequest& request : requests_.slots()) request.game.queue.reserve(64);
        thread_ = std::thread([this] { run(); });
    }
    ~BotWorker() {
        stop_ = true;
        wake();
        thread_.join();
    }

    BotRequest& next() { return requests_.next(); }
    void ask() {
        requests_.publish();
        wake();
    }
    bool poll(BotAnswer& answer) {
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
        uint32_t seenWakes = 0;
        while (true) {
            wakes_.wait(seenWakes);
            seenWakes = wakes_.load();
            if (stop_) return;
            if (!requests_.take()) continue;
            const BotRequest& request = requests_.latest();
            BotAnswer& answer = answers_.next();
            answer.id = request.id;
            bot_.pps = request.pps;  // its plan starts with the wait that keeps to it
            answer.ok = bot_.choose(request.game, choice_) && choice_.inputs.size() <= kBotPlanTicks;
            answer.ticks = answer.ok ? static_cast<int>(choice_.inputs.size()) : 0;
            for (int t = 0; t < answer.ticks; t++) {
                const auto& events = choice_.inputs[t];
                answer.ok = answer.ok && events.size() <= kBotTickEvents;
                answer.counts[t] = static_cast<uint8_t>(std::min<size_t>(events.size(), kBotTickEvents));
                std::copy_n(events.begin(), answer.counts[t], answer.events[t].begin());
            }
            answers_.publish();
        }
    }

    ai::Bot bot_;
    ai::BotChoice choice_;
    Latest<BotRequest> requests_;
    Latest<BotAnswer> answers_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> wakes_{0};
    std::thread thread_;  // last, so everything it uses exists first
};

Versus::Versus() : worker_(std::make_unique<BotWorker>()) {
    toYou.reserve(256);
    toBot.reserve(256);
    held_.reserve(256);
    bot.queue.reserve(64);
}

Versus::~Versus() = default;

void Versus::startRound(uint32_t seed) {
    core::Ruleset rules = kVersusRules;
    rules.garbageMultiplier = options.multiplier;
    bot = core::Game(rules, core::Handling{}, seed);
    toYou.assign({{core::GarbageEvent::Target, 1}});
    toBot.assign({{core::GarbageEvent::Target, 0}});
    held_.clear();
    sends_ = received_ = acked_ = {};
    callouts = {};
    roundOver = false;
    askId_++;  // an answer about the last round is dropped
    asking_ = false;
    planTicks_ = planAt_ = 0;
}

void Versus::send(int from, std::span<const core::LockResult> locks) {
    if (!(from == 0 ? options.toBot : options.toYou)) return;
    // The bot's garbage waits out its reaction delay.
    const uint64_t due = bot.frame + static_cast<uint64_t>(options.delayMs * core::kTicksPerSecond / 1000);
    auto out = [&](core::GarbageEvent e) {
        if (from == 0) held_.push_back({due, e});
        else toYou.push_back(e);
    };
    // Here sends are seen as soon as they're made, so say so at once with an empty Arrive. Core
    // otherwise only learns it from the next real send, and keeps everything sent till then.
    if (received_[from] != acked_[from]) {
        acked_[from] = received_[from];
        out({core::GarbageEvent::Arrive, from, sends_[from], received_[from], 0});
    }
    for (const core::LockResult& lock : locks) {
        for (int amount : lock.sent) {
            if (amount == 0) continue;
            const int iid = ++sends_[from];
            out({core::GarbageEvent::Arrive, from, iid, received_[from], amount});
            out({core::GarbageEvent::Confirm, from, iid});
        }
    }
}

// A clear, a spin or a perfect clear gets named.
void Versus::note(int side, std::span<const core::LockResult> locks, uint64_t frame) {
    for (const core::LockResult& lock : locks)
        if (lock.lines > 0 || lock.spin != core::Spin::None || lock.perfectClear) callouts[side] = {lock, frame, true};
}

void Versus::afterYourTick(std::span<const core::LockResult> locks, uint64_t frame) {
    for (const core::GarbageEvent& e : toYou)
        if (e.type == core::GarbageEvent::Arrive) received_[0] = e.iid;
    toYou.clear();
    note(0, locks, frame);
    send(0, locks);

    // The next piece is planned from the bot's game as it stands.
    if (planAt_ == planTicks_ && !asking_ && !bot.toppedOut) {
        BotRequest& request = worker_->next();
        request.id = ++askId_;
        request.pps = pps;
        request.game = bot;
        worker_->ask();
        asking_ = true;
    }
    if (asking_) {
        BotAnswer answer;
        if (worker_->poll(answer) && answer.id == askId_) {
            asking_ = false;
            planAt_ = 0;
            planTicks_ = answer.ok ? answer.ticks : 1;
            if (answer.ok) {
                planCounts_ = answer.counts;
                plan_ = answer.events;
            } else {
                planCounts_[0] = 1;
                plan_[0][0] = {core::Action::HardDrop, true, 0};
            }
        }
    }
    std::span<const core::InputEvent> inputs;
    if (planAt_ < planTicks_) {
        inputs = std::span(plan_[planAt_].data(), planCounts_[planAt_]);
        planAt_++;
    }
    auto seen = std::find_if(held_.begin(), held_.end(), [&](const auto& h) { return h.first > bot.frame; });
    for (auto h = held_.begin(); h != seen; h++) toBot.push_back(h->second);
    held_.erase(held_.begin(), seen);
    for (const core::GarbageEvent& e : toBot)
        if (e.type == core::GarbageEvent::Arrive) received_[1] = e.iid;
    const auto& locks2 = bot.tick(inputs, toBot);
    toBot.clear();
    if (!locks2.empty()) planAt_ = planTicks_;  // a plan that locked early is done
    note(1, locks2, bot.frame);
    send(1, locks2);
}

bool Versus::endRound(const core::Game& you, uint64_t nowNs) {
    roundOver = true;
    roundEndNs = nowNs;
    lastWinner = you.toppedOut == bot.toppedOut ? -1 : you.toppedOut ? 1 : 0;
    if (lastWinner >= 0) match.wins[lastWinner]++;
    match.rounds++;
    const core::Game* sides[2] = {&you, &bot};
    for (int i = 0; i < 2; i++) {
        SideTotals& t = match.totals[i];
        t.pieces += sides[i]->stats.pieces;
        t.attack += sides[i]->stats.attack;
        t.garbageCleared += sides[i]->stats.garbageCleared;
        t.seconds += sides[i]->seconds();
    }
    return std::max(match.wins[0], match.wins[1]) >= kVersusRounds;
}
