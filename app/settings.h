// Saved settings and the 40L personal best, as JSON under SDL_GetPrefPath.
#pragma once

#include <SDL3/SDL_scancode.h>

#include <array>
#include <string>
#include <vector>

#include "core/game.h"

// The first eight match core::Action.
enum Bind { BindLeft, BindRight, BindSoftDrop, BindHardDrop, BindCW, BindCCW, Bind180, BindHold, BindRetry, BindMenu, kBindCount };

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
    }};
};

struct Settings {
    std::vector<Profile> profiles = {Profile{}};
    int active = 0;
    bool vsync = false;
    bool overlay = false;
    bool dark = false;

    Profile& profile() { return profiles[active]; }
    const Profile& profile() const { return profiles[active]; }
};

Settings loadSettings(const std::string& path);
void saveSettings(const Settings& settings, const std::string& path);

// Binds the key to this action only, and clears it from any other.
void bindKey(Profile& profile, Bind bind, SDL_Scancode key);

// Time each line was reached in the personal best run. Index 0 is unused, and
// splits[40] == 0 means there is no personal best yet.
using Splits = std::array<double, 41>;
Splits loadBest(const std::string& path);
void saveBest(const Splits& splits, const std::string& path);

// The settings screen is a flat list of rows.
enum Row { RowProfile, RowDas, RowArr, RowSdf, RowDcd, RowSafelock, RowIrs, RowIhs, RowBindFirst, RowVsync = RowBindFirst + kBindCount, RowOverlay, RowTheme, kRowCount };

std::string rowLabel(int row);
std::string rowValue(const Settings& settings, int row);
// Left or right on a row: dir is -1 or 1.
void adjustRow(Settings& settings, int row, int dir);
