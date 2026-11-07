#include "ai/pc_solver.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <random>

namespace ai {

namespace {

constexpr uint16_t kFullRow = (1 << core::kBoardWidth) - 1;
constexpr uint8_t kFullBag = 0xFE;  // a bit per core::Piece from I to Z

void fillBoard(core::Board& board, const std::array<uint16_t, kPcMaxRows>& rows) {
    board = core::Board{};
    for (int y = 0; y < kPcMaxRows; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (rows[y] >> x & 1) board.at(x, y) = core::Piece::Garbage;
}

void lockAndClear(core::Board& board, const std::array<core::Cell, 4>& cells, core::Piece piece) {
    for (const core::Cell& cell : cells) board.at(cell.x, cell.y) = piece;
    int write = 0;
    for (int y = 0; y < core::kBoardHeight; y++) {
        bool full = true;
        for (int x = 0; x < core::kBoardWidth; x++) full = full && board.at(x, y) != core::Piece::None;
        if (full) continue;
        for (int x = 0; x < core::kBoardWidth; x++) board.at(x, write) = board.at(x, y);
        write++;
    }
    for (; write < core::kBoardHeight; write++)
        for (int x = 0; x < core::kBoardWidth; x++) board.at(x, write) = core::Piece::None;
}

// The rest of this bag in a random order, then whole bags.
std::vector<core::Piece> randomBags(uint8_t bag, int length, std::mt19937& rng) {
    std::vector<core::Piece> pieces;
    if (!bag) bag = kFullBag;
    while (static_cast<int>(pieces.size()) < length) {
        std::vector<core::Piece> shuffled;
        for (int piece = 1; piece <= 7; piece++)
            if (bag >> piece & 1) shuffled.push_back(static_cast<core::Piece>(piece));
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        pieces.insert(pieces.end(), shuffled.begin(), shuffled.end());
        bag = kFullBag;
    }
    return pieces;
}

// The state a step leaves. The next piece hasn't been brought out yet.
void applyStep(PcQuery& state, const PcStep& step) {
    if (step.hold) {
        if (state.hold == core::Piece::None) state.queue.erase(state.queue.begin());
        state.hold = state.current;
    }
    lockAndClear(state.board, step.cells, step.piece);
    state.holdUsed = false;
    state.current = core::Piece::None;
}

int futureLength(const PcQuery& query) {
    const int cells = static_cast<int>(std::count_if(query.board.cells.begin(), query.board.cells.end(), [](core::Piece cell) { return cell != core::Piece::None; }));
    return std::max((10 * std::min(query.maxLines, kPcMaxRows) - cells) / 4, 0) + 1;
}

// Every occupied row has to clear, so there can't be more of them than lines left. And each
// column gets exactly `lines` cells before the end, whatever order rows clear in, so a piece only
// ever sits across columns that still need cells. Columns between two full ones therefore need a
// multiple of 4 between them. Empty regions of the board can't be checked that way: a clear
// joins the regions above and below it.
bool viable(const std::array<uint16_t, kPcMaxRows>& rows, const std::array<int8_t, core::kBoardWidth>& columns, int lines) {
    int occupied = 0;
    for (uint16_t row : rows) occupied += row != 0;
    if (occupied > lines) return false;
    int run = 0;
    for (int8_t column : columns) {
        int need = lines - column;
        if (need < 0) return false;
        if (need == 0 && run % 4 != 0) return false;
        run = need == 0 ? 0 : run + need;
    }
    return run % 4 == 0;
}

}  // namespace

size_t PcSolver::KeyHash::operator()(const Key& key) const {
    size_t hash = 0xcbf29ce484222325;
    auto mix = [&](size_t value) { hash = (hash ^ value) * 0x100000001b3; };
    for (uint16_t row : key.rows) mix(row);
    mix(key.next);
    mix(key.current);
    mix(key.hold);
    mix(key.bag);
    return hash;
}

uint8_t pcBagLeft(std::span<const core::Piece> seen) {
    const size_t inBag = seen.size() % 7;
    if (inBag == 0) return 0;
    uint8_t left = kFullBag;
    for (core::Piece piece : seen.last(inBag)) left &= ~(1 << static_cast<int>(piece));
    return left;
}

// Place `piece`, then carry on with the board it leaves.
bool PcSolver::place(const State& state, int lines, int placementsLeft, size_t next, core::Piece piece, core::Piece hold, bool pressedHold, uint8_t bag) {
    int top = 0;
    for (int y = 0; y < kPcMaxRows; y++)
        if (state.rows[y]) top = y + 1;
    core::Falling start = core::spawnFalling(piece);
    start.y = std::min(start.y, static_cast<double>(top + kPcAirRows));
    start.highestY = static_cast<int>(start.y);
    fillBoard(board_, state.rows);
    search_.run(board_, start, query_->rules);

    size_t depth = path_.size();
    if (candidates_.size() <= depth) candidates_.resize(depth + 1);
    std::vector<Candidate>& candidates = candidates_[depth];
    candidates.clear();
    for (const Placement& placement : search_.placements) {
        Candidate candidate{search_.cells(placement), 0};
        std::array<int, 4> ids;
        for (int i = 0; i < 4; i++) ids[i] = candidate.cells[i].y * core::kBoardWidth + candidate.cells[i].x;
        std::sort(ids.begin(), ids.end());
        for (int id : ids) candidate.id = candidate.id << 8 | id;
        // Spins don't matter here, and symmetric rotations are the same cells.
        if (std::none_of(candidates.begin(), candidates.end(), [&](const Candidate& seen) { return seen.id == candidate.id; })) candidates.push_back(candidate);
    }

    // Low placements first: a perfect clear fills from the bottom, so this finds one sooner.
    auto height = [](const Candidate& candidate) {
        int sum = 0;
        for (const core::Cell& cell : candidate.cells) sum += cell.y;
        return sum;
    };
    std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) { return height(a) < height(b); });

