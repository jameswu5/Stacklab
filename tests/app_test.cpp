// Self-checks for input timing, saved settings and the 40L run.
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "app/play.h"
#include "app/settings.h"
#include "app/timing.h"
#include "tests/move_driver.h"

// Only the thread playing: the PC coach's worker allocates on its own, as it's allowed to.
static size_t allocations = 0;
static thread_local bool onMainThread = false;

void* operator new(size_t size) {
    if (onMainThread) allocations++;
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
    // The same goes for the hint key, and a saved one stays.
    { std::ofstream(path) << R"({"profiles": [{"keys": {"left": [4]}}]})"; }
    assert(loadSettings(path).profile().keys[BindHint] == std::vector{SDL_SCANCODE_H});
    { std::ofstream(path) << R"({"profiles": [{"keys": {"hold": [11]}}]})"; }  // H
    assert(loadSettings(path).profile().keys[BindHint].empty());
    { std::ofstream(path) << R"({"profiles": [{"keys": {"hint": [5]}}]})"; }
    assert(loadSettings(path).profile().keys[BindHint] == std::vector{SDL_SCANCODE_B});
    assert(loadSettings(path).profile().keys[BindBag].empty());  // and the bag key's default is taken there

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
    // drop never does.
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
        assert(play.inputs.events[i].hoisted);
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
    const Mode row = {.name = "Dig", .blurb = "", .rules = {}, .goalLines = 3, .bestFile = "", .garbageRows = 3};
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

    // A wrong placement pauses on the missed board, clock and keys included, until retry or undo.
    {
        DrillPlayer p;
        p.play.mode = &row;
        p.play.drill = &set.drills[0];
        p.play.restart({}, 1, p.now);
        p.start();
        p.place(0, 0);
        assert(p.play.filled == 1 && p.play.game.stats.pieces == 1);
        p.place(0, 0);  // O on the I
        assert(p.play.missed && p.play.filled == 1 && p.play.game.stats.pieces == 2 && !p.play.over);
        assert(p.play.drill == &set.drills[0]);
        const uint64_t pausedAt = p.play.game.frame;
        const core::Board missedBoard = p.play.game.board;
        p.tap(BindLeft);
        p.tap(BindHardDrop);
        p.now += kTickNs * 60;
        p.play.stepTo(p.now);
        p.play.stepShown(p.now);
        assert(p.play.game.frame == pausedAt && p.play.game.board.cells == missedBoard.cells);
        assert(p.play.shown.board.cells == missedBoard.cells && p.play.missed);

        // Undo takes back only the missed piece, and the clock goes on from there.
        p.play.undo(p.now);
        assert(!p.play.missed && p.play.filled == 1 && p.play.game.stats.pieces == 1);
        assert(p.play.game.falling.piece == core::Piece::O);
        assert(p.play.game.frame <= pausedAt);
        p.place(0, 4);
        assert(!p.play.missed && p.play.filled == 3);

        // Retry starts the attempt over, with a countdown.
        p.place(0, 0);
        assert(p.play.missed);
        p.play.restart({}, 1, p.now + kCountdownNs);
        assert(!p.play.missed && !p.play.started && p.play.filled == 0 && p.play.game.stats.pieces == 0);
        assert(p.play.game.board.cells == core::Board{}.cells);
        p.start();
        assert(p.play.started);
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
        assert(p.play.game.stats.pieces == 2);
        assert(p.play.filled == (wrongSecond ? 1u : 3u));
        assert(p.play.missed == wrongSecond);
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
        assert(p.play.missed && p.play.filled == 1 && p.play.game.falling.piece == core::Piece::None);
        p.play.undo(p.now);  // takes back the piece that ran the queue out
        assert(!p.play.missed && p.play.filled == 0 && p.play.game.falling.piece == core::Piece::I);

        DrillPlayer top;
        top.play.mode = &row;
        top.play.drill = &set.drills[0];
        top.play.restart({}, 1, top.now);
        top.start();
        top.play.game.toppedOut = true;
        top.now += kTickNs;
        top.play.stepTo(top.now);
        assert(!top.play.over && top.play.missed);
        top.play.undo(top.now);
        assert(!top.play.missed && !top.play.game.toppedOut);
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

// How the owner adds a drill: only a name and a fumen. A garbage stack with a well,
// then a flat I, an O and an L standing on its long side, on top of the garbage.
constexpr const char* kFumenDrill = R"({"name": "Imported", "board": "v115@9gI8AeI8AeI8AeI8Kex6IvhBT3IK4I"})";

void testFumenDrill() {
    const Mode row = {.name = "Drills", .blurb = "", .rules = {}, .undo = true, .drills = true};

    auto dir = std::filesystem::temp_directory_path() / "stacklab_fumen_drills";
    std::filesystem::remove_all(dir);
    const std::filesystem::path folders[] = {dir};
    // The folder doesn't exist until the owner makes it, and that's no error.
    assert(loadDrills(folders).drills.empty() && loadDrills(folders).errors.empty());
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "imported.json") << kFumenDrill; }
    const DrillSet set = loadDrills(folders);
    std::filesystem::remove_all(dir);
    assert(set.errors.empty() && set.drills.size() == 1);
    const Drill& drill = set.drills[0];
    assert(drill.id == "imported" && drill.name == "Imported");

    using enum core::Piece;
    assert(drill.pieces == (std::vector{I, O, L}));
    assert(drill.board.at(0, 3) == Garbage && drill.board.at(8, 0) == Garbage && drill.board.at(9, 0) == None && drill.board.at(0, 4) == None);
    assert(drill.targets.size() == 3);
    auto covers = [](const DrillTarget& target, int x, int y) {
        for (const core::Cell& cell : target.cells)
            if (cell.x == x && cell.y == y) return true;
        return false;
    };
    assert(covers(drill.targets[0], 0, 4) && covers(drill.targets[0], 3, 4));
    assert(covers(drill.targets[1], 4, 4) && covers(drill.targets[1], 5, 5));
    assert(covers(drill.targets[2], 6, 6) && covers(drill.targets[2], 7, 4));

    DrillPlayer p;
    p.play.mode = &row;
    p.play.drill = &drill;
    p.play.restart({}, 1, p.now);
    p.start();

    // Three taps where holding left is one press, after a pointless wiggle: 5 presses for 1.
    p.tap(BindLeft);
    p.tap(BindRight);
    p.place(0, 0);
    assert(p.play.filled == 1);
    assert(p.play.finesse.judged && p.play.finesse.judged->presses == 5 && p.play.finesse.judged->minimum == 1);
    assert(p.play.faults.total == 4 && p.play.faults.last == 4);

    // Retrying a miss starts the attempt over, and its faults with it.
    p.place(0, 0);  // O on the I
    assert(p.play.missed && p.play.game.stats.pieces == 2);
    p.play.restart({}, 1, p.now + kCountdownNs);
    assert(p.play.filled == 0 && p.play.game.stats.pieces == 0 && !p.play.missed);
    assert(p.play.faults.total == 0 && p.play.faults.last == 0);
    p.start();

    // Undo on the first piece stays there, and forgets the press.
    p.tap(BindLeft);
    p.play.undo(p.now);
    p.place(0, 0);  // 3 presses for 1 again
    assert(p.play.faults.total == 2 && p.play.faults.last == 2);
    assert(p.play.faults.byPiece[static_cast<int>(I)][0] == 2);

    // A fault that's undone is gone, and the count starts over on the same piece.
    p.tap(BindLeft);
    p.tap(BindRight);
    p.place(0, 4);
    assert(p.play.filled == 3 && p.play.faults.total == 4 && p.play.faults.last == 2);
    p.play.undo(p.now);
    assert(p.play.filled == 1 && p.play.game.falling.piece == O);
    assert(p.play.faults.total == 2 && p.play.faults.last == 2);
    assert(p.play.faults.byPiece[static_cast<int>(O)][0] == 0);
    p.place(0, 4);
    assert(p.play.finesse.judged && p.play.finesse.judged->presses == 0);
    assert(p.play.filled == 3 && p.play.faults.total == 2 && p.play.faults.last == 0);

    // Clean, and that's the drill done.
    p.place(1, 5);
    assert(p.play.finesse.judged && p.play.finesse.judged->presses == 3 && p.play.finesse.judged->faults == 0);
    assert(p.play.over && p.play.filled == 7);
    assert(p.play.faults.total == 2);

    // The held piece comes back after the queue runs out, and is judged from its new spawn.
    DrillPlayer held;
    held.play.mode = &row;
    held.play.drill = &drill;
    held.play.restart({}, 1, held.now);
    held.start();
    held.tap(BindLeft);  // forgotten: hold restarts the count
    held.tap(BindHold);
    held.place(0, 4);
    assert(held.play.faults.total == 0);
    held.place(1, 5);
    assert(held.play.game.falling.piece == I && held.play.game.hold == None);
    held.place(0, 0);
    assert(held.play.over);
    assert(held.play.finesse.judged && held.play.finesse.judged->piece == I && held.play.finesse.judged->presses == 3);
    assert(held.play.faults.total == 2);

    // A fumen drill's placements, undo included, don't allocate once it's going.
    DrillPlayer quiet;
    quiet.play.mode = &row;
    quiet.play.drill = &drill;
    quiet.play.restart({}, 1, quiet.now);
    quiet.start();
    size_t before = allocations;
    quiet.place(0, 0);
    quiet.play.stepShown(quiet.now);
    quiet.play.undo(quiet.now);
    quiet.place(0, 0);
    quiet.place(0, 4);
    quiet.play.stepShown(quiet.now);
    assert(allocations == before);
}

