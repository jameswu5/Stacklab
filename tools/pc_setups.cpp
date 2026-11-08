// Setups for a 4-line perfect clear from an empty board: the boards the rest of a bag can build,
// and each one's exact PC rate over every order of the next bag's pieces the finish can draw on,
// with hold. A setup is the board plus the piece in hold when the bag ends. --residue says how
// many of the bag's pieces went before the PC: 0 is PC1, with hold empty; 1 has any piece in hold.
//
// A setup is a tiling of its board by the bag's pieces, which are all different apart from the
// held one. So setups come from boards 3 or 4 placements from a clear, found backwards from the
// empty board, cut into the bag's pieces, and checked against every order of the bag, with hold,
// using the real move search. That leaves out setups whose own pieces clear a line before the
// bag ends; --verify N walks N random orders forwards to count what that misses.
//
// The move search lets no time pass. --harddrop allows only placements a hard drop from above
// reaches: no tucks or spins, so its rates are a lower bound.
//   pc_setups [--residue R] [--harddrop] [--check N] [--verify N] [--top N] [--cache FILE] [--save FILE]
// --cache keeps the backward sets between runs; --save lists every setup at half the queues or more.
#include <algorithm>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ai/pc_solver.h"

using core::Piece;

namespace {

constexpr int kRows = 4;
constexpr int kThreads = 3;  // the machine is shared
constexpr uint64_t kRowBits = (1 << core::kBoardWidth) - 1;

bool hardDropOnly = false;
int residue = 0;  // pieces of the bag used before the PC starts; above 0, a piece from earlier is in hold
thread_local ai::MoveSearch moves;
const core::Ruleset rules;

double seconds(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Bit y * 10 + x is a filled cell, floor first.
using Bits = uint64_t;

// Lines left to clear, from the cells and the placements left.
int linesFor(Bits board, int n) { return (std::popcount(board) + 4 * n) / 10; }

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

// Where the move search can lock `piece` on `board`, as cell masks, with every cell below row `lines`.
std::vector<Bits> locks(Bits board, Piece piece, int lines) {
    core::Board grid;
    for (int i = 0; i < kRows * 10; i++)
        if (board >> i & 1) grid.at(i % 10, i / 10) = Piece::Garbage;
    core::Falling start = core::spawnFalling(piece);
    start.y = std::min(start.y, double(kRows + ai::kPcAirRows));  // as the solver does
    start.highestY = int(start.y);
    moves.run(grid, start, rules);

    std::vector<Bits> out;
    for (const ai::Placement& placement : moves.placements) {
        Bits cells = 0;
        bool fits = true;
        for (const core::Cell& cell : moves.cells(placement)) {
            fits = fits && cell.y < lines;
            if (fits) cells |= Bits(1) << (cell.y * 10 + cell.x);
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
        if (std::find(out.begin(), out.end(), cells) == out.end()) out.push_back(cells);
    }
    return out;
}

Bits clearRows(Bits board) {
    Bits out = 0;
    int write = 0;
    for (int y = 0; y < kRows; y++) {
        const Bits row = board >> (y * 10) & kRowBits;
        if (row != kRowBits) out |= row << (10 * write++);
    }
    return out;
}

// The boards placing `piece` can leave, after clears, that pass the column check.
std::vector<Bits> place(Bits board, Piece piece, int lines) {
    std::vector<Bits> out;
    for (Bits cells : locks(board, piece, lines)) {
        const Bits after = clearRows(board | cells);
        const int left = lines - (std::popcount(board) + 4 - std::popcount(after)) / 10;
        if (std::find(out.begin(), out.end(), after) == out.end() && viable(after, left)) out.push_back(after);
    }
    return out;
}

thread_local std::unordered_map<uint64_t, std::vector<Bits>> childCache;

const std::vector<Bits>& children(Bits board, Piece piece, int lines) {
    auto& cache = childCache;
    const uint64_t key = board | uint64_t(piece) << 40 | uint64_t(lines) << 44;
    auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    return cache.emplace(key, place(board, piece, lines)).first->second;
}

struct Tile {
    Piece piece;
    Bits cells;
};
std::vector<Tile> tiles;                       // every piece's cells within the 4 rows
std::array<std::vector<int>, kRows * 10> tilesFrom;  // per cell, the tiles whose lowest cell it is
std::vector<Bits> shapes;                      // the same cells, whatever the piece

void buildTiles() {
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
                    if (inside && std::none_of(tiles.begin(), tiles.end(), [&](const Tile& t) { return t.piece == Piece(p) && t.cells == mask; })) tiles.push_back({Piece(p), mask});
                }
    for (int i = 0; i < int(tiles.size()); i++) {
        tilesFrom[std::countr_zero(tiles[i].cells)].push_back(i);
        shapes.push_back(tiles[i].cells);
    }
    std::sort(shapes.begin(), shapes.end());
    shapes.erase(std::unique(shapes.begin(), shapes.end()), shapes.end());
}

// Every board that placing one piece could turn into `after`, `n` placements from a clear:
// put back the rows the piece cleared, then take out its cells. Reachability isn't checked.
template <typename F>
void before(Bits after, int n, F&& found) {
    const int had = linesFor(after, n);
    for (int lines = had; lines <= kRows; lines++)
        for (int full = 0; full < 1 << lines; full++) {  // the rows that cleared, as bits
            if (std::popcount(unsigned(full)) != lines - had) continue;
            Bits expanded = 0;
            for (int y = 0, from = 0; y < lines; y++) {
                if (full >> y & 1) expanded |= kRowBits << (10 * y);
                else expanded |= (after >> (10 * from++) & kRowBits) << (10 * y);
            }
            for (Bits shape : shapes) {
                if ((expanded & shape) != shape || shape >> (10 * lines)) continue;
                bool everyRow = true;  // a cleared row was finished by this piece
                for (int y = 0; y < lines; y++) everyRow = everyRow && (!(full >> y & 1) || (shape >> (10 * y) & kRowBits));
                if (everyRow) found(expanded & ~shape);
            }
        }
}

// clearable[n]: sorted, the boards n placements can clear. Built backwards from the empty board,
// keeping a board only if the move search really reaches a placement into the level below.
std::vector<std::vector<Bits>> clearable{{0}};

bool isClearable(Bits board, int n) { return std::binary_search(clearable[n].begin(), clearable[n].end(), board); }

void buildClearable(int upTo) {
    for (int n = int(clearable.size()); n <= upTo; n++) {
        auto clock = std::chrono::steady_clock::now();
        // In parts by hash, so the candidates never all sit in memory at once.
        const int parts = n < 4 ? 1 : 16;
        std::vector<Bits> kept;
        size_t total = 0;
        for (int part = 0; part < parts; part++) {
            std::vector<Bits> candidates;
            for (Bits after : clearable[n - 1]) before(after, n - 1, [&](Bits board) {
                if ((board * 0x9E3779B97F4A7C15 >> 32) % parts == uint64_t(part)) candidates.push_back(board);
            });
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            total += candidates.size();
            std::vector<std::vector<Bits>> found(kThreads);
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; t++)
                threads.emplace_back([&, t] {
                    for (size_t i = t; i < candidates.size(); i += kThreads) {
                        bool reaches = false;
                        for (int p = 1; p <= 7 && !reaches; p++)
                            for (Bits child : place(candidates[i], Piece(p), linesFor(candidates[i], n))) reaches = reaches || isClearable(child, n - 1);
                        if (reaches) found[t].push_back(candidates[i]);
                    }
                });
            for (auto& thread : threads) thread.join();
            for (const auto& list : found) kept.insert(kept.end(), list.begin(), list.end());
            if (parts > 1) std::fprintf(stderr, "clearable in %d, part %d: %zu so far (%.0f s)\n", n, part, kept.size(), seconds(clock));
        }
        std::sort(kept.begin(), kept.end());
        clearable.push_back(std::move(kept));
        std::fprintf(stderr, "clearable in %d: %zu of %zu candidates (%.1f s)\n", n, clearable[n].size(), total, seconds(clock));
    }
}

// A set of piece orders n long: bit i is the order whose digits in base 7 are its pieces less
// one, the first piece the most significant. Orders starting with the same piece are one block.
using Orders = std::vector<uint64_t>;

int pow7(int n) { return n ? 7 * pow7(n - 1) : 1; }

// dst |= src << shift
void orShifted(Orders& dst, const Orders& src, size_t shift) {
    const size_t words = shift / 64, bits = shift % 64;
    for (size_t i = 0; i < src.size(); i++) {
        if (!src[i]) continue;
        dst[i + words] |= src[i] << bits;
        if (bits && i + words + 1 < dst.size()) dst[i + words + 1] |= src[i] >> (64 - bits);
    }
}

// For the built levels, worked out once, in the order of clearable[n]; above them,
// remembered per thread and only trimmed between setups, since the search holds references.
std::vector<std::vector<Orders>> builtWins;
thread_local std::array<std::unordered_map<Bits, Orders>, 7> winMemo;
constexpr std::array<size_t, 7> kWinMemoCap{0, 0, 0, 0, 800'000, 100'000, 0};  // about 0.5 GB per thread

void trimMemos() {
    for (int n = 0; n < 7; n++)
        if (winMemo[n].size() > kWinMemoCap[n]) winMemo[n].clear();
    if (childCache.size() > 1'000'000) childCache.clear();
}

Orders workOutWins(Bits board, int n);

// The piece orders that clear `board` in `n` placements.
const Orders& wins(Bits board, int n) {
    if (n < int(builtWins.size())) {
        static const std::array<Orders, 4> none{Orders(1), Orders(1), Orders(1), Orders(6)};
        auto it = std::lower_bound(clearable[n].begin(), clearable[n].end(), board);
        return it != clearable[n].end() && *it == board ? builtWins[n][it - clearable[n].begin()] : none[n];
    }
    auto& memo = winMemo[n];
    auto found = memo.find(board);
    if (found != memo.end()) return found->second;
    return memo.emplace(board, workOutWins(board, n)).first->second;
}

Orders workOutWins(Bits board, int n) {
    Orders result((pow7(n) + 63) / 64);
    if (n == 0) {
        result[0] = board == 0;
        return result;
    }
    if (n < int(clearable.size()) && !isClearable(board, n)) return result;
    const int lines = linesFor(board, n);
    for (int p = 1; p <= 7; p++)
        for (Bits child : children(board, Piece(p), lines)) orShifted(result, wins(child, n - 1), size_t(p - 1) * pow7(n - 1));
    return result;
}

void buildWins() {
    for (int n = 0; n <= 3; n++) {
        std::vector<Orders> level;
        for (Bits board : clearable[n]) level.push_back(workOutWins(board, n));
        builtWins.push_back(std::move(level));
        childCache.clear();
    }
}

// The piece orders a queue and hold can place, `n` long, numbered as in Orders.
template <typename F>
void orders(Piece hold, const Piece* queue, int n, int seq, F&& found) {
    if (n == 0) return found(seq);
    const Piece current = queue[0];
    orders(hold, queue + 1, n - 1, seq * 7 + int(current) - 1, found);
    if (hold == Piece::None) orders(current, queue + 2, n - 1, seq * 7 + int(queue[1]) - 1, found);
    else if (hold != current) orders(current, queue + 1, n - 1, seq * 7 + int(hold) - 1, found);
}

struct Setup {
    Bits board;
    Piece hold;
    bool operator==(const Setup&) const = default;
};
struct SetupHash {
    size_t operator()(const Setup& s) const { return std::hash<uint64_t>()(s.board << 4 | uint64_t(s.hold)); }
};

using Queue = std::array<Piece, 7>;

// Every order of the next bag's first `length` pieces.
std::vector<Queue> allQueues(int length) {
    std::vector<Queue> queues;
    Queue bag{Piece::I, Piece::J, Piece::L, Piece::O, Piece::S, Piece::T, Piece::Z};
    do {
        if (std::is_sorted(bag.begin() + length, bag.end())) queues.push_back(bag);
    } while (std::next_permutation(bag.begin(), bag.end()));
    return queues;
}

// The setup pieces: what's left of the bag, and the piece carried in hold.
int setupPieces() { return 7 - residue; }
// Placements left after the bag, and how many of the next bag's pieces they can draw on.
int placementsLeft(Piece hold) { return 10 - setupPieces() - (residue > 0) + (hold != Piece::None); }
int queueLength() { return residue == 0 ? 4 : 3 + residue; }

// How many of the queues clear this setup.
int hits(const Setup& setup, const std::vector<Queue>& queues) {
    trimMemos();
    const int n = placementsLeft(setup.hold);
    const Orders& clears = wins(setup.board, n);
    if (std::all_of(clears.begin(), clears.end(), [](uint64_t w) { return !w; })) return 0;
    int count = 0;
    for (const auto& queue : queues) {
        bool won = false;
        orders(setup.hold, queue.data(), n, 0, [&](int seq) { won = won || (clears[seq / 64] >> (seq % 64) & 1); });
        count += won;
    }
    return count;
}

// The same rate from the solver, one queue at a time.
int solverHits(const Setup& setup, const std::vector<Queue>& queues) {
    ai::PcSolver solver;
    int count = 0;
    for (const auto& queue : queues) {
        ai::PcQuery query;
        for (int i = 0; i < kRows * 10; i++)
            if (setup.board >> i & 1) query.board.at(i % 10, i / 10) = Piece::Garbage;
        query.current = queue[0];
        query.hold = setup.hold;
        query.queue.assign(queue.begin() + 1, queue.begin() + queueLength());
        query.queueEnds = true;
        count += solver.solve(query, nullptr, 10'000'000).status == ai::PcStatus::Found;
    }
    return count;
}

// Every way to cut `board` into tiles, at most `most[p]` of each piece.
void tilings(Bits board, std::array<int, 8>& most, std::vector<int>& chosen, std::vector<std::vector<int>>& out) {
    if (!board) return out.push_back(chosen);
    for (int t : tilesFrom[std::countr_zero(board)]) {
        const Tile& tile = tiles[t];
        if ((board & tile.cells) != tile.cells || !most[int(tile.piece)]) continue;
        most[int(tile.piece)]--;
        chosen.push_back(t);
        tilings(board & ~tile.cells, most, chosen, out);
        chosen.pop_back();
        most[int(tile.piece)]++;
    }
}

// How a PC can start: the bag's pieces already used, and the piece in hold.
struct Start {
    uint8_t used;
    Piece hold;
};
std::vector<Start> starts() {
    std::vector<Start> out;
    for (int used = 0; used < 1 << 8; used += 2)
        if (std::popcount(unsigned(used)) == residue)
            for (int hold = residue ? 1 : 0; hold <= (residue ? 7 : 0); hold++) out.push_back({uint8_t(used), Piece(hold)});
    return out;
}

int factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }

