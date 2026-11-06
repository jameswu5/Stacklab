// Parses fumen strings, the format the community uses to share setups.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/game.h"

namespace core {

struct FumenPage {
    Board board;           // as the page shows it, before its piece locks
    Falling piece;         // piece.piece is Piece::None if the page has no piece
    bool lock = true;      // whether the piece locks on the way to the next page
    std::string comment;   // UTF-8, carried over from the last page that set one
};

// Takes a v115 or v110 string, alone or inside a URL. Throws std::invalid_argument
// saying what's wrong, since these strings come from outside.
std::vector<FumenPage> parseFumen(std::string_view text);

}  // namespace core
