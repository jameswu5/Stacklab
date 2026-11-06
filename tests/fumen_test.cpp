// Checks parseFumen. "Published" strings and their expected values come from
// tetris-fumen's own tests and README (MIT, see THIRD_PARTY_NOTICES.md), so they're
// independent of this code. "Worked" strings were encoded by hand from the format,
// so they only show the parser agrees with our reading of it.
#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/fumen.h"

using core::parseFumen;
using core::Piece;

namespace {

char charOf(Piece p) { return ".IJLOSTZX"[static_cast<int>(p)]; }

// Bottom row first, without the empty rows on top.
std::vector<std::string> rows(const core::Board& board) {
    std::vector<std::string> out;
    for (int y = 0; y < core::kBoardHeight; y++) {
        std::string row;
        for (int x = 0; x < core::kBoardWidth; x++) row += charOf(board.at(x, y));
        out.push_back(row);
    }
    while (!out.empty() && out.back() == "..........") out.pop_back();
    return out;
}

using Rows = std::vector<std::string>;

// Sorted {x, y} pairs of the piece's cells.
std::vector<std::pair<int, int>> cells(const core::Falling& f) {
    std::vector<std::pair<int, int>> out;
    for (const core::Cell& c : f.cells()) out.push_back({c.x, c.y});
    std::sort(out.begin(), out.end());
    return out;
}

using Cells = std::vector<std::pair<int, int>>;

void expectError(const std::string& text, const std::string& reason) {
    try {
        parseFumen(text);
    } catch (const std::invalid_argument& e) {
        assert(std::string(e.what()).find(reason) != std::string::npos);
        return;
    }
    assert(false && "expected an error");
}

std::string encode(long long value, int digits) {
    static const std::string kDigits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (int i = 0; i < digits; i++, value /= 64) out += kDigits[value % 64];
    return out;
}

// An empty v115 page holding one piece, with a comment if it's given.
std::string pageWith(int type, int rotation, int x, int y, const std::string& comment = "") {
    const int position = (22 - y) * 10 + x;
    const int flags = comment.empty() ? 0 : 8;
    std::string out = "v115@vhA" + encode(type + 8 * (rotation + 4 * (position + 240 * flags)), 3);
    if (comment.empty()) return out;
    out += encode(comment.size(), 2);
    for (size_t i = 0; i < comment.size(); i += 4) {
        long long word = 0, scale = 1;
        for (size_t j = i; j < i + 4; j++, scale *= 96) word += scale * (j < comment.size() ? comment[j] - ' ' : 0);
        out += encode(word, 5);
    }
    return out;
}

void publishedBoards() {
    const auto empty = parseFumen("v115@vhAAgH");
    assert(empty.size() == 1);
    assert(rows(empty[0].board).empty());
    assert(empty[0].piece.piece == Piece::None);
    assert(empty[0].lock);
    assert(empty[0].comment.empty());

    assert(!parseFumen("v115@vhAAgl")[0].lock);

    // 4 rows of grey, leaving the right column open.
    assert(rows(parseFumen("v115@9gI8AeI8AeI8AeI8KeAgH")[0].board) ==
           (Rows{"XXXXXXXXX.", "XXXXXXXXX.", "XXXXXXXXX.", "XXXXXXXXX."}));

    Rows allI(23, "IIIIIIIIII");
    assert(rows(parseFumen("v115@flAgH")[0].board) == allI);

    // The row under the floor flips nothing when mirrored...
    const auto mirrored = parseFumen("v115@RhA8IeB8HeC8GeAQLvhAAAA");
    assert(mirrored.size() == 2);
    assert(rows(mirrored[0].board) == (Rows{"XX........", "X........."}));
    assert(rows(mirrored[1].board) == (Rows{"........XX", ".........X"}));

    // ...and rises when sent.
    const auto sent = parseFumen("v115@RhA8IeB8HeC8GeAYJvhAAAA");
    assert(sent.size() == 2);
    assert(rows(sent[0].board) == (Rows{"XX........", "X........."}));
    assert(rows(sent[1].board) == (Rows{"XXX.......", "XX........", "X........."}));
}

void publishedPieces() {
    const auto iSpawn = parseFumen("v115@vhARQJ");
    assert(iSpawn.size() == 1);
    assert(iSpawn[0].piece.piece == Piece::I);
    assert(iSpawn[0].piece.rot == 0);
    assert(cells(iSpawn[0].piece) == (Cells{{3, 0}, {4, 0}, {5, 0}, {6, 0}}));
    assert(rows(iSpawn[0].board).empty());

    const auto quiz = parseFumen("v115@vhGSSYXAFLDmClcJSAVDEHBEooRBMoAVBUtfBAXsBAAANrBmnBAAAAAA");
    assert(quiz.size() == 7);
    assert(quiz[0].comment == "#Q=[](L)TSJ");
    // L spawn at 8,0.
    assert(quiz[0].piece.piece == Piece::L);
    assert(cells(quiz[0].piece) == (Cells{{7, 0}, {8, 0}, {9, 0}, {9, 1}}));
    // S spawn at 6,0.
    assert(quiz[1].piece.piece == Piece::S);
    assert(cells(quiz[1].piece) == (Cells{{5, 0}, {6, 0}, {6, 1}, {7, 1}}));
    assert(quiz[2].piece.piece == Piece::None);
    // T right at 4,1.
    assert(quiz[3].piece.piece == Piece::T);
    assert(quiz[3].piece.rot == 1);
    assert(cells(quiz[3].piece) == (Cells{{4, 0}, {4, 1}, {4, 2}, {5, 1}}));
    // J reverse at 7,2.
    assert(quiz[4].piece.piece == Piece::J);
    assert(quiz[4].piece.rot == 2);
    assert(cells(quiz[4].piece) == (Cells{{6, 2}, {7, 2}, {8, 1}, {8, 2}}));
    assert(quiz[5].piece.piece == Piece::None);

    const auto afterQuiz = parseFumen("v115@vhCWSYVAFLDmClcJSAVDEHBEooRBKoAVB6AAAAUoBT?pB");
    assert(afterQuiz.size() == 3);
    // J spawn at 8,0, Z spawn at 8,1, O spawn at 0,0.
    assert(cells(afterQuiz[0].piece) == (Cells{{7, 0}, {7, 1}, {8, 0}, {9, 0}}));
    assert(cells(afterQuiz[1].piece) == (Cells{{7, 2}, {8, 1}, {8, 2}, {9, 1}}));
    assert(afterQuiz[2].piece.piece == Piece::O);
    assert(cells(afterQuiz[2].piece) == (Cells{{0, 0}, {0, 1}, {1, 0}, {1, 1}}));
}

void publishedComments() {
    const auto pages = parseFumen("v115@vhDAgWFAooMDEPBAAAAAPFA3XaDEEBAAAAAAAAPDAFrmAA");
    assert(pages.size() == 4);
    assert(pages[0].comment == "hello");
    assert(pages[1].comment == "world");
    assert(pages[2].comment == "world");
    assert(pages[3].comment == "!");

    const auto readme = parseFumen("https://harddrop.com/fumen/?v115@vhGRQYHAvItJEJmhCAUGJKJJvMJTNJGBJFKYPAUEzP?EJG98AQmqhECDdCA");
    assert(readme.size() == 7);
    assert(readme[0].comment == "Opening");
    assert(readme[0].piece.piece == Piece::I);
    assert(cells(readme[0].piece) == (Cells{{3, 0}, {4, 0}, {5, 0}, {6, 0}}));
}

void publishedWrappers() {
    for (const char* text : {"m115@vhAAgH", "d115@vhAAgH", "http://fumen.zui.jp/?v115@vhAAgH", "https://knewjade.github.io/fumen-for-mobile/?d=v115@vhAAgH&dummy=3",
                             "     v115@vh A A gH   ", "v115@vhA?AgH"}) {
        assert(parseFumen(text).size() == 1);
    }
}

// Grey row with the right column open, a vertical I dropped into it, then a page
// showing the row cleared and the I's other three cells fallen by one.
void workedLineClear() {
    for (const char* text : {"v115@bhI8KepIJvhAAgH", "v110@neI3qbpWI7eAA4G"}) {
        const auto pages = parseFumen(text);
        assert(pages.size() == 2);
        assert(rows(pages[0].board) == (Rows{"XXXXXXXXX."}));
        assert(pages[0].piece.piece == Piece::I);
        assert(pages[0].piece.rot == 1);
        assert(cells(pages[0].piece) == (Cells{{9, 0}, {9, 1}, {9, 2}, {9, 3}}));
        assert(rows(pages[1].board) == (Rows{".........I", ".........I", ".........I"}));
    }
}

void workedRepeats() {
    // One field, repeated for one more page, then a fresh field.
    assert(parseFumen("v115@vhBAgHAgHvhAAgH").size() == 3);
    // Unlocked pieces leave no trace on the next page.
    const auto pages = parseFumen("v115@vhBRwlAgH");
    assert(pages.size() == 2);
    assert(!pages[0].lock);
    assert(pages[0].piece.piece == Piece::I);
    assert(rows(pages[1].board).empty());
    // A locked one does, even across a repeat.
    const auto locked = parseFumen("v115@vhBRQJAgH");
    assert(rows(locked[1].board) == (Rows{"...IIII..."}));
    // A lock clears full rows even with no piece, so the second fill starts from empty.
    const Rows allI(23, "IIIIIIIIII");
    const auto refilled = parseFumen("v115@flAgHflAgH");
    assert(rows(refilled[0].board) == allI);
    assert(rows(refilled[1].board) == allI);
}

// Every piece and rotation, placed well above the floor: fumen's cells must be the
// shape core draws for that rotation, or Falling couldn't reproduce them.
void workedAllRotations() {
    struct Shape { int type; Piece piece; Cells spawn; };
    const std::vector<Shape> shapes = {
        {1, Piece::I, {{0, 0}, {-1, 0}, {1, 0}, {2, 0}}}, {2, Piece::L, {{0, 0}, {-1, 0}, {1, 0}, {1, 1}}},
        {3, Piece::O, {{0, 0}, {1, 0}, {0, 1}, {1, 1}}},  {4, Piece::Z, {{0, 0}, {1, 0}, {0, 1}, {-1, 1}}},
        {5, Piece::T, {{0, 0}, {-1, 0}, {1, 0}, {0, 1}}}, {6, Piece::J, {{0, 0}, {-1, 0}, {1, 0}, {-1, 1}}},
        {7, Piece::S, {{0, 0}, {-1, 0}, {0, 1}, {1, 1}}},
    };
    // Fumen's stored rotations are reverse, right, spawn, left. Its stored position
    // for these isn't the rotation centre.
    const int anchor[8][4][2] = {{}, {{1, 0}, {0, 0}, {0, 0}, {0, -1}}, {}, {{1, 0}, {0, 0}, {0, -1}, {1, -1}},
                                 {{0, 0}, {0, 0}, {0, -1}, {1, 0}}, {}, {}, {{0, 0}, {-1, 0}, {0, -1}, {0, 0}}};
    const int coreRot[4] = {2, 1, 0, 3};
    for (const Shape& s : shapes) {
        for (int rotation = 0; rotation < 4; rotation++) {
            const int x = 4 + anchor[s.type][rotation][0], y = 10 + anchor[s.type][rotation][1];
            Cells want;
            for (const auto& [bx, by] : s.spawn) {
                if (rotation == 2) want.push_back({x + bx, y + by});
                if (rotation == 1) want.push_back({x + by, y - bx});
                if (rotation == 3) want.push_back({x - by, y + bx});
                if (rotation == 0) want.push_back({x - bx, y - by});
            }
            std::sort(want.begin(), want.end());
            const auto page = parseFumen(pageWith(s.type, rotation, 4, 10))[0];
            assert(page.piece.piece == s.piece);
            assert(page.piece.rot == coreRot[rotation]);
            assert(cells(page.piece) == want);
        }
    }
}

void workedCommentEscapes() {
    const auto comment = [](const std::string& raw) { return parseFumen(pageWith(5, 2, 4, 0, raw))[0].comment; };
    assert(comment("plain words") == "plain words");
    assert(comment("%u3042") == "\xE3\x81\x82");                // hiragana a
    assert(comment("%E9") == "\xC3\xA9");                       // e acute, as a one-byte escape
    assert(comment("%uD83D%uDE00") == "\xF0\x9F\x98\x80");      // a surrogate pair
    assert(comment("%uD83Dx") == "\xEF\xBF\xBDx");              // a lone surrogate
    assert(comment("100%") == "100%");                          // not an escape
    assert(comment("%zz%u12") == "%zz%u12");
}

void workedErrors() {
    expectError("", "no supported version");
    expectError("vhAAgH", "no supported version");
    expectError("v095@vhAAgH", "no supported version");
    expectError("v115@", "no pages");
    expectError("v115@   ?", "no pages");
    expectError("v115@vhAyO", "ends early");  // published as an illegal short fumen
    expectError("v115@vhA", "ends early");
    expectError("v115@vh!AAgH", "invalid character at data position 2");
    expectError("v115@\xE3\x81\x82", "invalid character");
    expectError("v115@lhTeAgH", "runs past the board on page 0");
    expectError("v115@/dAgH", "invalid field cell on page 0");
    expectError("v115@vhAAgHA8uhAgHwhuhAgH", "invalid field cell on page 2");  // grey, then one past it
    expectError("v115@vhAxSJ", "piece off the board on page 0");
    expectError("v115@vhAAA8", "invalid flags on page 0");
    expectError("v115@vhAAAPBAfBAAA", "invalid comment character");
    expectError("v115@vhAAAPBA/////", "invalid comment");
    expectError("v115@vhAAAPBA", "ends early");
}

}  // namespace

int main() {
    publishedBoards();
    publishedPieces();
    publishedComments();
    publishedWrappers();
    workedLineClear();
    workedRepeats();
    workedAllRotations();
    workedCommentEscapes();
    workedErrors();
}
