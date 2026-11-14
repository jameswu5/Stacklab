#include "app/versus.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

#include "ai/bot.h"
#include "app/latest.h"

namespace {

struct BotRequest {
    uint64_t id = 0;
    int preset = 0;
    bool sampled = false;  // play with trBot's settings
    TrBot trBot;
    core::Game game{kVersusRules, core::Handling{}, 1};  // constructed, so copying into it doesn't allocate
};

}  // namespace

class BotWorker {
public:
    explicit BotWorker(const std::vector<BotPreset>& presets) {
        for (const BotPreset& preset : presets) {
            bots_.emplace_back();
            if (!preset.path.empty()) ai::loadBot(preset.path, bots_.back());
            bots_.back().pps = preset.pps;
        }
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
    bool poll(BotPlan& answer) {
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
            if (request.sampled) {
                ai::Bot& bot = bots_[request.preset];
                bot.weights = request.trBot.weights;
                bot.pps = request.trBot.pps;
                bot.mistakes = request.trBot.mistakes;
            }
            BotPlan& answer = answers_.next();
            answer.id = request.id;
            answer.ok = bots_[request.preset].choose(request.game, choice_) && choice_.inputs.size() <= kBotPlanTicks;
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

    std::vector<ai::Bot> bots_;  // one per preset; each plan starts with the wait that keeps to its PPS cap
    ai::BotChoice choice_;
    Latest<BotRequest> requests_;
    Latest<BotPlan> answers_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> wakes_{0};
    std::thread thread_;  // last, so everything it uses exists first
};

std::vector<BotPreset> botPresets() {
    std::vector<BotPreset> presets;
    const std::string dir = std::string(SDL_GetBasePath()) + "data/bot/";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(dir + "presets", error)) {
        ai::Bot bot;
        if (entry.path().extension() == ".txt" && ai::loadBot(entry.path().string(), bot)) presets.push_back({bot.name, entry.path().string(), bot.pps});
    }
    std::sort(presets.begin(), presets.end(), [](const BotPreset& a, const BotPreset& b) { return a.path < b.path; });
    if (presets.empty()) presets.push_back({"Bot", "", 1});  // the bot's own fallbacks
    TrField field;
    ai::Bot base;
    if (!loadTrField(dir + "tr-field.json", field) || !ai::loadBot(dir + "presets/" + kTrBase, base)) return presets;
    // Every 1,000 TR, then every 500 from 20,000, where the ladder's ranks are closer together.
    for (int tr = 1000; tr <= 25000; tr += tr < 20000 ? 1000 : 500) {
        char name[32];
        std::snprintf(name, sizeof name, "TR %gk", tr / 1000.0);
        presets.push_back({name, dir + "presets/" + kTrBase, field.medianPps(tr), tr});
    }
    return presets;
}

Versus::Versus() : presets_(botPresets()) {
    const std::string dir = std::string(SDL_GetBasePath()) + "data/bot/";
    loadTrField(dir + "tr-field.json", field_);
    ai::Bot base;
    if (ai::loadBot(dir + "presets/" + kTrBase, base)) baseWeights_ = base.weights;
    worker_ = std::make_unique<BotWorker>(presets_);
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
    asking_ = hasNext_ = false;
    plan_.ticks = planAt_ = 0;
    playbackAt_ = 0;
    const BotPreset& p = presets_[preset];
    if (p.tr > 0) {
        std::mt19937 rng(seed);
        player = field_.sample(p.tr, rng);
        trBot_ = field_.bot(player, baseWeights_, rng);
        std::snprintf(label, sizeof label, "%s: %.2f PPS, APP %.2f", p.name.c_str(), player.pps, player.app);
    } else {
        std::snprintf(label, sizeof label, "%s", p.name.c_str());
    }
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

    // The next piece is planned as the last one's plan begins, from the game as that plan
    // will leave it, so the bot loses no ticks waiting for its worker.
    if (!playback && !asking_ && !hasNext_ && !bot.toppedOut) {
        BotRequest& request = worker_->next();
        request.id = ++askId_;
        request.preset = preset;
        request.sampled = presets_[preset].tr > 0;
        request.trBot = trBot_;
        request.game = bot;
        for (int t = planAt_; t < plan_.ticks && request.game.tick(std::span(plan_.events[t].data(), plan_.counts[t])).empty(); t++) {}
        foreseenBoard_ = request.game.board;
        foreseenPieces_ = request.game.stats.pieces;
        worker_->ask();
        asking_ = true;
    }
    if (asking_) {
        bool answered;
        while (!(answered = worker_->poll(next_) && next_.id == askId_) && waitForBot) std::this_thread::yield();
        asking_ = !answered;
        hasNext_ = answered;
    }
    if (planAt_ == plan_.ticks && hasNext_) {
        hasNext_ = false;
        // Garbage that rose, or a plan that went astray, leaves a different game: plan again.
        if (bot.board.cells == foreseenBoard_.cells && bot.stats.pieces == foreseenPieces_) {
            plan_ = next_;
            planAt_ = 0;
            if (!plan_.ok) {
                plan_.ticks = plan_.counts[0] = 1;
                plan_.events[0][0] = {core::Action::HardDrop, true, 0};
            }
        }
    }
    std::span<const core::InputEvent> inputs;
    if (playback) {
        size_t end = playbackAt_;
        while (end < playback->botInputs.size() && playback->botTicks[end] <= bot.frame) end++;
        inputs = std::span(playback->botInputs).subspan(playbackAt_, end - playbackAt_);
        playbackAt_ = end;
    } else if (planAt_ < plan_.ticks) {
        inputs = std::span(plan_.events[planAt_].data(), plan_.counts[planAt_]);
        planAt_++;
    }
    for (const core::InputEvent& e : inputs) {
        if (!recording || recording->botInputs.size() == recording->botInputs.capacity()) break;
        recording->botTicks.push_back(bot.frame);
        recording->botInputs.push_back(e);
    }
    auto seen = std::find_if(held_.begin(), held_.end(), [&](const auto& h) { return h.first > bot.frame; });
    for (auto h = held_.begin(); h != seen; h++) toBot.push_back(h->second);
    held_.erase(held_.begin(), seen);
    for (const core::GarbageEvent& e : toBot)
        if (e.type == core::GarbageEvent::Arrive) received_[1] = e.iid;
    const auto& locks2 = bot.tick(inputs, toBot);
    toBot.clear();
    if (!locks2.empty()) planAt_ = plan_.ticks;  // a plan that locked early is done
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
