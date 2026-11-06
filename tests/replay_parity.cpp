// Compares core::Game against recorded results. BRIEF.md section 5.4.
//
//   replay_parity <fixture.json>  every lock, against hand-derived or Triangle.js values
//   replay_parity <folder>        each .ttr and .ttrm under it, against TETR.IO's own end state
//
// Only the second proves we match TETR.IO. Replays hold no mid-game state, so a
// divergence on any lock only shows up at the end of the round. Exits 77 if the
// folder has no replays.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/game.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kExitSkipped = 77;  // SKIP_RETURN_CODE in CMakeLists.txt
constexpr size_t kRecentInputs = 12;

// X is any filled cell; fixtures store it as Z.
core::Piece pieceOf(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case 'I': return core::Piece::I;
        case 'J': return core::Piece::J;
        case 'L': return core::Piece::L;
        case 'O': return core::Piece::O;
        case 'S': return core::Piece::S;
        case 'T': return core::Piece::T;
        case 'Z': case 'X': return core::Piece::Z;
        case 'G': return core::Piece::Garbage;
        default: return core::Piece::None;
    }
}

char charOf(core::Piece p) { return ".IJLOSTZG"[static_cast<int>(p)]; }

// TETR.IO names each block by piece letter, with "gb" for garbage.
core::Piece pieceOfTetrio(const json& cell) {
    if (cell.is_null()) return core::Piece::None;
    const std::string name = cell.get<std::string>();
    return name == "gb" ? core::Piece::Garbage : pieceOf(name[0]);
}

const std::map<std::string, core::Action> kActions = {
    {"moveLeft", core::Action::Left},       {"moveRight", core::Action::Right},
    {"softDrop", core::Action::SoftDrop},   {"hardDrop", core::Action::HardDrop},
    {"rotateCW", core::Action::RotateCW},   {"rotateCCW", core::Action::RotateCCW},
    {"rotate180", core::Action::Rotate180}, {"hold", core::Action::Hold},
};

// Options missing from a file take Triangle.js's fallbacks, as Ruleset does.
core::Ruleset rulesFrom(const json& o) {
    core::Ruleset r;
    r.gravity = o.value("g", r.gravity);
    r.gravityIncrease = o.value("gincrease", r.gravityIncrease);
    r.gravityMargin = o.value("gmargin", r.gravityMargin);
    r.lockTime = o.value("locktime", r.lockTime);
    r.lockResets = o.value("lockresets", r.lockResets);
    r.may20g = o.value("gravitymay20g", r.may20g);
    r.allow180 = o.value("allow180", r.allow180);
    r.allowHold = o.value("display_hold", r.allowHold);
    r.clutch = o.value("clutch", r.clutch);
    r.previews = o.value("nextcount", r.previews);
    // Not Triangle.js's all-mini+ fallback: Blitz omits this and only counts T-spins.
    const std::string spins = o.value("spinbonuses", std::string("T-spins"));
    r.spinBonuses = spins == "none"      ? core::SpinBonuses::None
                    : spins == "T-spins" ? core::SpinBonuses::TSpins
                                         : core::SpinBonuses::AllMiniPlus;
    r.b2bCharging = o.value("b2bcharging", r.b2bCharging);
    r.b2bChaining = o.value("b2bchaining", !r.b2bCharging);
    r.b2bChargeAt = o.value("b2bcharge_at", r.b2bChargeAt);
    r.b2bChargeBase = o.value("b2bcharge_base", r.b2bChargeBase);
    r.pcGarbage = o.value("allclear_garbage", r.pcGarbage);
    r.pcB2b = o.value("allclear_b2b", r.pcB2b);
    r.garbageMultiplier = o.value("garbagemultiplier", r.garbageMultiplier);
    r.garbageIncrease = o.value("garbageincrease", r.garbageIncrease);
    r.garbageMargin = o.value("garbagemargin", r.garbageMargin);
    r.garbageSpecialBonus = o.value("garbagespecialbonus", r.garbageSpecialBonus);
    r.roundDown = o.value("roundmode", std::string("down")) != "rng";
    return r;
}

