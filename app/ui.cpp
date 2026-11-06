#include "app/ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

constexpr Color hex(uint32_t rgb) {
    return {((rgb >> 16) & 255) / 255.0f, ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f};
}

// Piece order: None, I, J, L, O, S, T, Z, Garbage.
constexpr Palette kLight = {
    hex(0xFBF6EF), hex(0xFFFFFF), hex(0x3A3440), hex(0x8F8791), hex(0xF1EBE3), hex(0xFFFFFF), hex(0xF9F5EE), hex(0xF4EFE7), hex(0x4FA88F),
    {hex(0), hex(0x7CCFC4), hex(0x86AEE8), hex(0xF4B076), hex(0xF2D06B), hex(0x9BD48A), hex(0xB9A0E6), hex(0xF19A9E), hex(0x8F8791)},
    0.42f,
};
constexpr Palette kDark = {
    hex(0x22212B), hex(0x2C2B37), hex(0xF2EEF2), hex(0x9C98A6), hex(0x383744), hex(0x4B4A5A), hex(0x25242E), hex(0x2A2933), hex(0x93DED3),
    {hex(0), hex(0x93DED3), hex(0xA0C2F0), hex(0xF7C394), hex(0xF6DD8A), hex(0xB0E09F), hex(0xC9B4F0), hex(0xF5AEB1), hex(0x9C98A6)},
    0.30f,
};

// Layout, in points.
constexpr float kLeftX = 266, kBoardCardX = 516, kRightX = 944, kColumnW = 230, kRowY = 68;
constexpr float kBoardX = kBoardCardX + 14, kBoardY = kRowY + 14, kCell = 38;
constexpr float kToggleW = 76, kToggleH = 38, kBarSide = 32;
constexpr float kLabelTracking = 1.1f;
constexpr float kTimeTracking = -1.32f, kPaceTracking = -0.8f;

constexpr const char* kFontFiles[] = {"PlusJakartaSans-Regular.ttf", "PlusJakartaSans-SemiBold.ttf", "PlusJakartaSans-Bold.ttf"};
struct FontSpec {
    int file;
    float size;
};
// Same order as Ui::FontId.
constexpr FontSpec kFontSpecs[] = {{2, 20}, {1, 14}, {1, 12}, {2, 44}, {0, 16}, {1, 15}, {2, 32}, {0, 12}};

float clamp01(float value) { return std::clamp(value, 0.0f, 1.0f); }

SDL_Texture* maskTexture(SDL_Renderer* renderer, SDL_Surface* surface) {
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(surface);
    return texture;
}

float distanceToSegment(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float t = clamp01(((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy));
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}

// Distance to the icon's stroke outline, on a 24 unit grid with a 2 unit stroke.
float sunDistance(float x, float y) {
    constexpr float rays[8][4] = {{12, 2, 12, 4}, {12, 20, 12, 22}, {4.93f, 4.93f, 6.34f, 6.34f}, {17.66f, 17.66f, 19.07f, 19.07f},
                                  {2, 12, 4, 12}, {20, 12, 22, 12}, {6.34f, 17.66f, 4.93f, 19.07f}, {19.07f, 4.93f, 17.66f, 6.34f}};
    float best = std::abs(std::hypot(x - 12, y - 12) - 4);
    for (const auto& ray : rays) best = std::min(best, distanceToSegment(x, y, ray[0], ray[1], ray[2], ray[3]));
    return best - 1;
}

float moonDistance(float x, float y) {
    float outer = std::hypot(x - 12, y - 12) - 9;
    float bite = std::hypot(x - 16.84f, y - 7.16f) - 7;
    return std::abs(std::max(outer, -bite)) - 1;
}

SDL_Texture* iconTexture(SDL_Renderer* renderer, int size, float (*distance)(float, float)) {
    SDL_Surface* surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    float unit = 24.0f / size;
    for (int y = 0; y < size; y++) {
        auto* row = reinterpret_cast<uint8_t*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < size; x++) {
            float coverage = clamp01(0.5f - distance((x + 0.5f) * unit, (y + 0.5f) * unit) / unit);
            uint8_t* pixel = row + x * 4;
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] = static_cast<uint8_t>(coverage * 255 + 0.5f);
        }
    }
    return maskTexture(renderer, surface);
}

}  // namespace

