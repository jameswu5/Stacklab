// The bot's side of a versus match: its game, a worker thread that picks its placements,
// the garbage between it and the player, and the match's rounds. The frame loop never
// waits for the worker or allocates because of it (BRIEF.md section 6).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "app/settings.h"
#include "app/tr_field.h"
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

// A bot to play: data/bot/presets in file-name order, then players by TR (app/tr_field.h).
struct BotPreset {
    std::string name, path;
    double pps;   // its cap; for a TR, the median player's
    int tr = 0;   // not 0: each round draws a player at this TR, on kTrBase's search
};
inline constexpr const char* kTrBase = "6-pps-3.0.txt";  // in data/bot/presets
std::vector<BotPreset> botPresets();  // at least one

// The bot's choice as plain arrays, so passing it back allocates nothing.
struct BotPlan {
    uint64_t id = 0;
    bool ok = false;  // false: nothing fits, so the piece just drops
    int ticks = 0;
    std::array<uint8_t, kBotPlanTicks> counts{};
    std::array<std::array<core::InputEvent, kBotTickEvents>, kBotPlanTicks> events{};
};

class BotWorker;

class Versus {
public:
    Versus();
    ~Versus();  // joins the worker

    core::Game bot{kVersusRules, core::Handling{}, 1};
    int preset = 0;  // into botPresets()
    TrPlayer player;      // the round's player, for a TR preset
    char label[64] = "";  // the bot's name, with the round's player for a TR preset
    VersusOptions options;
    bool waitForBot = false;  // tests: wait for each plan, so a round plays out the same at any load
    // The bot's inputs are recorded into this replay's bot fields, while there's room reserved,
    // or played back from them in place of the worker's plans.
    Replay* recording = nullptr;
    const Replay* playback = nullptr;
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

    std::vector<BotPreset> presets_;
    TrField field_;
    ai::BotWeights baseWeights_;  // kTrBase's
    TrBot trBot_;                 // the round's settings, for a TR preset
    std::unique_ptr<BotWorker> worker_;
    uint64_t askId_ = 0;
    bool asking_ = false, hasNext_ = false;
    // The plan being played, a tick of key events at a time, and the one for the next piece,
    // asked for as the last began. It's only played if the bot's game came out as foreseen.
    BotPlan plan_, next_;
    int planAt_ = 0;
    size_t playbackAt_ = 0;
    core::Board foreseenBoard_;
    int foreseenPieces_ = 0;
    std::array<int, 2> sends_{}, received_{};  // each side's sends so far, and the last of the other's it has been given
    std::array<int, 2> acked_{};               // the last of `received_` passed back to the sender
    std::vector<std::pair<uint64_t, core::GarbageEvent>> held_;  // sent to the bot, and the bot frame it sees them on
};
