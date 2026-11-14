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

constexpr const char* kBindKeys[kBindCount] = {"left", "right", "softDrop", "hardDrop", "cw", "ccw", "180", "hold", "retry", "menu", "undo", "hint", "bag", "setup"};
constexpr const char* kBindNames[kBindCount] = {"Move left", "Move right", "Soft drop", "Hard drop", "Rotate CW", "Rotate CCW", "Rotate 180", "Hold", "Retry", "Menu", "Undo", "PC hint", "Bag residue", "Setup hint"};

// A missing or damaged file reads as null, which leaves the defaults in place.
json readJson(const std::string& path) {
    std::ifstream in(path);
    return json::parse(in, nullptr, false);
}

// Written via a rename, so a crash can't leave a half-written file behind.
void writeJson(const json& value, const std::string& path, int indent = 2) {
    std::string temp = path + ".tmp";
    std::ofstream(temp) << value.dump(indent);
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
    profile.handling.safelock = get(handling, "safelock", profile.handling.safelock);
    profile.handling.irsHold = get(handling, "irs", std::string(profile.handling.irsHold ? "hold" : "tap")) == "hold";
    profile.handling.ihsHold = get(handling, "ihs", std::string(profile.handling.ihsHold ? "hold" : "tap")) == "hold";
    json keys = get(value, "keys", json::object());
    for (int bind = 0; bind < kBindCount; bind++) {
        if (!keys.contains(kBindKeys[bind])) continue;
        profile.keys[bind].clear();
        for (int code : get(keys, kBindKeys[bind], std::vector<int>{}))
            if (code > 0 && code < SDL_SCANCODE_COUNT) profile.keys[bind].push_back(static_cast<SDL_Scancode>(code));
    }
    // A file from before these existed keeps its keys: their defaults can't take one.
    for (Bind added : {BindUndo, BindHint, BindBag, BindSetup}) {
        if (keys.contains(kBindKeys[added])) continue;
        for (int bind = 0; bind < kBindCount; bind++)
            for (SDL_Scancode key : profile.keys[bind])
                if (bind != added) std::erase(profile.keys[added], key);
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
    settings.botPreset = get(file, "botPreset", settings.botPreset);
    const json versus = get(file, "versus", json::object());
    VersusOptions& v = settings.versus;
    v.delayMs = std::clamp(get(versus, "delayMs", v.delayMs), 0, 1000);
    v.toYou = get(versus, "garbageToYou", v.toYou);
    v.toBot = get(versus, "garbageToBot", v.toBot);
    v.multiplier = std::clamp(get(versus, "multiplier", v.multiplier), 0.1, 3.0);
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
            {"handling", {{"das", handling.das}, {"arr", handling.arr}, {"sdf", handling.sdf}, {"dcd", handling.dcd}, {"safelock", handling.safelock}, {"irs", handling.irsHold ? "hold" : "tap"}, {"ihs", handling.ihsHold ? "hold" : "tap"}}},
            {"keys", keys},
        });
    }
    const VersusOptions& v = settings.versus;
    writeJson({{"vsync", settings.vsync}, {"overlay", settings.overlay}, {"dark", settings.dark}, {"botPreset", settings.botPreset},
               {"versus", {{"delayMs", v.delayMs}, {"garbageToYou", v.toYou}, {"garbageToBot", v.toBot}, {"multiplier", v.multiplier}}},
               {"active", settings.active}, {"profiles", profiles}}, path);
}

void bindKey(Profile& profile, Bind bind, SDL_Scancode key) {
    for (auto& keys : profile.keys) std::erase(keys, key);
    profile.keys[bind].push_back(key);
}

double loadBest(const std::string& path) {
    json file = readJson(path);
    // Files from before splits were dropped hold the time per line, ending with the finish.
    std::vector<double> splits = get(file, "splits", std::vector<double>{});
    return get(file, "seconds", splits.empty() ? 0.0 : splits.back());
}

