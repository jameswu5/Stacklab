#include "ai/bot.h"

#include "ai/pc_solver.h"  // kPcAirRows

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace ai {

namespace {

constexpr uint16_t kFullRow = (1 << core::kBoardWidth) - 1;
constexpr uint8_t kFullBag = 0xFE;  // a bit per core::Piece from I to Z

bool filled(const std::array<uint16_t, core::kBoardHeight>& rows, int x, int y) {
    return x < 0 || x >= core::kBoardWidth || y < 0 || (y < core::kBoardHeight && (rows[y] >> x & 1));
}

bool spawnFits(const std::array<uint16_t, core::kBoardHeight>& rows, core::Piece piece) {
    for (const core::Cell& cell : core::spawnFalling(piece).cells())
        if (filled(rows, cell.x, cell.y)) return false;
    return true;
}

uint64_t hashOf(const std::array<uint16_t, core::kBoardHeight>& rows, core::Piece current, core::Piece hold, int next) {
    uint64_t h = 1469598103934665603ull;
    for (uint16_t row : rows) h = (h ^ row) * 1099511628211ull;
    return (h ^ (static_cast<uint64_t>(current) | static_cast<uint64_t>(hold) << 8 | static_cast<uint64_t>(next) << 16)) * 1099511628211ull;
}

}  // namespace

uint8_t botBagLeft(const core::Game& game) {
    // The pieces dealt so far are placed, falling, held or previewed, and bags are 7 long. The
    // falling piece came off the queue, so the bag's pieces so far end with it and the previews.
    const int previews = std::min<int>(game.rules.previews, static_cast<int>(game.queue.size()));
    const int inBag = (game.stats.pieces + 1 + (game.hold != core::Piece::None) + previews) % 7;
    uint8_t left = kFullBag;
    for (int i = previews - inBag; inBag && i < previews; i++) left &= ~(1 << static_cast<int>(i < 0 ? game.falling.piece : game.queue[i]));
    return left;
}

bool loadBot(const std::string& path, Bot& bot) {
    std::ifstream file(path);
    if (!file) return false;
    BotWeights& w = bot.weights;
    double bag = bot.bag;
    const std::pair<const char*, double*> weights[] = {
        {"holes", &w.holes}, {"height", &w.height}, {"danger", &w.danger}, {"bumpiness", &w.bumpiness}, {"well", &w.well},
        {"tSlot", &w.tSlot}, {"attack", &w.attack}, {"b2b", &w.b2b}, {"wasted", &w.wasted},
        {"incoming", &w.incoming}, {"surge", &w.surge}, {"combo", &w.combo}, {"pps", &bot.pps}, {"bag", &bag}, {"mistakes", &bot.mistakes},
    };
    const std::pair<const char*, int*> counts[] = {{"width", &bot.width}, {"depth", &bot.depth}};
    for (std::string line; std::getline(file, line);) {
        std::istringstream in(line.substr(0, line.find('#')));
        std::string name;
        double value;
        if (!(in >> name)) continue;
        if (name == "name") {
            std::getline(in >> std::ws, bot.name);
            continue;
        }
        if (!(in >> value)) return false;
        const auto weight = std::find_if(std::begin(weights), std::end(weights), [&](const auto& n) { return name == n.first; });
        const auto count = std::find_if(std::begin(counts), std::end(counts), [&](const auto& n) { return name == n.first; });
        if (weight != std::end(weights)) {
            *weight->second = value;
        } else if (count != std::end(counts)) {
            *count->second = static_cast<int>(value);
        } else {
            return false;
        }
    }
    bot.bag = bag != 0;
    return bot.width > 0 && bot.depth >= 0 && bot.pps >= 0 && bot.mistakes >= 0;
}

float Bot::evaluate(const Node& node) const {
    const auto& rows = node.rows;
    int heights[core::kBoardWidth] = {};
    int holes = 0;
    for (int x = 0; x < core::kBoardWidth; x++) {
        for (int y = core::kBoardHeight - 1; y >= 0; y--) {
            if (!(rows[y] >> x & 1)) continue;
            heights[x] = y + 1;
            break;
        }
        for (int y = 0; y < heights[x]; y++) holes += !(rows[y] >> x & 1);
    }

    int well = 0;
    for (int x = 1; x < core::kBoardWidth; x++)
        if (heights[x] < heights[well]) well = x;
    int sides = 99;
    if (well > 0) sides = heights[well - 1];
    if (well < core::kBoardWidth - 1) sides = std::min(sides, heights[well + 1]);
    const int wellDepth = std::clamp(sides - heights[well], 0, 4);

    int bumpiness = 0, total = 0, highest = 0;
    for (int x = 0; x < core::kBoardWidth; x++) {
        total += heights[x];
        highest = std::max(highest, heights[x]);
        if (x > 0 && x != well && x - 1 != well) bumpiness += std::abs(heights[x] - heights[x - 1]);
    }

    // A T-spin double slot: one gap in a row, under a row with three gaps around it and a roof on one side.
    bool tSlot = false;
    for (int x = 1; x < core::kBoardWidth - 1 && !tSlot; x++) {
        const int y = heights[x];
        if (y + 2 >= core::kBoardHeight) continue;
        const uint16_t three = 7 << (x - 1);
        tSlot = rows[y] == (kFullRow & ~(1 << x)) && rows[y + 1] == (kFullRow & ~three) &&
                (filled(rows, x - 1, y + 2) != filled(rows, x + 1, y + 2)) && !filled(rows, x, y + 2);
    }

    // ponytail: garbage that would rise on the way isn't simulated; it only weighs on the score
    const int pending = std::max(incoming_ - node.sent, 0);
    const int charge = rules_->b2bCharging ? std::max(node.b2b - rules_->b2bChargeAt, 0) : 0;

    const double over = std::max(highest - 10, 0);
    return static_cast<float>(node.reward + weights.holes * holes + weights.height * total / core::kBoardWidth + weights.danger * over * over +
                              weights.bumpiness * bumpiness + weights.well * wellDepth + weights.tSlot * tSlot +
                              weights.b2b * std::max<int>(node.b2b, 0) + weights.incoming * pending * highest +
                              weights.surge * charge + weights.combo * std::max<int>(node.combo, 0));
}

void Bot::addChild(const Node& parent, const Placement& placement, core::Piece piece, core::Piece hold, core::Piece current, int next, bool atRoot, bool held) {
    Node child = parent;
    core::Falling falling;
    falling.piece = piece;
    falling.rot = placement.rot;
    falling.x = placement.x;
    falling.y = placement.y;
    for (const core::Cell& cell : falling.cells()) {
        if (cell.y >= core::kBoardHeight) return;
        child.rows[cell.y] |= 1 << cell.x;
    }
    int lines = 0, write = 0;
    for (int y = 0; y < core::kBoardHeight; y++) {
        if (child.rows[y] == kFullRow) {
            lines++;
            continue;
        }
        child.rows[write++] = child.rows[y];
    }
    std::fill(child.rows.begin() + write, child.rows.end(), 0);

    // core::Game::lock's B2B, combo and attack, before multipliers and rounding.
    const core::Ruleset& rules = *rules_;
    const bool pc = lines > 0 && std::all_of(child.rows.begin(), child.rows.end(), [](uint16_t row) { return row == 0; });
    const bool spun = placement.spin != core::Spin::None;
    double sent = 0;
    if (lines > 0) {
        child.combo++;
        if (spun || lines >= 4 || (pc && rules.pcB2b)) {
            child.b2b += pc && rules.pcB2b ? rules.pcB2b : 1;
        } else {
            if (rules.b2bCharging && child.b2b + 1 > rules.b2bChargeAt) sent += child.b2b - rules.b2bChargeAt + rules.b2bChargeBase + 1;
            child.b2b = -1;
        }
        sent += std::floor(core::attackValue(lines, placement.spin, std::max<int>(child.b2b, 0), std::max<int>(child.combo, 0), rules));
        if (pc) sent += rules.pcGarbage;
    } else {
        child.combo = -1;
    }
    child.reward += static_cast<float>(weights.attack * sent + (sent == 0 ? weights.wasted * lines : 0));
    child.sent = static_cast<int16_t>(std::min(child.sent + sent, 1000.0));

    child.current = current;
    child.hold = hold;
    child.next = static_cast<uint8_t>(next);
    if (atRoot) {
        child.root = static_cast<int16_t>(roots_.size());
        roots_.push_back({held, placement});
    }
    if (current != core::Piece::None && !spawnFits(child.rows, current)) return;
    child.score = evaluate(child);
    children_.push_back(child);
}

// Adds a child for every placement of `start`'s piece.
void Bot::placeAll(const Node& node, core::Falling start, core::Piece hold, core::Piece current, int next, bool atRoot, bool held) {
    core::Board board;
    int top = 0;
    for (int y = 0; y < core::kBoardHeight; y++) {
        if (node.rows[y]) top = y + 1;
        for (int x = 0; x < core::kBoardWidth; x++)
            if (node.rows[y] >> x & 1) board.at(x, y) = core::Piece::Garbage;
    }
    // As in the PC solver: this far above the stack, a piece has the same choices as from the
    // spawn, for far fewer states. choose finds the inputs from where the piece really is.
    start.y = std::min(start.y, static_cast<double>(top + kPcAirRows));
    start.highestY = std::min(start.highestY, static_cast<int>(start.y));
    search_.run(board, start, *rules_);
    for (const Placement& p : search_.placements) addChild(node, p, start.piece, hold, current, next, atRoot, held);
}

void Bot::expand(const Node& node, const core::Game& game, bool atRoot) {
    auto peek = [&](int i) { return i < static_cast<int>(queue_.size()) ? queue_[i] : core::Piece::None; };
    placeAll(node, atRoot ? game.falling : core::spawnFalling(node.current), node.hold, peek(node.next), node.next + 1, atRoot, false);

    if (!rules_->allowHold || (atRoot && game.holdUsed)) return;
    const bool empty = node.hold == core::Piece::None;
    const core::Piece piece = empty ? peek(node.next) : node.hold;
    if (piece == core::Piece::None || piece == node.current) return;
    const int after = empty ? node.next + 1 : node.next;
    placeAll(node, core::spawnFalling(piece), node.current, peek(after), after + 1, atRoot, true);
}

// A state past the last preview, scored by the next piece: the mean over what the bag still
// allows of the best placement of it, or of the held piece instead.
float Bot::bagScore(const Node& node) {
    double total = 0;
    int count = 0;
    for (int piece = 1; piece <= 7; piece++) {
        if (!(bagLeft_ >> piece & 1)) continue;
        const auto next = static_cast<core::Piece>(piece);
        children_.clear();
        placeAll(node, core::spawnFalling(next), node.hold, core::Piece::None, node.next, false, false);
        if (rules_->allowHold && node.hold != core::Piece::None && node.hold != next)
            placeAll(node, core::spawnFalling(node.hold), next, core::Piece::None, node.next, false, true);
        float best = -1e6f;  // nothing fits: as bad as it gets
        for (const Node& child : children_) best = std::max(best, child.score);
        total += best;
        count++;
    }
    return static_cast<float>(total / count);
}

bool Bot::choose(const core::Game& game, BotChoice& choice, const std::atomic<bool>* cancel) {
    // The PPS cap waits before moving, in the air or for up to half the lock delay on the
    // ground. The wait is judged from the last plan's length, so placements come about 1/pps apart.
    core::Game now = game;
    int waited = 0;
    if (pps > 0) {
        const int wait = static_cast<int>(std::lround(core::kTicksPerSecond / pps)) - lastTicks_;
        for (; waited < wait && now.falling.locking + 1 < now.rules.lockTime / 2; waited++) now.tick({});
    }

    rules_ = &now.rules;
    queue_.assign(now.queue.begin(), now.queue.begin() + std::min<size_t>(now.rules.previews, now.queue.size()));
    roots_.clear();
    beam_.clear();

    Node start{};
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (now.board.at(x, y) != core::Piece::None) start.rows[y] |= 1 << x;
    start.current = now.falling.piece;
    start.hold = now.hold;
    start.b2b = static_cast<int8_t>(std::clamp(now.stats.b2b, -1, 127));
    start.combo = static_cast<int8_t>(std::clamp(now.stats.combo, -1, 127));
    incoming_ = now.incomingLines();
    bagLeft_ = botBagLeft(now);
    beam_.push_back(start);

    std::unordered_set<uint64_t> seen;
    int last = -1;  // the deepest layer kept
    for (int d = 0; !beam_.empty() && (depth == 0 || d < depth); d++) {
        children_.clear();
        for (const Node& node : beam_) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            if (node.current != core::Piece::None) expand(node, now, d == 0);
        }
        if (children_.empty()) break;
        last = d;
        std::sort(children_.begin(), children_.end(), [](const Node& a, const Node& b) { return a.score > b.score; });
        beam_.clear();
        seen.clear();
        for (const Node& child : children_) {
            if (static_cast<int>(beam_.size()) == width) break;
            if (!seen.insert(hashOf(child.rows, child.current, child.hold, child.next)).second) continue;
            beam_.push_back(child);
            Root& root = roots_[child.root];
            if (root.depth < d) {  // the beam is best first, so this is the root's best at this depth
                root.depth = d;
                root.score = child.score;
            }
        }
    }

    // Past the previews, each first move is judged by its best state's bag score instead.
    if (bag && depth == 0 && last >= 0) {
        for (const Node& node : beam_) roots_[node.root].score = -1e9f;
        for (const Node& node : beam_) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return false;
            Root& root = roots_[node.root];
            root.score = std::max(root.score, bagScore(node));
        }
    }

    // The search lets no time pass, so gravity can carry a piece off a ledge or under a kick
    // on the way. Each plan is played on a copy first, and the next best is tried if it misses.
    order_.resize(roots_.size());
    for (size_t i = 0; i < order_.size(); i++) order_[i] = static_cast<int>(i);
    std::sort(order_.begin(), order_.end(), [&](int a, int b) {
        return roots_[a].depth != roots_[b].depth ? roots_[a].depth > roots_[b].depth : roots_[a].score > roots_[b].score;
    });
    // A mistake swaps in one of the next few choices.
    if (mistakes > 0 && order_.size() > 1 && rng.nextFloat() < mistakes) {
        std::swap(order_[0], order_[1 + rng.next() % std::min<size_t>(order_.size() - 1, 7)]);
    }
    for (size_t i = 0; i < order_.size() && i < 32; i++) {
        const Root& root = roots_[order_[i]];
        const core::Piece piece = root.hold ? (now.hold != core::Piece::None ? now.hold : queue_[0]) : now.falling.piece;
        search_.run(now.board, root.hold ? core::spawnFalling(piece) : now.falling, now.rules);
        const auto found = std::find_if(search_.placements.begin(), search_.placements.end(), [&](const Placement& p) {
            return p.rot == root.placement.rot && p.x == root.placement.x && p.y == root.placement.y && p.spin == root.placement.spin;
        });
        if (found == search_.placements.end()) continue;
        choice.hold = root.hold;
        choice.placement = *found;
        choice.cells = search_.cells(*found);
        choice.moves = search_.inputs(*found);
        if (!plan(now, choice)) continue;
        lastTicks_ = static_cast<int>(choice.inputs.size());
        choice.inputs.insert(choice.inputs.begin(), waited, {});
        return true;
    }
    return false;
}

