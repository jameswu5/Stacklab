// Renders the play screen to PNGs at 2x, using the same hand-filled game in
// both themes, and the other screens, for comparing against the design. Also checks that drawing doesn't
// allocate once warmed up. Usage: ui_shot <out-dir> <base-dir>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "app/pc_coach.h"
#include "app/ui.h"
#include "app/versus.h"
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
    faults.byPiece[static_cast<int>(core::Piece::T)][0] = 2;
    faults.byPiece[static_cast<int>(core::Piece::I)][1] = 3;
    faults.byPiece[static_cast<int>(core::Piece::L)][2] = 2;
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

    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawSettings(Settings{}, dark ? RowSafelock : RowMultiplier, false);
        SDL_FlushRenderer(renderer);
        SDL_Surface* settingsPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(settingsPixels, (std::string(argv[1]) + (dark ? "/settings-dark.png" : "/settings-light.png")).c_str());
        SDL_DestroySurface(settingsPixels);
    }

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
        const std::array<double, kModeCount> bests{60.824, 0, 41.5};
        const BotPreset bot{"3 PPS", "", 3}, trBot{"TR 20.5k", "", 2.17, 20500};
        const MenuView menus[] = {{nullptr, 1, 1, bests, 3, &bot, nullptr, nullptr}, {&kMenu[3], 1, 0, bests, 3, &bot, nullptr, nullptr}, {&kMenu[2], 0, 0, bests, 3, &bot, nullptr, nullptr}, {nullptr, 4, 1, bests, 3, &trBot, nullptr, nullptr}};
        const char* names[] = {"menu", "menu-pc", "menu-setups", "menu-versus"};
        for (int i = 0; i < 4; i++) {
            ui.beginFrame();
            ui.drawMenu(menus[i]);
            SDL_FlushRenderer(renderer);
            SDL_Surface* menuPixels = SDL_RenderReadPixels(renderer, nullptr);
            SDL_SavePNG(menuPixels, (std::string(argv[1]) + "/" + names[i] + (dark ? "-dark.png" : "-light.png")).c_str());
            SDL_DestroySurface(menuPixels);
        }
        // Dig 18 part way: 7 cleared, so 10 rows are still on the board, 3 of them risen.
        core::Game dig(kModes[2].rules, {}, 3);
        dig.fillGarbage(10);
        for (int x = 0; x < 6; x++) dig.board.at(x, 10) = core::Piece::L;
        dig.stats.garbageCleared = 7;
        dig.stats.pieces = 21;
        dig.frame = 600;
        ui.beginFrame();
        ui.drawPlay({&kModes[2], &dig, dig.seconds(), false, nullptr, "", 41.5, {}, nullptr, 0, &faults});
        SDL_FlushRenderer(renderer);
        SDL_Surface* digPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(digPixels, (std::string(argv[1]) + (dark ? "/dig-18-dark.png" : "/dig-18-light.png")).c_str());
        SDL_DestroySurface(digPixels);
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
    const Mode& coachMode = kModes[4];
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
    // The LST coach: a slot ready with the T's hint, then a placement that broke the loop, then results.
    // Setup drills: the menu with four.lol's TKI picked, then TKI with I and L placed. Skipped
    // without data/four-lol/drills (tools/four_lol_drills.py), as four.lol's setups aren't in the repo.
    const std::filesystem::path drillDirs[] = {std::string(argv[2]) + "/data/four-lol/drills"};
    const DrillSet setupDrills = loadDrills(drillDirs);
    auto tki = std::ranges::find(setupDrills.drills, std::string("TKI"), &Drill::name);
    if (tki != setupDrills.drills.end()) {
        core::Game drillGame(core::Ruleset{}, core::Handling{}, 1);
        setUpDrill(drillGame, *tki);
        for (int i = 0; i < 2; i++)
            for (const core::Cell& cell : tki->targets[i].cells) drillGame.board.at(cell.x, cell.y) = tki->targets[i].piece;
        drillGame.queue.erase(drillGame.queue.begin());
        drillGame.respawn(tki->pieces[2]);
        drillGame.stats.pieces = 2;
        drillGame.frame = 90;
        const std::array<double, kModeCount> noBests{};
        const BotPreset bot{"3 PPS", "", 3};
        for (bool dark : {false, true}) {
            ui.dark = dark;
            ui.beginFrame();
            ui.drawMenu({&kMenu[2], 1, 0, noBests, 0, &bot, &*tki, nullptr});
            SDL_FlushRenderer(renderer);
            SDL_Surface* menuPixels = SDL_RenderReadPixels(renderer, nullptr);
            SDL_SavePNG(menuPixels, (std::string(argv[1]) + (dark ? "/menu-drills-dark.png" : "/menu-drills-light.png")).c_str());
            SDL_DestroySurface(menuPixels);
            ui.beginFrame();
            ui.drawPlay({&kModes[9], &drillGame, drillGame.seconds(), false, nullptr, "", 0, {}, &*tki, 3, &faults});
            SDL_FlushRenderer(renderer);
            SDL_Surface* drillPixels = SDL_RenderReadPixels(renderer, nullptr);
            SDL_SavePNG(drillPixels, (std::string(argv[1]) + (dark ? "/drill-tki-dark.png" : "/drill-tki-light.png")).c_str());
            SDL_DestroySurface(drillPixels);
        }
    }

    const Mode& lstMode = kModes[8];
    core::Game lstGame(lstMode.rules, core::Handling{}, 1);
    {
        const char* rows[] = {"L.........", "LL........", "L...SSOOJJ", "JJ.SSZOOJT", "IZ.ZZTTTIT", "IZ.LLLOOIT"};  // top first
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 10; x++)
                if (rows[5 - y][x] != '.') lstGame.board.at(x, y) = static_cast<core::Piece>(std::strchr(" IJLOSTZ", rows[5 - y][x]) - " IJLOSTZ");
        lstGame.respawn(core::Piece::T);
        lstGame.falling.y = 15;
        lstGame.hold = core::Piece::I;
        lstGame.stats.lines = 26;
        lstGame.stats.pieces = 72;
        lstGame.stats.keys = 230;
        lstGame.frame = 2400;
    }
    LstView lstFine;
    lstFine.status = "Alive";
    lstFine.verdict = LstVerdict::Fine;
    std::snprintf(lstFine.callout, sizeof lstFine.callout, "Fine");
    std::snprintf(lstFine.hintLine, sizeof lstFine.hintLine, "Place the T");
    lstFine.showHint = true;
    lstFine.hintPiece = core::Piece::T;
    lstFine.hintCells = {{{1, 3}, {2, 3}, {3, 3}, {2, 2}}};
    lstFine.tsds = 4;
    lstFine.b2b = 3;
    lstFine.loops = 4;
    LstView lstWorse = lstFine;
    lstWorse.verdict = LstVerdict::Worse;
    std::snprintf(lstWorse.callout, sizeof lstWorse.callout, "Worse than the best: Z to undo");
    std::snprintf(lstWorse.hintLine, sizeof lstWorse.hintLine, "H for a hint");
    lstWorse.showHint = false;
    LstView lstFatal = lstWorse;
    lstFatal.status = "Broken";
    lstFatal.verdict = LstVerdict::Fatal;
    std::snprintf(lstFatal.callout, sizeof lstFatal.callout, "Fatal, the loop broke: Z to undo");
    lstFatal.loops = 0;
    const std::vector<RunResult> lstHistory = {{&lstMode, 92.3, runStats, true, false}};
    auto lstShot = [&](const LstView& lst, bool over, const char* name, bool dark) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&lstMode, &lstGame, lstGame.seconds(), over, nullptr, "R to retry, Escape for the menu", 0, lstHistory, nullptr, 0, &faults, nullptr, false, &lst});
        SDL_FlushRenderer(renderer);
        SDL_Surface* lstPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(lstPixels, (std::string(argv[1]) + "/lst-" + name + (dark ? "-dark.png" : "-light.png")).c_str());
        SDL_DestroySurface(lstPixels);
    };
    for (bool dark : {false, true}) {
        lstShot(lstFine, false, "hint", dark);
        lstShot(lstWorse, false, "worse", dark);
        lstShot(lstFatal, false, "fatal", dark);
        lstShot(lstFatal, true, "results", dark);
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
    // A made-up setup, two pieces in, with a long condition to wrap.
    PcSetup setup;
    setup.name = "Sample setup with a long name / Hold T";
    setup.condition = "the 5 pieces are IJLOS (no T, no Z); Hold T; with a condition long enough that it has to be cut short somewhere on the card";
    {
        using enum core::Piece;
        constexpr const char* rows[4] = {"LLL.....SS", "LOO....SST", "JOO...ZZTT", "JJJ....ZZT"};
        for (int row = 0; row < 4; row++)
            for (int x = 0; x < 10; x++) setup.cells[(3 - row) * 10 + x] = std::array{None, L, S, T, O, J, Z}[std::string(".LSTOJZ").find(rows[row][x])];
    }
    PcView setupShown = thinking;
    setupShown.goal = 1;
    setupShown.residue[0] = '0';
    setupShown.setup = &setup;
    setupShown.setupGhost = true;
    std::snprintf(setupShown.setupLine, sizeof setupShown.setupLine, "84.64%%, 1 of 3");
    PcView setupAsk = setupShown;
    setupAsk.setup = nullptr;
    std::snprintf(setupAsk.setupLine, sizeof setupAsk.setupLine, "G for a setup");
    PcView setupBroken = setupAsk;
    std::snprintf(setupBroken.setupLine, sizeof setupBroken.setupLine, "Setup broken");
    core::Game practiceGame(core::Ruleset{}, core::Handling{}, 1);
    practiceGame.falling.piece = core::Piece::None;  // as if between pieces, so it doesn't sit over the top bar
    for (const core::Cell c : {core::Cell{0, 0}, {1, 0}, {2, 0}, {0, 1}}) practiceGame.board.at(c.x, c.y) = core::Piece::J;
    for (const core::Cell c : {core::Cell{8, 3}, {9, 3}, {7, 2}, {8, 2}}) practiceGame.board.at(c.x, c.y) = core::Piece::S;
    auto practiceShot = [&](const PcView& pc, const char* name, bool dark) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&kModes[5], &practiceGame, practiceGame.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults, &pc});
        SDL_FlushRenderer(renderer);
        SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(pixels, (std::string(argv[1]) + "/" + name + (dark ? "-dark.png" : "-light.png")).c_str());
        SDL_DestroySurface(pixels);
    };
    for (bool dark : {false, true}) {
        practiceShot(setupShown, "practice-setup", dark);
        practiceShot(setupAsk, "practice-setup-ask", dark);
        practiceShot(setupBroken, "practice-setup-broken", dark);
    }

    // Opener practice: the menu, and a second stage after the first TSD (the first stage's setup with
    // its full rows cleared) with some of its pieces in, if four.lol's openers are in
    // <base>/data/four-lol/openers (they aren't in the repo).
    auto openerShot = [&](const char* opener, int first, int second, std::string_view placed, const char* name) {
        const std::vector<PcSetup> setups = loadPcSetups(std::string(argv[2]) + "/data/four-lol/openers/" + opener + ".json");
        if (static_cast<int>(setups.size()) <= second) return;
        core::Game openerGame(core::Ruleset{}, core::Handling{}, 1);
        openerGame.falling.piece = core::Piece::None;
        int y = 0;
        for (int row = 0; row < kSetupRows; row++) {
            const auto cells = std::span(setups[first].cells).subspan(row * 10, 10);
            if (std::ranges::all_of(cells, [](core::Piece p) { return p != core::Piece::None; })) continue;
            for (int x = 0; x < 10; x++) openerGame.board.at(x, y) = cells[x];
            y++;
        }
        for (int i = 0; i < kSetupRows * 10; i++)
            if (placed.find(" IJLOSTZ"[static_cast<int>(setups[second].cells[i])]) != std::string_view::npos) openerGame.board.cells[i] = setups[second].cells[i];
        openerGame.queue = {core::Piece::J, core::Piece::L, core::Piece::T, core::Piece::O, core::Piece::I};
        openerGame.hold = core::Piece::Z;
        PcView view;
        view.status = "Building";
        std::snprintf(view.detail, sizeof view.detail, "stage 2 of 2, then the PC");
        view.goal = 1;
        view.setup = &setups[second];
        view.setupGhost = true;
        std::snprintf(view.setupLine, sizeof view.setupLine, "%s%s1 of 2", setups[second].chance.c_str(), setups[second].chance.empty() ? "" : ", ");
        const std::array<double, kModeCount> noBests{};
        const BotPreset bot{"3 PPS", "", 3};
        for (bool dark : {false, true}) {
            ui.dark = dark;
            ui.beginFrame();
            ui.drawMenu({&kMenu[2], 2, 0, noBests, 0, &bot, nullptr, opener});
            SDL_FlushRenderer(renderer);
            SDL_Surface* menuPixels = SDL_RenderReadPixels(renderer, nullptr);
            SDL_SavePNG(menuPixels, (std::string(argv[1]) + "/menu-" + name + (dark ? "-dark.png" : "-light.png")).c_str());
            SDL_DestroySurface(menuPixels);
            ui.beginFrame();
            ui.drawPlay({&kModes[10], &openerGame, 4.2, false, nullptr, "", 0, {}, nullptr, 0, &faults, &view});
            SDL_FlushRenderer(renderer);
            SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
            SDL_SavePNG(pixels, (std::string(argv[1]) + "/" + name + (dark ? "-dark.png" : "-light.png")).c_str());
            SDL_DestroySurface(pixels);
        }
    };
    openerShot("Black Tea Cannon", 0, 1, "ISZ", "opener-btc");
    openerShot("TKI", 1, 6, "SZO", "opener-tki");
    for (bool dark : {false, true}) {
        shot(practice, false, "practice", dark);
        shot(practice, true, "practice-results", dark);
        shot(bagCount, false, "count", dark);
        shot(undone, false, "undone", dark);
    }

    // A drill with one target placed, in both themes.
    const Mode drillMode = {.name = "Drill", .blurb = "", .rules = {}, .undo = true};
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
        ui.drawPlay({&drillMode, &drillGame, 2.5, false, dark ? "Missed" : nullptr, dark ? "R to retry, Q to undo" : "", 0, {}, &drills[0], 1, dark ? &afterMiss : &faults});
        SDL_FlushRenderer(renderer);
        SDL_Surface* drillPixels = SDL_RenderReadPixels(renderer, nullptr);
        SDL_SavePNG(drillPixels, (std::string(argv[1]) + (dark ? "/drill-dark.png" : "/drill-light.png")).c_str());
        SDL_DestroySurface(drillPixels);
    }

    // Versus mid-round with clears named and garbage on its way, then a round's card and the match's results.
    const Mode& versusMode = kModes[kModeCount - 1];
    Versus versus;
    core::Game you = sampleGame();
    you.stats.attack = 9;
    you.stats.pieces = 32;
    you.frame = 722;
    versus.bot = sampleGame();
    versus.bot.board = {};
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 10; x++)
            if (x != (y * 3) % 10) versus.bot.board.at(x, y) = y < 2 ? core::Piece::Garbage : core::Piece::L;
    versus.bot.frame = 722;
    versus.bot.stats.pieces = 30;
    versus.bot.stats.attack = 4;
    core::LockResult tsd;
    tsd.piece = core::Piece::T;
    tsd.lines = 2;
    tsd.spin = core::Spin::Normal;
    tsd.b2b = 2;
    tsd.combo = 1;
    versus.callouts[0] = {tsd, 700, true};
    core::LockResult quad;
    quad.piece = core::Piece::I;
    quad.lines = 4;
    versus.callouts[1] = {quad, 650, true};
    // Garbage on its way to you, so the meter shows.
    versus.toYou = {{core::GarbageEvent::Target, 1}, {core::GarbageEvent::Arrive, 1, 1, 0, 4}, {core::GarbageEvent::Confirm, 1, 1}};
    const core::InputEvent none[1] = {};
    you.tick(std::span(none, 0), versus.toYou);
    versus.match.wins = {3, 2};
    versus.match.rounds = 5;
    std::vector<RunResult> matches = {{&versusMode, 0, {}, false, false, {7, 4}}, {&versusMode, 0, {}, false, false, {5, 7}}};
    for (bool dark : {false, true}) {
        const char* suffix = dark ? "-dark.png" : "-light.png";
        auto save = [&](const char* name) {
            SDL_FlushRenderer(renderer);
            SDL_Surface* pixels = SDL_RenderReadPixels(renderer, nullptr);
            SDL_SavePNG(pixels, (std::string(argv[1]) + "/" + name + suffix).c_str());
            SDL_DestroySurface(pixels);
        };
        ui.dark = dark;
        versus.roundOver = false;
        ui.beginFrame();
        ui.drawVersus({&versusMode, &you, &versus, false, nullptr, "", matches, "TR 20.5k: 2.08 PPS"});
        save("versus");
        versus.roundOver = true;
        versus.lastWinner = 0;
        ui.beginFrame();
        ui.drawVersus({&versusMode, &you, &versus, false, nullptr, "", matches, "TR 20.5k: 2.08 PPS"});
        save("versus-round");
        versus.match.wins = {7, 4};
        versus.match.rounds = 11;
        versus.match.totals[0] = {402, 260, 31, 255.4};
        versus.match.totals[1] = {380, 199, 22, 255.4};
        ui.beginFrame();
        ui.drawVersus({&versusMode, &you, &versus, true, nullptr, "R to retry, Escape for the menu", matches, "TR 20.5k: 2.08 PPS"});
        save("versus-results");
        versus.match.wins = {3, 2};
    }
    versus.roundOver = false;

    auto draw = [&](bool dark) {
        ui.dark = dark;
        ui.beginFrame();
        game.frame++;
        ui.drawPlay({&kModes[0], &game, game.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults});
        ui.drawPlay({&coachMode, &game, game.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults, &killed});
        ui.drawPlay({&kModes[5], &practiceGame, practiceGame.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults, &setupShown});
        ui.drawVersus({&versusMode, &you, &versus, false, "Set", "", matches});
        ui.drawPlay({&lstMode, &lstGame, lstGame.seconds(), false, nullptr, "", 0, {}, nullptr, 0, &faults, nullptr, false, &lstFine});
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