void saveBest(double seconds, const std::string& path) {
    writeJson({{"seconds", seconds}}, path);
}

PcRecord loadPcRecord(const std::string& path) {
    json file = readJson(path);
    PcRecord record;
    record.best = get(file, "best", 0);
    const auto attempts = get(file, "attempts", std::vector<int>{}), successes = get(file, "successes", std::vector<int>{});
    if (attempts.size() != 7 || successes.size() != 7) return record;
    std::ranges::copy(attempts, record.tally.attempts.begin());
    std::ranges::copy(successes, record.tally.successes.begin());
    return record;
}

void savePcRecord(const PcRecord& record, const std::string& path) {
    writeJson({{"best", record.best}, {"attempts", record.tally.attempts}, {"successes", record.tally.successes}}, path);
}

std::string rowLabel(int row) {
    if (row >= RowBindFirst && row < RowVsync) return kBindNames[row - RowBindFirst];
    switch (row) {
        case RowProfile: return "Profile";
        case RowDas: return "DAS (frames)";
        case RowArr: return "ARR (frames)";
        case RowSdf: return "SDF";
        case RowDcd: return "DCD (frames)";
        case RowSafelock: return "Safelock";
        case RowIrs: return "IRS";
        case RowIhs: return "IHS";
        case RowVsync: return "VSync";
        case RowOverlay: return "Debug overlay";
        case RowTheme: return "Theme";
        case RowBotDelay: return "Bot reaction to garbage";
        case RowToYou: return "Garbage to you";
        case RowToBot: return "Garbage to the bot";
        case RowMultiplier: return "Attack multiplier";
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
        case RowSafelock: return handling.safelock ? "On" : "Off";
        case RowIrs: return handling.irsHold ? "Hold" : "Tap";
        case RowIhs: return handling.ihsHold ? "Hold" : "Tap";
        case RowVsync: return settings.vsync ? "On" : "Off";
        case RowOverlay: return settings.overlay ? "On" : "Off";
        case RowTheme: return settings.dark ? "Dark" : "Light";
        case RowBotDelay: std::snprintf(buffer, sizeof buffer, "%d ms", settings.versus.delayMs); return buffer;
        case RowToYou: return settings.versus.toYou ? "On" : "Off";
        case RowToBot: return settings.versus.toBot ? "On" : "Off";
        case RowMultiplier: std::snprintf(buffer, sizeof buffer, "%.1fx", settings.versus.multiplier); return buffer;
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
        case RowSafelock: handling.safelock = !handling.safelock; break;
        case RowIrs: handling.irsHold = !handling.irsHold; break;
        case RowIhs: handling.ihsHold = !handling.ihsHold; break;
        case RowVsync: settings.vsync = !settings.vsync; break;
        case RowOverlay: settings.overlay = !settings.overlay; break;
        case RowTheme: settings.dark = !settings.dark; break;
        case RowBotDelay: settings.versus.delayMs = std::clamp(settings.versus.delayMs + dir * 50, 0, 1000); break;
        case RowToYou: settings.versus.toYou = !settings.versus.toYou; break;
        case RowToBot: settings.versus.toBot = !settings.versus.toBot; break;
        case RowMultiplier: step(settings.versus.multiplier, 0.1, 0.1, 3); break;
    }
}

