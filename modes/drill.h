// Drills: a set board, a fixed queue and the placements to make, as JSON files
// (BRIEF.md section 4). A new drill is a new file.
#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/game.h"

struct DrillTarget {
    core::Piece piece;
    std::array<core::Cell, 4> cells;  // on the board as it stands when this piece locks
};

struct Drill {
    std::string id;  // the file's name, without .json
    std::string name;
    std::string blurb;
    core::Board board;
    std::vector<core::Piece> pieces;
    std::vector<DrillTarget> targets;
    std::string nextId;
    const Drill* next = nullptr;  // set by loadDrills
};

// Throws a std::exception saying what's wrong, since these files come from outside.
Drill parseDrill(std::string_view text, const std::string& id);

// Puts the drill's board and queue on a new game. The queue is padded with
// Piece::None, which shows as an empty preview and spawns once it runs out.
void setUpDrill(core::Game& game, const Drill& drill);

struct DrillSet {
    std::vector<Drill> drills;  // by id. Don't copy: `next` points into this
    std::vector<std::string> errors;  // one per file that couldn't be used
};

// Every .json file in these folders. A missing folder is fine.
DrillSet loadDrills(std::span<const std::filesystem::path> dirs);
