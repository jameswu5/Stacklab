// A run of one mode: feeds timestamped inputs to the simulation and ends the run.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ai/finesse.h"
#include "app/pc_coach.h"
#include "app/settings.h"
#include "app/timing.h"
#include "core/game.h"
#include "modes/drill.h"
#include "modes/modes.h"

// ponytail: the owner's replays use stride's shorter Ready-Set-Go, whose length
// isn't recorded or published. Tune this against TETR.IO by feel.
constexpr uint64_t kCountdownNs = 1'000'000'000;

// ponytail: older placements are forgotten past this; a slot costs about 1 KB.
constexpr int kUndoDepth = 256;

constexpr uint64_t kMissBannerNs = 600'000'000;

struct Play {
    const Mode* mode = &kModes[0];
    core::Game game{core::Ruleset{}, core::Handling{}, 1};
    // game stepped through the tick in progress, so inputs appear at once.
    // Constructed, not copied, so it has the capacity that copying into it relies on.
    core::Game shown{core::Ruleset{}, core::Handling{}, 1};
    ai::Finesse finesse{game};
    ai::FaultStats faults;
    InputQueue inputs;
    std::array<int, 8> held{};  // keys down per core::Action, since an action can have several keys
    Bind lastMove = BindLeft;   // the most recently pressed of left and right
    uint64_t startNs = 0;
    bool started = false;  // false during the countdown, which ends at startNs
    bool over = false;
    double endSeconds = 0;  // the clock once the run is over

    std::string bestPath;
    double best = 0;  // the mode's personal best in seconds, or its score if it's timed; 0 if there is none

    const Drill* drill = nullptr;  // the one being played, if the mode has drills
    uint64_t filled = 0;           // a bit per drill target that's been placed
    uint64_t missedUntilNs = 0;    // says so until then, since a miss resets the board at once

    std::unique_ptr<PcCoach> coach;  // made by the first run of a mode with the coach

    uint64_t lastInputNs = 0, worstInputNs = 0;

    // A ring of the state each placement started from, for modes with undo.
    // Constructed up front for the same reason as `shown`, so play doesn't allocate.
    struct UndoSlot {
        core::Game game;
        uint64_t filled;
        ai::FaultStats faults;
        PcProgress pc;
    };
    std::vector<UndoSlot> undoSlots;
    int undoCount = 0, undoNewest = 0;

    void restart(const core::Handling& handling, uint32_t seed, uint64_t runStartNs);
    void begin();                                       // ends the countdown
    void key(Bind action, bool down, uint64_t timeNs);  // action is one of the first eight
    void stepTo(uint64_t nowNs);                        // runs every tick completed by this time
    void stepShown(uint64_t nowNs);                     // call after stepTo, before drawing `shown`
    double seconds() const { return over ? endSeconds : shown.seconds(); }

    bool judgeDrill(std::span<const core::LockResult> locks, uint64_t nowNs);  // true if the attempt ended
    void saveUndo();
    // Back to where the last placement started, with the clock too. A key held
    // through this has to be pressed again.
    void undo(uint64_t timeNs);

    // Sandbox edits, on the real game. Painting skips the falling piece's cells, so it can't end up inside one.
    void paint(int x, int y, core::Piece piece);
    void cycleQueue(int slot);
    void cycleHold();
};