    for (const Candidate& candidate : candidates) {
        State after = state;
        bool fits = true;
        for (const core::Cell& cell : candidate.cells) {
            if (cell.y >= kPcMaxRows) {
                capped_ = capped_ || hollow_;
                fits = false;
                break;
            }
            after.rows[cell.y] |= 1 << cell.x;
            after.columns[cell.x]++;
        }
        if (!fits) continue;

        int write = 0;
        for (int y = 0; y < kPcMaxRows; y++)
            if (after.rows[y] != kFullRow) after.rows[write++] = after.rows[y];
        const int cleared = kPcMaxRows - write;
        while (write < kPcMaxRows) after.rows[write++] = 0;
        for (int8_t& column : after.columns) column -= cleared;

        path_.push_back({piece, pressedHold, candidate.cells});
        const bool empty = std::all_of(after.rows.begin(), after.rows.end(), [](uint16_t row) { return row == 0; });
        if (empty) return true;
        if (placementsLeft > 1 && viable(after.rows, after.columns, lines - cleared) && draw(after, lines - cleared, placementsLeft - 1, next, hold, bag)) return true;
        path_.pop_back();
        if (stopped_) return false;
    }
    return false;
}

// Bring out the next piece, which may be an unseen one.
bool PcSolver::draw(const State& state, int lines, int placementsLeft, size_t next, core::Piece hold, uint8_t bag) {
    if (next < query_->queue.size()) return search(state, lines, placementsLeft, next + 1, query_->queue[next], hold, query_->rules.allowHold, bag);
    if (!unseen_) return search(state, lines, placementsLeft, next, core::Piece::None, hold, query_->rules.allowHold, bag);
    const uint8_t from = bag ? bag : kFullBag;
    for (int piece = 1; piece <= 7; piece++) {
        if (!(from >> piece & 1)) continue;
        drawn_.push_back(static_cast<core::Piece>(piece));
        if (search(state, lines, placementsLeft, next, static_cast<core::Piece>(piece), hold, query_->rules.allowHold, from & ~(1 << piece))) return true;
        drawn_.pop_back();
        if (stopped_) return false;
    }
    return false;
}

