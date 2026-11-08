// The mode table: one row per menu entry, as plain data that app/play.cpp acts
// on. A submode is another row with different values.
#pragma once

#include <iterator>

#include "core/game.h"

// Which rows a mode's stats card shows, as flags.
enum Stat { StatLines = 1, StatPieces = 2, StatPPS = 4, StatKPP = 8, StatAPM = 16, StatAPP = 32, StatFinesse = 64, StatScore = 128, StatLevel = 256, StatGarbage = 512 };

// What a mode with pcFile does with it.
enum class PcStyle {
    Coach,     // a whole run with the solver beside you
    Practice,  // one PC from a chosen bag residue
    Count,     // a whole run with no solver: the player keeps count of the bag
};

struct Mode {
    const char* name;   // on the menu and in the top bar
    const char* blurb;  // under the name on the menu
    core::Ruleset rules;
    int goalLines = 0;               // reaching this many lines ends the run; 0 is no goal
    const char* bestFile = nullptr;  // holds the personal best, under the pref path; null keeps none
    int stats = StatLines | StatPieces | StatPPS | StatKPP | StatFinesse;  // the stats card's rows
    int timeLimitSeconds = 0;        // reaching this ends the run, and the best is then the highest score
    // Rows of garbage on the starting board, with a new hole every row. With these,
    // only lines that held garbage count toward the goal. This trainer's own garbage
    // isn't TETR.IO's path.
    int garbageRows = 0;
    bool editable = false;  // the mouse edits the board, hold and queue, and topping out clears the board
    bool undo = false;      // the undo key takes back the last piece, clock included
    bool drills = false;    // the menu opens the list of drills, and a run plays the chosen one
    const char* pcFile = nullptr;  // turns on the PC coach, and holds its record under the pref path
    PcStyle pcStyle = PcStyle::Coach;
    bool versus = false;  // you against the bot, first to kVersusRounds rounds

    int goalProgress(const core::Stats& stats) const { return garbageRows > 0 ? stats.garbageCleared : stats.lines; }
    bool bestIsScore() const { return timeLimitSeconds > 0; }
};

// The owner's 40L replays only differ from core's defaults in noSzo. They pass
// tests/replay_parity with these rules, so re-check there if they change.
inline constexpr core::Ruleset k40LRules{.noSzo = true};

// versus-standard: TETRA LEAGUE's options, from the most recent of the owner's three League
// replays. The other two agree on every rule; they differ only in display, countdown and timing
// fields core has no setting for. All three share one seed per round between the two players.
inline constexpr core::Ruleset kVersusRules{
    .gravityIncrease = 0.0035,
    .gravityMargin = 7200,
    .spinBonuses = core::SpinBonuses::AllMiniPlus,
    .b2bChaining = false,
    .b2bCharging = true,
    .b2bChargeBase = 3,
    .pcGarbage = 5,
    .pcB2b = 1,
    .garbageIncrease = 0.008,
    .garbageMargin = 10800,
    .garbageSpecialBonus = true,
    .openerPhase = 14,
};

inline const Mode kModes[] = {
    {"40 Lines", "Clear 40 lines as fast as you can", k40LRules, 40, "best-40l.json"},
    // The owner's Blitz replays pass tests/replay_parity with these rules, score included.
    {.name = "Blitz",
     .blurb = "Score as much as you can in 2 minutes",
     .rules = {.may20g = false, .noSzo = true, .levels = true, .levelSpeed = 0.42, .levelGBase = 0.65, .spinBonuses = core::SpinBonuses::TSpins},
     .bestFile = "best-blitz.json",
     .stats = StatScore | StatLevel | StatLines | StatPieces | StatPPS | StatKPP | StatAPM | StatAPP,
     .timeLimitSeconds = 120},
    {.name = "Dig 10",
     .blurb = "Clear 10 rows of garbage as fast as you can",
     .rules = {.noSzo = true},
     .goalLines = 10,
     .bestFile = "best-dig-10.json",
     .stats = StatGarbage | StatPieces | StatPPS | StatKPP | StatFinesse,
     .garbageRows = 10},
    {.name = "Dig 18",
     .blurb = "Clear 18 rows of garbage as fast as you can",
     .rules = {.noSzo = true},
     .goalLines = 18,
     .bestFile = "best-dig-18.json",
     .stats = StatGarbage | StatPieces | StatPPS | StatKPP | StatFinesse,
     .garbageRows = 18},
    {.name = "Sandbox",
     .blurb = "Mouse paints the board; click hold or the queue to change them",
     .rules = {.noSzo = true},
     .editable = true,
     .undo = true},
    {.name = "Drills",
     .blurb = "Build set stacks, piece by piece",
     .rules = {},
     .undo = true,
     .drills = true},
    {.name = "PC coach",
     .blurb = "40 Lines with a solver beside you, for 10 perfect clears in a run",
     .rules = k40LRules,
     .goalLines = 40,
     .undo = true,
     .pcFile = "pc-coach.json"},
    {.name = "PC practice",
     .blurb = "One PC from the bag residue you pick",
     .rules = k40LRules,
     .undo = true,
     .pcFile = "pc-coach.json",
     .pcStyle = PcStyle::Practice},
    {.name = "Bag count",
     .blurb = "40 Lines with no solver, for 10 perfect clears: keep count of the bag",
     .rules = k40LRules,
     .goalLines = 40,
     .pcFile = "pc-count.json",
     .pcStyle = PcStyle::Count},
    {.name = "Versus",
     .blurb = "You against the bot, first to 7, with League's rules",
     .rules = kVersusRules,
     .stats = 0,
     .versus = true},
};
inline constexpr int kModeCount = static_cast<int>(std::size(kModes));
