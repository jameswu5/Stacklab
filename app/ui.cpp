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
    hex(0xFBF6EF), hex(0xFFFFFF), hex(0x3A3440), hex(0x8F8791), hex(0xF1EBE3), hex(0xFFFFFF), hex(0xF9F5EE), hex(0xF4EFE7),
    {hex(0), hex(0x00AFB3), hex(0x2B6BE8), hex(0xF27A17), hex(0xF0B400), hex(0x2FB344), hex(0x8F45DE), hex(0xE5344B), hex(0x8F8791)},
    0.42f,
    0.20f,
};
constexpr Palette kDark = {
    hex(0x22212B), hex(0x2C2B37), hex(0xF2EEF2), hex(0x9C98A6), hex(0x383744), hex(0x4B4A5A), hex(0x25242E), hex(0x2A2933),
    {hex(0), hex(0x2BD9D9), hex(0x5A9BFF), hex(0xFF8C2B), hex(0xFFD52E), hex(0x4EDB4A), hex(0xC07EFF), hex(0xFF5F70), hex(0x9C98A6)},
    0.30f,
    0.16f,
};

// Layout, in points.
constexpr float kLeftX = 266, kBoardCardX = 516, kRightX = 944, kColumnW = 230, kRowY = 68;
constexpr float kBoardX = kBoardCardX + 14, kBoardY = kRowY + 14, kCell = 38;
constexpr float kToggleW = 76, kToggleH = 38, kBarSide = 32;
constexpr float kLabelTracking = 1.1f;
constexpr float kTimeTracking = -1.32f;
constexpr float kHoldCell = 24, kNextCell = 44, kNextSlot = 144;

constexpr const char* kFontFiles[] = {"PlusJakartaSans-Regular.ttf", "PlusJakartaSans-SemiBold.ttf", "PlusJakartaSans-Bold.ttf"};
struct FontSpec {
    int file;
    float size;
    bool digitsOnly = false;  // for a size whose full atlas could outgrow a texture
};
// Same order as Ui::FontId.
constexpr FontSpec kFontSpecs[] = {{2, 20}, {1, 14}, {1, 12}, {2, 44}, {0, 16}, {1, 15}, {0, 12}, {2, 140, true}};

void formatTime(char (&buffer)[32], double seconds) {
    int hundredths = static_cast<int>(std::lround(seconds * 100));
    std::snprintf(buffer, sizeof buffer, "%d:%02d.%02d", hundredths / 6000, hundredths / 100 % 60, hundredths % 100);
}

// What a mode ranks runs by, in `value`: a score if it's timed, otherwise a time.
void formatRank(char (&buffer)[32], const Mode& mode, double value) {
    if (mode.bestIsScore()) std::snprintf(buffer, sizeof buffer, "%d", static_cast<int>(value));
    else formatTime(buffer, value);
}

double rankOf(const RunResult& run) { return run.mode->bestIsScore() ? run.stats.score : run.seconds; }

constexpr struct {
    Stat stat;
    const char* name;
} kStatRows[] = {{StatScore, "Score"}, {StatLevel, "Level"}, {StatGarbage, "Garbage"}, {StatLines, "Lines"}, {StatPieces, "Pieces"}, {StatPPS, "PPS"}, {StatKPP, "KPP"}, {StatAPM, "APM"}, {StatAPP, "APP"}, {StatFinesse, "Finesse"}};

void formatStat(char (&buffer)[32], Stat stat, const core::Stats& stats, double seconds, int goalLines, const ai::FaultStats* faults = nullptr) {
    double perPiece = stats.pieces > 0 ? 1.0 / stats.pieces : 0;
    double perSecond = seconds > 0 ? 1 / seconds : 0;
    switch (stat) {
        case StatLines:
            if (goalLines > 0) std::snprintf(buffer, sizeof buffer, "%d / %d", std::min(stats.lines, goalLines), goalLines);
            else std::snprintf(buffer, sizeof buffer, "%d", stats.lines);
            break;
        case StatScore: std::snprintf(buffer, sizeof buffer, "%d", stats.score); break;
        case StatLevel: std::snprintf(buffer, sizeof buffer, "%d", stats.level); break;
        case StatGarbage: std::snprintf(buffer, sizeof buffer, "%d / %d", std::min(stats.garbageCleared, goalLines), goalLines); break;
        case StatPieces: std::snprintf(buffer, sizeof buffer, "%d", stats.pieces); break;
        case StatPPS: std::snprintf(buffer, sizeof buffer, "%.2f", stats.pieces * perSecond); break;
        case StatKPP: std::snprintf(buffer, sizeof buffer, "%.2f", stats.keys * perPiece); break;
        case StatAPM: std::snprintf(buffer, sizeof buffer, "%.1f", stats.attack * perSecond * 60); break;
        case StatAPP: std::snprintf(buffer, sizeof buffer, "%.2f", stats.attack * perPiece); break;
        case StatFinesse:
            if (faults) std::snprintf(buffer, sizeof buffer, "%d", faults->total);
            else std::snprintf(buffer, sizeof buffer, "-");
            break;
    }
}