void testFinesseInPlay() {
    const Mode plain = {.name = "Plain", .blurb = "", .rules = {}};
    DrillPlayer p;
    p.play.mode = &plain;
    p.play.restart({}, 1, p.now);
    p.start();
    p.tap(BindLeft);
    p.tap(BindRight);
    p.tap(BindHardDrop);
    assert(p.play.faults.total == 2 && p.play.faults.last == 2);
    // Never on `shown`, the copy that's stepped ahead: drawing it judges nothing.
    p.play.stepShown(p.now);
    assert(p.play.faults.total == 2);
    p.tap(BindHardDrop);
    assert(p.play.faults.total == 2 && p.play.faults.last == 0);

    // Retry starts the count and the totals over.
    p.tap(BindLeft);
    p.play.restart({}, 1, p.now);
    assert(p.play.faults.total == 0);
    p.start();
    p.tap(BindHardDrop);
    assert(p.play.finesse.judged && p.play.finesse.judged->presses == 0);

    // Keys held through the countdown aren't presses, soft drop included.
    p.play.restart({}, 1, p.now + 1000);
    p.play.key(BindLeft, true, p.now);
    p.play.key(BindSoftDrop, true, p.now);
    p.start();
    p.play.key(BindLeft, false, p.now + 1);
    p.play.key(BindSoftDrop, false, p.now + 1);
    p.tap(BindHardDrop);
    assert(p.play.finesse.judged && p.play.finesse.judged->presses == 0 && p.play.faults.total == 0);

    // A hidden mode judges nothing.
    const Mode levels = {.name = "Levels", .blurb = "", .rules = {.levels = true}};
    DrillPlayer hidden;
    hidden.play.mode = &levels;
    hidden.play.restart({}, 1, hidden.now);
    hidden.start();
    hidden.tap(BindLeft);
    hidden.tap(BindRight);
    hidden.tap(BindHardDrop);
    assert(hidden.play.game.stats.pieces == 1);
    assert(!hidden.play.finesse.judged && hidden.play.faults.total == 0 && hidden.play.faults.last == 0);
    for (const Mode& mode : kModes)
        if (mode.rules.levels) assert(!(mode.stats & StatFinesse));

    // A Sandbox edit starts the piece's count over against the new board, which is
    // judged as it now stands. Editing the queue or hold doesn't touch this piece.
    const Mode sandbox = {.name = "Sandbox", .blurb = "", .rules = {}, .editable = true};
    DrillPlayer edit;
    edit.play.mode = &sandbox;
    edit.play.restart({}, 1, edit.now);
    edit.start();
    edit.tap(BindLeft);
    edit.play.paint(0, 0, core::Piece::Garbage);
    edit.play.cycleQueue(0);
    edit.play.cycleHold();
    edit.tap(BindHardDrop);
    assert(edit.play.finesse.judged && edit.play.finesse.judged->presses == 0);
    assert(edit.play.game.board.at(0, 0) == core::Piece::Garbage);

    // Topping out in Sandbox clears the board, and the next piece is judged on it.
    bool cleared = false;
    for (int i = 0; i < 60 && !cleared; i++) {
        edit.tap(BindHardDrop);
        cleared = edit.play.game.board.cells == core::Board{}.cells;
    }
    assert(cleared);
    edit.tap(BindLeft);
    edit.tap(BindHardDrop);
    assert(edit.play.finesse.judged && edit.play.finesse.judged->presses == 1 && edit.play.finesse.judged->faults == 0);
}

