// Draws the "Cloud" design (BRIEF.md section 3). All sizes are points on a
// 1440x900 layout, scaled to the current output in native pixels.
#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <array>
#include <span>
#include <string>
#include <vector>

#include "ai/finesse.h"
#include "app/pc_coach.h"
#include "app/settings.h"
#include "core/game.h"
#include "modes/drill.h"
#include "modes/modes.h"

struct Color {
    float r, g, b;
};

struct Palette {
    Color background, card, text, muted, line, knob, boardBackground, checker;
    std::array<Color, 9> pieces;  // indexed by core::Piece
    float ghostAlpha;
    float linesLeftAlpha;
};

// One ended run, for the session history.
struct RunResult {
    const Mode* mode;
    double seconds;
    core::Stats stats;
    bool toppedOut;
    bool newBest;
};

struct PlayView {
    const Mode* mode;
    const core::Game* game;
    double seconds;  // the clock
    bool over;       // the piece is hidden, and results replace the banner and queue
    const char* banner;  // the countdown's word, or null
    const char* hint;    // under the results
    double best;
    std::span<const RunResult> history;  // this session's ended runs, oldest first; when over, the last is this run
    const Drill* drill = nullptr;  // names the run, and its targets are drawn
    uint64_t filled = 0;           // a bit per target already placed
    const ai::FaultStats* faults = nullptr;  // null if the mode can't judge finesse
    const PcView* pc = nullptr;              // null without the PC coach
};

class Ui {
public:
    Ui(SDL_Renderer* renderer, const std::string& fontDir);

    bool dark = false;

    // Every draw starts with beginFrame, which follows the output size.
    void beginFrame();
    void drawMenu(int selected, std::span<const double> bests, int residue);  // one row per mode, then Settings
    void drawDrills(int selected, std::span<const Drill> drills, const char* error);  // error is null or a file that couldn't be used
    void drawPlay(const PlayView& view);
    void drawSettings(const Settings& settings, int selected, bool capturing);
    void drawOverlay(const char* frameLine, const char* inputLine, const char* presentLine);

    // In output pixels.
    bool toggleHit(float x, float y) const;
    bool boardCellAt(float x, float y, int& cellX, int& cellY) const;  // false outside the board
    int queueSlotAt(float x, float y) const;                           // -1 outside the queue
    bool holdHit(float x, float y) const;

private:
    enum FontId { FontWord, FontPill, FontLabel, FontTime, FontRow, FontValue, FontSmall, FontLinesLeft, kFontCount };
    enum class Align { Left, Center, Right };

    struct Glyph {
        SDL_FRect source{};
        float offset = 0;
        float advance = 0;
    };

    struct Font {
        TTF_Font* ttf = nullptr;
        float size = 0;
        bool digitsOnly = false;
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
    void preview(core::Piece piece, float centerX, float centerY, float cell);
    void statRow(float x, float y, float w, const char* name, const char* value);
    void coachRows(const PcView& pc, float y, bool over);
    void results(const PlayView& view);
    void sessionCard(const PlayView& view);

    SDL_Renderer* renderer_;
    std::array<Font, kFontCount> fonts_;
    std::array<SDL_Texture*, 256> corners_{};
    std::array<SDL_Texture*, 2> icons_{};  // sun, moon
    int width_ = 0, height_ = 0;
    float scale_ = 0, originX_ = 0, originY_ = 0;
};
