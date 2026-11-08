// Residue 0 (PC1): every board the first bag can build toward a 4-line perfect clear, and its
// exact PC rate over the 840 orders of the next bag's first 4 pieces, which is all the 3 pieces
// after the setup can draw on, with hold. A setup is the board plus the piece in hold when the
// bag ends. Uses the real move search, which lets no time pass: at real gravity, tucks may fail.
//   pc_setups [--harddrop] [--check N] [--top N] [--prove N] [--lazy N] [--cache FILE]
// --prove builds the boards up to N placements from a clear in full, --lazy checks boards up to
// N placements from a clear on demand, and --cache keeps the built sets in FILE.
// --harddrop allows only placements a hard drop from above reaches, for the gravity question.
#include <algorithm>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai/pc_solver.h"

using core::Piece;

namespace {

constexpr int kRows = 4;
constexpr uint64_t kRowBits = (1 << core::kBoardWidth) - 1;

bool hardDropOnly = false;
int lazyUpTo = 5;  // the most placements left at which boards are checked as clearable
ai::MoveSearch moves;
const core::Ruleset rules;

double seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Bit y * 10 + x is a filled cell, floor first.
using Bits = uint64_t;

// What a state's lines and placements left are, from its cells.
int linesLeft(Bits board, int placed) { return kRows - (4 * placed - std::popcount(board)) / 10; }

// Each column gets exactly `lines` more cells, so columns between two finished ones need a
// multiple of 4 between them. The same check the solver makes.
bool viable(Bits board, int lines) {
    int run = 0;
    for (int x = 0; x < core::kBoardWidth; x++) {
        int need = lines;
        for (int y = 0; y < kRows; y++) need -= board >> (y * 10 + x) & 1;
        if (need < 0) return false;
        if (need == 0 && run % 4 != 0) return false;
        run = need == 0 ? 0 : run + need;
    }
    return run % 4 == 0;
}

// The boards placing `piece` can leave, after clears, with every cell below row `lines`.
std::vector<Bits> place(Bits board, Piece piece, int lines) {
    core::Board grid;
    for (int i = 0; i < kRows * 10; i++)
        if (board >> i & 1) grid.at(i % 10, i / 10) = Piece::Garbage;
    core::Falling start = core::spawnFalling(piece);
    start.y = std::min(start.y, double(kRows + ai::kPcAirRows));  // as the solver does
    start.highestY = int(start.y);
    moves.run(grid, start, rules);

    std::vector<Bits> out;
    for (const ai::Placement& placement : moves.placements) {
        Bits after = board;
        bool fits = true;
        for (const core::Cell& cell : moves.cells(placement)) {
            fits = fits && cell.y < lines;
            if (fits) after |= Bits(1) << (cell.y * 10 + cell.x);
        }
        if (!fits) continue;
        if (hardDropOnly) {
            core::Falling up{piece, placement.rot, placement.x, double(placement.y)};
            for (int lift = 1; fits && lift <= kRows + 2; lift++) {
                up.y = placement.y + lift;
                for (const core::Cell& cell : up.cells()) fits = fits && (cell.y >= kRows || !(board >> (cell.y * 10 + cell.x) & 1));
            }
            if (!fits) continue;
        }
        Bits cleared = 0;
        int write = 0;
        for (int y = 0; y < kRows; y++) {
            const Bits row = after >> (y * 10) & kRowBits;
            if (row != kRowBits) cleared |= row << (10 * write++);
        }
        const int left = lines - (kRows - write);
        if (std::find(out.begin(), out.end(), cleared) == out.end() && viable(cleared, left)) out.push_back(cleared);
    }
    return out;
}

const std::vector<Bits>& children(Bits board, Piece piece, int lines) {
    static std::unordered_map<uint64_t, std::vector<Bits>> cache;
    const uint64_t key = board | uint64_t(piece) << 40 | uint64_t(lines) << 44;
    auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    return cache.emplace(key, place(board, piece, lines)).first->second;
}

// clearable[n]: sorted, the boards n placements can clear, with lines left from the cell count.
// Built backwards from the empty board: put back the rows a placement cleared, take out a
// tetromino's cells, then keep the board if the move search really reaches that placement.
std::vector<std::vector<Bits>> clearable{{0}};

int linesFor(Bits board, int n) { return (std::popcount(board) + 4 * n) / 10; }

// Past the built levels, worked out on demand from the level below and remembered.
bool isClearable(Bits board, int n) {
    if (n < int(clearable.size())) return std::binary_search(clearable[n].begin(), clearable[n].end(), board);
    static std::unordered_map<uint64_t, bool> memo;
    const uint64_t key = board | uint64_t(n) << 40;
    auto found = memo.find(key);
    if (found != memo.end()) return found->second;
    bool can = false;
    for (int p = 1; p <= 7 && !can; p++)
        for (Bits child : place(board, Piece(p), linesFor(board, n)))
            if (isClearable(child, n - 1)) {
                can = true;
                break;
            }
    return memo[key] = can;
}

void buildClearable(int upTo) {
    std::vector<Bits> shapes;  // every tetromino's cells within the 4 rows
    for (int p = 1; p <= 7; p++)
        for (int rot = 0; rot < 4; rot++)
            for (int x = -3; x < 13; x++)
                for (int y = -3; y < kRows + 3; y++) {
                    Bits mask = 0;
                    bool inside = true;
                    for (const core::Cell& cell : core::Falling{Piece(p), rot, x, double(y)}.cells()) {
                        inside = inside && cell.x >= 0 && cell.x < 10 && cell.y >= 0 && cell.y < kRows;
                        if (inside) mask |= Bits(1) << (cell.y * 10 + cell.x);
                    }
                    if (inside) shapes.push_back(mask);
                }
    std::sort(shapes.begin(), shapes.end());
    shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());

