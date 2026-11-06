// The format as read from tetris-fumen's decoder (MIT, see THIRD_PARTY_NOTICES.md).
#include "core/fumen.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <stdexcept>

namespace core {
namespace {

constexpr std::string_view kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr int kBase = 64;
constexpr int kCommentBase = 96;
constexpr int kCommentChars = 95;  // printable ASCII, from space
constexpr int kFieldRows = 23;     // rows a fumen board can hold; a rise pushes higher ones off
constexpr int kNoChange = 8;       // a field diff stores its change plus this
constexpr int kMaxCell = 8;

// Fumen's numbering, which differs from core::Piece. The last is grey, for cells only.
constexpr Piece kPieces[] = {Piece::None, Piece::I, Piece::L, Piece::O, Piece::Z, Piece::T, Piece::J, Piece::S, Piece::Garbage};

enum Rotation { Reverse, Right, Spawn, Left };

// Spawn cells around the anchor, with y up.
constexpr int kShapes[8][4][2] = {
    {},
    {{0, 0}, {-1, 0}, {1, 0}, {2, 0}},
    {{0, 0}, {-1, 0}, {1, 0}, {1, 1}},
    {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
    {{0, 0}, {1, 0}, {0, 1}, {-1, 1}},
    {{0, 0}, {-1, 0}, {1, 0}, {0, 1}},
    {{0, 0}, {-1, 0}, {1, 0}, {-1, 1}},
    {{0, 0}, {-1, 0}, {0, 1}, {1, 1}},
};

// Added to a stored position to get the anchor, by piece and rotation.
constexpr int kAnchors[8][4][2] = {
    {},
    {{1, 0}, {0, 0}, {0, 0}, {0, -1}},
    {},
    {{1, 0}, {0, 0}, {0, -1}, {1, -1}},
    {{0, 0}, {0, 0}, {0, -1}, {1, 0}},
    {},
    {},
    {{0, 0}, {-1, 0}, {0, -1}, {0, 0}},
};

[[noreturn]] void fail(const std::string& why) { throw std::invalid_argument("fumen: " + why); }

struct Field {
    std::array<int, kBoardHeight * kBoardWidth> cells{};
    std::array<int, kBoardWidth> below{};  // the row under the floor, which a rise brings up

    int& at(int x, int y) { return cells[y * kBoardWidth + x]; }
};

struct Reader {
    std::string_view data;
    size_t pos = 0;

    int poll(int digits) {
        int value = 0, scale = 1;
        for (int i = 0; i < digits; i++, scale *= kBase) {
            if (pos >= data.size()) fail("data ends early");
            value += static_cast<int>(kDigits.find(data[pos++])) * scale;
        }
        return value;
    }
};

// Reads the data out of a bare string or a URL. Returns the rows above the floor.
int extract(std::string_view text, std::string& data) {
    for (const auto& [version, rows] : {std::pair{std::string_view("115@"), 23}, std::pair{std::string_view("110@"), 21}}) {
        for (size_t at = text.find(version); at != std::string_view::npos; at = text.find(version, at + 1)) {
            if (at == 0 || std::string_view("vmd").find(text[at - 1]) == std::string_view::npos) continue;
            const std::string_view rest = text.substr(at + version.size());
            for (char c : rest.substr(0, rest.find('&'))) {
                if (c != '?' && std::string_view(" \t\r\n").find(c) == std::string_view::npos) data += c;
            }
            const size_t bad = data.find_first_not_of(kDigits);
            if (bad != std::string::npos) fail("invalid character at data position " + std::to_string(bad));
            return rows;
        }
    }
    fail("no supported version found, expected v115@ or v110@");
}

void appendUtf8(std::string& out, unsigned c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | c >> 18);
        out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// One JavaScript escape() unit from s at i, moving i past it: %XX, %uXXXX or a plain
// character. A % that starts neither stays a plain character, as in unescape().
unsigned readUnit(std::string_view s, size_t& i) {
    const auto hex = [&](size_t from, size_t count, unsigned& value) {
        if (from + count > s.size()) return false;
        const auto [end, error] = std::from_chars(s.data() + from, s.data() + from + count, value, 16);
        return error == std::errc{} && end == s.data() + from + count;
    };
    unsigned value = 0;
    if (s[i] == '%' && i + 1 < s.size() && s[i + 1] == 'u' && hex(i + 2, 4, value)) {
        i += 6;
    } else if (s[i] == '%' && hex(i + 1, 2, value)) {
        i += 3;
    } else {
        value = static_cast<unsigned char>(s[i++]);
    }
    return value;
}

std::string unescape(std::string_view s) {
    constexpr unsigned kReplacement = 0xFFFD;
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned c = readUnit(s, i);
        if (c >= 0xDC00 && c < 0xE000) {
            c = kReplacement;
        } else if (c >= 0xD800 && c < 0xDC00) {
            size_t after = i;
            const unsigned low = after < s.size() ? readUnit(s, after) : 0;
            if (low >= 0xDC00 && low < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                i = after;
            } else {
                c = kReplacement;
            }
        }
        appendUtf8(out, c);
    }
    return out;
}

std::string readComment(Reader& r) {
    const int length = r.poll(2);
    std::string escaped;
    for (int word = 0; word < (length + 3) / 4; word++) {
        int value = r.poll(5);
        for (int i = 0; i < 4; i++, value /= kCommentBase) {
            const int index = value % kCommentBase;
            if (index >= kCommentChars) fail("invalid comment character");
            escaped += static_cast<char>(' ' + index);
        }
        if (value != 0) fail("invalid comment data");
    }
    escaped.resize(length);
    return unescape(escaped);
}

// Applies one page's field diff. Returns the repeat count if it says nothing changed.
int readField(Reader& r, Field& field, int rows, int page) {
    const int blocks = (rows + 1) * kBoardWidth;
    int repeat = 0;
    for (int index = 0; index < blocks;) {
        const int value = r.poll(2);
        const int change = value / blocks - kNoChange;
        const int run = value % blocks + 1;
        if (index + run > blocks) fail("field data runs past the board on page " + std::to_string(page));
        if (change == 0 && run == blocks) repeat = r.poll(1);
        for (int i = 0; i < run; i++, index++) {
            const int x = index % kBoardWidth;
            const int y = rows - index / kBoardWidth - 1;
            int& cell = y >= 0 ? field.at(x, y) : field.below[x];
            cell += change;
            if (cell < 0 || cell > kMaxCell) fail("invalid field cell on page " + std::to_string(page));
        }
    }
    return repeat;
}

std::array<Cell, 4> pieceCells(int type, int rotation, int x, int y) {
    std::array<Cell, 4> cells;
    for (int i = 0; i < 4; i++) {
        const int bx = kShapes[type][i][0], by = kShapes[type][i][1];
        const int dx = rotation == Spawn ? bx : rotation == Right ? by : rotation == Left ? -by : -bx;
        const int dy = rotation == Spawn ? by : rotation == Right ? -bx : rotation == Left ? bx : -by;
        cells[i] = {x + dx, y + dy};
    }
    return cells;
}

// The Falling with these cells. Fumen and core anchor pieces differently, so this
// lines up the corners of the two.
Falling fallingAt(int type, int rotation, const std::array<Cell, 4>& cells) {
    Falling f;
    f.piece = kPieces[type];
    f.rot = (Spawn - rotation + 4) % 4;
    const auto low = [](const std::array<Cell, 4>& c, int Cell::*axis) {
        return (*std::min_element(c.begin(), c.end(), [&](const Cell& a, const Cell& b) { return a.*axis < b.*axis; })).*axis;
    };
    const auto origin = f.cells();
    f.x = low(cells, &Cell::x) - low(origin, &Cell::x);
    f.y = low(cells, &Cell::y) - low(origin, &Cell::y);
    return f;
}

void clearLines(Field& field) {
    int write = 0;
    for (int y = 0; y < kBoardHeight; y++) {
        const auto row = field.cells.begin() + y * kBoardWidth;
        if (std::find(row, row + kBoardWidth, 0) == row + kBoardWidth) continue;
        if (write != y) std::copy_n(row, kBoardWidth, field.cells.begin() + write * kBoardWidth);
        write++;
    }
    std::fill(field.cells.begin() + write * kBoardWidth, field.cells.end(), 0);
}

void riseBelow(Field& field) {
    std::copy_backward(field.cells.begin(), field.cells.begin() + (kFieldRows - 1) * kBoardWidth, field.cells.begin() + kFieldRows * kBoardWidth);
    std::copy(field.below.begin(), field.below.end(), field.cells.begin());
    std::fill(field.cells.begin() + kFieldRows * kBoardWidth, field.cells.end(), 0);
    field.below.fill(0);
}

void mirror(Field& field) {
    for (int y = 0; y < kBoardHeight; y++) {
        const auto row = field.cells.begin() + y * kBoardWidth;
        std::reverse(row, row + kBoardWidth);
    }
}

}  // namespace

std::vector<FumenPage> parseFumen(std::string_view text) {
    std::string data;
    const int rows = extract(text, data);
    const int blocks = (rows + 1) * kBoardWidth;
    if (data.empty()) fail("no pages");

    Reader r{data};
    Field field;
    std::string comment;
    int repeat = 0;
    std::vector<FumenPage> pages;
    // ponytail: quiz comments (#Q=) stay as written on every page; advance the queue here if the drill runner wants it per page
    while (r.pos < data.size()) {
        const int page = static_cast<int>(pages.size());
        if (repeat > 0) repeat--;
        else repeat = readField(r, field, rows, page);

        int action = r.poll(3);
        const int type = action % 8;
        action /= 8;
        const int rotation = action % 4;
        action /= 4;
        const int position = action % blocks;
        action /= blocks;
        if (action >= 32) fail("invalid flags on page " + std::to_string(page));
        const bool rise = action & 1, mirrored = action & 2, hasComment = action & 8, lock = !(action & 16);

        if (hasComment) comment = readComment(r);

        FumenPage out;
        for (int i = 0; i < kBoardHeight * kBoardWidth; i++) {
            out.board.cells[i] = kPieces[field.cells[i]];
        }
        out.lock = lock;
        out.comment = comment;

        if (type != 0) {
            const int x = position % kBoardWidth + kAnchors[type][rotation][0];
            const int y = rows - position / kBoardWidth - 1 + kAnchors[type][rotation][1];
            const auto cells = pieceCells(type, rotation, x, y);
            for (const Cell& c : cells) {
                if (c.x < 0 || c.x >= kBoardWidth || c.y < 0 || c.y >= kBoardHeight) fail("piece off the board on page " + std::to_string(page));
            }
            out.piece = fallingAt(type, rotation, cells);
            if (lock) {
                for (const Cell& c : cells) field.at(c.x, c.y) = type;
            }
        }
        pages.push_back(std::move(out));

        if (lock) {
            clearLines(field);
            if (rise) riseBelow(field);
            if (mirrored) mirror(field);
        }
    }
    return pages;
}

}  // namespace core
