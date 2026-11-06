// A 40L run: feeds timestamped inputs to the simulation and tracks splits.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "app/settings.h"
#include "app/timing.h"
#include "core/game.h"

constexpr int kGoalLines = 40;

struct Play {
    core::Game game{core::Ruleset{}, core::Handling{}, 1};
    core::Game shown = game;  // game stepped through the tick in progress, so inputs appear at once
    InputQueue inputs;
    std::array<int, 8> held{};  // keys down per core::Action, since an action can have several keys
    uint64_t startNs = 0;
    bool over = false;

    std::string bestPath;
    Splits best{};
    Splits splits{};
    int splitLines = 0;
    char pace[16] = "-";
    bool paceAhead = false;

    uint64_t lastInputNs = 0, worstInputNs = 0;

    void restart(const core::Handling& handling, uint32_t seed, uint64_t timeNs);
    void key(Bind action, bool down, uint64_t timeNs);  // action is one of the first eight
    void stepTo(uint64_t nowNs);                        // runs every tick completed by this time
    void stepShown(uint64_t nowNs);                     // call after stepTo, before drawing `shown`
};
