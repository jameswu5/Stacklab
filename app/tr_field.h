// Versus opponents that play like a TETRA LEAGUE player at a chosen TR. data/bot/tr-field.json
// holds the ladder's stats by TR band and the bot's APP by mistakes; each round draws a player
// from them and sets the bot to match. Known limits: the bot's APP tops out near 0.95, so the
// field ends around X rank, and its VS/APM runs about 2.3 against humans' 2.0-2.15, so its VS
// reads about 15% high for the same PPS and APP.
#pragma once

#include <array>
#include <random>
#include <string>
#include <vector>

#include "ai/bot.h"

// One player's stats over a game.
struct TrPlayer {
    double pps = 0, app = 0, vsApm = 0;
};

// The bot's settings for a round. Width and depth stay the base preset's (48, full).
struct TrBot {
    ai::BotWeights weights;
    double pps = 0, mistakes = 0;
};

struct TrField {
    struct Band {
        double tr;                                   // the band's median TR
        std::array<double, 3> mean;                  // of ln PPS, ln APP, ln VS/APM
        std::array<std::array<double, 3>, 3> cov;
    };
    std::vector<Band> bands;                          // TR rising
    std::vector<std::array<double, 2>> mistakesApp;   // (mistakes, the bot's mirror APP), mistakes rising

    // A player at `tr`: log-normal, interpolated between the bands either side, then one game's
    // noise (3% PPS, 9% APP, 4% VS/APM, the owner's match-to-match spread). Doesn't allocate.
    TrPlayer sample(double tr, std::mt19937& rng) const;
    // The median player's PPS at `tr`, for the menu.
    double medianPps(double tr) const;
    // Settings that play like `player`: its PPS as the cap, the base weights each scaled by
    // 0.8-1.2 for a style of its own, and the mistakes that give its APP (after the style's cost, kStyleApp).
    TrBot bot(const TrPlayer& player, const ai::BotWeights& base, std::mt19937& rng) const;
};

// False if the file can't be read.
bool loadTrField(const std::string& path, TrField& field);
