// Saved settings and personal bests, as JSON under SDL_GetPrefPath.
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

// A mode's personal best in seconds, 0 if there is none.
double loadBest(const std::string& path);
void saveBest(double seconds, const std::string& path);

// The settings screen is a flat list of rows.
enum Row { RowProfile, RowDas, RowArr, RowSdf, RowDcd, RowSafelock, RowIrs, RowIhs, RowBindFirst, RowVsync = RowBindFirst + kBindCount, RowOverlay, RowTheme, kRowCount };

std::string rowLabel(int row);
std::string rowValue(const Settings& settings, int row);
// Left or right on a row: dir is -1 or 1.
void adjustRow(Settings& settings, int row, int dir);
