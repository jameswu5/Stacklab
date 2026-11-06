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
    assert(loaded.profile().keys[BindRight].size() == 2);

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

    Splits splits{};
    splits[40] = 31.5;
    saveBest(splits, path);
    assert(loadBest(path)[40] == 31.5);
    std::filesystem::remove(path);
    assert(loadBest(path)[40] == 0);
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
    assert(play.inputs.events.size() == 1);
    assert(play.inputs.events[0].action == Action::Left);
    assert(play.inputs.events[0].pressed);
    assert(play.inputs.ticks[0] == 0);

    // Held left and right both carry over charged, most recently pressed last.
    play.held.fill(0);
    play.key(BindRight, true, 5000);
    play.key(BindLeft, true, 5000);
    play.restart({}, 1, 6000);
    assert(play.inputs.events.size() == 2);
    assert(play.inputs.events[0].action == Action::Right);
    assert(play.inputs.events[1].action == Action::Left);
    assert(play.inputs.events[0].hoisted && play.inputs.events[1].hoisted);
    assert(play.inputs.events[1].subframe == 0);
    // A press during play isn't.
    play.key(BindHardDrop, false, 6000);
    play.key(BindHardDrop, true, 7000);
    assert(!play.inputs.events.back().hoisted);

    // A release with nothing held does nothing.
    play.held.fill(0);
    play.restart({}, 1, 6000);
    play.key(BindRight, false, 6000);
    assert(play.inputs.events.empty());
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
    assert(std::string(play.pace) == "-");

    // Each line of a multi-line clear gets a split, and no best means no pace.
    play.game.stats.lines = 4;
    play.stepTo(kTickNs * 11);
    assert(play.splitLines == 4);
    assert(play.splits[1] == play.game.seconds());
    assert(play.splits[4] == play.game.seconds());
    assert(std::string(play.pace) == "-");

    // Finishing ends the run, freezes the clock and saves the first best.
    play.game.stats.lines = 41;
    play.stepTo(kTickNs * 600);
    assert(play.over);
    uint64_t finalFrame = play.game.frame;
    play.stepTo(kTickNs * 1000);
    assert(play.game.frame == finalFrame);
    assert(play.best[40] == play.splits[40]);
    assert(loadBest(path)[40] == play.splits[40]);

    // Pace compares against the best at the same line count.
    double best = play.best[40];
    play.restart({}, 1, 0);
    assert(!play.over);
    play.best[4] = 10;
    play.stepTo(kTickNs * 60);
    play.game.stats.lines = 4;
    play.stepTo(kTickNs * 61);  // about 1.02 s, 8.98 ahead
    assert(std::string(play.pace) == "-8.98s");
    assert(play.paceAhead);
    play.best[5] = 0.5;
    play.game.stats.lines = 5;
    play.stepTo(kTickNs * 62);  // about 1.03 s, 0.53 behind
    assert(std::string(play.pace) == "+0.53s");
    assert(!play.paceAhead);

    // A slower run doesn't replace the best.
    play.best[40] = best;
    play.game.frame = static_cast<uint64_t>(best * core::kTicksPerSecond) + 600;
    play.game.stats.lines = 40;
    play.stepTo(kTickNs * (play.game.frame + 1));
    assert(play.over);
    assert(play.best[40] == best);
    assert(loadBest(path)[40] == best);

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
        assert(finish.best[40] == finish.seconds());
    }
    std::filesystem::remove(path);
}

// A few minutes of fast play, after a warm-up: nothing may reach the allocator.
void testPlayDoesNotAllocate() {
    Play play;
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
        play.splitLines = 0;
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
    testPlayRun();
    testPlayDoesNotAllocate();
    return 0;
}