Ui::Ui(SDL_Renderer* renderer, const std::string& fontDir) : renderer_(renderer) {
    for (int id = 0; id < kFontCount; id++) {
        fonts_[id].ttf = TTF_OpenFont((fontDir + kFontFiles[kFontSpecs[id].file]).c_str(), kFontSpecs[id].size);
        if (!fonts_[id].ttf) SDL_Log("Couldn't open font: %s", SDL_GetError());
        fonts_[id].size = kFontSpecs[id].size;
        TTF_SetFontHinting(fonts_[id].ttf, TTF_HINTING_NONE);
    }
}

const Palette& Ui::palette() const { return dark ? kDark : kLight; }

void Ui::beginFrame() {
    SDL_GetCurrentRenderOutputSize(renderer_, &width_, &height_);
    float scale = std::min(width_ / 1440.0f, height_ / 900.0f);
    if (scale != scale_) rebuild(scale);
    originX_ = (width_ - 1440 * scale_) / 2;
    originY_ = (height_ - 900 * scale_) / 2;
    const Color& background = palette().background;
    SDL_SetRenderDrawColorFloat(renderer_, background.r, background.g, background.b, 1);
    SDL_RenderClear(renderer_);
}

// Everything sized to the output is rebuilt here. Corner masks are kept by
// pixel radius, so they carry over and only the new sizes get made.
void Ui::rebuild(float scale) {
    scale_ = scale;
    for (Font& font : fonts_) {
        TTF_SetFontSize(font.ttf, font.size * scale);
        font.ascent = TTF_GetFontAscent(font.ttf);
        font.kerning.assign(128 * 128, std::numeric_limits<float>::quiet_NaN());

        std::array<SDL_Surface*, 128> surfaces{};
        int atlasW = 1, atlasH = 1;
        constexpr SDL_Color white = {255, 255, 255, 255};
        for (int c = 32; c < 127; c++) {
            Uint32 codepoint = c == '-' ? 0x2212 : c;  // a true minus, for pace and empty stats
            int minX = 0, advance = 0;
            TTF_GetGlyphMetrics(font.ttf, codepoint, &minX, nullptr, nullptr, nullptr, &advance);
            font.glyphs[c].advance = static_cast<float>(advance);
            font.glyphs[c].offset = static_cast<float>(std::min(minX, 0));
            surfaces[c] = TTF_RenderGlyph_Blended(font.ttf, codepoint, white);
            if (!surfaces[c]) continue;  // a blank glyph has no bitmap
            atlasW += surfaces[c]->w + 1;
            atlasH = std::max(atlasH, surfaces[c]->h);
        }

        SDL_Surface* atlas = SDL_CreateSurface(atlasW, atlasH, SDL_PIXELFORMAT_ARGB8888);
        int penX = 1;
        for (int c = 32; c < 127; c++) {
            font.glyphs[c].source = {};
            if (!surfaces[c]) continue;
            SDL_SetSurfaceBlendMode(surfaces[c], SDL_BLENDMODE_NONE);
            SDL_Rect place = {penX, 0, surfaces[c]->w, surfaces[c]->h};
            SDL_BlitSurface(surfaces[c], nullptr, atlas, &place);
            font.glyphs[c].source = {static_cast<float>(penX), 0, static_cast<float>(surfaces[c]->w), static_cast<float>(surfaces[c]->h)};
            penX += surfaces[c]->w + 1;
            SDL_DestroySurface(surfaces[c]);
        }
        if (font.atlas) SDL_DestroyTexture(font.atlas);
        font.atlas = maskTexture(renderer_, atlas);
    }
    int iconSize = static_cast<int>(std::lround(16 * scale));
    for (SDL_Texture* icon : icons_)
        if (icon) SDL_DestroyTexture(icon);
    icons_ = {iconTexture(renderer_, iconSize, sunDistance), iconTexture(renderer_, iconSize, moonDistance)};
}

// A white rounded square, antialiased, for stretching with a nine-grid.
SDL_Texture* Ui::cornerMask(int radius) {
    radius = std::min(radius, static_cast<int>(corners_.size()) - 1);
    if (corners_[radius]) return corners_[radius];
    int size = 2 * radius + 1;
    SDL_Surface* surface = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < size; y++) {
        auto* row = reinterpret_cast<uint8_t*>(surface->pixels) + y * surface->pitch;
        float dy = std::max(radius - (std::min(y, 2 * radius - y) + 0.5f), 0.0f);
        for (int x = 0; x < size; x++) {
            float dx = std::max(radius - (std::min(x, 2 * radius - x) + 0.5f), 0.0f);
            float coverage = clamp01(radius - std::hypot(dx, dy) + 0.5f);
            uint8_t* pixel = row + x * 4;
            pixel[0] = pixel[1] = pixel[2] = 255;
            pixel[3] = static_cast<uint8_t>(coverage * 255 + 0.5f);
        }
    }
    return corners_[radius] = maskTexture(renderer_, surface);
}

