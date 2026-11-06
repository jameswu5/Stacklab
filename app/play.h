// A run of one mode: feeds timestamped inputs to the simulation and ends the run.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "app/settings.h"
#include "app/timing.h"
#include "core/game.h"
#include "modes/modes.h"

// ponytail: the owner's replays use stride's shorter Ready-Set-Go, whose length
// isn't recorded or published. Tune this against TETR.IO by feel.
constexpr uint64_t kCountdownNs = 1'000'000'000;

// ponytail: older placements are forgotten past this; a slot costs about 1 KB.
constexpr int kUndoDepth = 256;

struct Play {
    const Mode* mode = &kModes[0];
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
    double best = 0;  // the mode's personal best in seconds, or its score if it's timed; 0 if there is none

    uint64_t lastInputNs = 0, worstInputNs = 0;

    // A ring of the state each placement started from, for modes with undo.
    // Constructed up front for the same reason as `shown`, so play doesn't allocate.
    std::vector<core::Game> undoSlots;
    int undoCount = 0, undoNewest = 0;

    void restart(const core::Handling& handling, uint32_t seed, uint64_t runStartNs);
    void begin();                                       // ends the countdown
    void key(Bind action, bool down, uint64_t timeNs);  // action is one of the first eight
    void stepTo(uint64_t nowNs);                        // runs every tick completed by this time
    void stepShown(uint64_t nowNs);                     // call after stepTo, before drawing `shown`
    double seconds() const { return over ? endSeconds : shown.seconds(); }

    void saveUndo();
    // Back to where the last placement started, with the clock too. A key held
    // through this has to be pressed again.
    void undo(uint64_t timeNs);

    // Sandbox edits, on the real game. Painting skips the falling piece's cells, so it can't end up inside one.
    void paint(int x, int y, core::Piece piece);
    void cycleQueue(int slot);
    void cycleHold();
};
