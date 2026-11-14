// The mode table: one row per menu entry, as plain data that app/play.cpp acts
// on. A submode is another row with different values.
#pragma once

#include <iterator>
#include <span>

#include "core/game.h"

// Which rows a mode's stats card shows, as flags.
enum Stat { StatLines = 1, StatPieces = 2, StatPPS = 4, StatKPP = 8, StatAPM = 16, StatAPP = 32, StatFinesse = 64, StatGarbage = 128 };

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
    // A cheese race: rows of garbage on the starting board, with a new hole every row. Each
    // garbage line cleared raises a new row from the floor until goalLines rows have been
    // placed, and only lines that held garbage count toward the goal. This trainer's own
    // garbage isn't TETR.IO's path.
    int garbageRows = 0;
    bool undo = false;  // the undo key takes back the last piece, clock included
    const char* pcFile = nullptr;  // turns on the PC coach, and holds its record under the pref path
    PcStyle pcStyle = PcStyle::Coach;
    bool versus = false;  // you against the bot, first to kVersusRounds rounds
    bool lst = false;     // the LST coach (app/lst_coach.h)
    bool drills = false;  // plays the setup drill picked with left and right, from data/four-lol/drills

    int goalProgress(const core::Stats& stats) const { return garbageRows > 0 ? stats.garbageCleared : stats.lines; }
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

// kMenu below takes runs of these by position.
inline const Mode kModes[] = {
    {"40 Lines", "Clear 40 lines as fast as you can", k40LRules, 40, "best-40l.json"},
    // Jstris's cheese race, with at most 10 garbage rows on the board.
    {.name = "Dig 10",
     .blurb = "Clear 10 rows of garbage",
     .rules = {.noSzo = true},
     .goalLines = 10,
     .bestFile = "best-dig-10.json",
     .stats = StatGarbage | StatPieces | StatPPS | StatKPP | StatFinesse,
     .garbageRows = 10},
    {.name = "Dig 18",
     .blurb = "Clear 18 rows of garbage",
     .rules = {.noSzo = true},
     .goalLines = 18,
     .bestFile = "best-dig-18.json",
     .stats = StatGarbage | StatPieces | StatPPS | StatKPP | StatFinesse,
     .garbageRows = 10},
    {.name = "Dig 100",
     .blurb = "Clear 100 rows of garbage",
     .rules = {.noSzo = true},
     .goalLines = 100,
     .bestFile = "best-dig-100.json",
     .stats = StatGarbage | StatPieces | StatPPS | StatKPP | StatFinesse,
     .garbageRows = 10},
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
    {.name = "LST coach",
     .blurb = "Endless LST stacking, with the bot judging each placement",
     .rules = k40LRules,
     .undo = true,
     .lst = true},
    {.name = "Setup drills",
     .blurb = "Build four.lol's openers and DPC setups from a fixed queue",
     .stats = StatPieces | StatPPS | StatKPP | StatFinesse,
     .undo = true,
     .drills = true},
};
inline constexpr int kModeCount = static_cast<int>(std::size(kModes));

// A row on the top menu. With `sub`, it opens a menu listing `modes`, titled with its name.
// Otherwise it starts one of `modes`, picked with left and right if there are several, or
// opens Settings if there are none. A null name or blurb is the picked mode's.
struct MenuRow {
    const char* name;
    const char* blurb;
    std::span<const Mode> modes;
    bool sub = false;

    // The mode Enter starts, or null.
    const Mode* mode(int pick) const { return sub || modes.empty() ? nullptr : &modes[modes.size() > 1 ? pick : 0]; }
};

inline const MenuRow kMenu[] = {
    {nullptr, nullptr, {kModes, 1}},
    {"Dig", nullptr, {kModes + 1, 3}},
    {"Setups", "Openers and stacking setups to learn", {kModes + 8, 2}, true},
    {"Perfect Clear", "A coach, practice from a bag residue, and bag counting", {kModes + 4, 3}, true},
    {nullptr, nullptr, {kModes + 7, 1}},
    {"Settings", "Handling, keys and display", {}},
};
inline constexpr int kMenuCount = static_cast<int>(std::size(kMenu));