bool PcSolver::search(const State& state, int lines, int placementsLeft, size_t next, core::Piece current, core::Piece hold, bool canHold, uint8_t bag) {
    if (current == core::Piece::None && !toFinish_) return true;  // the known pieces are all placed

    // After the root, canHold only depends on the rules, so it needn't be in the key.
    Key key{state.rows, static_cast<uint8_t>(next), static_cast<uint8_t>(current), static_cast<uint8_t>(hold), bag};
    if (!failed_.insert(key).second) return false;

    if (++nodes_ > maxNodes_ || (cancel_ && cancel_->load(std::memory_order_relaxed))) {
        stopped_ = true;
        return false;
    }

    // Out of known pieces with one held: hold swaps it for whatever comes, and if
    // nothing comes, as in a drill, it comes back by itself.
    if (current == core::Piece::None) return hold != core::Piece::None && place(state, lines, placementsLeft, next, hold, core::Piece::None, !query_->queueEnds, bag);

    if (place(state, lines, placementsLeft, next, current, hold, false, bag)) return true;
    if (stopped_ || !canHold || hold == current) return false;
    if (hold != core::Piece::None) return place(state, lines, placementsLeft, next, hold, current, true, bag);

    // An empty hold brings out the next piece, so that's the one to place.
    if (next < query_->queue.size()) return place(state, lines, placementsLeft, next + 1, query_->queue[next], current, true, bag);
    if (!unseen_) return !toFinish_;  // holding the last known piece, waiting for what comes
    const uint8_t from = bag ? bag : kFullBag;
    for (int piece = 1; piece <= 7; piece++) {
        if (!(from >> piece & 1)) continue;
        drawn_.push_back(static_cast<core::Piece>(piece));
        if (place(state, lines, placementsLeft, next, static_cast<core::Piece>(piece), current, true, from & ~(1 << piece))) return true;
        drawn_.pop_back();
        if (stopped_) return false;
    }
    return false;
}

// The answer's inputs, from the real spawn. False if a step can't be reached from there,
// which would make the search's shortcut wrong, so it's then not an answer at all.
bool PcSolver::finish(const PcQuery& query, PcResult& result) {
    core::Board board = query.board;
    for (const PathStep& step : path_) {
        search_.run(board, core::spawnFalling(step.piece), query.rules);
        std::array<int, 4> want;
        for (int i = 0; i < 4; i++) want[i] = step.cells[i].y * core::kBoardWidth + step.cells[i].x;
        std::sort(want.begin(), want.end());
        bool found = false;
        for (const Placement& placement : search_.placements) {
            auto cells = search_.cells(placement);
            std::array<int, 4> have;
            for (int i = 0; i < 4; i++) have[i] = cells[i].y * core::kBoardWidth + cells[i].x;
            std::sort(have.begin(), have.end());
            if (have != want) continue;
            result.steps.push_back({step.piece, step.hold, step.cells, search_.inputs(placement)});
            found = true;
            break;
        }
        if (!found) return false;
        lockAndClear(board, step.cells, step.piece);
    }
    return true;
}

PcResult PcSolver::run(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes, bool unseen) {
    PcResult result;
    query_ = &query;
    cancel_ = cancel;
    maxNodes_ = maxNodes;
    nodes_ = 0;
    stopped_ = capped_ = false;
    path_.clear();
    drawn_.clear();
    candidates_.reserve(kPcMaxRows * core::kBoardWidth / 4 + 1);  // place holds a reference while deeper calls grow this

    State start;
    int cells = 0, occupied = 0;
    for (int y = 0; y < core::kBoardHeight; y++) {
        bool any = false;
        for (int x = 0; x < core::kBoardWidth; x++) {
            if (query.board.at(x, y) == core::Piece::None) continue;
            any = true;
            cells++;
            if (y >= kPcMaxRows) continue;
            start.rows[y] |= 1 << x;
            start.columns[x]++;
        }
        occupied += any;
        if (any && y >= kPcMaxRows) return result;  // Unknown: too tall to hold
    }
    hollow_ = false;
    for (int y = 0; y + 1 < kPcMaxRows; y++) hollow_ = hollow_ || (start.rows[y] == 0 && start.rows[y + 1] != 0);
    if (cells % 2 != 0) {
        result.status = PcStatus::Impossible;  // pieces add 4 cells and clears take 10
        return result;
    }

    const int known = (query.current != core::Piece::None) + (query.hold != core::Piece::None) + static_cast<int>(query.queue.size());
    const bool canHold = query.rules.allowHold && !query.holdUsed;
    auto attempt = [&](int lines, int placements, bool toFinish) {
        toFinish_ = toFinish;
        failed_.clear();
        if (!viable(start.rows, start.columns, lines)) return false;
        return search(start, lines, placements, 0, query.current, query.hold, canHold, query.bagLeft);
    };
    auto answer = [&](PcStatus status) {
        result.status = status;
        result.nodes = nodes_;
        return result;
    };

    const int maxLines = std::min(query.maxLines, kPcMaxRows);
    for (int lines = std::max(occupied, 1); lines <= maxLines; lines++) {
        const int spare = 10 * lines - cells;
        if (spare % 4 != 0 || spare == 0) continue;
        const int placements = spare / 4;

        // Past what's known, a queue that ends rules this out, and longer ones too.
        if (placements > known && query.queueEnds) break;
        result.lines = lines;
        result.piecesNeeded = placements;

        if (placements <= known) {
            unseen_ = false;
            if (attempt(lines, placements, true)) {
                if (!finish(query, result)) return answer(PcStatus::Unknown);
                return answer(PcStatus::Found);
            }
            if (stopped_ || capped_) return answer(PcStatus::Unknown);
            continue;
        }

        unseen_ = unseen;
        const bool possible = attempt(lines, placements, unseen);
        if (stopped_) return answer(PcStatus::Unknown);
        if (possible && unseen) {
            if (!finish(query, result)) return answer(PcStatus::Unknown);
            result.assumed = drawn_;
            return answer(PcStatus::Found);
        }
        if (!possible && capped_) return answer(PcStatus::Unknown);
        return answer(possible ? PcStatus::NeedMorePieces : PcStatus::Impossible);
    }
    result.lines = maxLines;
    return answer(PcStatus::Impossible);
}

