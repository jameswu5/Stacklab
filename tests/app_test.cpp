// Self-checks for input timing, saved settings and the 40L run.
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "app/play.h"
#include "app/settings.h"
#include "app/timing.h"

static size_t allocations = 0;

void* operator new(size_t size) {
    allocations++;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, size_t) noexcept { std::free(memory); }

namespace {

using core::Action;

void testTickAt() {
    assert(tickAt(0).tick == 0);
    assert(tickAt(0).subframe == 0);
    // 25 ms is 1.5 ticks.
    assert(tickAt(25'000'000).tick == 1);
    assert(tickAt(25'000'000).subframe == 0.5);
    // One second is exactly 60 ticks, and an hour doesn't drift.
    assert(tickAt(1'000'000'000).tick == 60);
    assert(tickAt(3600ull * 1'000'000'000).tick == 216000);
    assert(tickAt(3600ull * 1'000'000'000).subframe == 0);
}

void testQueue() {
    InputQueue queue;
    queue.push(Action::Left, true, 1'000'000 + 25'000'000, 1'000'000, 0);    // tick 1, 0.5
    queue.push(Action::Left, false, 1'000'000 + 40'000'000, 1'000'000, 0);   // tick 2, 0.4
    assert(queue.due(0).empty());
    assert(queue.due(1).size() == 1);
    assert(queue.due(1)[0].subframe == 0.5);
    assert(queue.due(2).size() == 2);
    queue.consume(1);
    assert(queue.due(2).size() == 1);
    queue.consume(1);
    assert(queue.events.empty());

    // A late event goes at the start of the current tick.
    queue.push(Action::Right, true, 25'000'000, 0, 5);
    assert(queue.due(5).size() == 1);
    assert(queue.due(5)[0].subframe == 0);
    // Positions stay in order when timestamps go backwards.
    queue.clear();
    queue.push(Action::Right, true, 25'000'000, 0, 0);
    queue.push(Action::Right, false, 20'000'000, 0, 0);
    assert(queue.ticks[1] == 1);
    assert(queue.events[1].subframe == 0.5);
    // An event from before the run started lands on tick 0.
    queue.clear();
    queue.push(Action::Left, true, 5, 100, 0);
    assert(queue.due(0).size() == 1);
}

void testSettings() {
    std::string path = (std::filesystem::temp_directory_path() / "stacklab_app_test.json").string();
    std::filesystem::remove(path);

    // Missing file: defaults.
    Settings settings = loadSettings(path);
    assert(settings.profiles.size() == 1);
    assert(settings.profile().keys[BindCW] == std::vector{SDL_SCANCODE_UP});

    // Several keys per action, and a key only ever has one action.
    bindKey(settings.profile(), BindLeft, SDL_SCANCODE_J);
    bindKey(settings.profile(), BindRight, SDL_SCANCODE_J);
    assert(settings.profile().keys[BindLeft] == std::vector{SDL_SCANCODE_LEFT});
    assert(settings.profile().keys[BindRight].size() == 2);

    adjustRow(settings, RowDas, -1);
    adjustRow(settings, RowArr, -1);  // already at the minimum
    adjustRow(settings, RowSdf, -1);
    adjustRow(settings, RowVsync, 1);
    adjustRow(settings, RowSafelock, 1);
    adjustRow(settings, RowIrs, 1);
    assert(rowValue(settings, RowSafelock) == "On");
    assert(rowValue(settings, RowIrs) == "Hold");
    assert(rowValue(settings, RowIhs) == "Tap");
    assert(settings.profile().handling.das == 5.9);
    assert(settings.profile().handling.arr == 0);
    assert(settings.profile().handling.sdf == 40);

    Profile second = settings.profile();
    second.name = "Second";
    settings.profiles.push_back(second);
    settings.active = 1;
    settings.dark = true;
    saveSettings(settings, path);

    Settings loaded = loadSettings(path);
    assert(loaded.profiles.size() == 2);
    assert(loaded.active == 1);
    assert(loaded.vsync);
    assert(loaded.dark);
    assert(loaded.profile().name == "Second");
    assert(loaded.profile().handling.das == 5.9);
    assert(loaded.profile().handling.safelock);
    assert(loaded.profile().handling.irsHold);
    assert(!loaded.profile().handling.ihsHold);
    assert(loaded.profile().keys[BindRight].size() == 2);

    // A file from before undo existed gets its default key, unless that key is taken.
    { std::ofstream(path) << R"({"profiles": [{"keys": {"left": [4]}}]})"; }
    assert(loadSettings(path).profile().keys[BindUndo] == std::vector{SDL_SCANCODE_Q});
    { std::ofstream(path) << R"({"profiles": [{"keys": {"left": [20]}}]})"; }  // Q
    assert(loadSettings(path).profile().keys[BindUndo].empty());

    // A damaged file reads as defaults.
    { std::ofstream(path) << "{ not json"; }
    assert(loadSettings(path).profiles.size() == 1);
    // Wrong types and out of range values don't get through.
    { std::ofstream(path) << R"({"active": 7, "profiles": [{"keys": {"left": [99999, 4], "hold": "x"}, "handling": {"das": "slow"}}]})"; }
    loaded = loadSettings(path);
    assert(loaded.active == 0);
    assert(loaded.profile().keys[BindLeft] == std::vector{SDL_SCANCODE_A});
    assert(loaded.profile().handling.das == core::Handling{}.das);
    std::filesystem::remove(path);

    saveBest(31.5, path);
    assert(loadBest(path) == 31.5);
    // A best saved before splits were dropped still counts.
    { std::ofstream(path) << R"({"splits": [0, 1.5, 60.5]})"; }
    assert(loadBest(path) == 60.5);
    std::filesystem::remove(path);
    assert(loadBest(path) == 0);
}

constexpr uint64_t kTickNs = 16'666'667;  // rounded up, so n * kTickNs has reached tick n

void testPlayKeys() {
    Play play;
    play.restart({}, 1, 1000);
    // Two keys on one action: one press, then one release once both are up.
    play.key(BindLeft, true, 1000);
    play.key(BindLeft, true, 1000);
    assert(play.inputs.events.size() == 1);
    play.key(BindLeft, false, 1000);
    assert(play.inputs.events.size() == 1);
    play.key(BindLeft, true, 1000);
    assert(play.inputs.events.size() == 1);

    // Retry keeps movement that's held, on tick 0, but not hard drop.
    play.key(BindHardDrop, true, 1000);
    play.restart({}, 1, 5000);
    play.stepTo(5000);
    assert(play.inputs.events.size() == 1);
    assert(play.inputs.events[0].action == Action::Left);
    assert(play.inputs.events[0].pressed);
    assert(play.inputs.ticks[0] == 0);

    // Held left and right both carry over charged, most recently pressed last.
    play.held.fill(0);
    play.key(BindRight, true, 5000);
    play.key(BindLeft, true, 5000);
    play.restart({}, 1, 6000);
    play.stepTo(6000);
    assert(play.inputs.events.size() == 2);
    assert(play.inputs.events[0].action == Action::Right);
    assert(play.inputs.events[1].action == Action::Left);
    assert(play.inputs.events[0].hoisted && play.inputs.events[1].hoisted);
    assert(play.inputs.events[1].subframe == 0);
    // A press during play isn't.
    play.key(BindHardDrop, false, 6000);
    play.key(BindHardDrop, true, 7000);
    assert(!play.inputs.events.back().hoisted);

    // A held hold or rotate carries over hoisted too, ahead of the moves. Hard
    // drop never does, and soft drop isn't hoisted.
    play.held.fill(0);
    play.key(BindLeft, true, 6000);
    for (Bind bind : {BindHardDrop, BindSoftDrop, Bind180, BindCCW, BindCW, BindHold}) play.key(bind, true, 6000);
    play.restart({}, 1, 7000);
    play.stepTo(7000);
    const std::vector<Action> order = {Action::SoftDrop, Action::Hold, Action::RotateCW, Action::RotateCCW, Action::Rotate180, Action::Left};
    assert(play.inputs.events.size() == order.size());
    for (size_t i = 0; i < order.size(); i++) {
        assert(play.inputs.events[i].action == order[i]);
        assert(play.inputs.events[i].pressed);
        assert(play.inputs.ticks[i] == 0);
        assert(play.inputs.events[i].subframe == 0);
        assert(play.inputs.events[i].hoisted == (order[i] != Action::SoftDrop));
    }
    core::Piece first = play.game.falling.piece;
    play.stepTo(7000 + kTickNs);
    assert(play.game.hold == first);

    // A release with nothing held does nothing.
    play.held.fill(0);
    play.restart({}, 1, 6000);
    play.key(BindRight, false, 6000);
    assert(play.inputs.events.empty());
}

void testCountdown() {
    Play play;
    play.restart({}, 1, 1000);
    // Keys pressed and released during the countdown do nothing, even hard drop.
    play.key(BindHardDrop, true, 500);
    play.key(BindHardDrop, false, 600);
    play.key(BindRight, true, 600);
    play.key(BindRight, false, 700);
    play.key(BindLeft, true, 800);
    play.stepTo(999);
    play.stepShown(999);
    assert(!play.started);
    assert(play.inputs.events.empty());
    assert(play.game.frame == 0);
    assert(play.shown.frame == 0);
    assert(play.game.stats.pieces == 0);

    // A key held when it ends carries over on tick 0.
    play.stepTo(1000);
    assert(play.started);
    assert(play.inputs.events.size() == 1);
    assert(play.inputs.events[0].action == Action::Left);
    assert(play.inputs.events[0].hoisted);
    assert(play.inputs.ticks[0] == 0);

    // A key event can end it before the frame's step does, and comes after the carried keys.
    play.restart({}, 1, 2000);
    play.key(BindHold, true, 2000 + kTickNs);
    assert(play.started);
    assert(play.inputs.events.size() == 2);
    assert(play.inputs.events[0].action == Action::Left);
    assert(play.inputs.events[1].action == Action::Hold);
    assert(!play.inputs.events[1].hoisted);
    assert(play.inputs.ticks[1] == 1);

    // A retry during the countdown starts it again.
    play.restart({}, 1, 3000);
    assert(!play.started);
    play.restart({}, 1, 4000);
    play.stepTo(3000);
    assert(!play.started);
}

// As in the 40L replays, whose options all set no_szo.
void testFirstPiece() {
    Play play;
    for (uint32_t seed = 1; seed <= 500; seed++) {
        play.restart({}, seed, 0);
        core::Piece piece = play.game.falling.piece;
        assert(piece != core::Piece::S && piece != core::Piece::Z && piece != core::Piece::O);
    }
}

void testPlayRun() {
    std::string path = (std::filesystem::temp_directory_path() / "stacklab_best_test.json").string();
    std::filesystem::remove(path);
    Play play;
    play.bestPath = path;
    play.restart({}, 1, 0);

    // Time only advances over completed ticks.
    play.stepTo(kTickNs * 10);
    assert(play.game.frame == 10);

    // Finishing ends the run, freezes the clock and saves the first best.
    play.game.stats.lines = 41;
    play.stepTo(kTickNs * 600);
    assert(play.over);
    uint64_t finalFrame = play.game.frame;
    play.stepTo(kTickNs * 1000);
    assert(play.game.frame == finalFrame);
    assert(play.best == play.seconds());
    assert(loadBest(path) == play.seconds());

    // A slower run doesn't replace the best.
    double best = play.best;
    play.restart({}, 1, 0);
    assert(!play.over);
    play.stepTo(0);
    play.game.frame = static_cast<uint64_t>(best * core::kTicksPerSecond) + 600;
    play.game.stats.lines = 40;
    play.stepTo(kTickNs * (play.game.frame + 1));
    assert(play.over);
    assert(play.best == best);
    assert(loadBest(path) == best);

    // The finish time includes the subframe of the hard drop that finished.
    {
        Play finish;
        finish.bestPath = path;
        finish.restart({}, 1, 0);
        finish.stepTo(kTickNs * 5);
        finish.game.stats.lines = 39;
        // Row 0 is full apart from where the piece will land.
        auto landing = finish.game.ghost().cells();
        for (int x = 0; x < core::kBoardWidth; x++) finish.game.board.at(x, 0) = core::Piece::Garbage;
        for (const core::Cell& cell : landing) finish.game.board.at(cell.x, cell.y) = core::Piece::None;
        uint64_t atTick5 = 5'000'000'000ull / core::kTicksPerSecond + 1;
        finish.key(BindHardDrop, true, atTick5 + 4'166'667);  // a quarter of the way into tick 5
        finish.stepTo(kTickNs * 7);
        assert(finish.over);
        assert(finish.game.frame == 6);
        assert(std::abs(finish.seconds() - 5.25 / 60) < 1e-6);
        assert(finish.best == finish.seconds());
    }

    // A new goal is one row: the run follows it, and keeps its own rules.
    {
        const Mode row = {"Ten", "", {.previews = 3}, 10, ""};
        Play ten;
        ten.mode = &row;
        ten.bestPath = path;
        ten.restart({}, 1, 0);
        assert(ten.game.rules.previews == 3);
        ten.game.stats.lines = 9;
        ten.stepTo(kTickNs);
        assert(!ten.over);
        ten.game.stats.lines = 10;
        ten.stepTo(kTickNs * 2);
        assert(ten.over);
        assert(ten.best == ten.seconds());
    }
    std::filesystem::remove(path);
}

void testBlitzEnd() {
    std::string path = (std::filesystem::temp_directory_path() / "stacklab_best_blitz.json").string();
    std::filesystem::remove(path);
    const Mode row = {.name = "Timed", .blurb = "", .rules = {}, .bestFile = "", .timeLimitSeconds = 2};
    Play play;
    play.mode = &row;
    play.bestPath = path;
    play.restart({}, 1, 0);
    play.stepTo(kTickNs * 119);
    assert(!play.over);
    play.game.stats.score = 500;
    play.stepTo(kTickNs * 120);
    assert(play.over && play.seconds() == 2);
    assert(play.best == 500 && loadBest(path) == 500);

    // The best is the highest score, not the lowest.
    play.restart({}, 1, 0);
    play.game.stats.score = 400;
    play.stepTo(kTickNs * 120);
    assert(play.over && play.best == 500 && loadBest(path) == 500);
    play.restart({}, 1, 0);
    play.game.stats.score = 600;
    play.stepTo(kTickNs * 120);
    assert(play.best == 600 && loadBest(path) == 600);

    // Topping out isn't a finished run.
    play.restart({}, 1, 0);
    play.game.stats.score = 700;
    play.game.toppedOut = true;
    play.stepTo(kTickNs * 200);
    assert(play.over && play.best == 600);
    std::filesystem::remove(path);
}

void testDigGoal() {
    std::string path = (std::filesystem::temp_directory_path() / "stacklab_best_dig.json").string();
    std::filesystem::remove(path);
    const Mode row = {.name = "Dig", .blurb = "", .rules = {}, .goalLines = 3, .bestFile = "", .garbageRows = 3, .messiness = 1};
    Play play;
    play.mode = &row;
    play.bestPath = path;
    play.restart({}, 1, 0);
    assert(play.game.board.at(0, 2) != core::Piece::None || play.game.board.at(1, 2) != core::Piece::None);
    assert(play.game.board.cells == play.shown.board.cells);

    // Plain lines don't count, and garbage lines do.
    play.game.stats.lines = 10;
    play.game.stats.garbageCleared = 2;
    play.stepTo(kTickNs);
    assert(!play.over);
    play.game.stats.garbageCleared = 3;
    play.stepTo(kTickNs * 2);
    assert(play.over && play.best == play.seconds() && loadBest(path) == play.best);
    std::filesystem::remove(path);
}

void testSandbox() {
    const Mode row = {.name = "Sandbox", .blurb = "", .rules = {}, .editable = true};
    Play play;
    play.mode = &row;
    play.restart({}, 1, 0);
    play.stepTo(kTickNs * 1000);
    assert(!play.over);

    // Painting stops at the falling piece, and the next tick sees the rest.
    const core::Cell inside = play.game.falling.cells()[0];
    play.paint(inside.x, inside.y, core::Piece::Garbage);
    assert(play.game.board.at(inside.x, inside.y) == core::Piece::None);
    play.paint(0, 0, core::Piece::Garbage);
    play.stepShown(kTickNs * 1000);
    assert(play.shown.board.at(0, 0) == core::Piece::Garbage);
    play.paint(0, 0, core::Piece::None);
    assert(play.game.board.at(0, 0) == core::Piece::None);

    // The queue cycles through the seven pieces, and hold through no piece as well.
    const core::Piece first = play.game.queue[0];
    for (int i = 0; i < 7; i++) {
        play.cycleQueue(0);
        assert(play.game.queue[0] != core::Piece::None && play.game.queue[0] != core::Piece::Garbage);
    }
    assert(play.game.queue[0] == first);
    assert(play.game.hold == core::Piece::None);
    play.cycleHold();
    assert(play.game.hold == core::Piece::I);
    for (int i = 0; i < 7; i++) play.cycleHold();
    assert(play.game.hold == core::Piece::None);

    // Topping out clears the board and carries on, without allocating.
    for (int x = 3; x < 7; x++) play.game.board.at(x, 21) = core::Piece::Garbage;
    play.key(BindHardDrop, true, kTickNs * 1000 + 1);
    size_t before = allocations;
    play.stepTo(kTickNs * 1002);
    assert(allocations == before);
    assert(play.game.stats.pieces > 0 && !play.over && !play.game.toppedOut);
    assert(play.game.board.at(3, 21) == core::Piece::None);
}

void testUndo() {
    const Mode row = {.name = "Undo", .blurb = "", .rules = {}, .undo = true};
    Play play;
    play.mode = &row;
    const uint64_t start = 1'000'000'000;
    play.restart({}, 1, start);
    uint64_t now = start;
    play.undo(now);  // during the countdown: nothing
    assert(!play.started);
    auto drop = [&](Bind move) {
        now += kTickNs * 30;
        play.stepTo(now);
        play.key(move, true, now);
        play.key(move, false, now + 1);
        play.key(BindHardDrop, true, now + 2);
        play.key(BindHardDrop, false, now + 3);
        now += kTickNs * 2;
        play.stepTo(now);
    };

    core::Piece first = play.game.falling.piece;
    drop(BindLeft);
    const core::Board afterOne = play.game.board;
    core::Piece second = play.game.falling.piece;
    uint64_t frameAfterOne = play.undoSlots[play.undoNewest].game.frame;  // the lock's tick, a little before now
    drop(BindRight);
    assert(play.game.stats.pieces == 2);

    // The state comes back, the piece is at the spawn, and the clock doesn't jump.
    now += kTickNs * 30;
    play.stepTo(now);
    size_t before = allocations;
    play.undo(now);
    assert(play.game.stats.pieces == 1);
    assert(play.game.board.cells == afterOne.cells);
    assert(play.game.falling.piece == second);
    assert(play.game.frame == frameAfterOne);
    play.stepTo(now);
    assert(play.game.frame == frameAfterOne);
    now += kTickNs * 3;
    play.stepTo(now);
    assert(play.game.frame == frameAfterOne + 3);
    assert(allocations == before);

    // Past the first piece it stays there, and the board is as it began.
    play.undo(now);
    play.undo(now);
    assert(play.game.stats.pieces == 0);
    assert(play.game.falling.piece == first);
    assert(play.game.board.cells == core::Board{}.cells);

    // A left held when the slot was saved doesn't keep moving the restored piece.
    play.key(BindLeft, true, now);
    now += kTickNs * 12;  // past DAS, so it has slid to the wall
    play.stepTo(now);
    play.key(BindHardDrop, true, now);
    play.key(BindHardDrop, false, now + 1);
    now += kTickNs;
    play.stepTo(now);
    play.undo(now);
    int spawnX = play.game.falling.x;
    now += kTickNs * 12;
    play.stepTo(now);
    assert(play.game.falling.x == spawnX);
    play.key(BindLeft, false, now);

    // The ring forgets the oldest, and undo stops there. Topping out here ends the run, so the board is emptied.
    play.restart({}, 1, now);
    play.stepTo(now);
    for (int i = 0; i < kUndoDepth + 10; i++) {
        drop(BindLeft);
        play.game.board = {};
    }
    assert(play.undoCount == kUndoDepth);
    for (int i = 0; i < kUndoDepth + 10; i++) play.undo(now);
    assert(play.game.stats.pieces == 11);

    // A mode without undo ignores the key.
    Play plain;
    plain.restart({}, 1, 0);
    plain.stepTo(kTickNs);
    plain.undo(kTickNs);
    assert(plain.game.frame == 1 && plain.undoSlots.empty());
}

Drill drillFrom(const std::string& text) { return parseDrill(text, "test"); }

bool rejected(const std::string& text) {
    try {
        drillFrom(text);
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void testDrillFiles() {
    const std::string queue = R"("queue": {"type": "fixed", "pieces": "I"}, "targets": [{"piece": "I", "x": 0, "y": 1, "rot": 0}])";
    assert(!rejected(R"({"name": "ok", )" + queue + "}"));
    assert(rejected("{ not json"));
    assert(rejected(R"({"name": "no queue", "targets": [{"piece": "I", "x": 0, "y": 1, "rot": 0}]})"));
    assert(rejected(R"({"name": "no targets", "queue": {"type": "fixed", "pieces": "I"}})"));
    assert(rejected(R"({"name": "bag", "queue": {"type": "bag", "pieces": "I"}, "targets": [{"piece": "I", "x": 0, "y": 1, "rot": 0}]})"));
    assert(rejected(R"({"name": "bad piece", "queue": {"type": "fixed", "pieces": "Q"}, "targets": [{"piece": "I", "x": 0, "y": 1, "rot": 0}]})"));
    assert(rejected(R"({"name": "bad rot", "queue": {"type": "fixed", "pieces": "I"}, "targets": [{"piece": "I", "x": 0, "y": 1, "rot": 4}]})"));
    assert(rejected(R"({"name": "off board", "queue": {"type": "fixed", "pieces": "I"}, "targets": [{"piece": "I", "x": 8, "y": 1, "rot": 0}]})"));
    assert(rejected(R"({"name": "wide", "board": ["XXXXXXXXXXX"], )" + queue + "}"));
    assert(rejected(R"({"name": "miss", "onMiss": "shrug", )" + queue + "}"));
    assert(rejected(R"({"name": "rules", "ruleset": "versus-standard", )" + queue + "}"));
    assert(rejected(R"({"name": "bad fumen", "board": "v115@!!", )" + queue + "}"));
    // Twenty rows leave the spawn clear, and rows can't reach it.
    std::string tall = R"({"name": "tall", "board": [)";
    for (int i = 0; i < 20; i++) tall += std::string(i ? "," : "") + R"("XXXXXXXXX.")";
    assert(!rejected(tall + "], " + queue + "}"));
    assert(rejected(tall + R"(,"XXXXXXXXX."], )" + queue + "}"));
    // A fumen can reach it, which would otherwise retry forever.
    assert(rejected(R"({"name": "blocked", "board": "v115@flAgH", )" + queue + "}"));

    // The rows' last entry is the floor, and letters are pieces.
    const Drill rows = drillFrom(R"({"name": "rows", "board": ["T.........", "XX........"], )" + queue + "}");
    assert(rows.board.at(0, 0) == core::Piece::Garbage && rows.board.at(1, 0) == core::Piece::Garbage && rows.board.at(2, 0) == core::Piece::None);
    assert(rows.board.at(0, 1) == core::Piece::T);

    // A fumen's piece is the queue and target when the file gives none. This one is an I on the floor.
    const Drill imported = drillFrom(R"({"name": "fumen", "board": "v115@vhARQJ"})");
    assert(imported.pieces == std::vector{core::Piece::I});
    assert(imported.targets.size() == 1 && imported.targets[0].piece == core::Piece::I);
    assert(imported.targets[0].cells[0].y == 0 && imported.targets[0].cells[0].x == 3);
    // A fumen with no pieces gives only the board, so the file's queue and targets are used.
    const Drill mixed = drillFrom(R"({"name": "mix", "board": "v115@9gI8AeI8AeI8AeI8KeAgH", )" + queue + "}");
    assert(mixed.board.at(0, 0) != core::Piece::None);
    assert(mixed.targets.size() == 1 && mixed.pieces.size() == 1);
    assert(rejected(R"({"name": "no pieces", "board": "v115@9gI8AeI8AeI8AeI8KeAgH"})"));

    // The bundled files, and a bad next is reported without losing the drill.
    const std::filesystem::path bundled[] = {"data/drills"};
    const DrillSet set = loadDrills(bundled);
    assert(set.errors.empty());
    assert(set.drills.size() == 2);
    assert(set.drills[0].id == "first-steps-1" && set.drills[0].next == &set.drills[1]);

    auto dir = std::filesystem::temp_directory_path() / "stacklab_drills_test";
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "broken.json") << "{"; }
    { std::ofstream(dir / "lost.json") << R"({"name": "lost", "next": "nowhere", )" + queue + "}"; }
    { std::ofstream(dir / "notes.txt") << "ignored"; }
    const std::filesystem::path both[] = {dir, dir / "missing"};
    const DrillSet bad = loadDrills(both);
    assert(bad.drills.size() == 1 && bad.drills[0].next == nullptr);
    assert(bad.errors.size() == 2);
    std::filesystem::remove_all(dir);
}

struct DrillPlayer {
    Play play;
    uint64_t now = 1'000'000'000;

    void tap(Bind bind) {
        play.key(bind, true, now);
        play.key(bind, false, now + 1);
        now += kTickNs;
        play.stepTo(now);
    }
    // Runs the countdown out.
    void start() {
        now = play.startNs;
        play.stepTo(now);
    }
    void place(int rot, int x) {
        if (rot) tap(rot == 1 ? BindCW : rot == 2 ? Bind180 : BindCCW);
        while (play.game.falling.x != x) tap(play.game.falling.x > x ? BindLeft : BindRight);
        tap(BindHardDrop);
    }
};

void testDrillRun() {
    const Mode row = {.name = "Drills", .blurb = "", .rules = {}, .undo = true, .drills = true};
    const std::filesystem::path bundled[] = {"data/drills"};
    const DrillSet set = loadDrills(bundled);

    // The bundled chain, by real inputs: the first in another order, then on to the next.
    {
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &set.drills[0];
        p.play.restart({}, 1, p.now);
        assert(p.play.game.falling.piece == core::Piece::I);
        assert(p.play.game.queue[0] == core::Piece::O && p.play.game.queue[1] == core::Piece::J && p.play.game.queue[2] == core::Piece::None);
        p.start();
        p.tap(BindHold);  // I waits, so O goes first
        assert(p.play.game.falling.piece == core::Piece::O);
        p.place(0, 4);
        assert(p.play.filled == 2);
        p.place(0, 6);
        assert(p.play.filled == 6);
        assert(p.play.game.falling.piece == core::Piece::I);  // out of queue, so the held piece comes back
        assert(p.play.game.hold == core::Piece::None);
        p.place(2, 0);  // the same cells as the target's rot 0, from the other rotation
        assert(p.play.drill == &set.drills[1]);
        assert(!p.play.started && !p.play.over && p.play.filled == 0);
        assert(p.play.game.board.at(0, 0) == core::Piece::Garbage);

        // The second clears lines first, so its next target is on the moved board. Then it ends.
        p.start();
        p.place(0, 8);
        assert(p.play.game.stats.lines == 2 && p.play.filled == 1);
        p.place(0, 3);
        assert(p.play.over && p.play.seconds() > 0);
    }

    // A wrong placement resets the attempt at once, and the clock starts over without a countdown.
    {
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &set.drills[0];
        p.play.restart({}, 1, p.now);
        p.start();
        p.place(0, 0);
        assert(p.play.filled == 1 && p.play.game.stats.pieces == 1);
        p.place(0, 0);  // O on the I
        assert(p.play.filled == 0 && p.play.game.stats.pieces == 0 && !p.play.over);
        assert(p.play.drill == &set.drills[0]);
        assert(p.play.game.board.cells == core::Board{}.cells);
        assert(p.play.missedUntilNs > p.now - kTickNs);
        assert(p.play.game.frame == 0);
        p.now += kTickNs;
        p.play.stepTo(p.now);
        assert(p.play.started && p.play.game.frame == 1);
    }

    // Undo gives back a target, and a filled one can't be filled twice.
    {
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &set.drills[0];
        p.play.restart({}, 1, p.now);
        p.start();
        p.place(0, 0);
        p.play.undo(p.now);
        assert(p.play.filled == 0 && p.play.game.falling.piece == core::Piece::I);
        p.play.stepTo(p.now);
        p.place(0, 0);
        assert(p.play.filled == 1);
    }

    // Two locks in one tick are both judged. Two right ones fill two targets, and a
    // wrong second one is a miss, though the first was right.
    for (bool wrongSecond : {false, true}) {
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &set.drills[0];
        p.play.restart({}, 1, p.now);
        p.start();
        for (int i = 0; i < 3; i++) p.tap(BindLeft);
        p.play.key(BindHardDrop, true, p.now);
        p.play.key(BindHardDrop, false, p.now + 1);
        if (wrongSecond) {
            p.play.key(BindRight, true, p.now + 2);
            p.play.key(BindRight, false, p.now + 3);
        }
        p.play.key(BindHardDrop, true, p.now + 4);
        p.play.key(BindHardDrop, false, p.now + 5);
        p.now += kTickNs;
        p.play.stepTo(p.now);
        assert(p.play.game.stats.pieces == (wrongSecond ? 0 : 2));
        assert(p.play.filled == (wrongSecond ? 0u : 3u));
        assert((p.play.missedUntilNs > 0) == wrongSecond);
    }

    // Running out of queue with targets left is a miss, as is topping out.
    {
        const Drill shortQueue = drillFrom(R"({"name": "short", "queue": {"type": "fixed", "pieces": "I"}, "targets": [
            {"piece": "I", "x": 0, "y": 1, "rot": 0}, {"piece": "I", "x": 4, "y": 1, "rot": 0}]})");
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &shortQueue;
        p.play.restart({}, 1, p.now);
        p.start();
        p.place(0, 0);
        assert(p.play.filled == 0 && p.play.missedUntilNs > 0 && p.play.game.falling.piece == core::Piece::I);

        p = {};
        p.play.mode = &row;
        p.play.drill = &set.drills[0];
        p.play.restart({}, 1, p.now);
        p.start();
        p.play.game.toppedOut = true;
        p.now += kTickNs;
        p.play.stepTo(p.now);
        assert(!p.play.over && p.play.missedUntilNs > 0 && !p.play.game.toppedOut);
    }

    // A placed piece and a saved state don't allocate, even with a long queue.
    {
        std::string pieces(40, 'I');
        const Drill longQueue = drillFrom(R"({"name": "long", "queue": {"type": "fixed", "pieces": ")" + pieces + R"("}, "targets": [
            {"piece": "I", "x": 0, "y": 1, "rot": 0}, {"piece": "I", "x": 4, "y": 1, "rot": 0}]})");
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &longQueue;
        p.play.restart({}, 1, p.now);
        p.start();
        size_t before = allocations;
        p.place(0, 0);
        p.play.stepShown(p.now);
        assert(p.play.filled == 1);
        assert(allocations == before);
    }
}

// A mode without garbage leaves the garbage RNG alone, seen here through random rounding.
void testNoGarbageRowsKeepsRng() {
    auto sends = [](core::Game& game) {
        std::vector<int> out;
        for (int i = 0; i < 40; i++) {
            for (int y = 0; y < 4; y++)
                for (int x = 1; x < core::kBoardWidth; x++) game.board.at(x, y) = core::Piece::Garbage;
            game.board.at(1, 4) = core::Piece::Garbage;
            game.falling.piece = core::Piece::I;
            game.falling.rot = 1;
            game.falling.x = -2;
            game.falling.y = 10.5;
            game.stats.b2b = 1;  // a quad then sends 5.65 before rounding
            game.stats.combo = -1;
            const core::InputEvent drop[] = {{Action::HardDrop, true, 0}, {Action::HardDrop, false, 0}};
            out.push_back(game.tick(drop)[0].sends[3]);
        }
        return out;
    };
    const Mode row = {.name = "None", .blurb = "", .rules = {.roundDown = false}, .messiness = 1};
    Play play;
    play.mode = &row;
    play.restart({}, 1, 0);
    core::Game untouched(row.rules, {}, 1);
    assert(sends(play.game) == sends(untouched));
}

// A few minutes of fast play, after a warm-up: nothing may reach the allocator.
void testPlayDoesNotAllocate(const Mode& mode) {
    Play play;
    play.mode = &mode;
    play.restart({}, 1, 0);
    uint64_t now = 0;
    auto playFor = [&](int ticks) {
        for (int i = 0; i < ticks && !play.over; i++) {
            now += kTickNs;
            if (i % 10 == 0) play.key(i % 20 ? BindLeft : BindRight, true, now - kTickNs / 2);
            if (i % 10 == 5) play.key(i % 20 > 10 ? BindLeft : BindRight, false, now - kTickNs / 2);
            if (i % 20 == 12) {
                play.key(BindHardDrop, true, now - kTickNs / 4);
                play.key(BindHardDrop, false, now - kTickNs / 8);
            }
            play.stepTo(now - kTickNs / 2);
            play.stepShown(now - kTickNs / 2);
            play.stepTo(now);
            play.stepShown(now);
        }
    };
    size_t before = allocations;
    for (int seconds = 0; seconds < 180; seconds++) {
        playFor(60);
        // Keep the run alive; this test is only about what the loop allocates.
        play.game.board = {};
        play.game.stats.lines = 0;
    }
    assert(allocations == before);
    assert(!play.over);
    assert(play.game.stats.pieces > 400);
}

}  // namespace

int main() {
    testTickAt();
    testQueue();
    testSettings();
    testPlayKeys();
    testCountdown();
    testFirstPiece();
    testPlayRun();
    testBlitzEnd();
    testDigGoal();
    testSandbox();
    testUndo();
    testNoGarbageRowsKeepsRng();
    testDrillFiles();
    testDrillRun();
    testPlayDoesNotAllocate(kModes[0]);
    const Mode withUndo = {.name = "Undo", .blurb = "", .rules = {}, .undo = true};
    testPlayDoesNotAllocate(withUndo);
    return 0;
}
