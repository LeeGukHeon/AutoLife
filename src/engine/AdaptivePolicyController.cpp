#include "engine/AdaptivePolicyController.h"

#include <algorithm>
#include <tuple>

namespace autolife {
namespace engine {
namespace {
double regimeStress(analytics::MarketRegime regime) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_DOWN:
            return 1.0;
        case analytics::MarketRegime::HIGH_VOLATILITY:
            return 0.8;
        case analytics::MarketRegime::RANGING:
            return 0.45;
        case analytics::MarketRegime::TRENDING_UP:
            return 0.2;
        default:
            return 0.3;
    }
}

int liquidityBucket(double liquidity_score) {
    if (liquidity_score < 40.0) {
        return 0;
    }
    if (liquidity_score < 60.0) {
        return 1;
    }
    if (liquidity_score < 80.0) {
        return 2;
    }
    return 3;
}

double strategyPerformanceModifier(
    const strategy::Signal& s,
    const PolicyInput& input,
    int* out_trades,
    double* out_win_rate,
    double* out_profit_factor
) {
    int trades = s.strategy_trade_count;
    double win_rate = s.strategy_win_rate;
    double pf = s.strategy_profit_factor;
    double expectancy = 0.0;

    if (input.strategy_stats) {
        const auto it = input.strategy_stats->find(s.strategy_name);
        if (it != input.strategy_stats->end()) {
            trades = it->second.trades;
            win_rate = it->second.winRate();
            pf = it->second.profitFactor();
            expectancy = it->second.expectancy();
        }
    }

    if (out_trades) *out_trades = trades;
    if (out_win_rate) *out_win_rate = win_rate;
    if (out_profit_factor) *out_profit_factor = pf;

    if (trades <= 0) {
        return 0.0;
    }

    const double wr_score = std::clamp((win_rate - 0.50) / 0.20, -1.0, 1.0) * 0.10;
    const double pf_score = std::clamp((pf - 1.0) / 0.60, -1.0, 1.0) * 0.08;
    const double ex_score = std::clamp(expectancy / 1500.0, -1.0, 1.0) * 0.05;
    double total = wr_score + pf_score + ex_score;

    if (trades >= 10 && (win_rate < 0.45 || pf < 0.85)) {
        total -= 0.12;
    }
    return total;
}

double bucketPerformanceModifier(const strategy::Signal& s, const PolicyInput& input) {
    if (!input.bucket_stats) {
        return 0.0;
    }

    PerformanceBucketKey key;
    key.strategy_name = s.strategy_name;
    key.regime = s.market_regime;
    key.liquidity_bucket = liquidityBucket(s.liquidity_score);
    const auto it = input.bucket_stats->find(key);
    if (it == input.bucket_stats->end()) {
        return 0.0;
    }

    const auto& stats = it->second;
    if (stats.trades < 5) {
        return 0.0;
    }

    const double wr_score = std::clamp((stats.winRate() - 0.50) / 0.20, -1.0, 1.0) * 0.07;
    const double pf_score = std::clamp((stats.profitFactor() - 1.0) / 0.60, -1.0, 1.0) * 0.05;
    return wr_score + pf_score;
}

double computePolicyScore(const strategy::Signal& s, const PolicyInput& input) {
    const double base = (s.score > 0.0) ? s.score : s.strength;
    const double liq_bonus = std::clamp((s.liquidity_score - 50.0) / 40.0, -1.0, 1.0) * 0.08;
    const double vol_penalty = std::clamp((s.volatility - 2.5) / 6.0, 0.0, 1.0) * 0.08;
    const double ev_bonus = std::clamp(s.expected_value / 0.0035, -1.0, 1.0) * 0.10;
    const double stress = regimeStress(input.dominant_regime);

    double score = base + liq_bonus - vol_penalty + ev_bonus;
    score += (s.strength - 0.5) * (0.08 + (0.04 * stress));
    score += strategyPerformanceModifier(s, input, nullptr, nullptr, nullptr);
    score += bucketPerformanceModifier(s, input);

    if (input.small_seed_mode) {
        const double small_seed_liq_penalty = std::clamp((62.0 - s.liquidity_score) / 30.0, 0.0, 1.0) * 0.10;
        const double small_seed_vol_penalty = std::clamp((s.volatility - 3.0) / 5.0, 0.0, 1.0) * 0.08;
        score -= (small_seed_liq_penalty + small_seed_vol_penalty);
    }

    return score;
}
}