void Ui::fill(float x, float y, float w, float h, Color color, float radius, float alpha) {
    // Snapped to whole pixels, so edges stay sharp at any scale.
    float left = std::round(originX_ + x * scale_), right = std::round(originX_ + (x + w) * scale_);
    float top = std::round(originY_ + y * scale_), bottom = std::round(originY_ + (y + h) * scale_);
    SDL_FRect rect = {left, top, right - left, bottom - top};
    int pixels = std::min({static_cast<int>(std::lround(radius * scale_)), static_cast<int>(rect.w) / 2, static_cast<int>(rect.h) / 2});
    if (pixels <= 0) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColorFloat(renderer_, color.r, color.g, color.b, alpha);
        SDL_RenderFillRect(renderer_, &rect);
        return;
    }
    SDL_Texture* mask = cornerMask(pixels);
    SDL_SetTextureColorModFloat(mask, color.r, color.g, color.b);
    SDL_SetTextureAlphaModFloat(mask, alpha);
    float corner = static_cast<float>(pixels);
    SDL_RenderTexture9Grid(renderer_, mask, nullptr, corner, corner, corner, corner, 1, &rect);
}

// SDL_ttf's kerning call only reads the legacy kern table and this font's
// kerning is in GPOS, so pairs are measured through the shaper. Letters only:
// numbers are tabular, and this keeps measuring out of the play loop.
float Ui::kern(Font& font, char a, char b) {
    if (!std::isalpha(static_cast<unsigned char>(a)) || !std::isalpha(static_cast<unsigned char>(b))) return 0;
    float& value = font.kerning[a * 128 + b];
    if (std::isnan(value)) {
        char pair[] = {a, b};
        int width = 0;
        TTF_GetStringSize(font.ttf, pair, 2, &width, nullptr);
        value = width - font.glyphs[a].advance - font.glyphs[b].advance;
    }
    return value;
}

float Ui::textWidth(FontId id, const char* string, float tracking) {
    Font& font = fonts_[id];
    float width = 0;
    for (const char* c = string; *c; c++) {
        if (*c & 0x80) continue;
        width += font.glyphs[*c].advance + tracking * scale_;
        if (c[1] && !(c[1] & 0x80)) width += kern(font, c[0], c[1]);
    }
    return width;
}

void Ui::text(FontId id, const char* string, float x, float baseline, Color color, Align align, float tracking) {
    Font& font = fonts_[id];
    float pen = originX_ + x * scale_;
    if (align != Align::Left) {
        float width = textWidth(id, string, tracking);
        pen -= align == Align::Center ? width / 2 : width;
    }
    float top = std::round(originY_ + baseline * scale_) - font.ascent;
    SDL_SetTextureColorModFloat(font.atlas, color.r, color.g, color.b);
    for (const char* c = string; *c; c++) {
        if (*c & 0x80) continue;
        const Glyph& glyph = font.glyphs[*c];
        if (glyph.source.w > 0) {
            SDL_FRect dest = {std::round(pen + glyph.offset), top, glyph.source.w, glyph.source.h};
            SDL_RenderTexture(renderer_, font.atlas, &glyph.source, &dest);
        }
        pen += glyph.advance + tracking * scale_;
        if (c[1] && !(c[1] & 0x80)) pen += kern(font, c[0], c[1]);
    }
}

void Ui::label(const char* string, float x, float baseline) {
    text(FontLabel, string, x, baseline, palette().muted, Align::Left, kLabelTracking);
}

void Ui::card(float x, float y, float w, float h, const char* title) {
    fill(x, y, w, h, palette().card, 24);
    label(title, x + 20, y + 32);
}