// The piece and orientation to work on, such as "T CCW x3".
void formatWorstFaults(char (&buffer)[32], const ai::FaultStats& faults) {
    constexpr const char* kPieceNames[] = {"", "I", "J", "L", "O", "S", "T", "Z"};
    constexpr const char* kRotations[] = {"spawn", "CW", "180", "CCW"};
    int worst = 0, piece = 0, rot = 0;
    for (int p = 1; p < 8; p++)
        for (int r = 0; r < 4; r++)
            if (faults.byPiece[p][r] > worst) worst = faults.byPiece[p][r], piece = p, rot = r;
    if (worst == 0) std::snprintf(buffer, sizeof buffer, "-");
    else std::snprintf(buffer, sizeof buffer, "%s %s x%d", kPieceNames[piece], kRotations[rot], worst);
}

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
        fonts_[id].digitsOnly = kFontSpecs[id].digitsOnly;
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
            if (font.digitsOnly && !std::isdigit(c)) continue;
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

    // Beside the wordmark, since pieces spawn above the board in the centre.
    float pillX = left + kBarSide + textWidth(FontWord, "Stacklab") / scale_ + 16;
    float pillW = textWidth(FontPill, mode) / scale_ + 36;
    fill(pillX, top + 17, pillW, 34, colors.card, 17);
    text(FontPill, mode, pillX + pillW / 2, top + 39.5f, colors.text, Align::Center);

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

bool Ui::boardCellAt(float x, float y, int& cellX, int& cellY) const {
    float pointX = (x - originX_) / scale_ - kBoardX, pointY = (y - originY_) / scale_ - kBoardY;
    if (pointX < 0 || pointY < 0 || pointX >= core::kBoardWidth * kCell || pointY >= core::kVisibleHeight * kCell) return false;
    cellX = static_cast<int>(pointX / kCell);
    cellY = core::kVisibleHeight - 1 - static_cast<int>(pointY / kCell);
    return true;
}

int Ui::queueSlotAt(float x, float y) const {
    float pointX = (x - originX_) / scale_ - kRightX, pointY = (y - originY_) / scale_ - (kRowY + 120 - kNextSlot / 2);
    if (pointX < 0 || pointX >= kColumnW || pointY < 0 || pointY >= 5 * kNextSlot) return -1;
    return static_cast<int>(pointY / kNextSlot);
}

bool Ui::holdHit(float x, float y) const {
    float pointX = (x - originX_) / scale_ - kLeftX, pointY = (y - originY_) / scale_ - kRowY;
    return pointX >= 0 && pointX < kColumnW && pointY >= 0 && pointY < 127;
}

void Ui::block(int x, int y, Color color, float alpha) {
    fill(kBoardX + x * kCell + 1, kBoardY + (core::kVisibleHeight - 1 - y) * kCell + 1, kCell - 2, kCell - 2, color, 7, alpha);
}

