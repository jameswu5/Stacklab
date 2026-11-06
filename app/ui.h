// Draws the "Cloud" design (BRIEF.md section 3). All sizes are points on a
// 1440x900 layout, scaled to the current output in native pixels.
#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <string>
#include <vector>

#include "app/settings.h"
#include "core/game.h"

struct Color {
    float r, g, b;
};

struct Palette {
    Color background, card, text, muted, line, knob, boardBackground, checker, good;
    std::array<Color, 9> pieces;  // indexed by core::Piece
    float ghostAlpha;
};

struct PlayView {
    const core::Game* game;
    double seconds;         // the clock
    bool over;              // the piece is hidden and the banner shows
    const char* pace;       // text for the pace card
    bool paceAhead;         // whether the pace shows in the good colour
    const char* bannerTitle;
    const char* bannerHint;  // null shows the title alone, with no card
};

class Ui {
public:
    Ui(SDL_Renderer* renderer, const std::string& fontDir);

    bool dark = false;

    // Every draw starts with beginFrame, which follows the output size.
    void beginFrame();
    void drawPlay(const PlayView& view);
    void drawSettings(const Settings& settings, int selected, bool capturing);
    void drawOverlay(const char* frameLine, const char* inputLine, const char* presentLine);

    // In output pixels.
    bool toggleHit(float x, float y) const;

private:
    enum FontId { FontWord, FontPill, FontLabel, FontTime, FontRow, FontValue, FontPace, FontSmall, kFontCount };
    enum class Align { Left, Center, Right };

    struct Glyph {
        SDL_FRect source{};
        float offset = 0;
        float advance = 0;
    };

    struct Font {
        TTF_Font* ttf = nullptr;
        float size = 0;
        SDL_Texture* atlas = nullptr;
        std::array<Glyph, 128> glyphs;
        std::vector<float> kerning;  // lazily filled, NaN until measured
        int ascent = 0;
    };

    const Palette& palette() const;
    void rebuild(float scale);
    SDL_Texture* cornerMask(int radius);
    void fill(float x, float y, float w, float h, Color color, float radius = 0, float alpha = 1);
    float kern(Font& font, char a, char b);
    float textWidth(FontId id, const char* text, float tracking = 0);
    void text(FontId id, const char* text, float x, float baseline, Color color, Align align = Align::Left, float tracking = 0);
    void label(const char* text, float x, float baseline);
    void card(float x, float y, float w, float h, const char* title);
    void topBar(const char* mode);
    void block(int x, int y, Color color, float alpha);
    void preview(core::Piece piece, float centerX, float centerY);
    void statRow(int index, const char* name, const char* value);

    SDL_Renderer* renderer_;
    std::array<Font, kFontCount> fonts_;
    std::array<SDL_Texture*, 256> corners_{};
    std::array<SDL_Texture*, 2> icons_{};  // sun, moon
    int width_ = 0, height_ = 0;
    float scale_ = 0, originX_ = 0, originY_ = 0;
};
