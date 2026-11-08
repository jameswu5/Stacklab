// The bot's side of a versus match: its game, a worker thread that picks its placements,
// the garbage between it and the player, and the match's rounds. The frame loop never
// waits for the worker or allocates because of it (BRIEF.md section 6).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "app/settings.h"
#include "core/game.h"
#include "modes/modes.h"

constexpr int kVersusRounds = 7;                     // first to this many
constexpr uint64_t kRoundCardNs = 2'000'000'000;     // the round's result, before the next countdown
constexpr uint64_t kCalloutFrames = 90;              // how long a clear's name stays, fading at the end
constexpr int kBotPlanTicks = 320, kBotTickEvents = 4;  // the longest plan the bot plays, its PPS wait included, in ticks of key events

// The last clear worth naming on one side, and when.
struct Callout {
    core::LockResult lock;
    uint64_t frame = 0;
    bool any = false;
};

// One side's totals over the match's rounds so far, for the results.
struct SideTotals {
    int pieces = 0, attack = 0, garbageCleared = 0;
    double seconds = 0;
};

struct Match {
    std::array<int, 2> wins{};  // rounds won: you, then the bot
    int rounds = 0;             // rounds played, draws included
    std::array<SideTotals, 2> totals{};
};

class BotWorker;

class Versus {
public:
    Versus();
    ~Versus();  // joins the worker

    core::Game bot{kVersusRules, core::Handling{}, 1};
    double pps = 1;  // the most pieces a second the bot starts
    VersusOptions options;
    bool waitForBot = false;  // tests: wait for each plan, so a round plays out the same at any load
    Match match;
    bool roundOver = false;
    uint64_t roundEndNs = 0;
    int lastWinner = -1;  // of the round just played: 0 you, 1 the bot, -1 a draw
    std::array<Callout, 2> callouts{};
    std::vector<core::GarbageEvent> toYou, toBot;  // for each side's next tick

    // Both sides start on the same seed, as in League.
    void startRound(uint32_t seed);
    // Call after the player's tick, with its locks: sends them on, then runs the bot's tick.
    void afterYourTick(std::span<const core::LockResult> locks, uint64_t frame);
    // A side topped out: scores the round, and says if that ends the match.
    bool endRound(const core::Game& you, uint64_t nowNs);
    // The VS score, as TETR.IO counts it: attack and garbage cleared, per 100 seconds.
    static double vsScore(int attack, int garbageCleared, double seconds) { return seconds > 0 ? 100 * (attack + garbageCleared) / seconds : 0; }

private:
    void send(int from, std::span<const core::LockResult> locks);
    void note(int side, std::span<const core::LockResult> locks, uint64_t frame);

    std::unique_ptr<BotWorker> worker_;
    uint64_t askId_ = 0;
    bool asking_ = false;
    // The plan being played, a tick of key events at a time.
    std::array<std::array<core::InputEvent, kBotTickEvents>, kBotPlanTicks> plan_{};
    std::array<uint8_t, kBotPlanTicks> planCounts_{};
    int planTicks_ = 0, planAt_ = 0;
    std::array<int, 2> sends_{}, received_{};  // each side's sends so far, and the last of the other's it has been given
    std::array<int, 2> acked_{};               // the last of `received_` passed back to the sender
    std::vector<std::pair<uint64_t, core::GarbageEvent>> held_;  // sent to the bot, and the bot frame it sees them on
};