PcAdvice PcSolver::advise(const PcQuery& query, const std::atomic<bool>* cancel, const PcAdviceOptions& options) {
    PcAdvice advice;
    const PcResult visible = solve(query, cancel);
    advice.status = visible.status;
    advice.lines = visible.lines;
    if (visible.status == PcStatus::Found) {
        advice.hasStep = true;
        advice.step = visible.steps[0];
        advice.chance = 1;
        return advice;
    }
    advice.partial = cancel && cancel->load(std::memory_order_relaxed);
    if (visible.status != PcStatus::NeedMorePieces || advice.partial) return advice;

    // Every placement open to the player now, and the state each leaves.
    struct Choice {
        PcStep step;
        PcQuery after;
        int hits = 0, samples = 0, unknown = 0;
    };
    std::vector<Choice> choices;
    auto addChoices = [&](core::Piece piece, bool pressHold) {
        search_.run(query.board, core::spawnFalling(piece), query.rules);
        const std::vector<Placement> placements = search_.placements;
        std::vector<std::array<int, 4>> seen;
        for (const Placement& placement : placements) {
            auto cells = search_.cells(placement);
            std::array<int, 4> ids;
            for (int i = 0; i < 4; i++) ids[i] = cells[i].y * core::kBoardWidth + cells[i].x;
            std::sort(ids.begin(), ids.end());
            if (std::find(seen.begin(), seen.end(), ids) != seen.end()) continue;
            seen.push_back(ids);

            PcStep step{piece, pressHold, cells, search_.inputs(placement)};
            PcQuery after = query;
            applyStep(after, step);
            choices.push_back({std::move(step), std::move(after)});
        }
    };
    addChoices(query.current, false);
    if (query.rules.allowHold && !query.holdUsed && query.hold != query.current) {
        if (query.hold != core::Piece::None) addChoices(query.hold, true);
        else if (!query.queue.empty()) addChoices(query.queue[0], true);
    }
    if (choices.empty()) return advice;

    // Enough pieces for any height allowed, one over since a piece can end the clear in hold, and
    // a rollout's previews on top.
    std::mt19937 rng(options.seed);
    const int length = futureLength(query) + static_cast<int>(query.queue.size());
    std::vector<std::vector<core::Piece>> futures;
    auto future = [&](int index) -> const std::vector<core::Piece>& {
        while (static_cast<int>(futures.size()) <= index) futures.push_back(randomBags(query.bagLeft, length, rng));
        return futures[index];
    };

    const auto begin = std::chrono::steady_clock::now();
    auto outOfTime = [&] {
        if (cancel && cancel->load(std::memory_order_relaxed)) return true;
        return options.timeBudgetMs > 0 && std::chrono::steady_clock::now() - begin > std::chrono::milliseconds(options.timeBudgetMs);
    };

    // Successive halving: every option on the same futures, keeping the better half each time.
    std::vector<Choice*> alive;
    for (Choice& choice : choices) alive.push_back(&choice);
    auto chanceOf = [](const Choice& choice) { return choice.samples ? static_cast<double>(choice.hits) / choice.samples : -1.0; };
    PcQuery trial;
    int target = std::min(8, options.maxSamples);
    while (!advice.partial) {
        for (Choice* choice : alive) {
            while (choice->samples < target) {
                if (outOfTime()) {
                    advice.partial = true;
                    break;
                }
                const std::vector<core::Piece>& pieces = future(choice->samples);
                PcResult result;
                if (options.rollouts) {
                    result.status = rollout(choice->after, pieces, query.queue.size(), options.seed + choice->samples, cancel, options.nodesPerSample);
                } else {
                    trial = choice->after;
                    trial.queue.insert(trial.queue.end(), pieces.begin(), pieces.end());
                    trial.current = trial.queue.front();
                    trial.queue.erase(trial.queue.begin());
                    trial.queueEnds = true;
                    result = solve(trial, cancel, options.nodesPerSample);
                }
                if (result.status == PcStatus::Found) choice->hits++;
                if (result.status == PcStatus::Unknown) {
                    if (outOfTime()) {
                        advice.partial = true;
                        break;
                    }
                    choice->unknown++;
                }
                choice->samples++;
            }
            if (advice.partial) break;
        }
        if (advice.partial) break;
        std::stable_sort(alive.begin(), alive.end(), [&](const Choice* a, const Choice* b) { return chanceOf(*a) > chanceOf(*b); });
        if (target >= options.maxSamples) break;
        if (alive.size() > 1) alive.resize((alive.size() + 1) / 2);
        target = std::min(target * 2, options.maxSamples);
    }

    // Stopped partway, options rest on different counts, so go by those with the most futures behind them.
    const Choice* best = alive[0];
    for (const Choice* choice : alive) {
        if (choice->samples > best->samples || (choice->samples == best->samples && chanceOf(*choice) > chanceOf(*best))) best = choice;
    }
    advice.hasStep = true;
    advice.step = best->step;
    advice.chance = std::max(chanceOf(*best), 0.0);
    advice.samples = best->samples;
    advice.unknownSamples = best->unknown;
    return advice;
}