void Ui::preview(core::Piece piece, float centerX, float centerY, float cell) {
    if (piece == core::Piece::None) return;
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

void Ui::statRow(float x, float y, float w, const char* name, const char* value) {
    const Palette& colors = palette();
    fill(x, y, w, 1, colors.line);
    text(FontRow, name, x, y + 26, colors.muted);
    text(FontValue, value, x + w, y + 26, colors.text, Align::Right);
}

// Over the board, so retrying from here is the same key on the same screen.
void Ui::results(const PlayView& view) {
    const Palette& colors = palette();
    const RunResult& run = view.history.back();
    const bool finesse = (view.mode->stats & StatFinesse) && view.faults;
    constexpr float width = 300, x = kBoardX + 190 - width / 2;
    const float height = view.pc ? 562 : finesse ? 422 : 346, top = kBoardY + 380 - height / 2;
    fill(x, top, width, height, colors.card, 24);
    const char* title = run.toppedOut ? "Topped out" : run.newBest ? "New best" : "Finished";
    text(FontWord, title, x + width / 2, top + 42, colors.text, Align::Center);
    char buffer[32];
    formatRank(buffer, *run.mode, rankOf(run));
    text(FontTime, buffer, x + width / 2, top + 94, colors.text, Align::Center, kTimeTracking);

    float y = top + 114;
    for (const auto& row : kStatRows) {
        if (!(row.stat & (StatPPS | StatKPP | StatAPM | StatAPP))) continue;
        formatStat(buffer, row.stat, run.stats, run.seconds, view.mode->goalLines);
        statRow(x + 24, y, width - 48, row.name, buffer);
        y += 38;
    }
    if (finesse) {
        formatStat(buffer, StatFinesse, run.stats, run.seconds, 0, view.faults);
        statRow(x + 24, y, width - 48, "Finesse", buffer);
        y += 38;
        formatWorstFaults(buffer, *view.faults);
        statRow(x + 24, y, width - 48, "Most faults", buffer);
        y += 38;
    }
    if (view.pc) {
        std::snprintf(buffer, sizeof buffer, "%d / %d", view.pc->pcs, kPcGoal);
        statRow(x + 24, y, width - 48, "Perfect clears", buffer);
        y += 38;
        std::snprintf(buffer, sizeof buffer, "%d", view.pc->bestPcs);
        statRow(x + 24, y, width - 48, "Best", buffer);
        y += 38;
        fill(x + 24, y, width - 48, 1, colors.line);
        text(FontSmall, "By bag phase, over all runs", x + 24, y + 22, colors.muted);
        for (int phase = 0; phase < 7; phase++) {
            std::snprintf(buffer, sizeof buffer, "%d: %d of %d", phase, view.pc->tally.successes[phase], view.pc->tally.attempts[phase]);
            text(FontSmall, buffer, x + 24 + (phase % 2) * 126, y + 44 + (phase / 2) * 20.0f, colors.text);
        }
    } else {
        formatRank(buffer, *view.mode, view.best);
        statRow(x + 24, y, width - 48, "Best", view.best > 0 ? buffer : "-");
    }
    text(FontSmall, view.hint, x + width / 2, top + height - 16, colors.muted, Align::Center);
}

// Newest first, for this mode. Takes the queue's place once a run has ended.
void Ui::sessionCard(const PlayView& view) {
    const Palette& colors = palette();
    card(kRightX, kRowY, kColumnW, 788, "SESSION");
    constexpr int maxRows = 18;
    int row = 0;
    for (auto run = view.history.rbegin(); run != view.history.rend() && row < maxRows; ++run) {
        if (run->mode != view.mode) continue;
        float y = kRowY + 52 + row++ * 38.0f;
        char time[32], value[32], pps[40];
        if (run->newBest) fill(kRightX + 8, y, kColumnW - 16, 38, colors.line, 12);
        formatRank(time, *run->mode, rankOf(*run));
        formatStat(value, StatPPS, run->stats, run->seconds, 0);
        std::snprintf(pps, sizeof pps, "%s PPS", value);
        text(FontValue, run->toppedOut ? "Topped out" : time, kRightX + 20, y + 25, run->toppedOut ? colors.muted : colors.text);
        text(FontSmall, run->newBest ? "New best" : pps, kRightX + kColumnW - 20, y + 25, colors.muted, Align::Right);
    }
}

void Ui::drawMenu(int selected, std::span<const double> bests) {
    const Palette& colors = palette();
    topBar("Menu");
    constexpr float width = 560, rowH = 76, x = (1440 - width) / 2;
    float height = 12 + (kModeCount + 1) * rowH + 44;
    float top = kRowY + (788 - height) / 2;
    fill(x, top, width, height, colors.card, 24);
    for (int row = 0; row <= kModeCount; row++) {
        float y = top + 12 + row * rowH;
        if (row == selected) fill(x + 12, y, width - 24, rowH, colors.line, 16);
        bool isMode = row < kModeCount;
        text(FontWord, isMode ? kModes[row].name : "Settings", x + 32, y + 34, colors.text);
        text(FontRow, isMode ? kModes[row].blurb : "Handling, keys and display", x + 32, y + 58, colors.muted);
        if (isMode && bests[row] > 0) {
            char buffer[32];
            formatRank(buffer, kModes[row], bests[row]);
            text(FontLabel, "BEST", x + width - 32, y + 31, colors.muted, Align::Right, kLabelTracking);
            text(FontValue, buffer, x + width - 32, y + 54, colors.text, Align::Right);
        }
    }
    text(FontSmall, "Up and down choose, Enter opens", 720, top + height - 17, colors.muted, Align::Center);
}

void Ui::drawDrills(int selected, std::span<const Drill> drills, const char* error) {
    const Palette& colors = palette();
    topBar("Drills");
    constexpr float width = 560, rowH = 76, x = (1440 - width) / 2;
    constexpr int maxRows = 9;
    const int count = static_cast<int>(drills.size()), rows = std::clamp(count, 1, maxRows);
    float height = 12 + rows * rowH + 44;
    float top = kRowY + (788 - height) / 2;
    fill(x, top, width, height, colors.card, 24);
    if (drills.empty()) text(FontRow, "No drills found", 720, top + 12 + 44, colors.muted, Align::Center);
    const int first = std::clamp(selected - maxRows + 1, 0, std::max(count - maxRows, 0));
    for (int row = first; row < count && row < first + maxRows; row++) {
        float y = top + 12 + (row - first) * rowH;
        if (row == selected) fill(x + 12, y, width - 24, rowH, colors.line, 16);
        text(FontWord, drills[row].name.c_str(), x + 32, y + 34, colors.text);
        text(FontRow, drills[row].blurb.c_str(), x + 32, y + 58, colors.muted);
    }
    text(FontSmall, error ? error : "Up and down choose, Enter starts, Esc goes back", 720, top + height - 17, colors.muted, Align::Center);
}

// Always the same lines, so the card doesn't jump as the answers change.
void Ui::coachRows(const PcView& pc, float y, bool over) {
    const Palette& colors = palette();
    const float x = kLeftX + 20, width = kColumnW - 40;
    if (!over) {
        fill(x, y, width, 1, colors.line);
        text(FontValue, pc.status, x, y + 24, colors.text);
        text(FontSmall, pc.detail, x, y + 42, colors.muted);
        if (pc.alert) {
            fill(kLeftX + 10, y + 49, kColumnW - 20, 22, colors.pieces[static_cast<int>(core::Piece::Z)], 8, 0.22f);
            text(FontSmall, pc.alert, x, y + 64, colors.text);
        }
        text(FontSmall, pc.hintLine, x, y + 86, colors.muted);
        y += 98;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%d / %d", pc.pcs, kPcGoal);
    statRow(x, y, width, "Perfect clears", buffer);
    y += 38;
    std::snprintf(buffer, sizeof buffer, "%d", pc.streak);
    statRow(x, y, width, "Streak", buffer);
    y += 38;
    if (pc.outOfReach) {
        std::snprintf(buffer, sizeof buffer, "%d is out of reach this run", kPcGoal);
        text(FontSmall, buffer, x, y + 18, colors.muted);
    }
}

void Ui::drawPlay(const PlayView& view) {
    const Palette& colors = palette();
    const core::Game& game = *view.game;
    topBar(view.drill ? view.drill->name.c_str() : view.mode->name);

    card(kLeftX, kRowY, kColumnW, 127, "HOLD");
    preview(game.hold, kLeftX + kColumnW / 2, kRowY + 77, kHoldCell);

    int statCount = 0;
    for (const auto& row : kStatRows) statCount += (view.mode->stats & row.stat) != 0;
    const float coachHeight = !view.pc ? 0 : view.over ? 106 : 204;
    card(kLeftX, 215, kColumnW, 131 + statCount * 38.0f + coachHeight, "TIME");
    char buffer[32];
    formatTime(buffer, view.seconds);
    text(FontTime, buffer, kLeftX + 20, 303, colors.text, Align::Left, kTimeTracking);
    float rowY = 326;
    for (const auto& row : kStatRows) {
        if (!(view.mode->stats & row.stat)) continue;
        formatStat(buffer, row.stat, game.stats, view.seconds, view.mode->goalLines, view.faults);
        // Stays until the next piece locks. Beside the board, so it can't hide the stack.
        if (row.stat == StatFinesse && view.faults && view.faults->last > 0 && !view.over) {
            fill(kLeftX + 10, rowY + 2, kColumnW - 20, 36, colors.pieces[static_cast<int>(core::Piece::Z)], 10, 0.22f);
            std::snprintf(buffer, sizeof buffer, "%d (+%d)", view.faults->total, view.faults->last);
        }
        statRow(kLeftX + 20, rowY, kColumnW - 40, row.name, buffer);
        rowY += 38;
    }
    if (view.pc) coachRows(*view.pc, rowY, view.over);

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
    // Lines left, under everything that's played, so it can never hide a block.
    if (view.mode->goalLines > 0 && view.mode->garbageRows == 0 && !view.over) {
        std::snprintf(buffer, sizeof buffer, "%d", std::max(view.mode->goalLines - game.stats.lines, 0));
        SDL_SetTextureAlphaModFloat(fonts_[FontLinesLeft].atlas, colors.linesLeftAlpha);
        text(FontLinesLeft, buffer, kBoardX + 190, kBoardY + 138, colors.muted, Align::Center);
    }
    for (int y = 0; y < core::kBoardHeight; y++)
        for (int x = 0; x < core::kBoardWidth; x++)
            if (game.board.at(x, y) != core::Piece::None) block(x, y, colors.pieces[static_cast<int>(game.board.at(x, y))], 1);
    if (view.drill && !view.over) {
        for (size_t i = 0; i < view.drill->targets.size(); i++) {
            if (view.filled >> i & 1) continue;
            const DrillTarget& target = view.drill->targets[i];
            // Fainter than the ghost, so the two can't be confused.
            for (const core::Cell& c : target.cells) block(c.x, c.y, colors.pieces[static_cast<int>(target.piece)], colors.ghostAlpha * 0.5f);
        }
    }
    if (view.pc && view.pc->showHint) {
        for (const core::Cell& c : view.pc->hintCells) block(c.x, c.y, colors.pieces[static_cast<int>(view.pc->hintPiece)], colors.ghostAlpha);
    }
    if (!view.over && game.falling.piece != core::Piece::None) {
        Color color = colors.pieces[static_cast<int>(game.falling.piece)];
        for (const core::Cell& c : game.ghost().cells()) block(c.x, c.y, color, colors.ghostAlpha);
        for (const core::Cell& c : game.falling.cells()) block(c.x, c.y, color, 1);
    }

    if (view.over) {
        sessionCard(view);
        results(view);
        return;
    }

    // Large, because the owner reads the queue in peripheral vision.
    card(kRightX, kRowY, kColumnW, 788, "NEXT");
    // Fixed slots, so the queue doesn't jump as flat I pieces pass through it.
    for (int i = 0; i < 5 && i < static_cast<int>(game.queue.size()); i++) preview(game.queue[i], kRightX + kColumnW / 2, kRowY + 120 + kNextSlot * i, kNextCell);

    if (view.banner) text(FontTime, view.banner, kBoardX + 190, kBoardY + 396, colors.text, Align::Center);
}

void Ui::drawSettings(const Settings& settings, int selected, bool capturing) {
    const Palette& colors = palette();
    topBar("Settings");
    constexpr float width = 560, rowH = 31, x = (1440 - width) / 2, firstRow = kRowY + 20;
    fill(x, kRowY, width, 788, colors.card, 24);
    for (int row = 0; row < kRowCount; row++) {
        float y = firstRow + row * rowH;
        if (row == selected) fill(x + 12, y, width - 24, rowH, colors.line, 12);
        text(FontRow, rowLabel(row).c_str(), x + 24, y + 21, colors.muted);
        std::string value = capturing && row == selected ? "Press a key" : rowValue(settings, row);
        text(FontValue, value.c_str(), x + width - 24, y + 21, colors.text, Align::Right);
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
