// The bot's key presses put each piece where it chose, through core's real rules.
#include <cassert>
#include <cstdio>

#include "ai/bot.h"

int main() {
    ai::Bot bot;
    assert(ai::loadBotWeights("data/bot/default.txt", bot.weights));
    int spins = 0, softDrops = 0;
    for (uint32_t seed = 1; seed <= 4; seed++) {
        core::Game game(core::Ruleset{}, core::Handling{}, seed);
        game.fillGarbage(6);  // a messy start, for tucks and spins
        for (int piece = 0; piece < 150 && !game.toppedOut; piece++) {
            ai::BotChoice choice;
            assert(bot.choose(game, choice));
            bool locked = false;
            for (const auto& tick : choice.inputs) {
                assert(!locked);
                const auto& locks = game.tick(tick);
                if (locks.empty()) continue;
                assert(locks[0].cells == choice.cells);
                assert(locks[0].spin == choice.placement.spin);
                locked = true;
            }
            assert(locked);
            spins += choice.placement.spin != core::Spin::None;
            for (ai::Move move : choice.moves) softDrops += move == ai::Move::SoftDrop;
        }
        assert(!game.toppedOut);
    }
    std::printf("bot_test: 600 placements, %d spins, %d soft drops\n", spins, softDrops);
}