// Plays a future as a player would: the previews come into view one at a time, and each placement
// comes from a plan for a guessed future. That's a real way to play, so its rate can be reached.
PcStatus PcSolver::rollout(PcQuery state, const std::vector<core::Piece>& future, size_t window, uint32_t seed, const std::atomic<bool>* cancel, int nodes) {
    std::mt19937 rng(seed);
    size_t used = 0;
    while (true) {
        while (state.queue.size() < window + (state.current == core::Piece::None)) {
            if (used == future.size()) return PcStatus::Impossible;
            const core::Piece piece = future[used++];
            state.queue.push_back(piece);
            state.bagLeft = (state.bagLeft ? state.bagLeft : kFullBag) & ~(1 << static_cast<int>(piece));
        }
        if (state.current == core::Piece::None) {
            state.current = state.queue.front();
            state.queue.erase(state.queue.begin());
        }
        const PcStatus visible = solve(state, cancel, nodes).status;
        if (visible != PcStatus::NeedMorePieces) return visible;

        bool planned = false;
        for (int tries = 0; tries < 3 && !planned; tries++) {
            PcQuery guess = state;
            const std::vector<core::Piece> pieces = randomBags(state.bagLeft, futureLength(state), rng);
            guess.queue.insert(guess.queue.end(), pieces.begin(), pieces.end());
            guess.queueEnds = true;
            PcResult plan = solve(guess, cancel, nodes);
            if (plan.status != PcStatus::Found) continue;
            applyStep(state, plan.steps[0]);
            planned = true;
        }
        if (!planned) return PcStatus::Impossible;
    }
}

double PcSolver::chanceAfter(const PcQuery& query, const PcStep& step, int samples, uint32_t seed, int nodesPerSolve) {
    PcQuery after = query;
    applyStep(after, step);
    std::mt19937 rng(seed);
    const int length = futureLength(query) + static_cast<int>(query.queue.size());
    int hits = 0;
    for (int i = 0; i < samples; i++) hits += rollout(after, randomBags(query.bagLeft, length, rng), query.queue.size(), seed + i, nullptr, nodesPerSolve) == PcStatus::Found;
    return static_cast<double>(hits) / samples;
}

PcResult PcSolver::solve(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes) { return run(query, cancel, maxNodes, false); }

PcResult PcSolver::solveWithUnseen(const PcQuery& query, const std::atomic<bool>* cancel, int maxNodes) { return run(query, cancel, maxNodes, true); }

}  // namespace ai
