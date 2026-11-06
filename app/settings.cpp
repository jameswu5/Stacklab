#include "app/settings.h"

#include <SDL3/SDL_keyboard.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

constexpr const char* kBindKeys[kBindCount] = {"left", "right", "softDrop", "hardDrop", "cw", "ccw", "180", "hold", "retry", "menu"};
constexpr const char* kBindNames[kBindCount] = {"Move left", "Move right", "Soft drop", "Hard drop", "Rotate CW", "Rotate CCW", "Rotate 180", "Hold", "Retry", "Menu"};

// A missing or damaged file reads as null, which leaves the defaults in place.
json readJson(const std::string& path) {
    std::ifstream in(path);
    return json::parse(in, nullptr, false);
}

// Written via a rename, so a crash can't leave a half-written file behind.
void writeJson(const json& value, const std::string& path) {
    std::string temp = path + ".tmp";
    std::ofstream(temp) << value.dump(2);
    std::error_code ignored;
    std::filesystem::rename(temp, path, ignored);
}

template <typename T>
T get(const json& object, const char* key, T fallback) {
    if (!object.is_object() || !object.contains(key)) return fallback;
    try {
        return object[key].get<T>();
    } catch (const json::exception&) {
        return fallback;
    }
}

Profile profileFromJson(const json& value) {
    Profile profile;
    profile.name = get(value, "name", profile.name);
    json handling = get(value, "handling", json::object());
    profile.handling.das = get(handling, "das", profile.handling.das);
    profile.handling.arr = get(handling, "arr", profile.handling.arr);
    profile.handling.sdf = get(handling, "sdf", profile.handling.sdf);
    profile.handling.dcd = get(handling, "dcd", profile.handling.dcd);
    json keys = get(value, "keys", json::object());
    for (int bind = 0; bind < kBindCount; bind++) {
        if (!keys.contains(kBindKeys[bind])) continue;
        profile.keys[bind].clear();
        for (int code : get(keys, kBindKeys[bind], std::vector<int>{}))
            if (code > 0 && code < SDL_SCANCODE_COUNT) profile.keys[bind].push_back(static_cast<SDL_Scancode>(code));
    }
    return profile;
}

}  // namespace

Settings loadSettings(const std::string& path) {
    json file = readJson(path);
    Settings settings;
    if (!file.is_object()) return settings;
    settings.vsync = get(file, "vsync", settings.vsync);
    settings.overlay = get(file, "overlay", settings.overlay);
    settings.dark = get(file, "dark", settings.dark);
    json profiles = get(file, "profiles", json::array());
    if (profiles.is_array() && !profiles.empty()) {
        settings.profiles.clear();
        for (const json& profile : profiles) settings.profiles.push_back(profileFromJson(profile));
    }
    settings.active = std::clamp(get(file, "active", 0), 0, static_cast<int>(settings.profiles.size()) - 1);
    return settings;
}

void saveSettings(const Settings& settings, const std::string& path) {
    json profiles = json::array();
    for (const Profile& profile : settings.profiles) {
        json keys = json::object();
        for (int bind = 0; bind < kBindCount; bind++) {
            keys[kBindKeys[bind]] = json::array();
            for (SDL_Scancode key : profile.keys[bind]) keys[kBindKeys[bind]].push_back(static_cast<int>(key));
        }
        const core::Handling& handling = profile.handling;
        profiles.push_back({
            {"name", profile.name},
            {"handling", {{"das", handling.das}, {"arr", handling.arr}, {"sdf", handling.sdf}, {"dcd", handling.dcd}}},
            {"keys", keys},
        });
    }
    writeJson({{"vsync", settings.vsync}, {"overlay", settings.overlay}, {"dark", settings.dark}, {"active", settings.active}, {"profiles", profiles}}, path);
}

void bindKey(Profile& profile, Bind bind, SDL_Scancode key) {
    for (auto& keys : profile.keys) std::erase(keys, key);
    profile.keys[bind].push_back(key);
}

Splits loadBest(const std::string& path) {
    json file = readJson(path);
    Splits splits{};
    std::vector<double> saved = get(file, "splits", std::vector<double>{});
    if (saved.size() == splits.size()) std::copy(saved.begin(), saved.end(), splits.begin());
    return splits;
}

void saveBest(const Splits& splits, const std::string& path) {
    writeJson({{"splits", splits}}, path);
}

std::string rowLabel(int row) {
    if (row >= RowBindFirst && row < RowVsync) return kBindNames[row - RowBindFirst];
    switch (row) {
        case RowProfile: return "Profile";
        case RowDas: return "DAS (frames)";
        case RowArr: return "ARR (frames)";
        case RowSdf: return "SDF";
        case RowDcd: return "DCD (frames)";
        case RowVsync: return "VSync";
        case RowOverlay: return "Debug overlay";
        case RowTheme: return "Theme";
    }
    return "";
}

std::string rowValue(const Settings& settings, int row) {
    char buffer[32];
    const core::Handling& handling = settings.profile().handling;
    if (row >= RowBindFirst && row < RowVsync) {
        std::string keys;
        for (SDL_Scancode key : settings.profile().keys[row - RowBindFirst]) keys += (keys.empty() ? "" : ", ") + std::string(SDL_GetScancodeName(key));
        return keys.empty() ? "None" : keys;
    }
    switch (row) {
        case RowProfile: return settings.profile().name;
        case RowDas: std::snprintf(buffer, sizeof buffer, "%.1f", handling.das); return buffer;
        case RowArr: std::snprintf(buffer, sizeof buffer, "%.1f", handling.arr); return buffer;
        case RowSdf:
            if (handling.sdf >= 41) return "Instant";
            std::snprintf(buffer, sizeof buffer, "%.0f", handling.sdf);
            return buffer;
        case RowDcd: std::snprintf(buffer, sizeof buffer, "%.1f", handling.dcd); return buffer;
        case RowVsync: return settings.vsync ? "On" : "Off";
        case RowOverlay: return settings.overlay ? "On" : "Off";
        case RowTheme: return settings.dark ? "Dark" : "Light";
    }
    return "";
}

void adjustRow(Settings& settings, int row, int dir) {
    // Rounded to one decimal, so repeated steps don't pile up floating point error.
    auto step = [dir](double& value, double by, double low, double high) {
        value = std::clamp(std::round((value + dir * by) * 10) / 10, low, high);
    };
    core::Handling& handling = settings.profile().handling;
    int count = static_cast<int>(settings.profiles.size());
    switch (row) {
        case RowProfile: settings.active = (settings.active + dir + count) % count; break;
        case RowDas: step(handling.das, 0.1, 0, 20); break;
        case RowArr: step(handling.arr, 0.1, 0, 5); break;
        case RowSdf: step(handling.sdf, 1, 5, 41); break;
        case RowDcd: step(handling.dcd, 0.1, 0, 20); break;
        case RowVsync: settings.vsync = !settings.vsync; break;
        case RowOverlay: settings.overlay = !settings.overlay; break;
        case RowTheme: settings.dark = !settings.dark; break;
    }
}
