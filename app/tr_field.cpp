#include "app/tr_field.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

// The bot's mirror APP over the table's, once its weights are jittered: the calibration knob.
// Two ±20% draws cost 4% and 11% at mistakes 0.02, but over 24 drawn players at 15k-23k (12
// mirror games each) aiming 7% high overshot by 2-6%, and 4% high by 7% at 20.5k, so in
// practice the style costs nothing measurable here and the bot aims at the player's APP.
constexpr double kStyleApp = 1.0;

}  // namespace

static void interpolate(const TrField& field, double tr, std::array<double, 3>& mean, std::array<std::array<double, 3>, 3>& cov) {
    const auto& bands = field.bands;
    auto upper = std::lower_bound(bands.begin(), bands.end(), tr, [](const TrField::Band& b, double t) { return b.tr < t; });
    const TrField::Band& b = upper == bands.end() ? bands.back() : *upper;
    const TrField::Band& a = upper == bands.begin() || upper == bands.end() ? b : upper[-1];
    const double f = &a == &b ? 0 : (tr - a.tr) / (b.tr - a.tr);
    for (int i = 0; i < 3; i++) {
        mean[i] = a.mean[i] + f * (b.mean[i] - a.mean[i]);
        for (int j = 0; j < 3; j++) cov[i][j] = a.cov[i][j] + f * (b.cov[i][j] - a.cov[i][j]);
    }
}

TrPlayer TrField::sample(double tr, std::mt19937& rng) const {
    std::array<double, 3> mean;
    std::array<std::array<double, 3>, 3> cov, l{};
    interpolate(*this, tr, mean, cov);
    // Cholesky, so the draw keeps the band's correlations (PPS and APP pull against each other).
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j <= i; j++) {
            double s = cov[i][j];
            for (int k = 0; k < j; k++) s -= l[i][k] * l[j][k];
            l[i][j] = i == j ? std::sqrt(std::max(s, 0.0)) : l[j][j] > 0 ? s / l[j][j] : 0;
        }
    }
    std::normal_distribution<double> normal;
    std::array<double, 3> n, z;
    for (double& x : n) x = normal(rng);
    for (int i = 0; i < 3; i++) {
        z[i] = mean[i];
        for (int k = 0; k <= i; k++) z[i] += l[i][k] * n[k];
    }
    return {std::exp(z[0]) * (1 + 0.03 * normal(rng)), std::exp(z[1]) * (1 + 0.09 * normal(rng)), std::exp(z[2]) * (1 + 0.04 * normal(rng))};
}

double TrField::medianPps(double tr) const {
    std::array<double, 3> mean;
    std::array<std::array<double, 3>, 3> cov;
    interpolate(*this, tr, mean, cov);
    return std::exp(mean[0]);
}

TrBot TrField::bot(const TrPlayer& player, const ai::BotWeights& base, std::mt19937& rng) const {
    TrBot bot;
    bot.pps = player.pps;
    // APP falls as mistakes rise: read the table backwards, clamped at both ends.
    const double app = player.app / kStyleApp;
    bot.mistakes = mistakesApp.back()[0];
    if (app >= mistakesApp.front()[1]) bot.mistakes = 0;
    for (size_t i = 0; i + 1 < mistakesApp.size(); i++) {
        const auto &a = mistakesApp[i], &b = mistakesApp[i + 1];
        if (app <= a[1] && app >= b[1]) {
            bot.mistakes = a[0] + (a[1] - app) / (a[1] - b[1]) * (b[0] - a[0]);
            break;
        }
    }
    std::uniform_real_distribution<double> jitter(0.8, 1.2);
    bot.weights = base;
    for (double* w : {&bot.weights.holes, &bot.weights.height, &bot.weights.danger, &bot.weights.bumpiness, &bot.weights.well,
                      &bot.weights.tSlot, &bot.weights.attack, &bot.weights.b2b, &bot.weights.wasted})
        *w *= jitter(rng);
    return bot;
}

bool loadTrField(const std::string& path, TrField& field) {
    std::ifstream in(path);
    const nlohmann::json file = nlohmann::json::parse(in, nullptr, false);
    if (file.is_discarded()) return false;
    try {
        field = {};
        for (const auto& b : file.at("bands")) field.bands.push_back({b.at("tr_median").get<double>(), b.at("logmean"), b.at("logcov")});
        for (const auto& row : file.at("mistakes_app")) field.mistakesApp.push_back(row);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    return !field.bands.empty() && field.mistakesApp.size() > 1;
}
