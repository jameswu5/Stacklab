// A 40L run: feeds timestamped inputs to the simulation and tracks splits.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "app/settings.h"
#include "app/timing.h"
#include "core/game.h"

constexpr int kGoalLines = 40;
// ponytail: the owner's replays use stride's shorter Ready-Set-Go, whose length
// isn't recorded or published. Tune this against TETR.IO by feel.
constexpr uint64_t kCountdownNs = 1'000'000'000;

struct Play {
    core::Game game{core::Ruleset{}, core::Handling{}, 1};
    // game stepped through the tick in progress, so inputs appear at once.
    // Constructed, not copied, so it has the capacity that copying into it relies on.
    core::Game shown{core::Ruleset{}, core::Handling{}, 1};
    InputQueue inputs;
    std::array<int, 8> held{};  // keys down per core::Action, since an action can have several keys
    Bind lastMove = BindLeft;   // the most recently pressed of left and right
    uint64_t startNs = 0;
    bool started = false;  // false during the countdown, which ends at startNs
    bool over = false;
    double endSeconds = 0;  // the clock once the run is over

    std::string bestPath;
    Splits best{};
    Splits splits{};
    int splitLines = 0;
    char pace[16] = "-";
    bool paceAhead = false;

    uint64_t lastInputNs = 0, worstInputNs = 0;

    void restart(const core::Handling& handling, uint32_t seed, uint64_t runStartNs);
    void begin();                                       // ends the countdown
    void key(Bind action, bool down, uint64_t timeNs);  // action is one of the first eight
    void stepTo(uint64_t nowNs);                        // runs every tick completed by this time
    void stepShown(uint64_t nowNs);                     // call after stepTo, before drawing `shown`
    double seconds() const { return over ? endSeconds : shown.seconds(); }
};