namespace {

json inputsJson(std::span<const uint64_t> ticks, std::span<const core::InputEvent> events) {
    json inputs = json::array();
    for (size_t i = 0; i < events.size(); i++) {
        const core::InputEvent& e = events[i];
        inputs.push_back({ticks[i], static_cast<int>(e.action), e.pressed, e.subframe, e.hoisted});
    }
    return inputs;
}

void inputsFromJson(const json& inputs, std::vector<uint64_t>& ticks, std::vector<core::InputEvent>& events) {
    ticks.clear();
    events.clear();
    for (const json& input : inputs) {
        ticks.push_back(input.at(0).get<uint64_t>());
        events.push_back({static_cast<core::Action>(input.at(1).get<int>()), input.at(2).get<bool>(), input.at(3).get<double>(), input.at(4).get<bool>()});
    }
}

json replayJson(const Replay& replay) {
    const core::Handling& h = replay.handling;
    json file = {
        {"version", 2},  // Dig 18 has refilled from 10 rows since 2
        {"mode", replay.mode},
        {"residue", replay.residue},
        {"seed", replay.seed},
        {"handling", {{"arr", h.arr}, {"das", h.das}, {"dcd", h.dcd}, {"sdf", h.sdf}, {"safelock", h.safelock}, {"cancel", h.cancel}, {"may20g", h.may20g}, {"irsHold", h.irsHold}, {"ihsHold", h.ihsHold}}},
        {"inputs", inputsJson(replay.ticks, replay.inputs)},
    };
    if (!replay.botPreset.empty()) {
        const VersusOptions& v = replay.versus;
        file["bot"] = {
            {"preset", replay.botPreset},
            {"versus", {{"delayMs", v.delayMs}, {"garbageToYou", v.toYou}, {"garbageToBot", v.toBot}, {"multiplier", v.multiplier}}},
            {"inputs", inputsJson(replay.botTicks, replay.botInputs)},
        };
    }
    return file;
}

// Throws json::exception on a file that doesn't fit.
void replayFromJson(const json& file, Replay& replay) {
    replay.mode = file.at("mode").get<std::string>();
    replay.residue = file.at("residue").get<int>();
    replay.seed = file.at("seed").get<uint32_t>();
    const json& h = file.at("handling");
    replay.handling = {h.at("arr"), h.at("das"), h.at("dcd"), h.at("sdf"), h.at("safelock"), h.at("cancel"), h.at("may20g"), h.at("irsHold"), h.at("ihsHold")};
    inputsFromJson(file.at("inputs"), replay.ticks, replay.inputs);
    replay.botPreset.clear();
    replay.botTicks.clear();
    replay.botInputs.clear();
    if (!file.contains("bot")) return;
    const json& bot = file.at("bot");
    const json& v = bot.at("versus");
    replay.botPreset = bot.at("preset").get<std::string>();
    replay.versus = {v.at("delayMs").get<int>(), v.at("garbageToYou").get<bool>(), v.at("garbageToBot").get<bool>(), v.at("multiplier").get<double>()};
    inputsFromJson(bot.at("inputs"), replay.botTicks, replay.botInputs);
}

void writeReplayFile(const json& file, const std::string& path) {
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
    writeJson(file, path, -1);
}

}  // namespace

bool loadReplay(const std::string& path, Replay& replay) {
    const json file = readJson(path);
    if (!file.is_object() || !file.contains("inputs")) return false;
    try {
        replayFromJson(file, replay);
    } catch (const json::exception&) {
        return false;
    }
    // Dig 18 used to start with all 18 rows on the board, so its old runs no longer play back.
    return !(replay.mode == "Dig 18" && get(file, "version", 1) < 2);
}

void saveReplay(const Replay& replay, const std::string& path) { writeReplayFile(replayJson(replay), path); }

bool loadMatch(const std::string& path, std::vector<Replay>& rounds) {
    const json file = readJson(path);
    if (!file.is_object() || !file.contains("rounds")) return false;
    try {
        rounds.assign(file.at("rounds").size(), {});
        for (size_t i = 0; i < rounds.size(); i++) replayFromJson(file.at("rounds").at(i), rounds[i]);
    } catch (const json::exception&) {
        return false;
    }
    return !rounds.empty();
}

void saveMatch(std::span<const Replay> rounds, const std::string& path) {
    json file = {{"version", 1}, {"rounds", json::array()}};
    for (const Replay& round : rounds) file["rounds"].push_back(replayJson(round));
    writeReplayFile(file, path);
}
