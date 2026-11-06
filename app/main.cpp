#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <objc/message.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "app/play.h"
#include "app/settings.h"
#include "app/ui.h"

namespace {

constexpr uint64_t kUncappedFrameNs = 1'000'000'000 / 480;  // with VSync off, how often to poll and draw

enum class Screen { Menu, Play, Settings };

struct App {
    Settings settings;
    std::string prefPath, settingsPath;
    std::array<int, SDL_SCANCODE_COUNT> binds{};  // scancode to Bind, or -1
    SDL_Renderer* renderer = nullptr;
    Play play;

    Screen screen = Screen::Menu;
    int menuRow = 0;  // a mode, or Settings after the last one
    std::array<double, kModeCount> bests{};
    std::vector<RunResult> history;  // this session's ended runs; a retried run didn't end
    int selected = 0;
    bool capturing = false;

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

void handleMenuKey(App& app, const SDL_KeyboardEvent& key) {
    if (!key.down) return;
    int rows = kModeCount + 1;
    switch (key.scancode) {
        case SDL_SCANCODE_UP: app.menuRow = (app.menuRow + rows - 1) % rows; break;
        case SDL_SCANCODE_DOWN: app.menuRow = (app.menuRow + 1) % rows; break;
        case SDL_SCANCODE_RETURN:
            if (key.repeat) break;
            if (app.menuRow == kModeCount) {
                app.screen = Screen::Settings;
            } else {
                const Mode& mode = kModes[app.menuRow];
                app.play.mode = &mode;
                app.play.bestPath = mode.bestFile ? app.prefPath + mode.bestFile : "";
                app.play.best = app.bests[app.menuRow];
                app.screen = Screen::Play;
                restart(app, key.timestamp);
            }
            break;
        default: break;
    }
}

void handlePlayKey(App& app, const SDL_KeyboardEvent& key) {
    if (key.repeat) return;
    int bind = app.binds[key.scancode];
    if (bind < 0) return;
    if (bind == BindRetry) {
        if (key.down) restart(app, key.timestamp);
    } else if (bind == BindMenu) {
        if (key.down) {
            app.screen = Screen::Menu;
            app.play.held.fill(0);
        }
    } else if (bind == BindUndo) {
        if (key.down) app.play.undo(key.timestamp);
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

void recordRun(App& app) {
    const Play& play = app.play;
    double& best = app.bests[app.menuRow];
    app.history.push_back({play.mode, play.endSeconds, play.game.stats, play.game.toppedOut, play.best != best});
    best = play.best;
}

void drawFrame(App& app, Ui& ui) {
    const Play& play = app.play;
    ui.dark = app.settings.dark;
    ui.beginFrame();
    if (app.screen == Screen::Menu) {
        ui.drawMenu(app.menuRow, app.bests);
    } else if (app.screen == Screen::Settings) {
        ui.drawSettings(app.settings, app.selected, app.capturing);
    } else {
        const char* banner = nullptr;
        std::string hint;
        auto keyName = [&](Bind bind) { return rowValue(app.settings, static_cast<int>(RowBindFirst) + bind); };
        if (play.over) hint = keyName(BindRetry) + " to retry, " + keyName(BindMenu) + " for the menu";
        else if (!play.started) banner = SDL_GetTicksNS() < play.startNs - kCountdownNs / 2 ? "Ready" : "Set";
        ui.drawPlay({play.mode, &play.shown, play.seconds(), play.over, banner, hint.c_str(), play.best, app.history});
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
    TTF_Quit();
    SDL_Quit();
    return 0;
}