// For one tiling: every order of the bag, from every start, that builds it with hold, and the
// piece then in hold. Orders are numbered start by start, each start's in lexicographic order, so
// the orders that begin with a given piece are one block and can be worked out once.
void buildable(const std::vector<int>& tiling, const std::vector<Start>& from, std::vector<std::pair<int, Piece>>& out) {
    using BagOrders = std::bitset<5040>;
    const int k = int(tiling.size());
    const int all = (1 << k) - 1;
    std::array<int8_t, 128 * 8> reach;  // placed tiles * 8 + tile: whether it can lock there, -1 unknown
    reach.fill(-1);
    auto reaches = [&](int placed, int t) {
        int8_t& known = reach[placed * 8 + t];
        if (known < 0) {
            Bits board = 0;
            for (int i = 0; i < k; i++)
                if (placed >> i & 1) board |= tiles[tiling[i]].cells;
            const Tile& tile = tiles[tiling[t]];
            // Resting, and clear straight up, it's a hard drop from open air, where any rotation
            // and column is reachable. Only the rest need the move search.
            bool open = (tile.cells & kRowBits) || ((tile.cells >> 10) & board);
            for (int lift = 1; lift < kRows; lift++) open = open && !((tile.cells << (10 * lift)) & board);
            if (open || hardDropOnly) {
                known = open;
            } else {
                const auto cells = locks(board, tile.piece, kRows);
                known = std::find(cells.begin(), cells.end(), tile.cells) != cells.end();
            }
        }
        return known == 1;
    };
    // The orders of the pieces not yet drawn that finish the tiling from here.
    std::unordered_map<uint32_t, BagOrders> memo;
    auto finish = [&](auto& self, int placed, Piece hold, uint8_t drawn, int depth) -> const BagOrders& {
        const uint32_t key = placed | uint32_t(hold) << 7 | uint32_t(drawn) << 11;
        auto found = memo.find(key);
        if (found != memo.end()) return found->second;
        BagOrders result;
        if (depth == 7) {
            if (placed == all) result.set(0);
        } else {
            const int width = factorial(6 - depth);  // orders under each choice here
            int rank = 0;
            for (int p = 1; p <= 7; p++) {
                if (drawn >> p & 1) continue;
                auto then = [&](int nowPlaced, Piece nowHeld) {
                    const BagOrders& rest = self(self, nowPlaced, nowHeld, drawn | 1 << p, depth + 1);
                    if (rest.any()) result |= rest << (rank * width);
                };
                auto put = [&](Piece piece, Piece held) {
                    for (int t = 0; t < k; t++)
                        if (!(placed >> t & 1) && tiles[tiling[t]].piece == piece && reaches(placed, t)) then(placed | 1 << t, held);
                };
                put(Piece(p), hold);
                if (hold == Piece::None) then(placed, Piece(p));
                else put(hold, Piece(p));
                rank++;
            }
        }
        return memo.emplace(key, result).first->second;
    };
    const int perStart = factorial(setupPieces());
    for (size_t s = 0; s < from.size(); s++) {
        // The pieces this start has must cover the tiles, with one left over in hold, or none at PC1.
        std::array<int, 8> left{};
        left[int(from[s].hold)]++;
        for (int p = 1; p <= 7; p++) left[p] += !(from[s].used >> p & 1);
        for (int t : tiling) left[int(tiles[t].piece)]--;
        Piece held = Piece::None;
        bool fits = true;
        for (int p = 1; p <= 7; p++) {
            fits = fits && left[p] >= 0;
            if (left[p] > 0) held = Piece(p);
        }
        if (!fits) continue;
        const BagOrders& orders = finish(finish, 0, from[s].hold, from[s].used, residue);
        for (int j = 0; j < perStart; j++)
            if (orders[j]) out.push_back({int(s) * perStart + j, held});
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

// The setups one order reaches, walked forwards with hold and no shortcuts.
std::unordered_set<Setup, SetupHash> walkOrder(const Start& start, const std::array<Piece, 7>& order) {
    std::vector<Setup> states{{0, start.hold}};
    for (int depth = residue; depth < 7; depth++) {
        const Piece p = order[depth];
        std::vector<Setup> next;
        for (const Setup& s : states) {
            const int placed = depth - residue + (residue > 0) - (s.hold != Piece::None);
            const int lines = linesFor(s.board, 10 - placed);
            for (Bits child : children(s.board, p, lines)) next.push_back({child, s.hold});
            if (s.hold == Piece::None) next.push_back({s.board, p});
            else
                for (Bits child : children(s.board, s.hold, lines)) next.push_back({child, p});
        }
        std::sort(next.begin(), next.end(), [](const Setup& a, const Setup& b) { return a.board != b.board ? a.board < b.board : a.hold < b.hold; });
        next.erase(std::unique(next.begin(), next.end()), next.end());
        states = std::move(next);
    }
    return {states.begin(), states.end()};
}

// Order `index` of the bag, numbered as buildable numbers them.
std::array<Piece, 7> decodeOrder(const Start& start, int index) {
    std::array<Piece, 7> order{};
    std::vector<int> left;
    for (int p = 1; p <= 7; p++)
        if (!(start.used >> p & 1)) left.push_back(p);
    for (int depth = residue, rest = index; depth < 7; depth++) {
        const int width = factorial(6 - depth);
        order[depth] = Piece(left[rest / width]);
        left.erase(left.begin() + rest / width);
        rest %= width;
    }
    return order;
}

}  // namespace

int main(int argc, char** argv) {
    int check = 0, verify = 0, top = 20, every = 1;
    const char* cache = nullptr;
    const char* save = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--harddrop")) hardDropOnly = true;
        else if (!std::strcmp(argv[i], "--check") && i + 1 < argc) check = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--verify") && i + 1 < argc) verify = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--top") && i + 1 < argc) top = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--residue") && i + 1 < argc) residue = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!std::strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
        else if (!std::strcmp(argv[i], "--every") && i + 1 < argc) every = std::atoi(argv[++i]);
    }
    if (residue < 0 || residue > 3) {
        std::fprintf(stderr, "residues 0 to 3 only\n");
        return 1;
    }
    const auto queues = allQueues(queueLength());
    const double total = queues.size();
    const std::vector<Start> from = starts();
    const int perStart = factorial(setupPieces());
    const int orderCount = int(from.size()) * perStart;
    buildTiles();
    // The backward sets take minutes, so --cache keeps them in a file between runs. Residues 2
    // and 3 also use the boards 4 placements from a clear.
    const int levels = residue >= 2 ? 4 : 3;
    if (FILE* in = cache ? std::fopen(cache, "rb") : nullptr) {
        uint64_t size;
        while (int(clearable.size()) <= levels && std::fread(&size, 8, 1, in) == 1) {
            clearable.emplace_back(size);
            if (std::fread(clearable.back().data(), 8, size, in) != size) return 1;
        }
        std::fclose(in);
    }
    if (int(clearable.size()) <= levels) {
        buildClearable(levels);
        if (FILE* out = cache ? std::fopen(cache, "wb") : nullptr) {
            for (size_t n = 1; n < clearable.size(); n++) {
                const uint64_t size = clearable[n].size();
                std::fwrite(&size, 8, 1, out);
                std::fwrite(clearable[n].data(), 8, size, out);
            }
            std::fclose(out);
        }
    }

    auto built = std::chrono::steady_clock::now();
    buildWins();
    std::fprintf(stderr, "win sets for the built levels (%.0f s)\n", seconds(built));

    // 1. Setup boards: 7 tiles from clearable[3] (residue 0, hold empty), or 6 tiles a placement
    // before it. There are too many of the second to hold at once, so they come in parts.
    // 2. Cut each into the bag's pieces, find the orders that build it, and rate each setup once.
    auto clock = std::chrono::steady_clock::now();
    std::unordered_map<Setup, int, SetupHash> rate;                     // every setup some order builds
    // Setups at half the queues or more, which is where the covering list picks from: orders that
    // build them. --verify needs every setup with a PC, which takes more memory.
    std::unordered_map<Setup, std::vector<uint16_t>, SetupHash> reached;
    const int listFrom = verify ? 1 : int(total) / 2;
    std::vector<int> best(orderCount, 0);  // per order, the best rate it can build
    std::atomic<long> tilingCount = 0;
    std::mutex lock;
    auto work = [&](const std::vector<Bits>& boards, int pieces, size_t first) {
        for (size_t i = first; i < boards.size(); i += kThreads) {
            std::array<int, 8> most;
            most.fill(residue > 0 ? 2 : 1);  // the held piece can repeat one of the bag's
            std::vector<int> chosen;
            std::vector<std::vector<int>> found;
            tilings(boards[i], most, chosen, found);
            for (const auto& tiling : found) {
                if (int(tiling.size()) != pieces) continue;
                int repeats = 0;  // at most one piece twice, and only the held one can be
                std::array<int, 8> count{};
                for (int t : tiling) repeats += ++count[int(tiles[t].piece)] == 2;
                if (repeats > (residue > 0)) continue;
                tilingCount++;
                std::vector<std::pair<int, Piece>> builds;
                buildable(tiling, from, builds);
                std::sort(builds.begin(), builds.end(), [](auto a, auto b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
                for (size_t j = 0; j < builds.size();) {
                    const Setup setup{boards[i], builds[j].second};
                    std::vector<uint16_t> orders;
                    for (; j < builds.size() && builds[j].second == setup.hold; j++) orders.push_back(uint16_t(builds[j].first));
                    std::unique_lock guard(lock);
                    auto known = rate.find(setup);
                    if (known == rate.end()) {
                        guard.unlock();
                        const int h = hits(setup, queues);
                        guard.lock();
                        known = rate.emplace(setup, h).first;
                    }
                    for (uint16_t o : orders) best[o] = std::max(best[o], known->second);
                    if (known->second < listFrom) continue;
                    std::vector<uint16_t>& list = reached[setup];
                    std::vector<uint16_t> merged;
                    std::set_union(list.begin(), list.end(), orders.begin(), orders.end(), std::back_inserter(merged));
                    list = std::move(merged);
                }
            }
            if (i % 1000000 == 0) std::fprintf(stderr, "%zu of %zu boards, %ld tilings, %zu setups (%.0f s)\n", i, boards.size(), tilingCount.load(), rate.size(), seconds(clock));
        }
    };
    auto run = [&](const std::vector<Bits>& boards, int pieces) {
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; t++) threads.emplace_back(work, std::cref(boards), pieces, t);
        for (auto& thread : threads) thread.join();
    };
    // With few setup pieces, every order is walked forwards instead, which also finds setups
    // whose pieces clear a line. --every K walks every Kth order only, to size a run.
    if (residue == 3) {
        std::atomic<int> walked = 0;
        auto walkAll = [&](int first) {
            for (int o = first; o < orderCount; o += kThreads * every) {
                const Start& start = from[o / perStart];
                for (const Setup& setup : walkOrder(start, decodeOrder(start, o % perStart))) {
                    std::unique_lock guard(lock);
                    auto known = rate.find(setup);
                    if (known == rate.end()) {
                        guard.unlock();
                        const int h = hits(setup, queues);
                        guard.lock();
                        known = rate.emplace(setup, h).first;
                    }
                    best[o] = std::max(best[o], known->second);
                    if (known->second >= listFrom) reached[setup].push_back(uint16_t(o));
                }
                if (++walked % 10 == 0) std::fprintf(stderr, "%d orders, %zu setups (%.0f s)\n", walked.load(), rate.size(), seconds(clock));
            }
        };
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; t++) threads.emplace_back(walkAll, t * every);
        for (auto& thread : threads) thread.join();
        for (auto& [setup, list] : reached) std::sort(list.begin(), list.end());
    }
    size_t candidates = 0;
    if (residue == 0) {
        std::vector<Bits> seven;
        for (Bits b : clearable[3])
            if (std::popcount(b) == 28) seven.push_back(b);
        candidates += seven.size();
        run(seven, 7);
    }
    // The setup's pieces less the one held, a placement before the boards that many from a clear.
    const int placed = residue == 0 ? 6 : setupPieces();
    const int level = 9 - placed;
    const int parts = residue == 2 ? 32 : 8;
    for (int part = 0; residue < 3 && part < parts; part++) {
        std::vector<Bits> boards;
        for (Bits b : clearable[level]) before(b, level, [&](Bits board) {
            if (std::popcount(board) == 4 * placed && (board * 0x9E3779B97F4A7C15 >> 32) % parts == uint64_t(part)) boards.push_back(board);
        });
        std::sort(boards.begin(), boards.end());
        boards.erase(std::unique(boards.begin(), boards.end()), boards.end());
        candidates += boards.size();
        std::fprintf(stderr, "part %d: %zu boards (%.0f s)\n", part, boards.size(), seconds(clock));
        run(boards, placed);
    }
    const size_t live = std::count_if(rate.begin(), rate.end(), [](const auto& r) { return r.second > 0; });
    // Every setup at half the queues or more: board as hex, hold, queues that clear it, orders that build it.
    if (FILE* out = save ? std::fopen(save, "w") : nullptr) {
        for (const auto& [setup, list] : reached) std::fprintf(out, "%010llx %d %d %zu\n", (unsigned long long)setup.board, int(setup.hold), rate[setup], list.size());
        std::fclose(out);
    }
    std::printf("residue %d: %d orders of the bag, %d queues after it\n", residue, orderCount, int(total));
    std::printf("candidate boards %zu, tilings %ld, setups some order builds %zu, with a PC for some queue %zu (%.0f s)\n", candidates, tilingCount.load(), rate.size(), live, seconds(clock));

    if (check) {
        std::mt19937 rng(1);
        std::vector<Setup> sample;
        for (const auto& [setup, h] : rate) sample.push_back(setup);
        std::shuffle(sample.begin(), sample.end(), rng);
        sample.resize(std::min<size_t>(check, sample.size()));
        int mismatches = 0;
        for (const Setup& s : sample) {
            const int solver = solverHits(s, queues);
            if (solver != rate[s]) {
                mismatches++;
                std::printf("MISMATCH %d vs solver %d\n%s", rate[s], solver, draw(s).c_str());
            }
        }
        std::printf("checked %zu setups against the solver, queue by queue: %d mismatches\n", sample.size(), mismatches);
    }

    // 4. Best per order, then a greedy covering list.
    const double perfect = std::accumulate(best.begin(), best.end(), 0.0) / (orderCount * total);
    std::printf("perfect play, setup chosen from this bag alone: %.2f%%\n", 100 * perfect);

    auto builders = [&](const Setup& setup) {
        auto it = reached.find(setup);
        return it == reached.end() ? size_t(0) : it->second.size();
    };
    std::vector<std::pair<Setup, int>> ranked(rate.begin(), rate.end());
    std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return builders(a.first) > builders(b.first);
    });
    std::printf("\ntop %d setups: rate, orders of the bag that build it (of %d)\n", top, orderCount);
    for (int i = 0; i < top && i < int(ranked.size()); i++)
        std::printf("%.2f%%  %zu orders\n%s\n", 100.0 * ranked[i].second / total, builders(ranked[i].first), draw(ranked[i].first).c_str());

    // Greedy covering lists: over every order, then per start, which the player knows before the setup.
    std::vector<int> covered(orderCount, 0);
    auto cover = [&](int lo, int hi, int picks, auto&& report) {
        for (int pick = 0; pick < picks; pick++) {
            long bestGain = 0;
            const Setup* chosen = nullptr;
            for (const auto& [setup, list] : reached) {
                long gain = 0;
                for (auto o = std::lower_bound(list.begin(), list.end(), lo); o != list.end() && *o < hi; o++) gain += std::max(0, rate[setup] - covered[*o]);
                if (gain > bestGain) bestGain = gain, chosen = &setup;
            }
            if (!chosen) break;
            const auto& list = reached[*chosen];
            for (auto o = std::lower_bound(list.begin(), list.end(), lo); o != list.end() && *o < hi; o++) covered[*o] = std::max(covered[*o], rate[*chosen]);
            const double sofar = std::accumulate(covered.begin() + lo, covered.begin() + hi, 0.0) / ((hi - lo) * total);
            const long builds = std::count_if(covered.begin() + lo, covered.begin() + hi, [](int c) { return c > 0; });
            report(pick, *chosen, sofar, 100.0 * builds / (hi - lo));
        }
    };
    std::printf("greedy covering list (each order uses the best listed setup it can build):\n");
    cover(0, orderCount, 12, [&](int pick, const Setup& chosen, double sofar, double share) {
        std::printf("%d. rate %.2f%%, built by %zu orders; list so far %.2f%%, some setup for %.1f%% of orders (perfect %.2f%%)\n%s\n", pick + 1, 100.0 * rate[chosen] / total, builders(chosen), 100 * sofar, share, 100 * perfect, draw(chosen).c_str());
    });
    if (from.size() > 1) {
        const char* names = ".IJLOSTZ";
        std::printf("\ncovering lists per start, 3 setups each: used, held, perfect play, then the list after 1, 2 and 3 setups\n");
        std::fill(covered.begin(), covered.end(), 0);
        double sums[4] = {};
        for (size_t s = 0; s < from.size(); s++) {
            const int lo = int(s) * perStart, hi = lo + perStart;
            std::string used;
            for (int p = 1; p <= 7; p++)
                if (from[s].used >> p & 1) used += names[p];
            const double best1 = std::accumulate(best.begin() + lo, best.begin() + hi, 0.0) / (perStart * total);
            sums[0] += best1;
            double after[3] = {};
            std::string boards;
            cover(lo, hi, 3, [&](int pick, const Setup& chosen, double sofar, double) {
                after[pick] = sofar;
                boards += draw(chosen);
            });
            for (int k = 0; k < 3; k++) {
                if (k && !after[k]) after[k] = after[k - 1];
                sums[k + 1] += after[k];
            }
            std::printf("used %s held %c: perfect %.2f%%, list %.2f%% %.2f%% %.2f%%\n%s\n", used.c_str(), names[int(from[s].hold)], 100 * best1, 100 * after[0], 100 * after[1], 100 * after[2], boards.c_str());
        }
        std::printf("average over starts: perfect %.2f%%, with 1, 2 and 3 setups per start %.2f%% %.2f%% %.2f%%\n", 100 * sums[0] / from.size(), 100 * sums[1] / from.size(), 100 * sums[2] / from.size(), 100 * sums[3] / from.size());
    }

    // 5. What the tilings leave out, on a few orders walked forwards.
    std::mt19937 rng(2);
    for (int v = 0; v < verify; v++) {
        const int index = int(rng() % orderCount);
        const Start& start = from[index / perStart];
        const auto order = decodeOrder(start, index % perStart);
        clock = std::chrono::steady_clock::now();
        const auto walked = walkOrder(start, order);
        // Only setups with a PC count: the tilings keep no others.
        int live = 0, listed = 0, cleared = 0, clearedBest = 0, other = 0, extra = 0;
        for (const Setup& s : walked) {
            auto it = reached.find(s);
            if (it != reached.end() && std::binary_search(it->second.begin(), it->second.end(), index)) {
                live++;
                listed++;
                continue;
            }
            const int h = hits(s, queues);
            if (!h) continue;
            live++;
            if (std::popcount(s.board) < 4 * (10 - placementsLeft(s.hold))) {
                cleared++;
                clearedBest = std::max(clearedBest, h);
            } else if (++other <= 3) {
                std::printf("missed with no line clear:\n%s", draw(s).c_str());
            }
        }
        for (const auto& [setup, list] : reached)
            extra += std::binary_search(list.begin(), list.end(), index) && !walked.count(setup);
        std::printf("order %d walked forwards: %d setups with a PC, %d listed, %d missed with a line clear in the bag (best %.2f%%; best listed %.2f%%), %d missed without one, %d listed but not walked (%.0f s)\n", index, live, listed, cleared, 100.0 * clearedBest / total, 100.0 * best[index] / total, other, extra, seconds(clock));
    }
}