void Ui::topBar(const char* mode) {
    const Palette& colors = palette();
    float left = -originX_ / scale_, right = (width_ - originX_) / scale_, top = -originY_ / scale_;
    text(FontWord, "Stacklab", left + kBarSide, top + 43, colors.text);

    float pillW = textWidth(FontPill, mode) / scale_ + 36;
    float center = (left + right) / 2;
    fill(center - pillW / 2, top + 17, pillW, 34, colors.card, 17);
    text(FontPill, mode, center, top + 39.5f, colors.text, Align::Center);

    float toggleX = right - kBarSide - kToggleW, toggleY = top + 15;
    fill(toggleX, toggleY, kToggleW, kToggleH, colors.line, kToggleH / 2);
    fill(toggleX + (dark ? 42 : 4), toggleY + 4, 30, 30, colors.knob, 15);
    for (int i = 0; i < 2; i++) {
        bool active = (i == 1) == dark;
        Color color = active ? colors.text : colors.muted;
        SDL_SetTextureColorModFloat(icons_[i], color.r, color.g, color.b);
        float iconX = std::round(originX_ + (toggleX + (i ? 57 : 19)) * scale_), iconY = std::round(originY_ + (toggleY + 19) * scale_);
        float size = static_cast<float>(icons_[i]->w);
        SDL_FRect dest = {iconX - std::floor(size / 2), iconY - std::floor(size / 2), size, size};
        SDL_RenderTexture(renderer_, icons_[i], nullptr, &dest);
    }
}

bool Ui::toggleHit(float x, float y) const {
    return x >= width_ - (kBarSide + kToggleW) * scale_ && x <= width_ - kBarSide * scale_ && y >= 15 * scale_ && y <= (15 + kToggleH) * scale_;
}

void Ui::block(int x, int y, Color color, float alpha) {
    fill(kBoardX + x * kCell + 1, kBoardY + (core::kVisibleHeight - 1 - y) * kCell + 1, kCell - 2, kCell - 2, color, 7, alpha);
}

void Ui::preview(core::Piece piece, float centerX, float centerY) {
    if (piece == core::Piece::None) return;
    constexpr float cell = 24;
    core::Falling shape;
    shape.piece = piece;
    auto cells = shape.cells();
    int minX = cells[0].x, maxX = minX, minY = cells[0].y, maxY = minY;
    for (const core::Cell& c : cells) {
        minX = std::min(minX, c.x), maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y), maxY = std::max(maxY, c.y);
    }
    float left = centerX - (maxX - minX + 1) * cell / 2, top = centerY - (maxY - minY + 1) * cell / 2;
    for (const core::Cell& c : cells)
        fill(left + (c.x - minX) * cell + 1, top + (maxY - c.y) * cell + 1, cell - 2, cell - 2, palette().pieces[static_cast<int>(piece)], 7 * cell / kCell);
}

void Ui::statRow(int index, const char* name, const char* value) {
    const Palette& colors = palette();
    float top = 326 + index * 38.0f;
    fill(kLeftX + 20, top, kColumnW - 40, 1, colors.line);
    text(FontRow, name, kLeftX + 20, top + 26, colors.muted);
    text(FontValue, value, kLeftX + kColumnW - 20, top + 26, colors.text, Align::Right);
}

