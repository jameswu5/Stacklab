#include "modes/drill.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

#include "core/fumen.h"

using nlohmann::json;

namespace {

constexpr int kMaxTargets = 63;  // Play keeps which are filled in a uint64_t

[[noreturn]] void fail(const std::string& what) { throw std::invalid_argument(what); }

core::Piece pieceFrom(char name) {
    constexpr std::string_view kNames = "IJLOSTZ";
    size_t index = kNames.find(name);
    if (index == std::string_view::npos) fail(std::string("unknown piece '") + name + "'");
    return static_cast<core::Piece>(index + 1);
}

// Top row first, and the last is the floor.
core::Board boardFromRows(const json& rows) {
    if (!rows.is_array() || rows.size() > core::kVisibleHeight) fail("board must be a fumen string or up to 20 rows");
    core::Board board;
    for (size_t i = 0; i < rows.size(); i++) {
        const std::string row = rows[i].get<std::string>();
        if (row.size() != core::kBoardWidth) fail("board row \"" + row + "\" isn't 10 wide");
        int y = static_cast<int>(rows.size() - 1 - i);
        for (int x = 0; x < core::kBoardWidth; x++)
            board.at(x, y) = row[x] == '.' ? core::Piece::None : row[x] == 'X' ? core::Piece::Garbage : pieceFrom(row[x]);
    }
    return board;
}

// Either a placement, or the piece's four cells as [x, y].
DrillTarget targetFromJson(const json& value) {
    if (value.contains("cells")) {
        DrillTarget target{pieceFrom(value.at("piece").get<std::string>().at(0)), {}};
        const json& cells = value["cells"];
        if (!cells.is_array() || cells.size() != 4) fail("a target needs 4 cells");
        for (size_t i = 0; i < 4; i++) {
            target.cells[i] = {cells[i].at(0).get<int>(), cells[i].at(1).get<int>()};
            if (target.cells[i].x < 0 || target.cells[i].x >= core::kBoardWidth || target.cells[i].y < 0 || target.cells[i].y >= core::kBoardHeight) fail("a target is off the board");
        }
        return target;
    }
    core::Falling placed;
    placed.piece = pieceFrom(value.at("piece").get<std::string>().at(0));
    placed.x = value.at("x").get<int>();
    placed.y = value.at("y").get<int>();
    placed.rot = value.at("rot").get<int>();
    if (placed.rot < 0 || placed.rot > 3) fail("rot must be 0 to 3");
    auto cells = placed.cells();
    for (const core::Cell& cell : cells)
        if (cell.x < 0 || cell.x >= core::kBoardWidth || cell.y < 0 || cell.y >= core::kBoardHeight) fail("a target is off the board");
    return {placed.piece, cells};
}

}  // namespace

Drill parseDrill(std::string_view text, const std::string& id) {
    const json file = json::parse(text);
    Drill drill;
    drill.id = id;
    drill.name = file.at("name").get<std::string>();
    drill.blurb = file.value("blurb", "");
    drill.nextId = file.value("next", "");
    if (file.value("onMiss", "retry") != "retry") fail("onMiss must be \"retry\"");
    if (file.contains("ruleset")) fail("ruleset isn't supported yet");

    // A fumen's locked pieces also serve as the queue and targets, unless the file gives its own.
    std::vector<DrillTarget> fumenTargets;
    if (file.contains("board") && file["board"].is_string()) {
        const std::vector<core::FumenPage> pages = core::parseFumen(file["board"].get<std::string>());
        if (pages.empty()) fail("the fumen has no pages");
        drill.board = pages[0].board;
        for (const core::FumenPage& page : pages)
            if (page.piece.piece != core::Piece::None && page.lock) fumenTargets.push_back({page.piece.piece, page.piece.cells()});
    } else if (file.contains("board")) {
        drill.board = boardFromRows(file["board"]);
    }

    if (file.contains("queue")) {
        if (file["queue"].at("type") != "fixed") fail("queue type must be \"fixed\"");
        for (char name : file["queue"].at("pieces").get<std::string>()) drill.pieces.push_back(pieceFrom(name));
    } else {
        for (const DrillTarget& target : fumenTargets) drill.pieces.push_back(target.piece);
    }
    if (drill.pieces.empty()) fail("the queue is empty");

    if (file.contains("targets")) {
        for (const json& target : file["targets"]) drill.targets.push_back(targetFromJson(target));
    } else {
        drill.targets = fumenTargets;
    }
    if (drill.targets.empty()) fail("there are no targets");
    if (drill.targets.size() > kMaxTargets) fail("there are more than 63 targets");

    // Otherwise every attempt would top out at once, and retry forever.
    core::Game game(core::Ruleset{}, core::Handling{}, 1);
    setUpDrill(game, drill);
    if (game.toppedOut) fail("the board blocks the spawn");
    return drill;
}

void setUpDrill(core::Game& game, const Drill& drill) {
    game.board = drill.board;
    game.queue.assign(drill.pieces.begin() + 1, drill.pieces.end());
    game.queue.insert(game.queue.end(), game.rules.previews + 1, core::Piece::None);
    game.respawn(drill.pieces[0]);
}

DrillSet loadDrills(std::span<const std::filesystem::path> dirs) {
    DrillSet set;
    for (const std::filesystem::path& dir : dirs) {
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ignored)) {
            if (entry.path().extension() != ".json") continue;
            try {
                std::ifstream in(entry.path());
                std::stringstream text;
                text << in.rdbuf();
                set.drills.push_back(parseDrill(text.str(), entry.path().stem().string()));
            } catch (const std::exception& error) {
                set.errors.push_back(entry.path().filename().string() + ": " + error.what());
            }
        }
    }
    std::ranges::sort(set.drills, {}, &Drill::id);
    for (Drill& drill : set.drills) {
        if (drill.nextId.empty()) continue;
        auto next = std::ranges::find(set.drills, drill.nextId, &Drill::id);
        if (next == set.drills.end()) set.errors.push_back(drill.id + ": next drill \"" + drill.nextId + "\" not found");
        else drill.next = &*next;
    }
    return set;
}
