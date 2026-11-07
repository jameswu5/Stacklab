// A first bot: beam search over the move search's placements of the visible pieces,
// scored by a hand-weighted evaluation. It sees what a player sees: the board, the
// falling piece, hold and the previews. botInputs turns its choice into real key
// presses for core, so it can't break the rules.
#pragma once

#include <string>
#include <vector>

#include "ai/move_search.h"

namespace ai {

// Read from a data file by loadBotWeights; these are the fallbacks.
struct BotWeights {
    double holes = -4;        // per empty cell under a column's top
    double height = -0.3;     // per row of the average column height
    double danger = -1;       // per row squared of the highest column above 10
    double bumpiness = -0.4;  // per row of difference between neighbours, the well left out
    double well = 0.6;        // per row of depth of the lowest column, up to 4
    double tSlot = 2.5;       // a T-spin double slot is open
    double attack = 2;        // per line sent on the way
    double b2b = 1.5;         // per level of the back-to-back chain
    double wasted = -1.5;     // per line cleared without sending anything
};

// `name value` lines; `#` starts a comment. False if the file can't be read or a name isn't known.
bool loadBotWeights(const std::string& path, BotWeights& weights);

struct BotChoice {
    bool hold = false;  // press hold first
    Placement placement{};
    std::array<core::Cell, 4> cells{};  // where it locks, before lines clear
    std::vector<Move> moves;            // from where the piece is; a hard drop follows
};

class Bot {
public:
    BotWeights weights;
    int width = 48;  // states kept at each depth

    // False when nothing can be placed.
    bool choose(const core::Game& game, BotChoice& choice);

private:
    struct Node {
        std::array<uint16_t, core::kBoardHeight> rows;  // a bit per column, floor first
        core::Piece current, hold;
        uint8_t next;  // index into the previews of the piece after `current`
        int8_t b2b, combo;
        int16_t root;  // which first move this came from
        float reward, score;
    };
    struct Root {
        bool hold;
        Placement placement;
    };

    void expand(const Node& node, const core::Game& game, bool atRoot);
    void addChild(const Node& parent, const Placement& placement, core::Piece piece, core::Piece hold, core::Piece current, int next, bool atRoot, bool held);
    float evaluate(const Node& node) const;

    MoveSearch search_;
    std::vector<Node> beam_, children_;
    std::vector<Root> roots_;
    std::vector<core::Piece> queue_;
    const core::Ruleset* rules_ = nullptr;
};

// One tick's key events each, from `game` as it stands through the hard drop. They're
// worked out on a copy of the game, which doesn't change it.
std::vector<std::vector<core::InputEvent>> botInputs(const core::Game& game, const BotChoice& choice);

}  // namespace ai
