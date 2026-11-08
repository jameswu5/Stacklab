// Frame time in the PC coach with the solver's worker busy: the app's own Play, coach and
// drawing, in real time, with a scripted player pressing keys. A hidden window, so it never
// takes focus, and no pref path, so it saves nothing.
// Usage: frame_bench <base-dir> <follow|stack|random|control> <runs>
//   follow: takes the hint whenever a PC is ready, else stacks; stack: never takes it;
//   random: random placements, for awkward stacks with holes; control: stack in Bag count, which never asks.
#include <SDL3/SDL_main.h>
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "ai/move_search.h"
#include "app/play.h"
#include "app/ui.h"

namespace {

constexpr uint64_t kMs = 1'000'000;
constexpr uint64_t kGapNs = 17 * kMs;  // a tick apart, so presses land on separate ticks

struct Press {
    uint64_t at;
    Bind bind;
    bool down;
};

std::array<int, 4> key(std::array<core::Cell, 4> cells) {
    std::array<int, 4> out;
    for (int i = 0; i < 4; i++) out[i] = cells[i].y * core::kBoardWidth + cells[i].x;
    std::sort(out.begin(), out.end());
    return out;
}

// Lower, flatter and fewer holes is better; cleared lines count for it.
int score(core::Board board, std::array<core::Cell, 4> cells) {
    for (const core::Cell& cell : cells) board.at(cell.x, cell.y) = core::Piece::I;
    int lines = 0, holes = 0, heights = 0, bumps = 0, last = -1;
    for (int y = 0; y < core::kBoardHeight; y++) {
        bool full = true;
        for (int x = 0; x < core::kBoardWidth; x++) full = full && board.at(x, y) != core::Piece::None;
        lines += full;
    }
    for (int x = 0; x < core::kBoardWidth; x++) {
        int height = 0;
        for (int y = core::kBoardHeight - 1; y >= 0 && !height; y--)
            if (board.at(x, y) != core::Piece::None) height = y + 1;
        for (int y = 0; y < height; y++) holes += board.at(x, y) == core::Piece::None;
        heights += height;
        if (last >= 0) bumps += std::abs(height - last);
        last = height;
    }
    return 8 * holes + heights + bumps - 6 * lines;
}

void pressMoves(std::vector<Press>& out, uint64_t& t, bool hold, const std::vector<ai::Move>& moves, int dasMs) {
    auto tap = [&](Bind bind, uint64_t heldNs) {
        out.push_back({t, bind, true});
        out.push_back({t + heldNs, bind, false});
        t += heldNs + kGapNs;
    };
    if (hold) tap(BindHold, kMs);
    for (ai::Move move : moves) {
        switch (move) {
            case ai::Move::Left: tap(BindLeft, kMs); break;
            case ai::Move::Right: tap(BindRight, kMs); break;
            case ai::Move::DasLeft: tap(BindLeft, (dasMs + 40) * kMs); break;
            case ai::Move::DasRight: tap(BindRight, (dasMs + 40) * kMs); break;
            case ai::Move::RotateCW: tap(BindCW, kMs); break;
            case ai::Move::RotateCCW: tap(BindCCW, kMs); break;
            case ai::Move::Rotate180: tap(Bind180, kMs); break;
            case ai::Move::SoftDrop:
            case ai::Move::Down: tap(BindSoftDrop, 2 * kGapNs); break;  // ponytail: instant soft drop has no one-row press
        }
    }
    tap(BindHardDrop, kMs);
}

struct Stats {
    std::vector<double> frames, busyFrames, solves;
    struct Worst {
        double ms;
        bool busy;
        double solveMsSoFar;
        int pieces;
    };
    std::vector<Worst> worst;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) return 1;
    const std::string style = argv[2];
    const int runs = std::atoi(argv[3]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) return 1;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("frame_bench", 400, 300, SDL_WINDOW_HIDDEN, &window, &renderer)) return 1;
    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 2880, 1800);
    SDL_SetRenderTarget(renderer, target);
    Ui ui(renderer, std::string(argv[1]) + "/assets/fonts/");

    const Mode* coachMode = nullptr;
    for (const Mode& mode : kModes)
        if (mode.pcFile && mode.pcStyle == (style == "control" ? PcStyle::Count : PcStyle::Coach)) coachMode = &mode;
    Profile profile;
    Play play;
    play.mode = coachMode;
    ai::MoveSearch search;
    std::mt19937 rng(7);
    Stats stats;
    stats.frames.reserve(1 << 20);
    stats.busyFrames.reserve(1 << 20);
    int finished = 0, pcs = 0, toppedOut = 0, lines = 0;

    for (int run = 0; run < runs; run++) {
        uint64_t start = SDL_GetTicksNS();
        play.restart(profile.handling, 1000 + run, start + kCountdownNs);
        std::vector<Press> presses;
        size_t next = 0;
        int plannedFor = -1;
        uint64_t askedAt = start, lastAsk = play.coach->askId(), frameStart = SDL_GetTicksNS();
        bool waiting = true;
        while (!play.over) {
            const uint64_t now = SDL_GetTicksNS();
            uint64_t planStart = 0, planNs = 0;  // the scripted player's work, which isn't the app's
            for (; next < presses.size() && presses[next].at <= now; next++) play.key(presses[next].bind, presses[next].down, presses[next].at);
            play.stepTo(now);
            play.stepShown(now);
            if (play.over) break;  // the results need this session's history, which isn't kept here

            PcCoach& coach = *play.coach;
            if (coach.askId() != lastAsk) {
                lastAsk = coach.askId();
                askedAt = now;
                waiting = true;
            }
            if (waiting && coach.answered()) {
                stats.solves.push_back((now - askedAt) / 1e6);
                waiting = false;
            }
            PcView view = coach.view(play.shown, play.mode->goalLines, play.over);
            const bool readyHint = view.status == std::string("PC ready");
            // Plan once the last presses are done, and in follow, once the coach has answered.
            if (!play.over && play.started && next == presses.size() && plannedFor != play.game.stats.pieces && (style != "follow" || coach.answered())) {
                if (style == "follow" && readyHint && !view.showHint) {
                    coach.toggleHint();
                    view = coach.view(play.shown, play.mode->goalLines, play.over);
                }
                planStart = SDL_GetTicksNS();
                const core::Game& game = play.game;
                const core::Piece other = game.hold != core::Piece::None ? game.hold : game.queue[0];
                bool bestHold = false;
                std::vector<ai::Move> bestMoves;
                int bestScore = 1 << 30;
                for (bool hold : {false, true}) {
                    if (hold && game.holdUsed) continue;
                    const core::Piece piece = hold ? other : game.falling.piece;
                    search.run(game.board, core::spawnFalling(piece), game.rules);
                    if (search.placements.empty()) continue;
                    if (style == "random") {
                        if (hold) continue;
                        bestMoves = search.inputs(search.placements[rng() % search.placements.size()]);
                        bestScore = 0;
                        break;
                    }
                    for (const ai::Placement& placement : search.placements) {
                        const auto cells = search.cells(placement);
                        int s = score(game.board, cells);
                        if (view.showHint) s = view.hintPiece == piece && hold == (view.hintLine[0] == 'H') && key(cells) == key(view.hintCells) ? -(1 << 20) : 1 << 20;
                        if (s < bestScore) {
                            bestScore = s;
                            bestHold = hold;
                            bestMoves = search.inputs(placement);
                        }
                    }
                }
                uint64_t t = now;
                presses.clear();
                next = 0;
                pressMoves(presses, t, bestHold, bestMoves, profile.handling.das * 1000 / 60);
                plannedFor = game.stats.pieces;
                planNs = SDL_GetTicksNS() - planStart;
            }

            ui.beginFrame();
            ui.drawPlay({play.mode, &play.shown, play.seconds(), play.over, nullptr, "", play.best, {}, nullptr, 0, &play.faults, &view});
            SDL_FlushRenderer(renderer);

            const uint64_t done = SDL_GetTicksNS();
            const double ms = (done - frameStart - planNs) / 1e6;
            if (play.started) {
                stats.frames.push_back(ms);
                if (waiting) stats.busyFrames.push_back(ms);
                stats.worst.push_back({ms, waiting, (done - askedAt) / 1e6, play.game.stats.pieces});
                std::partial_sort(stats.worst.begin(), stats.worst.begin() + std::min<size_t>(stats.worst.size(), 10), stats.worst.end(), [](auto& a, auto& b) { return a.ms > b.ms; });
                if (stats.worst.size() > 10) stats.worst.resize(10);
            }
            // As the app does with VSync off: 480 Hz. Frame time is the work, not this wait.
            const uint64_t spent = done - frameStart;
            if (spent < 1'000'000'000 / 480) SDL_DelayPrecise(1'000'000'000 / 480 - spent);
            frameStart = SDL_GetTicksNS();
        }
        finished += play.game.stats.lines >= 40;
        toppedOut += play.game.toppedOut;
        pcs += play.coach->progress.pcs;
        lines += play.game.stats.lines;
    }

    auto pct = [](std::vector<double> v, double p) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
    };
    double load[3] = {};
    getloadavg(load, 3);  // a loaded machine stalls frames on its own, so say how loaded it was
    std::printf("%s, %d runs: %d reached 40 lines, %d topped out, %d PCs, %d lines; load average %.1f %.1f %.1f at the end\n", style.c_str(), runs, finished, toppedOut, pcs, lines, load[0], load[1], load[2]);
    std::printf("frames %zu: median %.2f ms, p99 %.2f, p99.9 %.2f, worst %.2f; over 16.7 ms: %zu\n", stats.frames.size(), pct(stats.frames, 0.5), pct(stats.frames, 0.99), pct(stats.frames, 0.999), pct(stats.frames, 1),
                static_cast<size_t>(std::count_if(stats.frames.begin(), stats.frames.end(), [](double ms) { return ms > 16.7; })));
    std::printf("frames with a solve running %zu: median %.2f ms, p99.9 %.2f, worst %.2f\n", stats.busyFrames.size(), pct(stats.busyFrames, 0.5), pct(stats.busyFrames, 0.999), pct(stats.busyFrames, 1));
    std::printf("answers %zu, ask to answer: median %.1f ms, p99 %.1f, worst %.1f\n", stats.solves.size(), pct(stats.solves, 0.5), pct(stats.solves, 0.99), pct(stats.solves, 1));
    for (const auto& w : stats.worst) std::printf("  worst frame %.2f ms, %s (%.0f ms since the ask), piece %d\n", w.ms, w.busy ? "solve running" : "worker idle", w.solveMsSoFar, w.pieces);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
