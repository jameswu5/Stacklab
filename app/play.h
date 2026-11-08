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
#include "app/versus.h"
#include "core/game.h"
#include "modes/drill.h"
#include "modes/modes.h"

// ponytail: the owner's replays use stride's shorter Ready-Set-Go, whose length
// isn't recorded or published. Tune this against TETR.IO by feel.
constexpr uint64_t kCountdownNs = 1'000'000'000;

// ponytail: a run with more inputs than this isn't saved as a replay; raise it if long runs need one.
constexpr size_t kReplayInputs = 1 << 16;

// ponytail: older placements are forgotten past this; a slot costs about 1 KB.
constexpr int kUndoDepth = 256;

// Beside the mode's best file: "best-40l.json" keeps its run in "best-40l-replay.json".
inline std::string bestReplayPath(const std::string& bestPath) { return bestPath.substr(0, bestPath.size() - 5) + "-replay.json"; }

// Starts a game as if `residue` pieces were already placed, with one more carried in
// hold, chosen with `seed`. Hold is empty at residue 0, which starts a plain game.
// Returns the pieces dealt before the falling one, hold's among them.
std::vector<core::Piece> dealResidue(core::Game& game, int residue, uint32_t seed);

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
    bool missed = false;           // paused on the missed board, until retry or undo

    std::unique_ptr<PcCoach> coach;  // made by the first run of a mode with the coach
    int residue = 0;                 // the bag residue PC practice starts from

    std::unique_ptr<Versus> versus;  // made by the first versus match
    double botPps = 1;               // the bot's cap, chosen on the menu

    uint64_t lastInputNs = 0, worstInputNs = 0;

    // This run's inputs, as a replay, and the replay being played back instead of keys, if any.
    Replay recording;
    bool recordingFull = false;
    bool usedUndo = false;
    const Replay* playback = nullptr;
    size_t playbackAt = 0;

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
    // Plays a replay of this mode through the same path, with keys ignored. It saves nothing.
    void startPlayback(const Replay& replay, uint64_t runStartNs);
    // Undo, drill chains, Sandbox's edits and the bot's moves aren't in the inputs, so those runs have none.
    bool replayable() const { return !usedUndo && !recordingFull && !drill && !mode->editable && !mode->versus; }
    std::span<const core::InputEvent> due() const;      // the inputs for the next tick, from keys or the replay
    void nextRound(uint64_t nowNs);                     // versus: the next round of the same match, after its countdown
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
