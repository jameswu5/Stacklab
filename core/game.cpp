#include "core/game.h"

namespace core {

std::array<Cell, 4> Falling::cells() const { return {}; }

Rng::Rng(uint32_t seed) : value(seed) {}
uint32_t Rng::next() { return value; }
double Rng::nextFloat() { return 0; }

double attackValue(int, Spin, int, int, const Ruleset&) { return 0; }

Game::Game(const Ruleset& rules, const Handling& handling, uint32_t seed)
    : rules(rules), handling(handling), rng(seed) {}

const std::vector<LockResult>& Game::tick(std::span<const InputEvent>) {
    ++frame;
    return locks_;
}

Falling Game::ghost() const { return falling; }

}  // namespace core
