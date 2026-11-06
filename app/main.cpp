#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include "core/game.h"

int main(int, char**) {
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) return 1;
    TTF_Quit();
    SDL_Quit();
    return 0;
}
