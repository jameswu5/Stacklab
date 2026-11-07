// Headless bot games on core, deterministic from a seed. Games run in parallel.
//
//   bot_match versus <games> <seed> <weights A> [weights B]   two bots with League's garbage
//   bot_match sprint <games> <seed> <weights>                 40 lines, timed
//   bot_match survival <games> <seed> <weights> [apm]         garbage at a steady rate until top out
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ai/bot.h"

namespace {

constexpr uint64_t kMaxFrames = 10 * 60 * core::kTicksPerSecond;

// TETRA LEAGUE's options from a real replay; anything it leaves out keeps core's fallback.
core::Ruleset leagueRules() {
    core::Ruleset r;
    r.gravityIncrease = 0.0035;
    r.gravityMargin = 7200;
    r.garbageMargin = 10800;
    r.garbageIncrease = 0.008;
    r.openerPhase = 14;
    r.spinBonuses = core::SpinBonuses::AllMiniPlus;
    r.b2bCharging = true;
    r.b2bChaining = false;
    r.b2bChargeBase = 3;
    r.pcGarbage = 5;
    r.pcB2b = 1;
    r.garbageSpecialBonus = true;
    return r;
}

struct Player {
    core::Game game;
    ai::Bot bot;
    ai::BotChoice choice;
    std::vector<std::vector<core::InputEvent>> plan;
    size_t at = 0;
    int misplaced = 0;  // locks that didn't land where the bot chose
    int sends = 0;      // this player's sends so far, numbered as core numbers them

    Player(const core::Ruleset& rules, uint32_t seed, const ai::BotWeights& weights) : game(rules, core::Handling{}, seed) {
        bot.weights = weights;
    }

