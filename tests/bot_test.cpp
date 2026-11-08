// The bot's key presses put each piece where it chose, through core's real rules.
#include <cassert>
#include <cstdio>
#include <filesystem>

#include "ai/bot.h"

static int spins = 0, softDrops = 0, placements = 0, bagChecks = 0;

static void play(ai::Bot& bot, uint32_t seed, int pieces) {
    core::Game game(core::Ruleset{}, core::Handling{}, seed);
    game.fillGarbage(6);  // a messy start, for tucks and spins
    for (int piece = 0; piece < pieces && !game.toppedOut; piece++) {
        // The first piece past the previews, once core has dealt it, is always one the bag allows.
        const size_t previews = game.rules.previews;
        if (game.queue.size() > previews) {
            assert(ai::botBagLeft(game) >> static_cast<int>(game.queue[previews]) & 1);
            bagChecks++;
        }
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
        placements++;
        spins += choice.placement.spin != core::Spin::None;
        for (ai::Move move : choice.moves) softDrops += move == ai::Move::SoftDrop;
    }
    assert(!game.toppedOut);
}

int main() {
    ai::Bot bot;
    assert(ai::loadBot("data/bot/presets/6-pps-3.0.txt", bot));
    bot.pps = 0;  // full speed, for a quick test
    bot.mistakes = 0;
    // Each preset loads and has a name for the menu.
    int presets = 0;
    for (const auto& entry : std::filesystem::directory_iterator("data/bot/presets")) {
        ai::Bot preset;
        assert(ai::loadBot(entry.path().string(), preset) && !preset.name.empty());
        presets++;
    }
    assert(presets > 0);
    for (uint32_t seed = 1; seed <= 4; seed++) play(bot, seed, 150);

    // A slow, sloppy bot: its waits let gravity act before it moves.
    bot.pps = 1.5;
    bot.mistakes = 0.3;
    play(bot, 5, 100);

    std::atomic<bool> cancel = true;
    core::Game game(core::Ruleset{}, core::Handling{}, 1);
    ai::BotChoice choice;
    assert(!bot.choose(game, choice, &cancel));

    assert(bagChecks > placements / 2);
    std::printf("bot_test: %d placements, %d spins, %d soft drops, %d bag checks\n", placements, spins, softDrops, bagChecks);
}
