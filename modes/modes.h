// The mode table: one row per menu entry, as plain data that app/play.cpp acts
// on. A submode is another row with different values.
#pragma once

#include <iterator>

#include "core/game.h"

// Which rows a mode's stats card shows, as flags.
enum Stat { StatLines = 1, StatPieces = 2, StatPPS = 4, StatKPP = 8, StatAPM = 16, StatAPP = 32, StatFinesse = 64 };

struct Mode {
    const char* name;   // on the menu and in the top bar
    const char* blurb;  // under the name on the menu
    core::Ruleset rules;
    int goalLines;         // reaching this many lines ends the run
    const char* bestFile;  // holds the personal best, under the pref path
    int stats = StatLines | StatPieces | StatPPS | StatKPP | StatFinesse;
};

inline const Mode kModes[] = {
    // The owner's 40L replays only differ from core's defaults in noSzo. They pass
    // tests/replay_parity with these rules, so re-check there if they change.
    {"40 Lines", "Clear 40 lines as fast as you can", {.noSzo = true}, 40, "best-40l.json"},
};
inline constexpr int kModeCount = static_cast<int>(std::size(kModes));