    // One tick, starting a new plan when the last one has finished.
    const std::vector<core::LockResult>& tick(std::span<const core::GarbageEvent> garbage) {
        if (at == plan.size()) {
            plan = bot.choose(game, choice) ? choice.inputs : std::vector<std::vector<core::InputEvent>>{{{core::Action::HardDrop, true, 0}}};
            at = 0;
        }
        const auto& locks = game.tick(plan[at++], garbage);
        if (!locks.empty()) {
            if (locks[0].cells != choice.cells) misplaced++;
            at = plan.size();
        }
        return locks;
    }
};

struct Result {
    bool won = false, draw = false;
    double seconds = 0;
    int pieces = 0, attack = 0, misplaced = 0;
};

Result versus(uint32_t seed, const ai::BotWeights& a, const ai::BotWeights& b) {
    const core::Ruleset rules = leagueRules();
    // ponytail: different seeds per side, since two equal bots on one queue mirror each other
    Player players[2] = {{rules, seed * 2 + 1, a}, {rules, seed * 2 + 2, b}};
    std::vector<core::GarbageEvent> inbox[2] = {{{core::GarbageEvent::Target, 1}}, {{core::GarbageEvent::Target, 0}}};
    int received[2] = {0, 0};  // the last of the other's sends each has been given
    std::vector<core::GarbageEvent> mail;
    while (!players[0].game.toppedOut && !players[1].game.toppedOut && players[0].game.frame < kMaxFrames) {
        for (int i = 0; i < 2; i++) {
            mail.swap(inbox[i]);
            inbox[i].clear();
            for (const auto& e : mail)
                if (e.type == core::GarbageEvent::Arrive) received[i] = e.iid;
            for (const core::LockResult& lock : players[i].tick(mail)) {
                for (int amount : lock.sent) {
                    if (amount == 0) continue;
                    const int iid = ++players[i].sends;
                    inbox[1 - i].push_back({core::GarbageEvent::Arrive, i, iid, received[i], amount});
                    inbox[1 - i].push_back({core::GarbageEvent::Confirm, i, iid});
                }
            }
            mail.clear();
        }
    }
    const core::Game& g = players[0].game;
    Result r;
    r.draw = g.toppedOut == players[1].game.toppedOut;
    r.won = !g.toppedOut && players[1].game.toppedOut;
    r.seconds = g.seconds();
    r.pieces = g.stats.pieces;
    r.attack = g.stats.attack;
    r.misplaced = players[0].misplaced + players[1].misplaced;
    return r;
}

// Sprint without `apm`, survival with it: `apm` lines a minute arrive, 4 at a time.
Result solo(uint32_t seed, const ai::BotWeights& weights, int apm) {
    const core::Ruleset rules = apm ? leagueRules() : core::Ruleset{};
    Player p(rules, seed, weights);
    const uint64_t every = apm ? 4 * 60 * core::kTicksPerSecond / apm : 0;
    int iid = 0;
    while (!p.game.toppedOut && p.game.frame < kMaxFrames && (apm || p.game.stats.lines < 40)) {
        std::vector<core::GarbageEvent> garbage;
        if (apm && p.game.frame % every == every - 1) {
            iid++;
            garbage = {{core::GarbageEvent::Arrive, 1, iid, 0, 4}, {core::GarbageEvent::Confirm, 1, iid}};
        }
        p.tick(garbage);
    }
    Result r;
    r.won = apm ? !p.game.toppedOut : p.game.stats.lines >= 40;
    r.seconds = p.game.seconds();
    r.pieces = p.game.stats.pieces;
    r.attack = p.game.stats.attack;
    r.misplaced = p.misplaced;
    return r;
}

ai::BotWeights load(const char* path) {
    ai::BotWeights w;
    if (!ai::loadBotWeights(path, w)) {
        std::fprintf(stderr, "can't read weights from %s\n", path);
        std::exit(1);
    }
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: bot_match versus|sprint|survival <games> <seed> <weights> [weights B | apm]\n");
        return 1;
    }
    const std::string mode = argv[1];
    const int games = std::atoi(argv[2]);
    const uint32_t seed = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const ai::BotWeights a = load(argv[4]);
    const ai::BotWeights b = mode == "versus" && argc > 5 ? load(argv[5]) : a;
    const int apm = mode == "survival" ? (argc > 5 ? std::atoi(argv[5]) : 60) : 0;
    if (mode != "versus" && mode != "sprint" && mode != "survival") return 1;

    std::vector<Result> results(games);
    std::atomic<int> nextGame = 0;
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); t++) {
        threads.emplace_back([&] {
            for (int i; (i = nextGame++) < games;) results[i] = mode == "versus" ? versus(seed + i, a, b) : solo(seed + i, a, apm);
        });
    }
    for (auto& t : threads) t.join();

    double seconds = 0, best = 1e9;
    int pieces = 0, attack = 0, wins = 0, draws = 0, misplaced = 0;
    for (const Result& r : results) {
        seconds += r.seconds;
        pieces += r.pieces;
        attack += r.attack;
        wins += r.won;
        draws += r.draw;
        misplaced += r.misplaced;
        if (r.won) best = std::min(best, r.seconds);
    }
    // In versus these are bot A's; misplaced counts both bots.
    std::printf("%s, %d games from seed %u\n", mode.c_str(), games, seed);
    std::printf("PPS %.2f  APP %.3f  APM %.1f  pieces %d  misplaced %d\n", pieces / seconds, static_cast<double>(attack) / pieces,
                attack / seconds * 60, pieces, misplaced);
    if (mode == "versus") std::printf("A won %d, lost %d, drew %d (win rate %.1f%%)\n", wins, games - wins - draws, draws, 100.0 * wins / games);
    if (mode == "sprint") std::printf("finished %d of %d, mean %.2f s, best %.2f s\n", wins, games, seconds / games, best);
    if (mode == "survival") std::printf("%d APM incoming: survived %d of %d to %.0f s, mean %.1f s\n", apm, wins, games, kMaxFrames / 60.0, seconds / games);
}
