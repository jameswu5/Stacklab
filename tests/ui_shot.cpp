// Renders the play screen to PNGs at 2x, using the same hand-filled game in
// both themes, for comparing against the design. Also checks that drawing doesn't
// allocate once warmed up. Usage: ui_shot <out-dir> <base-dir>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

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
    for (bool dark : {false, true}) {
        ui.dark = dark;
        ui.beginFrame();
        ui.drawPlay({&game, game.seconds(), false, "-0.38s", true, nullptr, nullptr});
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
    ui.drawPlay({&fresh, 0, false, "-", false, "Ready", nullptr});
    SDL_FlushRenderer(renderer);
    SDL_Surface* countdownPixels = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SavePNG(countdownPixels, (std::string(argv[1]) + "/countdown.png").c_str());
    SDL_DestroySurface(countdownPixels);

    auto draw = [&](bool dark) {
        ui.dark = dark;
        ui.beginFrame();
        game.frame++;
        ui.drawPlay({&game, game.seconds(), false, "-0.38s", true, nullptr, nullptr});
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
