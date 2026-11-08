#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <objc/message.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "app/play.h"
#include "app/settings.h"
#include "app/ui.h"

namespace {

constexpr uint64_t kUncappedFrameNs = 1'000'000'000 / 480;  // with VSync off, how often to poll and draw

enum class Screen { Menu, Drills, Play, Settings };

struct App {
    Settings settings;
    std::string prefPath, settingsPath;
    std::array<int, SDL_SCANCODE_COUNT> binds{};  // scancode to Bind, or -1
    SDL_Renderer* renderer = nullptr;
    Play play;

    Screen screen = Screen::Menu;
    int menuRow = 0;  // a mode, or Settings after the last one
    DrillSet drillSet;
    std::string drillError;  // what couldn't be loaded, or empty
    int drillRow = 0;
    std::array<double, kModeCount> bests{};
    std::vector<RunResult> history;  // this session's ended runs; a retried run didn't end
    int selected = 0;
    bool capturing = false;
    Replay replay;  // the one being played back
    std::vector<RunResult> playbackHistory;  // the session with the replay's run last, for its results
    bool hasBestReplay = false;  // the mode's best run was saved as a replay

    uint64_t frameNs = 0, worstFrameNs = 0;
};

void applyBinds(App& app) {
    app.binds.fill(-1);
    for (int bind = 0; bind < kBindCount; bind++)
        for (SDL_Scancode key : app.settings.profile().keys[bind]) app.binds[key] = bind;
}

// With fewer drawables queued, what reaches the screen was drawn more recently.
void limitDrawableQueue(SDL_Renderer* renderer) {
    void* layer = SDL_GetRenderMetalLayer(renderer);
    if (!layer) return;
    reinterpret_cast<void (*)(void*, SEL, unsigned long)>(objc_msgSend)(layer, sel_registerName("setMaximumDrawableCount:"), 2);
}

void applyPresentMode(App& app) {
    SDL_SetRenderVSync(app.renderer, app.settings.vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
}

void restart(App& app, uint64_t timeNs) {
    app.play.restart(app.settings.profile().handling, static_cast<uint32_t>(SDL_GetPerformanceCounter()), timeNs + kCountdownNs);
}

bool editing(const App& app) { return app.screen == Screen::Play && app.play.mode->editable; }

// The left button paints garbage, and any other erases.
void paintAt(App& app, const Ui& ui, float x, float y, SDL_MouseButtonFlags buttons) {
    int cellX, cellY;
    if (buttons == 0 || !ui.boardCellAt(x, y, cellX, cellY)) return;
    app.play.paint(cellX, cellY, buttons & SDL_BUTTON_LMASK ? core::Piece::Garbage : core::Piece::None);
}

void startRun(App& app, const Drill* drill, uint64_t timeNs) {
    const Mode& mode = kModes[app.menuRow];
    app.play.mode = &mode;
    app.play.drill = drill;
    app.play.bestPath = mode.bestFile ? app.prefPath + mode.bestFile : "";
    app.play.best = app.bests[app.menuRow];
    app.hasBestReplay = mode.bestFile && std::filesystem::exists(bestReplayPath(app.play.bestPath));
    app.screen = Screen::Play;
    restart(app, timeNs);
    if (mode.pcFile) {
        app.play.coach->path = app.prefPath + mode.pcFile;
        app.play.coach->record = loadPcRecord(app.play.coach->path);
        app.play.coach->hintKey = rowValue(app.settings, static_cast<int>(RowBindFirst) + BindHint);
        app.play.coach->bagKey = rowValue(app.settings, static_cast<int>(RowBindFirst) + BindBag);
    }
}

void handleMenuKey(App& app, const SDL_KeyboardEvent& key) {
    if (!key.down) return;
    int rows = kModeCount + 1;
    switch (key.scancode) {
        case SDL_SCANCODE_UP: app.menuRow = (app.menuRow + rows - 1) % rows; break;
        case SDL_SCANCODE_DOWN: app.menuRow = (app.menuRow + 1) % rows; break;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
            if (app.menuRow < kModeCount && kModes[app.menuRow].pcStyle == PcStyle::Practice) app.play.residue = (app.play.residue + (key.scancode == SDL_SCANCODE_LEFT ? 6 : 1)) % 7;
            if (app.menuRow < kModeCount && kModes[app.menuRow].versus) app.play.botPps = std::clamp(app.play.botPps + (key.scancode == SDL_SCANCODE_LEFT ? -0.5 : 0.5), 0.5, 3.0);
            break;
        case SDL_SCANCODE_RETURN:
            if (key.repeat) break;
            if (app.menuRow == kModeCount) {
                app.screen = Screen::Settings;
            } else if (kModes[app.menuRow].drills) {
                app.screen = Screen::Drills;
            } else {
                startRun(app, nullptr, key.timestamp);
            }
            break;
        default: break;
    }
}

void handleDrillsKey(App& app, const SDL_KeyboardEvent& key) {
    if (!key.down) return;
    const auto& drills = app.drillSet.drills;
    int rows = static_cast<int>(drills.size());
    switch (key.scancode) {
        case SDL_SCANCODE_UP:
            if (rows > 0) app.drillRow = (app.drillRow + rows - 1) % rows;
            break;
        case SDL_SCANCODE_DOWN:
            if (rows > 0) app.drillRow = (app.drillRow + 1) % rows;
            break;
        case SDL_SCANCODE_RETURN:
            if (!key.repeat && rows > 0) startRun(app, &drills[app.drillRow], key.timestamp);
            break;
        case SDL_SCANCODE_ESCAPE:
            if (!key.repeat) app.screen = Screen::Menu;
            break;
        default: break;
    }
}

// Fixed keys on the results: P watches this run again, O the mode's best. Only Esc stops a replay.
void handlePlayKey(App& app, const SDL_KeyboardEvent& key) {
    if (key.repeat) return;
    Play& play = app.play;
    if (key.down && play.over && (key.scancode == SDL_SCANCODE_P || key.scancode == SDL_SCANCODE_O)) {
        if (key.scancode == SDL_SCANCODE_P && !play.playback) {
            if (!play.replayable()) return;
            app.replay = play.recording;
        }
        if (key.scancode == SDL_SCANCODE_O && !(app.hasBestReplay && loadReplay(bestReplayPath(play.bestPath), app.replay))) return;
        play.startPlayback(app.replay, key.timestamp + kCountdownNs);
        return;
    }
    if (play.playback && !play.over && app.binds[key.scancode] != BindMenu) return;
    int bind = app.binds[key.scancode];
    if (bind < 0) return;
    if (bind == BindRetry) {
        if (key.down) restart(app, key.timestamp);
    } else if (bind == BindMenu) {
        if (key.down) {
            app.screen = app.play.drill ? Screen::Drills : Screen::Menu;
            app.play.held.fill(0);
            if (app.play.mode->pcFile) app.play.coach->save();
        }
    } else if (bind == BindUndo) {
        if (key.down) app.play.undo(key.timestamp);
    } else if (bind == BindHint) {
        if (key.down && app.play.mode->pcFile) app.play.coach->toggleHint();
    } else if (bind == BindBag) {
        if (key.down && app.play.mode->pcFile) app.play.coach->progress.residueShown = true;
    } else {
        app.play.key(static_cast<Bind>(bind), key.down, key.timestamp);
    }
}

// Fixed keys here, so a bad keybind can't lock anyone out of fixing it.
void handleSettingsKey(App& app, const SDL_KeyboardEvent& key) {
    if (!key.down) return;
    Settings& settings = app.settings;
    bool onBindRow = app.selected >= RowBindFirst && app.selected < RowVsync;
    if (app.capturing) {
        if (key.repeat) return;
        if (key.scancode != SDL_SCANCODE_ESCAPE) {
            bindKey(settings.profile(), static_cast<Bind>(app.selected - RowBindFirst), key.scancode);
            applyBinds(app);
        }
        app.capturing = false;
        return;
    }
    switch (key.scancode) {
        case SDL_SCANCODE_ESCAPE:
            if (key.repeat) break;
            app.screen = Screen::Menu;
            saveSettings(settings, app.settingsPath);
            break;
        case SDL_SCANCODE_UP: app.selected = (app.selected + kRowCount - 1) % kRowCount; break;
        case SDL_SCANCODE_DOWN: app.selected = (app.selected + 1) % kRowCount; break;
        case SDL_SCANCODE_LEFT:
        case SDL_SCANCODE_RIGHT:
            adjustRow(settings, app.selected, key.scancode == SDL_SCANCODE_LEFT ? -1 : 1);
            applyBinds(app);
            applyPresentMode(app);
            break;
        case SDL_SCANCODE_RETURN:
            if (key.repeat) break;
            if (app.selected == RowProfile) {
                Profile copy = settings.profile();
                copy.name = "Profile " + std::to_string(settings.profiles.size() + 1);
                settings.profiles.push_back(copy);
                settings.active = static_cast<int>(settings.profiles.size()) - 1;
                applyBinds(app);
            } else if (onBindRow) {
                app.capturing = true;
            }
            break;
        case SDL_SCANCODE_BACKSPACE:
            if (onBindRow) {
                settings.profile().keys[app.selected - RowBindFirst].clear();
                applyBinds(app);
            }
            break;
        default: break;
    }
}

// A replay's run isn't recorded again: it only gets a copy of the session to show its results in.
void recordRun(App& app) {
    const Play& play = app.play;
    if (play.playback) {
        app.playbackHistory = app.history;
        app.playbackHistory.push_back({play.mode, play.endSeconds, play.game.stats, play.game.toppedOut, false});
        return;
    }
    double& best = app.bests[app.menuRow];
    app.history.push_back({play.mode, play.endSeconds, play.game.stats, play.game.toppedOut, play.best != best});
    if (play.mode->versus) app.history.back().rounds = play.versus->match.wins;
    if (play.best != best && play.replayable()) app.hasBestReplay = true;
    best = play.best;
    if (!play.replayable()) return;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S-", std::localtime(&now));
    std::string name = play.mode->name;
    for (char& c : name) c = c == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    saveReplay(play.recording, app.prefPath + "replays/" + stamp + name + ".json");
}

void drawFrame(App& app, Ui& ui) {
    const Play& play = app.play;
    ui.dark = app.settings.dark;
    ui.beginFrame();
    if (app.screen == Screen::Menu) {
        ui.drawMenu(app.menuRow, app.bests, app.play.residue, app.play.botPps);
    } else if (app.screen == Screen::Drills) {
        ui.drawDrills(app.drillRow, app.drillSet.drills, app.drillError.empty() ? nullptr : app.drillError.c_str());
    } else if (app.screen == Screen::Settings) {
        ui.drawSettings(app.settings, app.selected, app.capturing);
    } else {
        const char* banner = nullptr;
        std::string hint;
        auto keyName = [&](Bind bind) { return rowValue(app.settings, static_cast<int>(RowBindFirst) + bind); };
        if (play.over && (play.playback || play.replayable())) hint = keyName(BindRetry) + " retry, " + keyName(BindMenu) + " menu, P replay" + (app.hasBestReplay ? ", O best" : "");
        else if (play.over) hint = keyName(BindRetry) + " to retry, " + keyName(BindMenu) + " for the menu";
        else if (play.missed) {
            banner = "Missed";
            hint = keyName(BindRetry) + " to retry, " + keyName(BindUndo) + " to undo";
        }
        else if (!play.started) banner = SDL_GetTicksNS() < play.startNs - kCountdownNs / 2 ? "Ready" : "Set";
        if (play.mode->versus) {
            ui.drawVersus({play.mode, &play.shown, play.versus.get(), play.over, banner, hint.c_str(), app.history});
        } else {
            PcView pcView;
            const PcView* pc = nullptr;
            if (play.mode->pcFile) {
                pcView = play.coach->view(play.shown, play.mode->goalLines, play.over);
                pc = &pcView;
            }
            ui.drawPlay({play.mode, &play.shown, play.seconds(), play.over, banner, hint.c_str(), play.best, play.playback ? app.playbackHistory : app.history, play.drill, play.filled, play.mode->rules.levels ? nullptr : &play.faults, pc});
        }
    }
    if (app.settings.overlay) {
        char frameLine[64], inputLine[64];
        std::snprintf(frameLine, sizeof frameLine, "Frame %.1f ms, worst %.1f", app.frameNs / 1e6, app.worstFrameNs / 1e6);
        std::snprintf(inputLine, sizeof inputLine, "Input to sim %.2f ms, worst %.2f", play.lastInputNs / 1e6, play.worstInputNs / 1e6);
        ui.drawOverlay(frameLine, inputLine, app.settings.vsync ? "VSync on" : "VSync off");
    }
}

}  // namespace

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
        SDL_Log("Couldn't start: %s", SDL_GetError());
        return 1;
    }
    SDL_Window* window = nullptr;
    App app;
    if (!SDL_CreateWindowAndRenderer("Stacklab", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &app.renderer)) {
        SDL_Log("Couldn't create the window: %s", SDL_GetError());
        return 1;
    }
    limitDrawableQueue(app.renderer);

    char* prefPath = SDL_GetPrefPath("Stacklab", "Stacklab");
    app.prefPath = prefPath ? prefPath : "";
    SDL_free(prefPath);
    app.settingsPath = app.prefPath + "settings.json";
    app.settings = loadSettings(app.settingsPath);
    for (int i = 0; i < kModeCount; i++)
        if (kModes[i].bestFile) app.bests[i] = loadBest(app.prefPath + kModes[i].bestFile);
    applyBinds(app);
    applyPresentMode(app);

    // The bundled drills, and any the owner adds under the pref path.
    const std::filesystem::path drillDirs[] = {std::filesystem::path(SDL_GetBasePath()) / "data/drills", std::filesystem::path(app.prefPath) / "drills"};
    app.drillSet = loadDrills(drillDirs);
    for (const std::string& error : app.drillSet.errors) SDL_Log("Drill: %s", error.c_str());
    if (!app.drillSet.errors.empty()) {
        app.drillError = app.drillSet.errors[0].substr(0, 100);
        if (app.drillSet.errors.size() > 1) app.drillError += " (+" + std::to_string(app.drillSet.errors.size() - 1) + " more)";
    }

    Ui ui(app.renderer, std::string(SDL_GetBasePath()) + "assets/fonts/");
    uint64_t frameStart = SDL_GetTicksNS(), worstWindowStart = frameStart;

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    if (app.screen == Screen::Menu) handleMenuKey(app, event.key);
                    else if (app.screen == Screen::Drills) handleDrillsKey(app, event.key);
                    else if (app.screen == Screen::Settings) handleSettingsKey(app, event.key);
                    else handlePlayKey(app, event.key);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    SDL_ConvertEventToRenderCoordinates(app.renderer, &event);
                    if (ui.toggleHit(event.button.x, event.button.y)) app.settings.dark = !app.settings.dark;
                    if (editing(app)) {
                        int slot = ui.queueSlotAt(event.button.x, event.button.y);
                        if (slot >= 0) app.play.cycleQueue(slot);
                        if (ui.holdHit(event.button.x, event.button.y)) app.play.cycleHold();
                        paintAt(app, ui, event.button.x, event.button.y, SDL_BUTTON_MASK(event.button.button));
                    }
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    SDL_ConvertEventToRenderCoordinates(app.renderer, &event);
                    if (editing(app)) paintAt(app, ui, event.motion.x, event.motion.y, event.motion.state);
                    break;
                default: break;
            }
        }

        if (app.screen == Screen::Play) {
            uint64_t now = SDL_GetTicksNS();
            bool wasOver = app.play.over;
            app.play.stepTo(now);
            app.play.stepShown(now);
            // The run has ended, so allocating here can't disturb play.
            if (app.play.over && !wasOver) recordRun(app);
        }
        drawFrame(app, ui);
        SDL_RenderPresent(app.renderer);

        if (!app.settings.vsync) {
            uint64_t spent = SDL_GetTicksNS() - frameStart;
            if (spent < kUncappedFrameNs) SDL_DelayPrecise(kUncappedFrameNs - spent);
        }
        uint64_t now = SDL_GetTicksNS();
        app.frameNs = now - frameStart;
        frameStart = now;
        app.worstFrameNs = std::max(app.worstFrameNs, app.frameNs);
        if (now - worstWindowStart > 1'000'000'000) {  // the worst values cover about the last second
            app.worstFrameNs = app.frameNs;
            app.play.worstInputNs = app.play.lastInputNs;
            worstWindowStart = now;
        }
    }

    saveSettings(app.settings, app.settingsPath);
    if (app.play.coach) app.play.coach->save();
    TTF_Quit();
    SDL_Quit();
    return 0;
}
