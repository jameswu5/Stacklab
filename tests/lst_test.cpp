// The LST matcher on hand-drawn boards (the shapes of four.lol's /stacking/lst, drawn here,
// not copied), and the LST bot's choices from data/bot/lst.txt.
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "ai/bot.h"
#include "ai/lst.h"
#include "modes/modes.h"

namespace {

// Rows top first, '#' a block.
core::Board board(std::initializer_list<const char*> rows) {
    core::Board out;
    int y = static_cast<int>(rows.size());
    for (const char* row : rows) {
        y--;
        for (int x = 0; x < core::kBoardWidth; x++)
            if (row[x] == '#') out.at(x, y) = core::Piece::Garbage;
    }
    return out;
}

bool alive(const core::Board& b) { return ai::lstAlive(ai::rowsOf(b)); }

}  // namespace

int main() {
    // Alive.
    assert(alive(core::Board{}));
    // An L overhang over the slot: the TSD is ready.
    const core::Board ready = board({
        "#.........",
        "#.........",
        "##........",
        "#...######",
        "##.#######",
        "##.#######",
    });
    assert(alive(ready));
    // A slot covered on the right, as after a TKI, with the cell above the cover filled.
    assert(alive(board({
        "...##.....",
        "#..##.....",
        "#...######",
        "##.#######",
    })));
    // A fractal: a slot covered on both sides, under one that's ready.
    assert(alive(board({
        "##........",
        "#...######",
        "##.#######",
        "#...######",
        "##.#######",
    })));

    // Broken.
    // four.lol's common mistake: covered on the right with the cell above the cover open, so
    // after the TSD column 3 is a one-wide gap.
    assert(!alive(board({
        "....#.....",
        "#..##.....",
        "#...######",
        "##.#######",
    })));
    assert(!alive(board({"..#.......", "##.#######"})));  // a block in the well
    assert(!alive(board({"##.##.####", "##.###.###"})));  // a hole in the stack
    assert(!alive(board({"##........", "#...######", "#...######", "##.#######"})));  // a slot two deep under the overhang
    assert(!alive(board({"##..######", "##..######", "##..######", "##..######"})));  // column 3 left behind: a two-wide well

    // The normal bot's files leave the LST terms off.
    ai::Bot preset;
    assert(ai::loadBot("data/bot/presets/6-pps-3.0.txt", preset) && preset.weights.lst == 0 && preset.weights.notTsd == 0);

    ai::Bot bot;
    assert(ai::loadBot("data/bot/lst.txt", bot) && bot.weights.lst < 0);

    // With the slot ready and a T in hand, the bot takes the TSD.
    {
        core::Game game(k40LRules, core::Handling{}, 1);
        game.board = ready;
        game.respawn(core::Piece::T);
        ai::BotChoice choice;
        assert(bot.choose(game, choice));
        assert(!choice.hold && choice.placement.spin == core::Spin::Normal);
        for (const core::Cell& c : choice.cells) assert((c.y == 2 && c.x >= 1 && c.x <= 3) || (c.y == 1 && c.x == 2));
    }

    // Ten bags from an empty board: the loop never breaks, and it makes TSDs.
    core::Game game(k40LRules, core::Handling{}, 1);
    int tsds = 0;
    for (int piece = 0; piece < 70; piece++) {
        ai::BotChoice choice;
        assert(bot.choose(game, choice));
        bool locked = false;
        for (const auto& tick : choice.inputs) {
            for (const core::LockResult& lock : game.tick(tick)) {
                locked = true;
                tsds += lock.piece == core::Piece::T && lock.lines == 2 && lock.spin == core::Spin::Normal;
            }
            if (locked) break;
        }
        assert(locked && !game.toppedOut && alive(game.board));
    }
    assert(tsds >= 3);
    std::printf("lst_test: %d TSDs in 70 pieces\n", tsds);
}
