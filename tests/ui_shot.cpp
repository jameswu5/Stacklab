// Renders the play screen to PNGs at 2x, using the same hand-filled game in
// both themes, and the other screens, for comparing against the design. Also checks that drawing doesn't
// allocate once warmed up. Usage: ui_shot <out-dir> <base-dir>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "app/ui.h"
#include "core/game.h"

// Only this thread, while drawing: Apple's GPU driver allocates on its own threads.
static thread_local bool counting = false;
static size_t allocations = 0;

void* operator new(size_t size) {
    if (counting) allocations++;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }

namespace {

SDL_malloc_func realMalloc;
SDL_calloc_func realCalloc;
SDL_realloc_func realRealloc;
SDL_free_func realFree;

void* countedMalloc(size_t size) {
    if (counting) allocations++;
    return realMalloc(size);
}
void* countedCalloc(size_t count, size_t size) {
    if (counting) allocations++;
    return realCalloc(count, size);
}
void* countedRealloc(void* memory, size_t size) {
    if (counting) allocations++;
    return realRealloc(memory, size);
}

core::Game sampleGame() {
    core::Game game(core::Ruleset{}, core::Handling{}, 1);
    using enum core::Piece;
    constexpr core::Piece rows[6][10] = {
        {I, I, I, I, L, L, L, O, O, None},
        {J, J, J, T, T, T, L, O, O, None},
        {J, L, L, L, T, I, I, I, I, None},
        {T, T, T, L, J, I, I, I, I, None},
        {None, T, Z, Z, J, L, L, L, None, None},
        {None, Z, Z, None, J, J, None, L, None, None},
    };
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 10; x++) game.board.at(x, y) = rows[y][x];
    game.falling.piece = T;
    game.falling.rot = 2;
    game.falling.x = 5;
    game.falling.y = 15;
    game.hold = S;
    game.queue = {J, I, O, L, Z};
    game.stats.lines = 22;
    game.stats.pieces = 67;
    game.stats.keys = 195;
    game.stats.attack = 40;
    game.frame = 1288;
    return game;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 1;
    SDL_GetOriginalMemoryFunctions(&realMalloc, &realCalloc, &realRealloc, &realFree);
    SDL_SetMemoryFunctions(countedMalloc, countedCalloc, countedRealloc, realFree);
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) return 1;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("ui_shot", 400, 300, SDL_WINDOW_HIDDEN, &window, &renderer)) return 1;
    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 2880, 1800);
    SDL_SetRenderTarget(renderer, target);

    Ui ui(renderer, std::string(argv[2]) + "/assets/fonts/");
    core::Game game = sampleGame();
    // The last placement had two faults, as the Finesse row shows them.
    ai::FaultStats faults;
    faults.total = 12;
    faults.last = 2;
    faults.byPiece[static_cast<int>(core::Piece::T)][3] = 5;
    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&kModes[0], &game, game.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults});
        SDL_FlushRenderer(renderer);
        SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
        std::string path = std::string(argv[1]) + (dark ? "/dark.png" : "/light.png");
        if (!pixels || !SDL_SavePNG(pixels, path.c_str())) {
            std::fprintf(stderr, "Couldn't save %s: %s\n", path.c_str(), SDL_GetError());
            return 1;
        }
        SDL_DestroySurface(pixels);
    }

    ui.dark = true;
    ui.beginFrame();
    ui.drawSettings(Settings{}, RowSafelock, false);
    SDL_FlushRenderer(renderer);
    SDL_Surface* settingsPixels = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SavePNG(settingsPixels, (std::string(argv[1]) + "/settings.png").c_str());
    SDL_DestroySurface(settingsPixels);

    core::Game fresh(core::Ruleset{}, core::Handling{}, 1);
    ui.beginFrame();
    ui.drawPlay({&kModes[0], &fresh, 0, false, "Ready", "", 0, {}});
    SDL_FlushRenderer(renderer);
    SDL_Surface* countdownPixels = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SavePNG(countdownPixels, (std::string(argv[1]) + "/countdown.png").c_str());
    SDL_DestroySurface(countdownPixels);

    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        const std::array<double, kModeCount> bests{60.824};
        ui.drawMenu(dark ? 7 : 0, bests, 3);
        SDL_FlushRenderer(renderer);
        SDL_Surface* menuPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(menuPixels, (std::string(argv[1]) + (dark ? "/menu-dark.png" : "/menu-light.png")).c_str());
        SDL_DestroySurface(menuPixels);
    }

    // A session in both themes: a topped out run, a new best, then the latest run.
    core::Stats runStats = game.stats;
    const std::vector<RunResult> history = {
        {&kModes[0], 18.4, runStats, true, false},
        {&kModes[0], 62.31, runStats, false, false},
        {&kModes[0], 58.02, runStats, false, true},
        {&kModes[0], 60.47, runStats, false, false},
    };
    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&kModes[0], &game, 60.47, true, nullptr, "R to retry, Escape for the menu", 58.02, history, nullptr, 0, &faults});
        SDL_FlushRenderer(renderer);
        SDL_Surface* resultsPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(resultsPixels, (std::string(argv[1]) + (dark ? "/results-dark.png" : "/results-light.png")).c_str());
        SDL_DestroySurface(resultsPixels);
    }

    // A row that asks for every stat, so the card grows to fit them.
    Mode allStats = kModes[0];
    allStats.stats |= StatAPM | StatAPP;
    ui.dark = false;
    ui.beginFrame();
    ui.drawPlay({&allStats, &game, game.seconds(), false, nullptr, "", 0, {}});
    SDL_FlushRenderer(renderer);
    SDL_Surface* statsPixels = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SavePNG(statsPixels, (std::string(argv[1]) + "/all-stats.png").c_str());
    SDL_DestroySurface(statsPixels);

    // Blitz, whose results have no finesse, and a levels mode that lists finesse, which shows "-".
    Mode levelsWithFinesse = kModes[1];
    levelsWithFinesse.stats |= StatFinesse;
    ui.beginFrame();
    ui.drawPlay({&levelsWithFinesse, &game, game.seconds(), false, nullptr, "", 0, {}});
    SDL_FlushRenderer(renderer);
    SDL_Surface* levelsPixels = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SavePNG(levelsPixels, (std::string(argv[1]) + "/levels-finesse.png").c_str());
    SDL_DestroySurface(levelsPixels);

    // Lines left under a stack tall enough to cover part of it, then at the start of a run.
    core::Game tall = sampleGame();
    for (int y = 6; y < 19; y++)
        for (int x = 0; x < 4; x++) tall.board.at(x, y) = core::Piece::S;
    tall.stats.lines = 3;
    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&kModes[0], &tall, tall.seconds(), false, nullptr, "", 0, {}});
        SDL_FlushRenderer(renderer);
        SDL_Surface* linesPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(linesPixels, (std::string(argv[1]) + (dark ? "/lines-left-dark.png" : "/lines-left-light.png")).c_str());
        SDL_DestroySurface(linesPixels);
        ui.beginFrame();
        ui.drawPlay({&kModes[0], &fresh, 0, false, nullptr, "", 0, {}});
        SDL_FlushRenderer(renderer);
        SDL_Surface* startPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(startPixels, (std::string(argv[1]) + (dark ? "/lines-left-start-dark.png" : "/lines-left-start-light.png")).c_str());
        SDL_DestroySurface(startPixels);
    }

    // The coach mid-run with a hint, then just after a placement killed the PC, then its results.
    const Mode& coachMode = kModes[6];
    PcView coached;
    coached.status = "PC ready";
    std::snprintf(coached.hintLine, sizeof coached.hintLine, "Hold first, then place the T");
    coached.showHint = true;
    coached.hintPiece = core::Piece::T;
    coached.hintCells = {{{8, 7}, {7, 6}, {8, 6}, {9, 6}}};
    coached.pcs = 3;
    coached.streak = 2;
    PcView killed;
    killed.status = "No PC";
    std::snprintf(killed.detail, sizeof killed.detail, "in 4 lines with these pieces");
    killed.alert = "That piece killed it: undo";
    std::snprintf(killed.hintLine, sizeof killed.hintLine, "H for a hint");
    killed.pcs = 3;
    killed.outOfReach = true;
    PcView thinking;
    thinking.status = "Possible";
    std::snprintf(thinking.detail, sizeof thinking.detail, "need more pieces");
    std::snprintf(thinking.hintLine, sizeof thinking.hintLine, "No hint for this one");
    PcView finished = killed;
    finished.bestPcs = 7;
    finished.tally.attempts = {12, 9, 11, 8, 10, 9, 7};
    finished.tally.successes = {5, 3, 6, 2, 4, 3, 1};
    const std::vector<RunResult> coachHistory = {{&coachMode, 60.47, runStats, false, false}};
    auto shot = [&](const PcView& pc, bool over, const char* name, bool dark) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&coachMode, &game, game.seconds(), over, nullptr, "R to retry, Escape for the menu", 0, coachHistory, nullptr, 0, &faults, &pc});
        SDL_FlushRenderer(renderer);
        SDL_Surface* coachPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(coachPixels, (std::string(argv[1]) + "/coach-" + name + (dark ? "-dark.png" : "-light.png")).c_str());
        SDL_DestroySurface(coachPixels);
    };
    for (bool dark : {false, true}) {
        shot(coached, false, "hint", dark);
        shot(killed, false, "killed", dark);
        shot(thinking, false, "possible", dark);
        shot(finished, true, "results", dark);
    }
    // PC practice mid-run and its results, then Bag count, then a run that used undo.
    PcView practice = thinking;
    practice.goal = 1;
    practice.residue[0] = '3';
    practice.pcs = 1;
    practice.outcome = "Perfect clear";
    PcView bagCount;
    bagCount.status = "Keep count";
    std::snprintf(bagCount.detail, sizeof bagCount.detail, "B shows the bag residue");
    bagCount.residue[0] = '?';
    bagCount.pcs = 4;
    PcView undone = killed;
    undone.nullified = true;
    for (bool dark : {false, true}) {
        shot(practice, false, "practice", dark);
        shot(practice, true, "practice-results", dark);
        shot(bagCount, false, "count", dark);
        shot(undone, false, "undone", dark);
    }

    // A drill with one target placed, with the list of drills, in both themes.
    std::vector<Drill> drills(2);
    drills[0].name = "First steps 1";
    drills[0].blurb = "A sample: lay a flat base, in any order";
    drills[1].name = "First steps 2";
    drills[1].blurb = "A sample: close the gap, then build on the clean board";
    drills[0].targets = {{core::Piece::I, {{{0, 0}, {1, 0}, {2, 0}, {3, 0}}}}, {core::Piece::O, {{{4, 0}, {5, 0}, {4, 1}, {5, 1}}}}, {core::Piece::J, {{{6, 1}, {6, 0}, {7, 0}, {8, 0}}}}};
    core::Game drillGame(core::Ruleset{}, core::Handling{}, 1);
    drillGame.board.at(0, 0) = drillGame.board.at(1, 0) = drillGame.board.at(2, 0) = drillGame.board.at(3, 0) = core::Piece::I;
    drillGame.queue = {core::Piece::J, core::Piece::None, core::Piece::None, core::Piece::None, core::Piece::None};
    drillGame.falling.piece = core::Piece::O;
    drillGame.falling.x = 4;
    const ai::FaultStats afterMiss;  // a miss starts the attempt over, and its faults with it
    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&kModes[5], &drillGame, 2.5, false, dark ? "Missed" : nullptr, "", 0, {}, &drills[0], 1, dark ? &afterMiss : &faults});
        SDL_FlushRenderer(renderer);
        SDL_Surface* drillPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(drillPixels, (std::string(argv[1]) + (dark ? "/drill-dark.png" : "/drill-light.png")).c_str());
        SDL_DestroySurface(drillPixels);
        ui.beginFrame();
        ui.drawDrills(1, drills, dark ? "broken.json: the board blocks the spawn" : nullptr);
        SDL_FlushRenderer(renderer);
        SDL_Surface* listPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(listPixels, (std::string(argv[1]) + (dark ? "/drills-dark.png" : "/drills-light.png")).c_str());
        SDL_DestroySurface(listPixels);
    }

    auto draw = [&](bool dark) {
        ui.dark = dark;
        ui.beginFrame();
        game.frame++;
        ui.drawPlay({&kModes[0], &game, game.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults});
        ui.drawPlay({&coachMode, &game, game.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults, &killed});
    };
    for (int i = 0; i < 10; i++) {
        draw(i % 2);
        SDL_FlushRenderer(renderer);
    }
    // The flush is left out: what SDL's Metal backend does there is out of our hands.
    for (int i = 0; i < 2000; i++) {
        counting = true;
        draw(i % 2);
        counting = false;
        SDL_FlushRenderer(renderer);
    }
    if (allocations != 0) {
        std::fprintf(stderr, "Drawing allocated %zu times\n", allocations);
        return 1;
    }

    TTF_Quit();
    SDL_Quit();
    return 0;
}
