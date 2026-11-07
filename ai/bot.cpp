#include "ai/bot.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace ai {

namespace {

constexpr uint16_t kFullRow = (1 << core::kBoardWidth) - 1;

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

bool loadBotWeights(const std::string& path, BotWeights& w) {
    std::ifstream file(path);
    if (!file) return false;
    const std::pair<const char*, double*> names[] = {
        {"holes", &w.holes}, {"height", &w.height}, {"danger", &w.danger}, {"bumpiness", &w.bumpiness}, {"well", &w.well},
        {"tSlot", &w.tSlot}, {"attack", &w.attack}, {"b2b", &w.b2b}, {"wasted", &w.wasted},
    };
    for (std::string line; std::getline(file, line);) {
        std::istringstream in(line.substr(0, line.find('#')));
        std::string name;
        double value;
        if (!(in >> name)) continue;
        if (!(in >> value)) return false;
        const auto found = std::find_if(std::begin(names), std::end(names), [&](const auto& n) { return name == n.first; });
        if (found == std::end(names)) return false;
        *found->second = value;
    }
    return true;
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
    // ponytail: only the board as found is scored; garbage on its way, surge charge and combo aren't

    const double over = std::max(highest - 10, 0);
    return static_cast<float>(node.reward + weights.holes * holes + weights.height * total / core::kBoardWidth + weights.danger * over * over +
                              weights.bumpiness * bumpiness + weights.well * wellDepth + weights.tSlot * tSlot +
                              weights.b2b * std::max<int>(node.b2b, 0));
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

void Bot::expand(const Node& node, const core::Game& game, bool atRoot) {
    core::Board board;
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (node.rows[y] >> x & 1) board.at(x, y) = core::Piece::Garbage;
    auto peek = [&](int i) { return i < static_cast<int>(queue_.size()) ? queue_[i] : core::Piece::None; };

    search_.run(board, atRoot ? game.falling : core::spawnFalling(node.current), *rules_);
    for (const Placement& p : search_.placements) addChild(node, p, node.current, node.hold, peek(node.next), node.next + 1, atRoot, false);

    if (!rules_->allowHold || (atRoot && game.holdUsed)) return;
    const bool empty = node.hold == core::Piece::None;
    const core::Piece piece = empty ? peek(node.next) : node.hold;
    if (piece == core::Piece::None || piece == node.current) return;
    const int after = empty ? node.next + 1 : node.next;
    search_.run(board, core::spawnFalling(piece), *rules_);
    for (const Placement& p : search_.placements) addChild(node, p, piece, node.current, peek(after), after + 1, atRoot, true);
}

bool Bot::choose(const core::Game& game, BotChoice& choice) {
    rules_ = &game.rules;
    queue_.assign(game.queue.begin(), game.queue.begin() + std::min<size_t>(game.rules.previews, game.queue.size()));
    roots_.clear();
    beam_.clear();

    Node start{};
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (game.board.at(x, y) != core::Piece::None) start.rows[y] |= 1 << x;
    start.current = game.falling.piece;
    start.hold = game.hold;
    start.b2b = static_cast<int8_t>(std::clamp(game.stats.b2b, -1, 127));
    start.combo = static_cast<int8_t>(std::clamp(game.stats.combo, -1, 127));
    beam_.push_back(start);

    std::unordered_set<uint64_t> seen;
    int best = -1;  // the best first move so far
    for (bool atRoot = true; !beam_.empty(); atRoot = false) {
        children_.clear();
        for (const Node& node : beam_)
            if (node.current != core::Piece::None) expand(node, game, atRoot);
        if (children_.empty()) break;
        std::sort(children_.begin(), children_.end(), [](const Node& a, const Node& b) { return a.score > b.score; });
        beam_.clear();
        seen.clear();
        for (const Node& child : children_) {
            if (static_cast<int>(beam_.size()) == width) break;
            if (seen.insert(hashOf(child.rows, child.current, child.hold, child.next)).second) beam_.push_back(child);
        }
        best = beam_[0].root;
    }
    if (best < 0) return false;

    const Root& root = roots_[best];
    const core::Piece piece = root.hold ? (game.hold != core::Piece::None ? game.hold : queue_[0]) : game.falling.piece;
    core::Board board = game.board;
    search_.run(board, root.hold ? core::spawnFalling(piece) : game.falling, game.rules);
    choice.hold = root.hold;
    choice.placement = root.placement;
    choice.cells = search_.cells(root.placement);
    choice.moves = search_.inputs(root.placement);
    return true;
}

std::vector<std::vector<core::InputEvent>> botInputs(const core::Game& game, const BotChoice& choice) {
    core::Game copy = game;
    std::vector<std::vector<core::InputEvent>> ticks;
    bool locked = false;
    auto tick = [&](std::initializer_list<core::InputEvent> inputs) {
        if (locked) return;
        ticks.emplace_back(inputs);
        locked = !copy.tick(ticks.back()).empty() || copy.toppedOut;
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
                tick({{core::Action::SoftDrop, true, 0}});
                tick({{core::Action::SoftDrop, false, 0}});
                break;
        }
    }
    tap(core::Action::HardDrop);
    return ticks;
}

}  // namespace ai
