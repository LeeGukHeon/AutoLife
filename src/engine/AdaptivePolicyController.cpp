#include "engine/AdaptivePolicyController.h"

#include <algorithm>
#include <tuple>
#include <unordered_set>

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

std::string normalizedArchetype(const std::string& archetype) {
    return archetype.empty() ? "UNSPECIFIED" : archetype;
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

    // Keep policy conservative: use historical stats for mild bonus only.
    // Heavy penalties are handled in execution/risk layers.
    const double wr_bonus = std::clamp((win_rate - 0.50) / 0.20, 0.0, 1.0) * 0.06;
    const double pf_bonus = std::clamp((pf - 1.0) / 0.60, 0.0, 1.0) * 0.05;
    const double ex_bonus = std::clamp(expectancy / 1500.0, 0.0, 1.0) * 0.03;
    return wr_bonus + pf_bonus + ex_bonus;
}

double bucketPerformanceModifier(const strategy::Signal& s, const PolicyInput& input) {
    if (!input.bucket_stats) {
        return 0.0;
    }

    PerformanceBucketKey key;
    key.strategy_name = s.strategy_name;
    key.regime = s.market_regime;
    key.liquidity_bucket = liquidityBucket(s.liquidity_score);
    key.entry_archetype = normalizedArchetype(s.entry_archetype);
    auto it = input.bucket_stats->find(key);
    if (it == input.bucket_stats->end() && key.entry_archetype != "UNSPECIFIED") {
        key.entry_archetype = "UNSPECIFIED";
        it = input.bucket_stats->find(key);
    }
    if (it == input.bucket_stats->end()) {
        return 0.0;
    }

    const auto& stats = it->second;
    if (stats.trades < 5) {
        return 0.0;
    }

    // Archetype bucket is used as a mild ranking hint only.
    const double wr_bonus = std::clamp((stats.winRate() - 0.50) / 0.20, 0.0, 1.0) * 0.04;
    const double pf_bonus = std::clamp((stats.profitFactor() - 1.0) / 0.60, 0.0, 1.0) * 0.04;
    return wr_bonus + pf_bonus;
}

double computePolicyScore(const strategy::Signal& s, const PolicyInput& input) {
    const double base = s.strength;
    const double liq_bonus = std::clamp((s.liquidity_score - 50.0) / 40.0, -1.0, 1.0) * 0.06;
    const double vol_penalty = std::clamp((s.volatility - 2.5) / 6.0, 0.0, 1.0) * 0.05;
    const double ev_bonus = std::clamp(s.expected_value / 0.0035, -1.0, 1.0) * 0.08;
    const double stress = regimeStress(input.dominant_regime);

    double score = base + liq_bonus - vol_penalty + ev_bonus;
    score += (s.strength - 0.5) * (0.06 + (0.03 * stress));
    score += strategyPerformanceModifier(s, input, nullptr, nullptr, nullptr);
    score += bucketPerformanceModifier(s, input);

    if (input.small_seed_mode) {
        const double small_seed_liq_penalty = std::clamp((60.0 - s.liquidity_score) / 35.0, 0.0, 1.0) * 0.04;
        const double small_seed_vol_penalty = std::clamp((s.volatility - 3.0) / 6.0, 0.0, 1.0) * 0.03;
        score -= (small_seed_liq_penalty + small_seed_vol_penalty);
    }

    return score;
}
}

PolicyOutput AdaptivePolicyController::selectCandidates(const PolicyInput& input) const {
    PolicyOutput out;
    std::vector<std::tuple<double, strategy::Signal, int, double, double>> ranked;
    ranked.reserve(input.candidates.size());
    auto makeDecisionRecord = [](
        const strategy::Signal& s,
        bool selected,
        const std::string& reason,
        double base_score,
        double policy_score,
        int hist_trades,
        double hist_wr,
        double hist_pf
    ) {
        PolicyDecisionRecord rec;
        rec.market = s.market;
        rec.strategy_name = s.strategy_name;
        rec.selected = selected;
        rec.reason = reason;
        rec.base_score = base_score;
        rec.policy_score = policy_score;
        rec.strength = s.strength;
        rec.expected_value = s.expected_value;
        rec.liquidity_score = s.liquidity_score;
        rec.volatility = s.volatility;
        rec.strategy_trades = hist_trades;
        rec.strategy_win_rate = hist_wr;
        rec.strategy_profit_factor = hist_pf;
        rec.used_preloaded_tf_5m = s.used_preloaded_tf_5m;
        rec.used_preloaded_tf_1h = s.used_preloaded_tf_1h;
        rec.used_resampled_tf_fallback = s.used_resampled_tf_fallback;
        return rec;
    };

    for (const auto& s : input.candidates) {
        int hist_trades = 0;
        double hist_wr = 0.0;
        double hist_pf = 0.0;
        (void)strategyPerformanceModifier(s, input, &hist_trades, &hist_wr, &hist_pf);

        const double stress = regimeStress(input.dominant_regime);
        const double min_strength_under_stress = 0.28 + (0.05 * stress);
        if (s.strength < min_strength_under_stress) {
            out.dropped_by_policy++;
            out.decisions.push_back(makeDecisionRecord(
                s,
                false,
                "dropped_low_strength",
                s.strength,
                0.0,
                hist_trades,
                hist_wr,
                hist_pf));
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
                          if (sig_a.expected_value == sig_b.expected_value) {
                              return sig_a.market < sig_b.market;
                          }
                          return sig_a.expected_value > sig_b.expected_value;
                      }
                      return sig_a.strength > sig_b.strength;
                  }
                  return score_a > score_b;
              });

    const int cap = std::max(1, input.max_new_orders_per_scan);
    out.selected_candidates.reserve(ranked.size());
    std::unordered_set<std::string> selected_markets;
    selected_markets.reserve(static_cast<size_t>(cap) * 2);

    int selected_count = 0;
    for (auto& item : ranked) {
        auto& s = std::get<1>(item);
        const double policy_score = std::get<0>(item);
        const int hist_trades = std::get<2>(item);
        const double hist_wr = std::get<3>(item);
        const double hist_pf = std::get<4>(item);
        bool selected = false;
        std::string reason = "dropped_capacity";

        if (selected_markets.find(s.market) != selected_markets.end()) {
            reason = "dropped_market_duplicate";
        } else if (selected_count >= cap) {
            reason = "dropped_capacity";
        } else {
            selected = true;
            reason = "selected";
        }

        out.decisions.push_back(makeDecisionRecord(
            s,
            selected,
            reason,
            s.strength,
            policy_score,
            hist_trades,
            hist_wr,
            hist_pf));
        if (selected) {
            out.selected_candidates.push_back(std::move(s));
            selected_markets.insert(out.selected_candidates.back().market);
            selected_count++;
        } else {
            out.dropped_by_policy++;
        }
    }

    return out;
}

} // namespace engine
} // namespace autolife