const Mode& kCoachMode = kModes[6];

// Plays a solver's steps through Play as real key presses, as tests/move_driver.h does in core.
struct CoachPlayer {
    Play play;
    uint64_t now = 1'000'000'000;

    PcCoach& coach() { return *play.coach; }
    PcView view() { return coach().view(play.game, kCoachMode.goalLines, play.over); }

    void advance(int ticks = 1) {
        now += kTickNs * ticks;
        play.stepTo(now);
    }
    void press(Bind bind, bool down) { play.key(bind, down, now); }
    void tap(Bind bind) {
        press(bind, true);
        press(bind, false);
        advance();
    }
    void waitForAnswer() {
        for (int i = 0; i < 60000 && !coach().answered(); i++) {
            coach().poll();
            if (!coach().answered()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(coach().answered());
    }
    void move(ai::Move move) {
        const Bind das = move == ai::Move::DasLeft ? BindLeft : BindRight;
        switch (move) {
            case ai::Move::Left: tap(BindLeft); break;
            case ai::Move::Right: tap(BindRight); break;
            case ai::Move::RotateCW: tap(BindCW); break;
            case ai::Move::RotateCCW: tap(BindCCW); break;
            case ai::Move::Rotate180: tap(Bind180); break;
            case ai::Move::DasLeft:
            case ai::Move::DasRight:
                press(das, true);
                advance(static_cast<int>(play.game.handling.das) + 2);
                press(das, false);
                advance();
                break;
            case ai::Move::SoftDrop:
                press(BindSoftDrop, true);
                advance(3);
                press(BindSoftDrop, false);
                advance();
                break;
            case ai::Move::Down:
                press(BindSoftDrop, true);
                advance();
                press(BindSoftDrop, false);
                advance();
                break;
        }
    }
    void place(const ai::PcStep& step) {
        if (step.hold) tap(BindHold);
        assert(play.game.falling.piece == step.piece);
        for (ai::Move each : step.moves) move(each);
        tap(BindHardDrop);
    }
};

// The first pieces a seed deals in this mode.
std::vector<core::Piece> dealt(uint32_t seed, int count) {
    core::Game game(kCoachMode.rules, {}, seed);
    std::vector<core::Piece> pieces;
    const core::InputEvent drop[] = {{Action::HardDrop, true, 0}, {Action::HardDrop, false, 0}};
    while (static_cast<int>(pieces.size()) < count) {
        pieces.push_back(game.falling.piece);
        game.tick(drop);
        game.board = {};
    }
    return pieces;
}

// A plan for the next perfect clear from an empty board, seeing further ahead than a player can.
ai::PcResult planPc(const std::vector<core::Piece>& pieces, size_t next, core::Piece hold) {
    ai::PcQuery query;
    query.current = pieces[next];
    query.queue.assign(pieces.begin() + next + 1, pieces.begin() + next + 11);
    query.hold = hold;
    query.queueEnds = true;
    query.rules = kCoachMode.rules;
    ai::PcSolver solver;
    return solver.solve(query, nullptr, 2'000'000);
}

void testPcCoachRules() {
    // 40L's goal, with undo. The rules are the same constant in both rows.
    assert(kCoachMode.goalLines == kModes[0].goalLines && kCoachMode.undo);

    // Each PC takes at least 2 lines, up to the exact limit.
    assert(!pcGoalOutOfReach(0, 40));
    assert(!pcGoalOutOfReach(0, 20));
    assert(pcGoalOutOfReach(0, 19));
    assert(!pcGoalOutOfReach(3, 14));
    assert(pcGoalOutOfReach(3, 13));
    assert(!pcGoalOutOfReach(10, 0));
    assert(!pcGoalOutOfReach(9, 2));  // a 2-line PC from 38 lines is still the badge
    assert(pcGoalOutOfReach(9, 1));
    for (const Mode& mode : kModes) assert(!(mode.undo && mode.bestFile));  // undo nullifies a run, so a best can't follow one

    // A tally's phases, and a PC that took too many lines isn't a success for the streak.
    PcProgress progress;
    const auto lock = [](int lines, bool pc) {
        core::LockResult result;
        result.lines = lines;
        result.perfectClear = pc;
        return result;
    };
    progress.note(lock(0, false), 1);
    progress.note(lock(0, false), 2);
    assert(progress.tally.attempts[0] == 1);
    progress.note(lock(4, true), 10);
    assert(progress.pcs == 1 && progress.streak == 1 && progress.tally.successes[0] == 1);
    assert(progress.phase == 3 && progress.linesInAttempt == 0 && !progress.attempted);
    assert(progress.tally.attempts[3] == 0);  // no piece placed in the next attempt yet
    progress.note(lock(2, false), 11);
    progress.note(lock(2, false), 12);
    assert(progress.streak == 1);
    progress.note(lock(1, false), 13);
    assert(progress.streak == 0);
    progress.note(lock(1, true), 14);
    assert(progress.pcs == 2 && progress.streak == 1);
    assert(progress.tally.attempts[3] == 1 && progress.tally.successes[3] == 0);

    // The record: a damaged or missing file reads as empty.
    std::string path = (std::filesystem::temp_directory_path() / "stacklab_pc_rules.json").string();
    std::filesystem::remove(path);
    assert(loadPcRecord(path).best == 0);
    PcRecord record;
    record.best = 4;
    record.tally.attempts[6] = 9;
    record.tally.successes[6] = 2;
    savePcRecord(record, path);
    PcRecord read = loadPcRecord(path);
    assert(read.best == 4 && read.tally.attempts[6] == 9 && read.tally.successes[6] == 2);
    { std::ofstream(path) << R"({"best": 3, "attempts": [1, 2], "successes": "x"})"; }
    read = loadPcRecord(path);
    assert(read.best == 3 && read.tally.attempts[0] == 0);
    std::filesystem::remove(path);
}

void testPcCoachRun() {
    // A seed with a perfect clear in its first pieces and another straight after.
    uint32_t seed = 0;
    std::vector<core::Piece> pieces;
    ai::PcResult first, second;
    core::Piece carried = core::Piece::None;
    size_t nextAfterFirst = 0;
    for (uint32_t candidate = 1; candidate < 400 && !seed; candidate++) {
        pieces = dealt(candidate, 40);
        first = planPc(pieces, 0, core::Piece::None);
        if (first.status != ai::PcStatus::Found || first.lines != 4) continue;
        // Follow the plan's holds to see what's left in hold, and which piece comes next.
        carried = core::Piece::None;
        size_t next = 0;
        core::Piece current = pieces[0];
        for (const ai::PcStep& step : first.steps) {
            if (step.hold) {
                const core::Piece held = carried;
                carried = current;
                if (held == core::Piece::None) current = pieces[++next];
                else current = held;
            }
            current = pieces[++next];
        }
        nextAfterFirst = next;
        second = planPc(pieces, next, carried);
        if (second.status == ai::PcStatus::Found && second.lines == 4) seed = candidate;
    }
    assert(seed != 0);

    std::string path = (std::filesystem::temp_directory_path() / "stacklab_pc_run.json").string();
    std::filesystem::remove(path);
    CoachPlayer p;
    p.play.mode = &kCoachMode;
    p.play.restart({}, seed, p.now);
    p.coach().path = path;
    p.coach().hintKey = "H";

    // Nothing is known until the worker has answered about this piece.
    assert(std::string(p.view().status) == "Thinking");
    assert(!p.view().showHint);
    p.now = p.play.startNs;
    p.play.stepTo(p.now);
    p.waitForAnswer();
    assert(std::string(p.view().status) == "Possible");
    assert(std::string(p.view().detail) == "need more pieces");

    // Only what's on show: the previews, not Game::queue, and every piece so far once.
    ai::PcQuery query;
    p.coach().fillQuery(p.play.game, query);
    assert(static_cast<int>(query.queue.size()) == kCoachMode.rules.previews);
    assert(static_cast<int>(p.coach().seen.size()) == 1 + kCoachMode.rules.previews);
    assert(p.play.game.queue.size() > query.queue.size());
    assert(query.bagLeft == ai::pcBagLeft(std::span(pieces).first(6)));

    // A stale answer is dropped, whatever it says, whether or not this piece has one.
    const uint64_t firstAsk = p.coach().askId();
    PcAnswer stale;
    stale.id = firstAsk - 1;
    stale.status = ai::PcStatus::Impossible;
    p.coach().take(stale);
    assert(std::string(p.view().status) == "Possible");

    bool sawReady = false;
    for (size_t i = 0; i < first.steps.size(); i++) {
        p.waitForAnswer();
        if (std::string(p.view().status) == "PC ready") {
            sawReady = true;
            // The hint is on demand, shows this piece's placement, and says if hold comes first.
            assert(!p.view().showHint);
            p.coach().toggleHint();
            assert(p.view().showHint);
            assert(std::string(p.view().hintLine).find("place the") != std::string::npos || std::string(p.view().hintLine).find("Place the") != std::string::npos);
            p.coach().toggleHint();
            p.coach().toggleHint();
        } else {
            assert(std::string(p.view().status) == "Possible");  // never a no, on the way to a PC
        }
        const PcView before = p.view();
        if (i + 1 == first.steps.size()) {
            assert(before.pcs == 0 && p.coach().progress.tally.attempts[0] == 1);
        }
        p.place(first.steps[i]);
        // Gone once the piece locks, and the question moves on with no answer yet.
        assert(!p.view().showHint);
        assert(std::string(p.view().status) == "Thinking");
        stale.id = p.coach().askId() - 1;
        p.coach().take(stale);
        assert(std::string(p.view().status) == "Thinking");
    }
    assert(sawReady);

    // The perfect clear.
    assert(p.play.game.stats.lines == 4);
    assert(p.coach().progress.pcs == 1 && p.coach().progress.streak == 1);
    assert(p.coach().progress.tally.attempts[0] == 1 && p.coach().progress.tally.successes[0] == 1);
    const int dealtBefore = p.play.game.stats.pieces + (p.play.game.hold != core::Piece::None);
    assert(p.coach().progress.phase == p.play.game.stats.pieces % 7);  // placed, whatever's in hold
    assert(static_cast<size_t>(dealtBefore) == nextAfterFirst);

    // Undo takes the PC back, stats and all, and placing it again counts it again.
    const size_t seenAfter = p.coach().seen.size();
    p.play.undo(p.now);
    assert(p.coach().progress.pcs == 0 && p.coach().progress.streak == 0);
    assert(p.coach().progress.tally.successes[0] == 0 && p.coach().progress.tally.attempts[0] == 1);
    assert(p.play.game.stats.lines < 4);  // an earlier piece of the plan may clear lines of its own
    assert(p.coach().seen.size() == seenAfter - 1);
    p.play.stepTo(p.now);
    p.waitForAnswer();
    p.place(first.steps.back());
    assert(p.coach().progress.pcs == 1 && p.coach().progress.streak == 1);
    assert(p.coach().progress.tally.successes[0] == 1);
    assert(p.coach().seen.size() == seenAfter);

    // A second in a row is a streak, tallied under its own phase.
    const int phase = p.coach().progress.phase;
    for (const ai::PcStep& step : second.steps) p.place(step);
    assert(p.coach().progress.pcs == 2 && p.coach().progress.streak == 2);
    assert(p.coach().progress.tally.attempts[phase] == (phase == 0 ? 2 : 1));
    assert(p.play.game.stats.lines == 8);

    // Stacking straight up soon rules out a PC in 4 lines, and that placement gets the blame.
    for (int placed = 0;; placed++) {
        p.waitForAnswer();
        if (std::string(p.view().status) == "No PC") {
            assert(placed > 0);
            break;
        }
        assert(p.view().alert == nullptr && placed < 8);
        p.tap(BindHardDrop);
    }
    assert(p.view().alert != nullptr && std::string(p.view().alert) == "That piece killed it: undo");
    assert(std::string(p.view().detail).find("lines with these pieces") != std::string::npos);
    assert(!p.view().showHint);

    // It stays a no on the next piece, without blaming that one too.
    p.tap(BindHardDrop);
    p.waitForAnswer();
    if (std::string(p.view().status) == "No PC") assert(p.view().alert == nullptr);

    // Undo goes back past it, and the callout goes with it.
    p.play.undo(p.now);
    p.play.stepTo(p.now);
    p.waitForAnswer();
    p.play.undo(p.now);
    p.play.stepTo(p.now);
    p.waitForAnswer();
    assert(std::string(p.view().status) != "No PC");
    assert(p.view().alert == nullptr);
    assert(p.coach().progress.pcs == 2);

    assert(p.view().nullified);

    // Retry starts clean on a new queue, and a run that used undo saves nothing.
    p.play.restart({}, seed + 1, p.now);
    assert(p.coach().progress.pcs == 0 && p.coach().seen.size() == 1 + static_cast<size_t>(kCoachMode.rules.previews));
    assert(!p.view().nullified);
    assert(!std::filesystem::exists(path));

    // The same PC with no undo is saved on retry.
    p.play.restart({}, seed, p.now);
    p.now = p.play.startNs;
    p.play.stepTo(p.now);
    for (const ai::PcStep& step : first.steps) p.place(step);
    assert(p.coach().progress.pcs == 1);
    p.play.restart({}, seed + 1, p.now);
    PcRecord saved = loadPcRecord(path);
    assert(saved.best == 1);
    assert(saved.tally.successes[0] == 1);

    // A run is only added to the record once.
    p.coach().save();
    p.coach().save();
    assert(loadPcRecord(path).tally.successes[0] == saved.tally.successes[0]);

    // Unknown is never shown as a no.
    PcAnswer unknown;
    unknown.id = p.coach().askId();
    unknown.status = ai::PcStatus::Unknown;
    p.coach().take(unknown);
    assert(std::string(p.view().status) == "Can't tell");
    assert(p.view().alert == nullptr);

    // A hold can kill it too, and undo blames nothing.
    p.now = p.play.startNs;
    p.play.stepTo(p.now);
    p.waitForAnswer();
    p.tap(BindHold);
    PcAnswer dead;
    dead.id = p.coach().askId();
    dead.status = ai::PcStatus::Impossible;
    p.coach().take(dead);
    assert(p.view().alert != nullptr && std::string(p.view().alert) == "That hold killed it: undo");
    p.tap(BindHardDrop);
    p.waitForAnswer();
    p.play.undo(p.now);
    assert(p.view().alert == nullptr);
    dead.id = p.coach().askId();
    p.coach().take(dead);
    assert(p.view().alert == nullptr);
    std::filesystem::remove(path);
}

// Practice starts mid-bag, with everything the solver is told agreeing with a real run's.
void testPcPractice() {
    const Mode& practice = kModes[7];
    assert(practice.pcStyle == PcStyle::Practice && practice.undo && practice.goalLines == 0);
    CoachPlayer p;
    p.play.mode = &practice;
    for (int residue = 0; residue < 7; residue++) {
        for (uint32_t seed = 1; seed <= 20; seed++) {
            p.play.residue = residue;
            p.play.restart({}, seed, p.now);
            const core::Game& game = p.play.game;
            assert(p.coach().progress.phase == residue);
            assert((game.hold == core::Piece::None) == (residue == 0));
            assert(static_cast<int>(game.queue.size()) >= game.rules.previews);
            const std::vector<core::Piece>& seen = p.coach().seen;
            // Bags stay whole: placed, held, falling and previews, from a new bag.
            const size_t dealt = residue == 0 ? 0 : residue + 1;
            assert(seen.size() == dealt + 1 + game.rules.previews);
            assert(seen[dealt] == game.falling.piece);
            if (residue > 0) assert(std::find(seen.begin(), seen.begin() + dealt, game.hold) != seen.begin() + dealt);
            for (size_t bag = 0; bag + 7 <= seen.size(); bag += 7) {
                uint8_t pieces = 0;
                for (size_t i = bag; i < bag + 7; i++) pieces |= 1 << static_cast<int>(seen[i]);
                assert(pieces == 0xFE);  // a bit per piece
            }
            // The real next piece is always one the solver was told could come.
            ai::PcQuery query;
            p.coach().fillQuery(game, query);
            const uint8_t left = query.bagLeft;
            if (left != 0) assert(left >> static_cast<int>(game.queue[game.rules.previews]) & 1);
        }
    }

    // A real PC, from a carried hold at residue 3, ends the run through Play.
    p.play.residue = 3;
    bool played = false;
    for (uint32_t seed = 1; seed < 100 && !played; seed++) {
        p.play.restart({}, seed, p.now);
        core::Game ahead = p.play.game;
        std::vector<core::Piece> pieces = {ahead.falling.piece};
        for (int i = 0; i < 11; i++) pieces.push_back(ahead.deal());
        const ai::PcResult plan = planPc(pieces, 0, ahead.hold);
        if (plan.status != ai::PcStatus::Found || plan.lines != 4) continue;
        played = true;
        p.now = p.play.startNs;
        p.play.stepTo(p.now);
        for (const ai::PcStep& step : plan.steps) {
            assert(!p.play.over);
            p.place(step);
        }
        assert(p.play.over && p.coach().progress.pcs == 1 && p.coach().progress.tally.successes[3] == 1);
        assert(std::string(p.view().residue) == "3");
    }
    assert(played);

    // A PC ends it, tallied under its residue.
    const std::string path = (std::filesystem::temp_directory_path() / "stacklab_pc_practice.json").string();
    std::filesystem::remove(path);
    p.play.restart({}, 1, p.now);
    p.coach().path = path;
    p.coach().record = {};
    p.now = p.play.startNs;
    p.play.stepTo(p.now);
    core::LockResult clear;
    clear.lines = 4;
    clear.perfectClear = true;
    p.coach().progress.note(clear, 10);
    assert(p.coach().runDone());
    assert(std::string(p.view().outcome) == "Perfect clear" && p.view().goal == 1 && !p.view().outOfReach);
    assert(std::string(p.view().residue) == "3");
    p.coach().save();
    assert(loadPcRecord(path).tally.successes[3] == 1);
    std::filesystem::remove(path);

    // Spending over 4 lines ends it.
    p.coach().progress = {};
    clear.perfectClear = false;
    clear.lines = 3;
    p.coach().progress.note(clear, 1);
    assert(!p.coach().runDone());
    p.coach().progress.note(clear, 2);
    assert(p.coach().runDone());
    assert(std::string(p.view().outcome) == "Over 4 lines");
    p.coach().progress = {};
}

// Bag count has no solver and hides the residue until asked, and the badge ends a run early.
void testBagCount() {
    const Mode& count = kModes[8];
    assert(count.pcStyle == PcStyle::Count && !count.undo && count.goalLines == 40);
    CoachPlayer p;
    p.play.mode = &count;
    p.play.restart({}, 1, p.now);
    p.coach().bagKey = "B";
    const uint64_t asked = p.coach().askId();
    p.now = p.play.startNs;
    p.play.stepTo(p.now);
    p.tap(BindHardDrop);
    assert(p.coach().askId() == asked + 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    p.coach().poll();
    assert(!p.coach().answered());  // the worker wasn't asked
    PcView view = p.view();
    assert(std::string(view.status) == "Keep count" && !view.showHint && view.hintLine[0] == 0);
    assert(std::string(view.residue) == "?");
    p.coach().progress.residueShown = true;
    assert(std::string(p.view().residue) == "0");
    core::LockResult clear;
    clear.lines = 4;
    clear.perfectClear = true;
    p.coach().progress.note(clear, 10);
    assert(!p.coach().progress.residueShown && std::string(p.view().residue) == "?");
    p.coach().progress.residueShown = true;
    assert(std::string(p.view().residue) == "3");
    // 2-line PCs move it by 5.
    clear.lines = 2;
    p.coach().progress.note(clear, 15);
    p.coach().progress.residueShown = true;
    assert(std::string(p.view().residue) == "1");

    // The tenth PC ends the run through Play, before the 40 lines are up.
    bool played = false;
    for (uint32_t seed = 1; seed < 100 && !played; seed++) {
        const ai::PcResult plan = planPc(dealt(seed, 12), 0, core::Piece::None);
        if (plan.status != ai::PcStatus::Found || plan.lines != 4) continue;
        played = true;
        p.play.restart({}, seed, p.now);
        p.coach().progress.pcs = 9;
        p.now = p.play.startNs;
        p.play.stepTo(p.now);
        for (const ai::PcStep& step : plan.steps) {
            assert(!p.play.over);
            p.place(step);
        }
        assert(p.play.over && p.play.game.stats.lines == 4 && !p.play.game.toppedOut);
    }
    assert(played);

    // Nine at 4 lines and one at 2 is 38 lines: the badge, with lines to spare.
    p.coach().progress.pcs = 9;
    assert(!p.coach().runDone());
    p.coach().progress.note(clear, 95);
    assert(p.coach().progress.pcs == 10 && p.coach().runDone());
    assert(std::string(p.view().outcome) == "10 perfect clears");
    p.coach().progress = {};
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
    const Mode row = {.name = "None", .blurb = "", .rules = {.roundDown = false}};
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
    onMainThread = true;
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
    testFumenDrill();
    testFinesseInPlay();
    testPlayDoesNotAllocate(kModes[0]);
    const Mode withUndo = {.name = "Undo", .blurb = "", .rules = {}, .undo = true};
    testPlayDoesNotAllocate(withUndo);
    const Mode withEdits = {.name = "Sandbox", .blurb = "", .rules = {}, .editable = true};
    testPlayDoesNotAllocate(withEdits);
    testPcCoachRules();
    testPcCoachRun();
    testPcPractice();
    testBagCount();
    testPlayDoesNotAllocate(kModes[6]);
    return 0;
}