void Ui::drawPlay(const PlayView& view) {
    const Palette& colors = palette();
    const core::Game& game = *view.game;
    topBar("40 Lines");

    card(kLeftX, kRowY, kColumnW, 127, "HOLD");
    preview(game.hold, kLeftX + kColumnW / 2, kRowY + 77);

    card(kLeftX, 215, kColumnW, 321, "TIME");
    char buffer[32];
    int hundredths = static_cast<int>(std::lround(game.seconds() * 100));
    std::snprintf(buffer, sizeof buffer, "%d:%02d.%02d", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
    text(FontTime, buffer, kLeftX + 20, 303, colors.text, Align::Left, kTimeTracking);
    std::snprintf(buffer, sizeof buffer, "%d / 40", game.stats.lines);
    statRow(0, "Lines", buffer);
    std::snprintf(buffer, sizeof buffer, "%d", game.stats.pieces);
    statRow(1, "Pieces", buffer);
    std::snprintf(buffer, sizeof buffer, "%.2f", game.seconds() > 0 ? game.stats.pieces / game.seconds() : 0.0);
    statRow(2, "PPS", buffer);
    std::snprintf(buffer, sizeof buffer, "%.2f", game.stats.pieces > 0 ? static_cast<double>(game.stats.keys) / game.stats.pieces : 0.0);
    statRow(3, "KPP", buffer);
    statRow(4, "Finesse", "-");

    fill(kBoardCardX, kRowY, 408, 788, colors.card, 28);
    fill(kBoardX, kBoardY, 380, 760, colors.boardBackground, 8);
    constexpr float cornerSquare = 8;
    for (int y = 0; y < core::kVisibleHeight; y++) {
        for (int x = (y + 1) % 2; x < core::kBoardWidth; x += 2) {
            float cellX = kBoardX + x * kCell, cellY = kBoardY + y * kCell;
            bool topRight = x == core::kBoardWidth - 1 && y == 0;
            bool bottomLeft = x == 0 && y == core::kVisibleHeight - 1;
            if (!topRight && !bottomLeft) {
                fill(cellX, cellY, kCell, kCell, colors.checker);
                continue;
            }
            // Only the corner on the board's edge is rounded.
            fill(cellX, cellY, kCell, kCell, colors.checker, cornerSquare);
            fill(topRight ? cellX : cellX + kCell - cornerSquare, topRight ? cellY + kCell - cornerSquare : cellY, cornerSquare, cornerSquare, colors.checker);
            fill(topRight ? cellX : cellX + kCell - cornerSquare, topRight ? cellY : cellY + kCell - cornerSquare, cornerSquare, cornerSquare, colors.checker);
            fill(topRight ? cellX + kCell - cornerSquare : cellX, topRight ? cellY + kCell - cornerSquare : cellY, cornerSquare, cornerSquare, colors.checker);
        }
    }
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (game.board.at(x, y) != core::Piece::None) block(x, y, colors.pieces[static_cast<int>(game.board.at(x, y))], 1);
    if (!view.over && game.falling.piece != core::Piece::None) {
        Color color = colors.pieces[static_cast<int>(game.falling.piece)];
        for (const core::Cell& c : game.ghost().cells()) block(c.x, c.y, color, colors.ghostAlpha);
        for (const core::Cell& c : game.falling.cells()) block(c.x, c.y, color, 1);
    }

    card(kRightX, kRowY, kColumnW, 383, "NEXT");
    // Fixed slots, so the queue doesn't jump as flat I pieces pass through it.
    for (int i = 0; i < 5 && i < static_cast<int>(game.queue.size()); i++) preview(game.queue[i], kRightX + kColumnW / 2, kRowY + 77 + 64.0f * i);

    card(kRightX, 471, kColumnW, 101, "PACE VS BEST");
    text(FontPace, view.pace, kRightX + 20, 545, view.paceAhead ? colors.good : colors.text, Align::Left, kPaceTracking);

    if (view.bannerTitle) {
        float centerX = kBoardX + 190, centerY = kBoardY + 380;
        fill(centerX - 130, centerY - 56, 260, 112, colors.card, 24);
        text(FontTime, view.bannerTitle, centerX, centerY + 6, colors.text, Align::Center);
        text(FontRow, view.bannerHint, centerX, centerY + 34, colors.muted, Align::Center);
    }
}

void Ui::drawSettings(const Settings& settings, int selected, bool capturing) {
    const Palette& colors = palette();
    topBar("Settings");
    constexpr float width = 560, rowH = 38, x = (1440 - width) / 2, firstRow = kRowY + 20;
    fill(x, kRowY, width, 788, colors.card, 24);
    for (int row = 0; row < kRowCount; row++) {
        float y = firstRow + row * rowH;
        if (row == selected) fill(x + 12, y, width - 24, rowH, colors.line, 12);
        text(FontRow, rowLabel(row).c_str(), x + 24, y + 24.5f, colors.muted);
        std::string value = capturing && row == selected ? "Press a key" : rowValue(settings, row);
        text(FontValue, value.c_str(), x + width - 24, y + 24.5f, colors.text, Align::Right);
    }
    const char* hint = selected >= RowBindFirst && selected < RowVsync ? "Enter adds a key, Backspace clears, Esc goes back"
                       : selected == RowProfile                       ? "Left and right switch, Enter copies to a new profile, Esc goes back"
                                                                      : "Left and right change, Esc goes back";
    text(FontSmall, hint, 720, kRowY + 788 - 32, colors.muted, Align::Center);
}

void Ui::drawOverlay(const char* frameLine, const char* inputLine, const char* presentLine) {
    float x = -originX_ / scale_ + kBarSide, bottom = (height_ - originY_) / scale_ - 16;
    text(FontSmall, frameLine, x, bottom - 36, palette().muted);
    text(FontSmall, inputLine, x, bottom - 18, palette().muted);
    text(FontSmall, presentLine, x, bottom, palette().muted);
}