PolicyOutput AdaptivePolicyController::selectCandidates(const PolicyInput& input) const {
    PolicyOutput out;
    std::vector<std::tuple<double, strategy::Signal, int, double, double>> ranked;
    ranked.reserve(input.candidates.size());

    for (const auto& s : input.candidates) {
        int hist_trades = 0;
        double hist_wr = 0.0;
        double hist_pf = 0.0;
        (void)strategyPerformanceModifier(s, input, &hist_trades, &hist_wr, &hist_pf);

        const double stress = regimeStress(input.dominant_regime);
        const double min_strength_under_stress = 0.36 + (0.10 * stress);
        if (s.strength < min_strength_under_stress) {
            out.dropped_by_policy++;
            out.decisions.push_back(PolicyDecisionRecord{
                s.market, s.strategy_name, false, "dropped_low_strength",
                (s.score > 0.0 ? s.score : s.strength), 0.0,
                s.strength, s.expected_value, s.liquidity_score, s.volatility,
                hist_trades, hist_wr, hist_pf
            });
            continue;
        }

        if (input.small_seed_mode &&
            hist_trades >= 10 &&
            (hist_wr < 0.50 || hist_pf < 0.90)) {
            out.dropped_by_policy++;
            out.decisions.push_back(PolicyDecisionRecord{
                s.market, s.strategy_name, false, "dropped_small_seed_quality",
                (s.score > 0.0 ? s.score : s.strength), 0.0,
                s.strength, s.expected_value, s.liquidity_score, s.volatility,
                hist_trades, hist_wr, hist_pf
            });
            continue;
        }

        ranked.emplace_back(computePolicyScore(s, input), s, hist_trades, hist_wr, hist_pf);
    }

    // Keep deterministic ordering for reproducibility.
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) {
                  const double score_a = std::get<0>(a);
                  const double score_b = std::get<0>(b);
                  const auto& sig_a = std::get<1>(a);
                  const auto& sig_b = std::get<1>(b);
                  if (score_a == score_b) {
                      if (sig_a.strength == sig_b.strength) {
                          return sig_a.score > sig_b.score;
                      }
                      return sig_a.strength > sig_b.strength;
                  }
                  return score_a > score_b;
              });

    std::vector<std::tuple<double, strategy::Signal, int, double, double>> filtered_ranked;
    filtered_ranked.reserve(ranked.size());
    for (auto& item : ranked) {
        const auto& s = std::get<1>(item);
        const int hist_trades = std::get<2>(item);
        const double hist_wr = std::get<3>(item);
        const double hist_pf = std::get<4>(item);
        if (input.small_seed_mode) {
            if (s.liquidity_score < 45.0 || s.volatility > 8.0) {
                out.dropped_by_policy++;
                out.decisions.push_back(PolicyDecisionRecord{
                    s.market, s.strategy_name, false, "dropped_small_seed_liqvol",
                    (s.score > 0.0 ? s.score : s.strength), std::get<0>(item),
                    s.strength, s.expected_value, s.liquidity_score, s.volatility,
                    hist_trades, hist_wr, hist_pf
                });
                continue;
            }
        }
        filtered_ranked.push_back(std::move(item));
    }

    const int cap = std::max(1, input.max_new_orders_per_scan);
    out.selected_candidates.reserve(filtered_ranked.size());
    for (size_t i = 0; i < filtered_ranked.size(); ++i) {
        auto& item = filtered_ranked[i];
        auto& s = std::get<1>(item);
        const double policy_score = std::get<0>(item);
        const int hist_trades = std::get<2>(item);
        const double hist_wr = std::get<3>(item);
        const double hist_pf = std::get<4>(item);
        const bool selected = static_cast<int>(i) < cap;
        out.decisions.push_back(PolicyDecisionRecord{
            s.market, s.strategy_name, selected, selected ? "selected" : "dropped_capacity",
            (s.score > 0.0 ? s.score : s.strength), policy_score,
            s.strength, s.expected_value, s.liquidity_score, s.volatility,
            hist_trades, hist_wr, hist_pf
        });
        if (selected) {
            out.selected_candidates.push_back(std::move(s));
        } else {
            out.dropped_by_policy++;
        }
    }

    return out;
}

} // namespace engine
} // namespace autolife