core::Handling handlingFrom(const json& o) {
    core::Handling h;
    const json e = o.value("handling", json::object());
    h.arr = e.value("arr", h.arr);
    h.das = e.value("das", h.das);
    h.dcd = e.value("dcd", h.dcd);
    h.sdf = e.value("sdf", h.sdf);
    h.safelock = e.value("safelock", h.safelock);
    h.cancel = e.value("cancel", h.cancel);
    h.may20g = e.value("may20g", h.may20g);
    h.irsHold = e.value("irs", std::string("tap")) == "hold";
    h.ihsHold = e.value("ihs", std::string("tap")) == "hold";
    return h;
}

// Rules core has no setting for. A replay using one can't be expected to match.
std::string unsupported(const json& o) {
    std::string out;
    const auto require = [&](const char* key, const json& wanted) {
        if (o.contains(key) && o[key] != wanted) out += std::string(key) + "=" + o[key].dump() + " ";
    };
    require("kickset", "SRS+");
    require("bagtype", "7-bag");
    require("combotable", "multiplier");
    require("boardwidth", core::kBoardWidth);
    require("boardheight", core::kVisibleHeight);
    require("boardbuffer", core::kBoardHeight - core::kVisibleHeight);
    require("infinite_movement", false);
    require("infinite_hold", false);
    require("allow_harddrop", true);
    require("are", 0);
    require("lineclear_are", 0);
    const std::string spins = o.value("spinbonuses", std::string("T-spins"));
    if (spins != "all-mini+" && spins != "T-spins" && spins != "none") out += "spinbonuses=" + spins + " ";
    return out;
}

// Bottom row first, without the empty rows on top.
std::vector<std::string> rowsOf(const core::Board& board) {
    std::vector<std::string> rows;
    for (int y = 0; y < core::kBoardHeight; y++) {
        std::string row;
        for (int x = 0; x < core::kBoardWidth; x++) row += charOf(board.at(x, y));
        rows.push_back(row);
    }
    while (!rows.empty() && rows.back().find_first_not_of('.') == std::string::npos) rows.pop_back();
    return rows;
}

std::string boardDiff(const std::vector<std::string>& want, const std::vector<std::string>& got) {
    const std::string empty(core::kBoardWidth, '.');
    std::ostringstream out;
    out << "  row  expected    actual\n";
    for (size_t y = std::max(want.size(), got.size()); y-- > 0;) {
        const std::string& a = y < want.size() ? want[y] : empty;
        const std::string& b = y < got.size() ? got[y] : empty;
        out << "  " << std::string(3 - std::min<size_t>(3, std::to_string(y).size()), ' ') << y << "  " << a
            << "  " << b << (a == b ? "\n" : "  <\n");
    }
    return out.str();
}

std::string boardText(const std::vector<std::string>& rows) {
    std::string out;
    for (size_t y = rows.size(); y-- > 0;) out += "  " + rows[y] + "\n";
    return out;
}

