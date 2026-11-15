// Versus opponents that play like a TETRA LEAGUE player at a chosen TR. data/bot/tr-field.json
// holds the ladder's stats by TR band and the bot's VS per PPS by mistakes; each round draws a
// player from them and sets the bot to its PPS and a VS that plays like it against a human
// (kHumanVs). Known limits: the bot's VS per PPS
// tops out near 114 against a same-TR opponent, just above the ladder's top band (107); its
// VS/APM runs about 2.3 against humans' 2.0-2.15, so at a matched VS its APP reads about 10%
// low; and below 1 PPS its real PPS runs over the cap (0.82 at 0.75), so its VS there reads up
// to 10% high.
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
        std::array<std::array<double, 3>, 3> cov;     // about a line in TR fitted within the band: the spread at one TR
    };
    std::vector<Band> bands;                          // TR rising
    std::vector<std::array<double, 2>> mistakesVs;    // (mistakes, the bot's mirror VS per PPS), mistakes rising

    // A player at `tr`: log-normal, interpolated between the bands either side, with the pool's
    // spread at one TR. Doesn't allocate.
    TrPlayer sample(double tr, std::mt19937& rng) const;
    // The median player's PPS at `tr`, for the menu.
    double medianPps(double tr) const;
    // Settings that play like `player` against a human: its PPS as the cap, the base weights,
    // and the mistakes that give its VS against a same-TR player (kPairVs), scaled down by
    // kHumanVs for the bot's edge over humans.
    TrBot bot(const TrPlayer& player, const ai::BotWeights& base) const;
};

// False if the file can't be read.
bool loadTrField(const std::string& path, TrField& field);
