// Saved settings and personal bests, as JSON under SDL_GetPrefPath.
#pragma once

#include <SDL3/SDL_scancode.h>

#include <array>
#include <span>
#include <string>
#include <vector>

#include "core/game.h"

// The first eight match core::Action.
enum Bind { BindLeft, BindRight, BindSoftDrop, BindHardDrop, BindCW, BindCCW, Bind180, BindHold, BindRetry, BindMenu, BindUndo, BindHint, BindBag, BindSetup, kBindCount };

struct Profile {
    std::string name = "Default";
    core::Handling handling;
    std::array<std::vector<SDL_Scancode>, kBindCount> keys = {{
        {SDL_SCANCODE_LEFT},
        {SDL_SCANCODE_RIGHT},
        {SDL_SCANCODE_DOWN},
        {SDL_SCANCODE_SPACE},
        {SDL_SCANCODE_UP},
        {SDL_SCANCODE_Z},
        {SDL_SCANCODE_A},
        {SDL_SCANCODE_C},
        {SDL_SCANCODE_R},
        {SDL_SCANCODE_ESCAPE},
        {SDL_SCANCODE_Q},
        {SDL_SCANCODE_H},
        {SDL_SCANCODE_B},
        {SDL_SCANCODE_G},
    }};
};

// Versus options from BRIEF.md section 8 that the app applies.
struct VersusOptions {
    int delayMs = 0;                  // the bot sees garbage sent to it this long after
    bool toYou = true, toBot = true;  // garbage on, each way
    double multiplier = 1;            // both sides' attack
};

struct Settings {
    std::vector<Profile> profiles = {Profile{}};
    int active = 0;
    bool vsync = false;
    bool overlay = false;
    bool dark = false;
    VersusOptions versus;
    std::string botPreset = "3 PPS";  // the versus bot, by name

    Profile& profile() { return profiles[active]; }
    const Profile& profile() const { return profiles[active]; }
};

Settings loadSettings(const std::string& path);
void saveSettings(const Settings& settings, const std::string& path);

// Binds the key to this action only, and clears it from any other.
void bindKey(Profile& profile, Bind bind, SDL_Scancode key);

// A mode's personal best, in seconds or as a score, 0 if there is none.
double loadBest(const std::string& path);
void saveBest(double seconds, const std::string& path);
void saveBestScore(int score, const std::string& path);

// The PC coach's attempts and successes, by bag phase.
struct PcTally {
    std::array<int, 7> attempts{}, successes{};
};

struct PcRecord {
    int best = 0;  // the most perfect clears in one run
    PcTally tally;
};

PcRecord loadPcRecord(const std::string& path);
void savePcRecord(const PcRecord& record, const std::string& path);

// A run, as BRIEF.md section 3 says replays are: the mode (which gives the ruleset), what the
// run chose, the seed, handling, and every input with the tick it ran on.
struct Replay {
    std::string mode;  // the mode's name
    int residue = 0;   // PC practice's bag residue
    uint32_t seed = 0;
    core::Handling handling;
    std::vector<uint64_t> ticks;
    std::vector<core::InputEvent> inputs;
    // Versus: the bot's preset by name (empty otherwise), the options, and the inputs the bot
    // played. The bot always has core::Handling's defaults.
    std::string botPreset;
    VersusOptions versus;
    std::vector<uint64_t> botTicks;
    std::vector<core::InputEvent> botInputs;
};

// False if the file can't be read or isn't a replay.
bool loadReplay(const std::string& path, Replay& replay);
void saveReplay(const Replay& replay, const std::string& path);
// A versus match, a replay per round.
bool loadMatch(const std::string& path, std::vector<Replay>& rounds);
void saveMatch(std::span<const Replay> rounds, const std::string& path);

// The settings screen is a flat list of rows.
enum Row { RowProfile, RowDas, RowArr, RowSdf, RowDcd, RowSafelock, RowIrs, RowIhs, RowBindFirst, RowVsync = RowBindFirst + kBindCount, RowOverlay, RowTheme, RowBotDelay, RowToYou, RowToBot, RowMultiplier, kRowCount };

std::string rowLabel(int row);
std::string rowValue(const Settings& settings, int row);
// Left or right on a row: dir is -1 or 1.
void adjustRow(Settings& settings, int row, int dir);
