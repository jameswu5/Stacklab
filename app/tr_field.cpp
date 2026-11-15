#include "app/tr_field.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

// The bot's VS against an independently drawn player at the same TR over its mirror VS, the
// table's: the calibration knob. 96 bots drawn at 15k, 20.5k and 23k (12 games each) gave
// 0.89 ± 0.01, flat across mistakes.
constexpr double kPairVs = 0.89;

// A bot at a human's VS beats that human: it clears more of the garbage it gets (VS/APM about
// 2.4 against the owner's 2.2) and rarely tops out. The owner (TR 20,495) lost 0-7 to bots
// drawn at 20.5k and won 7-5 at 17k, so they break even near 17.3k. Raising the bot's mistakes
// while it digs doesn't close the gap: VS/APM barely falls until VS has about halved. So its VS
// is scaled: at 0.63, bots drawn at 20.5k win 44% (± 9%) against main's former 17.3k bots,
// the ones the owner breaks even with (16 draws x 12 games).
// ponytail: one owner, two sets, applied at every TR; refit from their win rate if it drifts.
constexpr double kHumanVs = 0.63;

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
    return {std::exp(z[0]), std::exp(z[1]), std::exp(z[2])};
}

double TrField::medianPps(double tr) const {
    std::array<double, 3> mean;
    std::array<std::array<double, 3>, 3> cov;
    interpolate(*this, tr, mean, cov);
    return std::exp(mean[0]);
}

TrBot TrField::bot(const TrPlayer& player, const ai::BotWeights& base) const {
    TrBot bot;
    bot.pps = player.pps;
    // VS per PPS falls as mistakes rise: read the table backwards, clamped at both ends. The
    // player's is 60 × APP × VS/APM.
    const double vs = kHumanVs * 60 * player.app * player.vsApm / kPairVs;
    bot.mistakes = mistakesVs.back()[0];
    if (vs >= mistakesVs.front()[1]) bot.mistakes = 0;
    for (size_t i = 0; i + 1 < mistakesVs.size(); i++) {
        const auto &a = mistakesVs[i], &b = mistakesVs[i + 1];
        if (vs <= a[1] && vs >= b[1]) {
            bot.mistakes = a[0] + (a[1] - vs) / (a[1] - b[1]) * (b[0] - a[0]);
            break;
        }
    }
    bot.weights = base;
    return bot;
}

bool loadTrField(const std::string& path, TrField& field) {
    std::ifstream in(path);
    const nlohmann::json file = nlohmann::json::parse(in, nullptr, false);
    if (file.is_discarded()) return false;
    try {
        field = {};
        for (const auto& b : file.at("bands")) field.bands.push_back({b.at("tr_median").get<double>(), b.at("logmean"), b.at("logcov")});
        for (const auto& row : file.at("mistakes_vs")) field.mistakesVs.push_back(row);
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    return !field.bands.empty() && field.mistakesVs.size() > 1;
}