    for (int n = int(clearable.size()); n <= upTo; n++) {
        auto clock = std::chrono::steady_clock::now();
        std::vector<Bits> candidates;
        for (Bits after : clearable[n - 1]) {
            const int before = linesFor(after, n - 1);
            for (int lines = before; lines <= kRows; lines++)
                for (int full = 0; full < 1 << lines; full++) {  // the rows that cleared, as bits
                    if (std::popcount(unsigned(full)) != lines - before) continue;
                    Bits expanded = 0, fullRows = 0;
                    for (int y = 0, from = 0; y < lines; y++) {
                        if (full >> y & 1) fullRows |= kRowBits << (10 * y);
                        else expanded |= (after >> (10 * from++) & kRowBits) << (10 * y);
                    }
                    expanded |= fullRows;
                    for (Bits shape : shapes) {
                        if ((expanded & shape) != shape || shape >> (10 * lines)) continue;
                        bool everyRow = true;  // a cleared row was finished by this piece
                        for (int y = 0; y < lines; y++) everyRow = everyRow && (!(full >> y & 1) || (shape >> (10 * y) & kRowBits));
                        if (everyRow) candidates.push_back(expanded & ~shape);
                    }
                }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        std::vector<Bits> kept;
        for (Bits board : candidates) {
            const int lines = linesFor(board, n);
            bool reaches = false;
            for (int p = 1; p <= 7 && !reaches; p++)
                for (Bits child : place(board, Piece(p), lines)) reaches = reaches || isClearable(child, n - 1);
            if (reaches) kept.push_back(board);
        }
        clearable.push_back(std::move(kept));
        std::fprintf(stderr, "clearable in %d: %zu of %zu candidates (%.1f s)\n", n, clearable[n].size(), candidates.size(), seconds(clock));
    }
}

// Every piece order, as base-8 digits with the first piece lowest, that clears `board` in `n` placements.
void finishes(Bits board, int lines, int n, int seq, int depth, std::bitset<4096>& out) {
    for (int p = 1; p <= 7; p++) {
        // The last piece fills exactly the 4 cells left.
        if (n == 1 && std::popcount(board) != lines * 10 - 4) return;
        for (Bits child : children(board, Piece(p), lines)) {
            const int next = seq | p << (3 * depth);
            if (n == 1) {
                if (child == 0) out.set(next);
            } else {
                const int cleared = (std::popcount(board) + 4 - std::popcount(child)) / 10;
                if (n - 1 > lazyUpTo || isClearable(child, n - 1)) finishes(child, lines - cleared, n - 1, next, depth + 1, out);
            }
        }
    }
}

// The piece orders a queue and hold can place, `n` long.
template <typename F>
void orders(Piece hold, const Piece* queue, int n, int seq, int depth, F&& found) {
    if (depth == n) return found(seq);
    const Piece current = queue[0];
    orders(hold, queue + 1, n, seq | int(current) << (3 * depth), depth + 1, found);
    if (hold == Piece::None) orders(current, queue + 2, n, seq | int(queue[1]) << (3 * depth), depth + 1, found);
    else if (hold != current) orders(current, queue + 1, n, seq | int(hold) << (3 * depth), depth + 1, found);
}

struct Setup {
    Bits board;
    Piece hold;
    bool operator==(const Setup&) const = default;
};
struct SetupHash {
    size_t operator()(const Setup& s) const { return std::hash<uint64_t>()(s.board << 4 | uint64_t(s.hold)); }
};

std::vector<std::array<Piece, 4>> allQueues() {
    std::vector<std::array<Piece, 4>> queues;
    for (int a = 1; a <= 7; a++)
        for (int b = 1; b <= 7; b++)
            for (int c = 1; c <= 7; c++)
                for (int d = 1; d <= 7; d++)
                    if (a != b && a != c && a != d && b != c && b != d && c != d) queues.push_back({Piece(a), Piece(b), Piece(c), Piece(d)});
    return queues;
}

// How many of the 840 queues clear this setup.
int hits(const Setup& setup, const std::vector<std::array<Piece, 4>>& queues) {
    const int placed = 7 - (setup.hold != Piece::None);
    const int n = 10 - placed;
    std::bitset<4096> wins;
    finishes(setup.board, linesLeft(setup.board, placed), n, 0, 0, wins);
    if (wins.none()) return 0;
    int count = 0;
    for (const auto& queue : queues) {
        bool won = false;
        orders(setup.hold, queue.data(), n, 0, 0, [&](int seq) { won = won || wins[seq]; });
        count += won;
    }
    return count;
}

// The same rate from the solver, one queue at a time.
int solverHits(const Setup& setup, const std::vector<std::array<Piece, 4>>& queues) {
    ai::PcSolver solver;
    int count = 0;
    for (const auto& queue : queues) {
        ai::PcQuery query;
        for (int i = 0; i < kRows * 10; i++)
            if (setup.board >> i & 1) query.board.at(i % 10, i / 10) = Piece::Garbage;
        query.current = queue[0];
        query.hold = setup.hold;
        query.queue = {queue[1], queue[2], queue[3]};
        query.queueEnds = true;
        count += solver.solve(query, nullptr, 10'000'000).status == ai::PcStatus::Found;
    }
    return count;
}

// The setups each first-bag order can reach, walked as a tree of order prefixes.
template <typename F>
void walk(std::vector<Setup>& states, int depth, uint8_t drawn, std::array<Piece, 7>& order, F&& atEnd) {
    if (depth == 7) return atEnd(order, states);
    for (int p = 1; p <= 7; p++) {
        if (drawn >> p & 1) continue;
        std::vector<Setup> next;
        for (const Setup& s : states) {
            const int placed = depth - (s.hold != Piece::None);
            const int lines = linesLeft(s.board, placed);
            auto add = [&](Piece piece, Piece hold) {
                for (Bits child : children(s.board, piece, lines)) {
                    const int n = 10 - (placed + 1);
                    if (n > lazyUpTo || isClearable(child, n)) next.push_back({child, hold});
                }
            };
            add(Piece(p), s.hold);
            if (s.hold == Piece::None) next.push_back({s.board, Piece(p)});
            else add(s.hold, Piece(p));
        }
        std::sort(next.begin(), next.end(), [](const Setup& a, const Setup& b) { return a.board != b.board ? a.board < b.board : a.hold < b.hold; });
        next.erase(std::unique(next.begin(), next.end()), next.end());
        order[depth] = Piece(p);
        walk(next, depth + 1, drawn | 1 << p, order, atEnd);
    }
}

std::string draw(const Setup& s) {
    const char* names = ".IJLOSTZ";
    std::string out = "hold ";
    out += names[int(s.hold)];
    out += "\n";
    for (int y = kRows - 1; y >= 0; y--) {
        for (int x = 0; x < 10; x++) out += (s.board >> (y * 10 + x) & 1) ? '#' : '.';
        out += "\n";
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    int check = 0, top = 20, prove = 3;
    const char* cache = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--harddrop")) hardDropOnly = true;
        else if (!std::strcmp(argv[i], "--check") && i + 1 < argc) check = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--top") && i + 1 < argc) top = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--prove") && i + 1 < argc) prove = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--lazy") && i + 1 < argc) lazyUpTo = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
    }
    const auto queues = allQueues();
    // The sets take minutes to build, so they're kept in a file between runs.
    if (cache) {
        if (FILE* in = std::fopen(cache, "rb")) {
            uint64_t size;
            while (std::fread(&size, 8, 1, in) == 1) {
                clearable.emplace_back(size);
                if (std::fread(clearable.back().data(), 8, size, in) != size) return 1;
            }
            std::fclose(in);
        }
    }
    const size_t had = clearable.size();
    buildClearable(prove);
    if (cache && clearable.size() > had) {
        FILE* out = std::fopen(cache, "wb");
        for (size_t n = 1; n < clearable.size(); n++) {
            const uint64_t size = clearable[n].size();
            std::fwrite(&size, 8, 1, out);
            std::fwrite(clearable[n].data(), 8, size, out);
        }
        std::fclose(out);
    }
    auto clock = std::chrono::steady_clock::now();

    // 1. Every setup some first-bag order reaches, and how many orders reach it.
    std::unordered_map<Setup, int, SetupHash> reached;
    std::array<Piece, 7> order;
    std::vector<Setup> root{{0, Piece::None}};
    int done = 0;
    walk(root, 0, 0, order, [&](const auto&, const std::vector<Setup>& ends) {
        for (const Setup& s : ends) reached[s]++;
        if (++done % 100 == 0) std::fprintf(stderr, "%d orders, %zu setups, %.0f s\n", done, reached.size(), seconds(clock));
    });
    std::printf("setups reached: %zu (%.1f s)\n", reached.size(), seconds(clock));

    // 2. Each one's rate.
    clock = std::chrono::steady_clock::now();
    std::unordered_map<Setup, int, SetupHash> rate;
    for (const auto& [setup, _] : reached) rate[setup] = hits(setup, queues);
    const size_t live = std::count_if(rate.begin(), rate.end(), [](const auto& r) { return r.second > 0; });
    std::printf("with a PC for some queue: %zu (%.1f s)\n", live, seconds(clock));

    if (check) {
        std::mt19937 rng(1);
        std::vector<Setup> sample;
        for (const auto& [setup, h] : rate) sample.push_back(setup);
        std::shuffle(sample.begin(), sample.end(), rng);
        sample.resize(std::min<size_t>(check, sample.size()));
        // Hard-drop rates are a subset of what the solver allows, so only the full search is comparable.
        for (const Setup& s : sample) {
            const int solver = solverHits(s, queues);
            if (!hardDropOnly && solver != rate[s]) std::printf("MISMATCH %d vs solver %d\n%s", rate[s], solver, draw(s).c_str());
        }
        std::printf("checked %zu setups against the solver\n", sample.size());
    }

    // 3. Best per first-bag order, then a greedy covering list.
    std::vector<std::pair<Setup, int>> ranked(rate.begin(), rate.end());
    std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second : reached[a.first] > reached[b.first]; });
    const size_t shortlist = std::min<size_t>(ranked.size(), 3000);
    std::unordered_map<Setup, int, SetupHash> index;
    for (size_t i = 0; i < shortlist; i++) index[ranked[i].first] = int(i);

    std::vector<int> best;                      // per order, out of 840
    std::vector<std::vector<int>> reachable;    // per order, shortlisted setups
    walk(root, 0, 0, order, [&](const auto&, const std::vector<Setup>& ends) {
        int b = 0;
        std::vector<int> r;
        for (const Setup& s : ends) {
            b = std::max(b, rate[s]);
            auto it = index.find(s);
            if (it != index.end()) r.push_back(it->second);
        }
        best.push_back(b);
        reachable.push_back(std::move(r));
    });
    const double perfect = std::accumulate(best.begin(), best.end(), 0.0) / (best.size() * 840.0);
    std::printf("perfect play, setup chosen from the first bag alone: %.2f%% over %zu orders\n", 100 * perfect, best.size());

    std::printf("\ntop %d setups: rate, first-bag orders that can build it (of 5040)\n", top);
    for (int i = 0; i < top && i < int(ranked.size()); i++)
        std::printf("%.2f%%  %d orders\n%s\n", 100.0 * ranked[i].second / 840, reached[ranked[i].first], draw(ranked[i].first).c_str());

    std::vector<int> covered(best.size(), 0);
    std::printf("greedy covering list (each order uses the best listed setup it can build):\n");
    for (int pick = 0; pick < 10; pick++) {
        std::vector<long> gain(shortlist, 0);
        for (size_t o = 0; o < best.size(); o++)
            for (int s : reachable[o]) gain[s] += std::max(0, ranked[s].second - covered[o]);
        const int chosen = int(std::max_element(gain.begin(), gain.end()) - gain.begin());
        if (gain[chosen] == 0) break;
        int buildable = 0;
        for (size_t o = 0; o < best.size(); o++)
            if (std::find(reachable[o].begin(), reachable[o].end(), chosen) != reachable[o].end()) {
                covered[o] = std::max(covered[o], ranked[chosen].second);
                buildable++;
            }
        const double total = std::accumulate(covered.begin(), covered.end(), 0.0) / (best.size() * 840.0);
        std::printf("%d. rate %.2f%%, buildable in %d orders; list so far %.2f%% (perfect %.2f%%)\n%s\n", pick + 1, 100.0 * ranked[chosen].second / 840, buildable, 100 * total, 100 * perfect, draw(ranked[chosen].first).c_str());
    }
}