// Fills choice.inputs and plays them on a copy: true if the piece locks where it chose.
bool Bot::plan(const core::Game& game, BotChoice& choice) {
    core::Game copy = game;
    auto& ticks = choice.inputs;
    ticks.clear();
    bool locked = false, landed = false;
    auto tick = [&](std::initializer_list<core::InputEvent> inputs) {
        if (locked) return;
        ticks.emplace_back(inputs);
        const auto& locks = copy.tick(ticks.back());
        locked = !locks.empty() || copy.toppedOut;
        landed = !locks.empty() && locks[0].cells == choice.cells;
    };
    auto tap = [&](core::Action action) { tick({{action, true, 0}, {action, false, 0}}); };
    auto row = [&] { return static_cast<int>(std::floor(copy.falling.y)); };

    if (choice.hold) tap(core::Action::Hold);
    for (Move move : choice.moves) {
        switch (move) {
            case Move::Left: tap(core::Action::Left); break;
            case Move::Right: tap(core::Action::Right); break;
            case Move::RotateCW: tap(core::Action::RotateCW); break;
            case Move::RotateCCW: tap(core::Action::RotateCCW); break;
            case Move::Rotate180: tap(core::Action::Rotate180); break;
            case Move::DasLeft:
            case Move::DasRight: {
                const core::Action action = move == Move::DasLeft ? core::Action::Left : core::Action::Right;
                tick({{action, true, 0}});
                for (int i = 0; i < copy.handling.das + 1; i++) tick({});
                tick({{action, false, 0}});
                break;
            }
            case Move::SoftDrop:
                tick({{core::Action::SoftDrop, true, 0}});
                for (int last = -1; !locked && last != row();) {
                    last = row();
                    tick({});
                }
                tick({{core::Action::SoftDrop, false, 0}});
                break;
            case Move::Down:
                // ponytail: waits for gravity, since instant soft drop can't go one row; slow at low gravity.
                // A second without falling gives up, and the miss sends choose to its next choice.
                for (int from = row(), i = 0; !locked && row() == from && i < core::kTicksPerSecond; i++) tick({});
                break;
        }
    }
    tap(core::Action::HardDrop);
    return landed;
}

}  // namespace ai
