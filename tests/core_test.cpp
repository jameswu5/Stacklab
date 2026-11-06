#include <cassert>

#include "core/game.h"

int main() {
    core::Game game(core::Ruleset{}, core::Handling{}, 1);
    game.tick({});
    assert(game.frame == 1);
}