std::vector<int> sortedCells(const core::Falling& f) {
    auto cells = f.cells();
    std::sort(cells.begin(), cells.end(), [](auto& a, auto& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    std::vector<int> flat;
    for (const core::Cell& c : cells) {
        flat.push_back(c.x);
        flat.push_back(c.y);
    }
    return flat;
}

std::string text(const std::vector<int>& values) {
    std::string out;
    for (int v : values) out += std::to_string(v) + " ";
    return out;
}

std::vector<int> nonZero(const core::LockResult& lock) {
    std::vector<int> sends;
    for (int s : lock.sends) {
        if (s != 0) sends.push_back(s);
    }
    return sends;
}

const char* spinName(core::Spin s) {
    return s == core::Spin::Normal ? "normal" : s == core::Spin::Mini ? "mini" : "none";
}

struct Input {
    int frame;
    std::string key;
    core::InputEvent event;
};

std::string recentInputs(const std::vector<Input>& inputs, int frame) {
    std::vector<std::string> lines;
    for (const Input& in : inputs) {
        if (in.frame > frame) break;
        lines.push_back("  " + std::to_string(in.frame) + " +" + std::to_string(in.event.subframe).substr(0, 4) + " " +
                        in.key + (in.event.pressed ? " down" : " up") + (in.event.hoisted ? " hoisted" : "") + "\n");
    }
    std::string out = "recent inputs (frame +subframe):\n";
    const size_t from = lines.size() > kRecentInputs ? lines.size() - kRecentInputs : 0;
    for (size_t i = from; i < lines.size(); i++) out += lines[i];
    return out;
}

// Feeds inputs for one tick, advancing `next` through the frame-sorted list.
const std::vector<core::LockResult>& step(core::Game& game, const std::vector<Input>& inputs, size_t& next, int frame) {
    std::vector<core::InputEvent> events;
    for (; next < inputs.size() && inputs[next].frame == frame; next++) events.push_back(inputs[next].event);
    return game.tick(events);
}

// Returns the first mismatch; its first line is the summary.
std::optional<std::string> playFixture(const json& g) {
    const json options = g.value("options", json::object());
    core::Game game(rulesFrom(options), handlingFrom(options), g["seed"].get<uint32_t>());

    // Injected after the first piece spawns from the seed.
    const json rows = g.value("board", json::array());
    for (size_t y = 0; y < rows.size(); y++) {
        const std::string row = rows[y].get<std::string>();
        for (size_t x = 0; x < row.size(); x++) game.board.at(x, y) = pieceOf(row[x]);
    }
    if (g.contains("queue")) {
        game.queue.clear();
        for (const json& p : g["queue"]) game.queue.push_back(pieceOf(p.get<std::string>()[0]));
    }
    if (g.contains("b2b")) game.stats.b2b = g["b2b"];
    if (g.contains("combo")) game.stats.combo = g["combo"];

    std::vector<Input> inputs;
    for (const json& in : g["inputs"]) {
        const std::string key = in[1];
        inputs.push_back({in[0], key, {kActions.at(key), in[2].get<int>() != 0, in[3], in.size() > 4 && in[4].get<int>() != 0}});
    }

    const json locks = g.value("locks", json::array());
    const json trace = g.value("trace", json::array());
    const int frames = g["frames"];
    size_t next = 0, nextLock = 0, nextTrace = 0;
    std::vector<int> wantCells;

    const auto mismatch = [&](int frame, const std::string& what, const std::string& detail = "") {
        return "frame " + std::to_string(frame) + ": " + what + "\n" + detail + recentInputs(inputs, frame);
    };

    for (int frame = 0; frame < frames; frame++) {
        const auto& got = step(game, inputs, next, frame);
        for (size_t i = 0; i < got.size(); i++) {
            if (nextLock >= locks.size() || locks[nextLock]["frame"] != frame) {
                return mismatch(frame, "unexpected lock of " + std::string(1, charOf(got[i].piece)),
                                boardText(rowsOf(game.board)));
            }
            const json& want = locks[nextLock];
            const std::string n = "lock " + std::to_string(nextLock) + " ";
            if (want["piece"] != std::string(1, charOf(got[i].piece))) {
                return mismatch(frame, n + "piece: want " + want["piece"].get<std::string>() + " got " + charOf(got[i].piece));
            }
            if (want["lines"] != got[i].lines) {
                return mismatch(frame, n + "lines: want " + want["lines"].dump() + " got " + std::to_string(got[i].lines));
            }
            if (want.value("spin", "none") != spinName(got[i].spin)) {
                return mismatch(frame, n + "spin: want " + want.value("spin", "none") + " got " + spinName(got[i].spin));
            }
            if (want.value("pc", false) != got[i].perfectClear) {
                return mismatch(frame, n + "perfect clear: want " + want.value("pc", json(false)).dump());
            }
            const auto wantSends = want.value("sends", std::vector<int>{});
            if (wantSends != nonZero(got[i])) {
                return mismatch(frame, n + "sends: want " + text(wantSends) + "got " + text(nonZero(got[i])));
            }
            if (want["b2b"] != got[i].b2b) {
                return mismatch(frame, n + "b2b: want " + want["b2b"].dump() + " got " + std::to_string(got[i].b2b));
            }
            if (want["combo"] != got[i].combo) {
                return mismatch(frame, n + "combo: want " + want["combo"].dump() + " got " + std::to_string(got[i].combo));
            }
            // Game only exposes the board at the end of a tick, so when two
            // pieces lock in one tick only the last one's board is checked.
            if (want.contains("board") && i + 1 == got.size()) {
                const auto wantRows = want["board"].get<std::vector<std::string>>();
                if (wantRows != rowsOf(game.board)) {
                    return mismatch(frame, n + "board differs", boardDiff(wantRows, rowsOf(game.board)));
                }
            }
            nextLock++;
        }
        if (nextLock < locks.size() && locks[nextLock]["frame"] <= frame) {
            return mismatch(frame, "missing lock " + std::to_string(nextLock) + " of " + locks[nextLock]["piece"].get<std::string>(),
                            boardDiff(locks[nextLock].value("board", std::vector<std::string>{}), rowsOf(game.board)));
        }
        if (game.toppedOut && frame + 1 < frames) return mismatch(frame, "topped out early", boardText(rowsOf(game.board)));
        if (nextTrace < trace.size() && trace[nextTrace][0] == frame) {
            wantCells.clear();
            for (size_t i = 1; i < trace[nextTrace].size(); i++) wantCells.push_back(trace[nextTrace][i]);
            nextTrace++;
        }
        if (!trace.empty() && !game.toppedOut && wantCells != sortedCells(game.falling)) {
            return mismatch(frame, "falling cells: want " + text(wantCells) + "got " + text(sortedCells(game.falling)),
                            boardText(rowsOf(game.board)));
        }
    }

    const json end = g.value("end", json::object());
    const auto check = [&](const char* key, const json& got) -> std::optional<std::string> {
        if (!end.contains(key) || end[key] == got) return std::nullopt;
        return mismatch(frames - 1, std::string("end ") + key + ": want " + end[key].dump() + " got " + got.dump());
    };
    if (auto m = check("toppedOut", game.toppedOut)) return m;
    if (end.contains("board") && end["board"].get<std::vector<std::string>>() != rowsOf(game.board)) {
        return mismatch(frames - 1, "end board differs", boardDiff(end["board"], rowsOf(game.board)));
    }
    if (!game.toppedOut) {
        if (auto m = check("piece", std::string(1, charOf(game.falling.piece)))) return m;
        if (auto m = check("rot", game.falling.rot)) return m;
        if (auto m = check("cells", sortedCells(game.falling))) return m;
    }
    if (auto m = check("hold", game.hold == core::Piece::None ? "" : std::string(1, charOf(game.hold)))) return m;
    if (end.contains("next")) {
        std::string next5;
        const std::string want = end["next"];
        for (size_t i = 0; i < want.size() && i < game.queue.size(); i++) next5 += charOf(game.queue[i]);
        if (auto m = check("next", next5)) return m;
    }
    if (auto m = check("pieces", game.stats.pieces)) return m;
    if (auto m = check("lines", game.stats.lines)) return m;
    if (auto m = check("attack", game.stats.attack)) return m;
    if (auto m = check("b2b", game.stats.b2b)) return m;
    if (auto m = check("combo", game.stats.combo)) return m;
    return std::nullopt;
}

// A fixture's first failing game gets the full report, the rest one line each.
int runFixture(const fs::path& path) {
    std::ifstream file(path);
    const json fixture = json::parse(file);
    int passed = 0, failed = 0;
    for (const json& game : fixture["games"]) {
        const auto m = playFixture(game);
        if (!m) {
            passed++;
            continue;
        }
        const std::string name = path.filename().string() + ": " + game.value("name", "");
        if (failed++ == 0) std::cout << "FAIL " << name << "\n" << *m << "\n";
        else std::cout << "FAIL " << name << ": " << m->substr(0, m->find('\n')) << "\n";
    }
    std::cout << path.filename().string() << " (" << fixture.value("source", "?") << "): " << passed << " passed, "
              << failed << " failed\n";
    return failed ? 1 : 0;
}

// Counts locks the way TETR.IO's stats.clears does.
void countClear(std::map<std::string, int>& clears, const core::LockResult& lock) {
    static const char* const kLines[] = {"", "singles", "doubles", "triples", "quads", "pentas"};
    const int lines = std::min(lock.lines, 5);
    if (lock.spin == core::Spin::None) {
        if (lines > 0) clears[kLines[lines]]++;
    } else if (lines == 0) {
        clears[lock.spin == core::Spin::Mini ? "minitspins" : "realtspins"]++;
    } else {
        clears[std::string(lock.spin == core::Spin::Mini ? "minitspin" : "tspin") + kLines[lines]]++;
    }
    if (lock.perfectClear) clears["allclear"]++;
}

// Plays one player's round against what TETR.IO recorded. Every replay has final
// stats. League rounds also store the final game in their `end` event; solo .ttr
// files don't, so for those the board can't be checked directly. Solo stats also
// report 0 attack, so sends only get checked on replays with garbage on.
std::optional<std::string> playReplay(const json& replay, bool& skipped) {
    const json* endEvent = nullptr;
    std::vector<Input> inputs;
    for (const json& e : replay["events"]) {
        const std::string type = e["type"];
        // Incoming garbage is phase 6.
        if (type == "ige" && e["data"]["type"] == "interaction") {
            skipped = true;
            return std::nullopt;
        }
        if (type == "end") endEvent = &e;
        if (type != "keydown" && type != "keyup") continue;
        // Keys core has no action for, like retry and exit, have no effect here.
        const std::string key = e["data"]["key"];
        const auto action = kActions.find(key);
        if (action == kActions.end()) continue;
        const bool hoisted = e["data"].value("hoisted", false);
        if (hoisted && (e["frame"] != 0 || type != "keydown")) std::cout << "note: hoisted " << type << " on frame " << e["frame"] << "\n";
        inputs.push_back({e["frame"], key, {action->second, type == "keydown", e["data"]["subframe"], hoisted}});
    }
    if (!endEvent) return "no end event\n";
    const json& data = (*endEvent)["data"];
    const int endFrame = (*endEvent)["frame"];
    if (endFrame == 0) {  // disconnected before the game started
        skipped = true;
        return std::nullopt;
    }

    const json& options = data.contains("options") ? data["options"] : replay["options"];
    if (const std::string bad = unsupported(options); !bad.empty()) return "unsupported options: " + bad + "\n";

    core::Game game(rulesFrom(options), handlingFrom(options), options["seed"].get<uint32_t>());

    // The final press lands on the end frame itself, in which case the falling
    // piece isn't comparable: TETR.IO stopped mid-tick and we run the whole tick.
    bool pressOnEnd = false;
    for (const Input& in : inputs) pressOnEnd |= in.frame == endFrame && in.event.pressed;
    const int ticks = std::max(replay.value("frames", 0), pressOnEnd ? endFrame + 1 : endFrame);

    std::map<std::string, int> clears;
    int topB2b = -1, topCombo = -1, finishFrame = -1;
    const int objectiveLines = options.value("objective_type", std::string()) == "lines" ? options.value("objective_count", 0) : 0;
    size_t next = 0;
    for (int frame = 0; frame < ticks; frame++) {
        for (const core::LockResult& lock : step(game, inputs, next, frame)) {
            topB2b = std::max(topB2b, lock.b2b);
            topCombo = std::max(topCombo, lock.combo);
            countClear(clears, lock);
        }
        if (finishFrame < 0 && objectiveLines > 0 && game.stats.lines >= objectiveLines) finishFrame = frame;
        if (game.toppedOut && frame + 1 < ticks) {
            return "frame " + std::to_string(frame) + " of " + std::to_string(ticks) + ": topped out early after " +
                   std::to_string(game.stats.pieces) + " pieces, " + std::to_string(game.stats.lines) + " lines\n" +
                   boardText(rowsOf(game.board)) + recentInputs(inputs, frame);
        }
    }

    std::string diffs;
    const auto stat = [&](const std::string& name, int want, int got) {
        if (want != got) diffs += "  " + name + ": want " + std::to_string(want) + " got " + std::to_string(got) + "\n";
    };

    const json& s = data.contains("stats") ? data["stats"] : replay["results"]["stats"];
    // TETR.IO counts b2b and combo from 0 where core starts at -1.
    stat("pieces", s["piecesplaced"], game.stats.pieces);
    stat("lines", s["lines"], game.stats.lines);
    stat("btb", s["btb"], game.stats.b2b + 1);
    stat("combo", s["combo"], game.stats.combo + 1);
    stat("topbtb", s["topbtb"], topB2b + 1);
    stat("topcombo", s["topcombo"], topCombo + 1);
    for (const auto& [name, want] : s["clears"].items()) stat("clears." + name, want, clears[name]);
    if (options.value("hasgarbage", false)) stat("attack", s["garbage"]["attack"], game.stats.attack);
    if (objectiveLines > 0) stat("finish frame", endFrame, finishFrame);

    std::string boardReport;
    if (data.contains("game")) {
        const json& final = data["game"];
        std::vector<std::string> wantRows;  // TETR.IO stores rows top first
        for (auto row = final["board"].rbegin(); row != final["board"].rend(); ++row) {
            std::string text;
            for (const json& cell : *row) text += charOf(pieceOfTetrio(cell));
            wantRows.push_back(text);
        }
        while (!wantRows.empty() && wantRows.back().find_first_not_of('.') == std::string::npos) wantRows.pop_back();
        if (wantRows != rowsOf(game.board)) {
            diffs += "  board differs\n";
            boardReport = boardDiff(wantRows, rowsOf(game.board));
        }

        const core::Piece wantHold = pieceOfTetrio(final["hold"]["piece"]);
        if (wantHold != game.hold) diffs += std::string("  hold: want ") + charOf(wantHold) + " got " + charOf(game.hold) + "\n";

        // Only the visible previews: TETR.IO's bag can run longer than ours.
        const json& bag = final["bag"];
        for (size_t i = 0; i < bag.size() && i < game.queue.size() && i < 5; i++) {
            if (pieceOf(bag[i].get<std::string>()[0]) != game.queue[i]) {
                diffs += "  next piece " + std::to_string(i) + " differs\n";
                break;
            }
        }

        if (!pressOnEnd && !game.toppedOut) {
            const json& f = final["falling"];
            core::Falling want;
            want.piece = pieceOf(f["type"].get<std::string>()[0]);
            want.rot = f["r"];
            // TETR.IO's y counts down from the top, and its x is one right of ours except for O.
            want.x = f["x"].get<int>() - (want.piece == core::Piece::O ? 0 : 1);
            want.y = core::kBoardHeight - f["y"].get<double>();
            if (want.piece != game.falling.piece || want.rot != game.falling.rot || sortedCells(want) != sortedCells(game.falling)) {
                diffs += "  falling: want " + std::string(1, charOf(want.piece)) + " rot " + std::to_string(want.rot) + " cells " +
                         text(sortedCells(want)) + "got " + charOf(game.falling.piece) + " rot " + std::to_string(game.falling.rot) +
                         " cells " + text(sortedCells(game.falling)) + "\n";
            }
        }
    }

    if (diffs.empty()) return std::nullopt;
    return "end frame " + std::to_string(endFrame) + " differs\n" + diffs + boardReport + recentInputs(inputs, ticks - 1);
}

int runReplays(const fs::path& folder) {
    std::vector<fs::path> files;
    if (fs::is_directory(folder)) {
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            const std::string ext = entry.path().extension().string();
            if (ext == ".ttr" || ext == ".ttrm") files.push_back(entry.path());
        }
    }
    if (files.empty()) {
        std::cout << "SKIPPED: no .ttr or .ttrm replays in " << folder.string() << "\n";
        return kExitSkipped;
    }
    std::sort(files.begin(), files.end());

    int passed = 0, failed = 0, skipped = 0;
    for (const fs::path& path : files) {
        const std::string name = fs::relative(path, folder).string();
        const auto fail = [&](const std::string& label, const std::string& why) {
            failed++;
            std::cout << "FAIL " << label << ": " << why << "\n";
        };
        std::ifstream file(path);
        const json top = json::parse(file, nullptr, false);
        if (top.is_discarded()) {
            fail(name, "not JSON");
            continue;
        }
        if (!top.contains("replay") || !top["replay"].is_object()) {
            fail(name, "unknown format, top-level keys: " + [&] { std::string k; for (const auto& [key, v] : top.items()) k += key + " "; return k; }());
            continue;
        }
        const json& replay = top["replay"];

        // .ttr holds one game in `replay`; .ttrm holds rounds of players.
        std::vector<std::pair<std::string, const json*>> rounds;
        if (replay.contains("rounds")) {
            for (size_t r = 0; r < replay["rounds"].size(); r++) {
                const json& players = replay["rounds"][r];
                for (size_t p = 0; p < players.size(); p++) {
                    rounds.push_back({name + " round " + std::to_string(r) + " player " + std::to_string(p), &players[p]["replay"]});
                }
            }
        } else if (replay.contains("events")) {
            rounds.push_back({name, &replay});
        } else {
            fail(name, "unknown format, replay has no events or rounds");
            continue;
        }

        for (const auto& [label, round] : rounds) {
            bool skip = false;
            const auto m = playReplay(*round, skip);
            if (skip) skipped++;
            else if (m) fail(label, *m);
            else passed++;
        }
    }
    std::cout << folder.filename().string() << " (TETR.IO recorded results): " << passed << " passed, " << failed << " failed, "
              << skipped << " skipped (incoming garbage or never started)\n";
    return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: replay_parity <fixture.json | replay folder>\n";
        return 2;
    }
    const fs::path path = argv[1];
    return path.extension() == ".json" ? runFixture(path) : runReplays(path);
}
