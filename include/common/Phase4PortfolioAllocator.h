#pragma once

#include "analytics/ProbabilisticRuntimeModel.h"
#include "strategy/IStrategy.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autolife {
namespace common {
namespace phase4 {

struct AllocatorSelectionResult {
    std::vector<std::size_t> ranked_indices;
    std::vector<std::size_t> selected_indices;
    int rejected_by_budget = 0;
};

struct RiskBudgetFilterResult {
    int selected_count = 0;
    int rejected_by_budget = 0;
    double gross_after = 0.0;
    double risk_after = 0.0;
    std::vector<double> selected_position_sizes;
};

struct ClusterFilterResult {
    std::vector<std::size_t> selected_indices;
    int rejected_by_cluster_cap = 0;
    std::map<std::string, double> exposure_by_cluster_before;
    std::map<std::string, double> exposure_by_cluster_after;
    std::map<std::string, double> cluster_cap_by_cluster;
    struct TelemetryRow {
        std::string market;
        std::string cluster_id;
        double exposure_current = 0.0;
        double cluster_cap_value = 0.0;
        double candidate_position_size = 0.0;
        double projected_exposure = 0.0;
        bool rejected_by_cluster_cap = false;
    };
    std::vector<TelemetryRow> telemetry_rows;
};

struct AllocatorClusterCapReallocateResult {
    std::vector<std::size_t> selected_indices;
    int skipped_by_cluster_cap = 0;
    std::vector<ClusterFilterResult::TelemetryRow> telemetry_rows;
};

struct ExecutionAwareFilterResult {
    std::vector<std::size_t> selected_indices;
    std::vector<double> adjusted_position_sizes;
    int rejected_by_execution_cap = 0;
};

inline double allocatorTailGapPct(const autolife::strategy::Signal& signal) {
    return std::max(0.0, signal.phase3.cost_tail_pct - signal.phase3.cost_used_pct);
}

inline double allocatorUncertainty(
    const autolife::strategy::Signal& signal,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::PortfolioAllocatorPolicy& policy
) {
    const double prob_unc = 1.0 - std::clamp(signal.probabilistic_h5_calibrated, 0.0, 1.0);
    const double ev_unc = 1.0 - std::clamp(signal.phase3.ev_confidence, 0.0, 1.0);
    const double prob_weight = std::max(0.0, policy.uncertainty_prob_weight);
    const double ev_weight = std::max(0.0, policy.uncertainty_ev_weight);
    const double denom = prob_weight + ev_weight;
    if (denom <= 1e-9) {
        return 0.5 * (prob_unc + ev_unc);
    }
    return ((prob_unc * prob_weight) + (ev_unc * ev_weight)) / denom;
}

inline double allocatorScore(
    const autolife::strategy::Signal& signal,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::PortfolioAllocatorPolicy& policy
) {
    const double ev_mean = signal.expected_value;
    const double tail_gap = allocatorTailGapPct(signal);
    const double cost_pct = std::max(0.0, signal.phase3.cost_used_pct);
    const double uncertainty = allocatorUncertainty(signal, policy);
    const double margin = signal.probabilistic_h5_margin;
    return ev_mean -
           (policy.lambda_tail * tail_gap) -
           (policy.lambda_cost * cost_pct) -
           (policy.lambda_uncertainty * uncertainty) +
           (policy.lambda_margin * margin);
}

inline AllocatorSelectionResult selectAllocatorCandidates(
    const std::vector<autolife::strategy::Signal>& candidates,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::PortfolioAllocatorPolicy& policy
) {
    AllocatorSelectionResult out;
    if (candidates.empty()) {
        return out;
    }

    struct Ranked {
        std::size_t index = 0;
        double score = 0.0;
    };

    std::vector<Ranked> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        Ranked item;
        item.index = i;
        item.score = allocatorScore(candidates[i], policy);
        ranked.push_back(item);
    }

    std::stable_sort(
        ranked.begin(),
        ranked.end(),
        [&](const Ranked& lhs, const Ranked& rhs) {
            if (std::fabs(lhs.score - rhs.score) > 1e-12) {
                return lhs.score > rhs.score;
            }
            const auto& l = candidates[lhs.index];
            const auto& r = candidates[rhs.index];
            if (std::fabs(l.probabilistic_h5_margin - r.probabilistic_h5_margin) > 1e-12) {
                return l.probabilistic_h5_margin > r.probabilistic_h5_margin;
            }
            if (std::fabs(l.expected_value - r.expected_value) > 1e-12) {
                return l.expected_value > r.expected_value;
            }
            if (std::fabs(l.probabilistic_h5_calibrated - r.probabilistic_h5_calibrated) > 1e-12) {
                return l.probabilistic_h5_calibrated > r.probabilistic_h5_calibrated;
            }
            if (l.market != r.market) {
                return l.market < r.market;
            }
            if (l.strategy_name != r.strategy_name) {
                return l.strategy_name < r.strategy_name;
            }
            return lhs.index < rhs.index;
        }
    );

    out.ranked_indices.reserve(ranked.size());
    for (const auto& item : ranked) {
        out.ranked_indices.push_back(item.index);
    }

    const int top_k = std::max(1, policy.top_k);
    const double min_score = policy.min_score;
    for (std::size_t i = 0; i < ranked.size(); ++i) {
        const bool under_topk = static_cast<int>(i) < top_k;
        const bool score_ok = ranked[i].score >= min_score;
        if (under_topk && score_ok) {
            out.selected_indices.push_back(ranked[i].index);
        } else {
            out.rejected_by_budget++;
        }
    }

    return out;
}

inline std::vector<autolife::strategy::Signal> materializeSignalsByIndex(
    const std::vector<autolife::strategy::Signal>& source,
    const std::vector<std::size_t>& indices
) {
    std::vector<autolife::strategy::Signal> out;
    out.reserve(indices.size());
    for (const std::size_t idx : indices) {
        if (idx < source.size()) {
            out.push_back(source[idx]);
        }
    }
    return out;
}

inline double signalRiskPct(
    const autolife::strategy::Signal& signal,
    double fallback_stop_pct
) {
    const double fallback = std::clamp(fallback_stop_pct, 0.0, 1.0);
    if (signal.entry_price <= 1e-12 ||
        signal.stop_loss <= 1e-12 ||
        signal.stop_loss >= signal.entry_price) {
        return fallback;
    }
    const double risk_pct = (signal.entry_price - signal.stop_loss) / signal.entry_price;
    if (!std::isfinite(risk_pct)) {
        return fallback;
    }
    return std::clamp(risk_pct, 0.0, 1.0);
}

struct RegimeBudgetMultiplierResolution {
    double multiplier = 1.0;
    bool configured = false;
    std::string matched_key;
};

inline std::string normalizeRegimeBudgetKey(std::string key) {
    std::string out;
    out.reserve(key.size());
    for (unsigned char c : key) {
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::toupper(c)));
        } else if (c == '_' || c == '-' || std::isspace(c)) {
            out.push_back('_');
        }
    }
    while (out.find("__") != std::string::npos) {
        out.erase(out.find("__"), 1);
    }
    if (!out.empty() && out.front() == '_') {
        out.erase(out.begin());
    }
    if (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out == "TRENDINGUP") {
        out = "TRENDING_UP";
    } else if (out == "TRENDINGDOWN") {
        out = "TRENDING_DOWN";
    } else if (out == "HIGHVOLATILITY" || out == "VOLATILE" || out == "HOSTILE") {
        out = "HIGH_VOLATILITY";
    } else if (out == "RANGE") {
        out = "RANGING";
    }
    return out;
}

inline std::string regimeBudgetKey(autolife::analytics::MarketRegime regime) {
    switch (regime) {
        case autolife::analytics::MarketRegime::TRENDING_UP:
            return "TRENDING_UP";
        case autolife::analytics::MarketRegime::TRENDING_DOWN:
            return "TRENDING_DOWN";
        case autolife::analytics::MarketRegime::RANGING:
            return "RANGING";
        case autolife::analytics::MarketRegime::HIGH_VOLATILITY:
            return "HIGH_VOLATILITY";
        case autolife::analytics::MarketRegime::UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

inline RegimeBudgetMultiplierResolution resolveRegimeBudgetMultiplier(
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::RiskBudgetPolicy& policy,
    autolife::analytics::MarketRegime regime
) {
    RegimeBudgetMultiplierResolution out;
    if (policy.regime_budget_multipliers.empty()) {
        return out;
    }

    const std::string regime_key = regimeBudgetKey(regime);
    const auto exact_it = policy.regime_budget_multipliers.find(regime_key);
    if (exact_it != policy.regime_budget_multipliers.end()) {
        out.multiplier = std::clamp(exact_it->second, 0.0, 1.0);
        out.configured = true;
        out.matched_key = regime_key;
        return out;
    }

    const auto any_it = policy.regime_budget_multipliers.find("ANY");
    if (any_it != policy.regime_budget_multipliers.end()) {
        out.multiplier = std::clamp(any_it->second, 0.0, 1.0);
        out.configured = true;
        out.matched_key = "ANY";
        return out;
    }
    return out;
}

inline RiskBudgetFilterResult applyRiskBudgetPrefixFilter(
    const std::vector<autolife::strategy::Signal>& ordered_candidates,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::RiskBudgetPolicy& policy,
    double existing_gross_exposure,
    double existing_risk_exposure
) {
    RiskBudgetFilterResult out;
    if (ordered_candidates.empty()) {
        return out;
    }

    double gross_used = std::max(0.0, existing_gross_exposure);
    double risk_used = std::max(0.0, existing_risk_exposure);
    out.gross_after = gross_used;
    out.risk_after = risk_used;

    for (std::size_t i = 0; i < ordered_candidates.size(); ++i) {
        const auto& signal = ordered_candidates[i];
        const double capped_size = std::min(
            std::clamp(signal.position_size, 0.0, 1.0),
            std::clamp(policy.per_market_cap, 0.0, 1.0)
        );
        if (capped_size <= 1e-9) {
            out.rejected_by_budget += static_cast<int>(ordered_candidates.size() - i);
            break;
        }

        const double next_gross = gross_used + capped_size;
        const double risk_pct = signalRiskPct(signal, policy.risk_proxy_stop_pct);
        const double next_risk = risk_used + (capped_size * std::max(risk_pct, policy.risk_proxy_stop_pct));
        if (next_gross > std::max(0.0, policy.gross_cap) + 1e-12 ||
            next_risk > std::max(0.0, policy.risk_budget_cap) + 1e-12) {
            out.rejected_by_budget += static_cast<int>(ordered_candidates.size() - i);
            break;
        }

        gross_used = next_gross;
        risk_used = next_risk;
        out.selected_count++;
        out.selected_position_sizes.push_back(capped_size);
    }

    out.gross_after = gross_used;
    out.risk_after = risk_used;
    return out;
}

inline double drawdownBudgetMultiplier(
    double current_drawdown,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::DrawdownGovernorPolicy& policy
) {
    if (!policy.enabled) {
        return 1.0;
    }
    const double dd = std::clamp(current_drawdown, 0.0, 1.0);
    if (dd >= policy.dd_threshold_hard) {
        return std::clamp(policy.budget_multiplier_hard, 0.0, 1.0);
    }
    if (dd >= policy.dd_threshold_soft) {
        return std::clamp(policy.budget_multiplier_soft, 0.0, 1.0);
    }
    return 1.0;
}

inline std::string clusterIdForMarket(
    const std::string& market,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::CorrelationControlPolicy& policy
) {
    const auto it = policy.market_cluster_map.find(market);
    if (it != policy.market_cluster_map.end() && !it->second.empty()) {
        return it->second;
    }
    return market;
}

inline double clusterCapForId(
    const std::string& cluster_id,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::CorrelationControlPolicy& policy
) {
    const auto it = policy.cluster_caps.find(cluster_id);
    if (it != policy.cluster_caps.end() && std::isfinite(it->second)) {
        return std::max(0.0, it->second);
    }
    return std::max(0.0, policy.default_cluster_cap);
}

inline ClusterFilterResult applyClusterCapFilter(
    const std::vector<autolife::strategy::Signal>& ordered_candidates,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::CorrelationControlPolicy& policy,
    const std::map<std::string, double>& existing_exposure_by_cluster
) {
    ClusterFilterResult out;
    out.exposure_by_cluster_before = existing_exposure_by_cluster;
    out.exposure_by_cluster_after = existing_exposure_by_cluster;
    if (ordered_candidates.empty()) {
        return out;
    }
    for (std::size_t i = 0; i < ordered_candidates.size(); ++i) {
        const auto& signal = ordered_candidates[i];
        const std::string cluster_id = clusterIdForMarket(signal.market, policy);
        const double cluster_cap = clusterCapForId(cluster_id, policy);
        const double pos_size = std::clamp(signal.position_size, 0.0, 1.0);
        const double used = out.exposure_by_cluster_after.count(cluster_id) > 0
            ? out.exposure_by_cluster_after.at(cluster_id)
            : 0.0;
        const double projected = used + pos_size;
        const bool rejected = projected > cluster_cap + 1e-12;
        out.cluster_cap_by_cluster[cluster_id] = cluster_cap;
        ClusterFilterResult::TelemetryRow telemetry;
        telemetry.market = signal.market;
        telemetry.cluster_id = cluster_id;
        telemetry.exposure_current = used;
        telemetry.cluster_cap_value = cluster_cap;
        telemetry.candidate_position_size = pos_size;
        telemetry.projected_exposure = projected;
        telemetry.rejected_by_cluster_cap = rejected;
        out.telemetry_rows.push_back(std::move(telemetry));
        if (rejected) {
            out.rejected_by_cluster_cap++;
            continue;
        }
        out.selected_indices.push_back(i);
        out.exposure_by_cluster_after[cluster_id] = projected;
    }
    return out;
}

inline AllocatorClusterCapReallocateResult selectAllocatorCandidatesWithClusterCapReallocate(
    const std::vector<autolife::strategy::Signal>& candidates,
    const std::vector<std::size_t>& ranked_indices,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::PortfolioAllocatorPolicy&
        allocator_policy,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::CorrelationControlPolicy&
        corr_policy,
    const std::map<std::string, double>& existing_exposure_by_cluster
) {
    AllocatorClusterCapReallocateResult out;
    if (candidates.empty() || ranked_indices.empty()) {
        return out;
    }

    const int top_k = std::max(1, allocator_policy.top_k);
    std::map<std::string, double> exposure_cursor = existing_exposure_by_cluster;
    for (const std::size_t candidate_idx : ranked_indices) {
        if (static_cast<int>(out.selected_indices.size()) >= top_k) {
            break;
        }
        if (candidate_idx >= candidates.size()) {
            continue;
        }
        const auto& signal = candidates[candidate_idx];
        const double score = allocatorScore(signal, allocator_policy);
        if (score < allocator_policy.min_score) {
            continue;
        }

        const std::string cluster_id = clusterIdForMarket(signal.market, corr_policy);
        const double cluster_cap = clusterCapForId(cluster_id, corr_policy);
        const double position_size = std::clamp(signal.position_size, 0.0, 1.0);
        const double exposure_current = exposure_cursor.count(cluster_id) > 0
            ? exposure_cursor.at(cluster_id)
            : 0.0;
        const double projected_exposure = exposure_current + position_size;
        const bool rejected = projected_exposure > cluster_cap + 1e-12;

        ClusterFilterResult::TelemetryRow telemetry;
        telemetry.market = signal.market;
        telemetry.cluster_id = cluster_id;
        telemetry.exposure_current = exposure_current;
        telemetry.cluster_cap_value = cluster_cap;
        telemetry.candidate_position_size = position_size;
        telemetry.projected_exposure = projected_exposure;
        telemetry.rejected_by_cluster_cap = rejected;
        out.telemetry_rows.push_back(std::move(telemetry));

        if (rejected) {
            out.skipped_by_cluster_cap += 1;
            continue;
        }
        out.selected_indices.push_back(candidate_idx);
        exposure_cursor[cluster_id] = projected_exposure;
    }
    return out;
}

inline double executionAwareSizeMultiplier(
    const autolife::strategy::Signal& signal,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::ExecutionAwareSizingPolicy& policy
) {
    double multiplier = policy.liquidity_high_size_multiplier;
    if (signal.liquidity_score < policy.liquidity_low_threshold) {
        multiplier = policy.liquidity_low_size_multiplier;
    } else if (signal.liquidity_score < policy.liquidity_mid_threshold) {
        multiplier = policy.liquidity_mid_size_multiplier;
    }

    if (signal.phase3.cost_tail_pct >= policy.tail_cost_hard_pct) {
        multiplier *= policy.tail_hard_multiplier;
    } else if (signal.phase3.cost_tail_pct >= policy.tail_cost_soft_pct) {
        multiplier *= policy.tail_soft_multiplier;
    }
    return std::clamp(multiplier, 0.0, 1.0);
}

inline ExecutionAwareFilterResult applyExecutionAwareSizingFilter(
    const std::vector<autolife::strategy::Signal>& ordered_candidates,
    const autolife::analytics::ProbabilisticRuntimeModel::Phase4Policy::ExecutionAwareSizingPolicy& policy
) {
    ExecutionAwareFilterResult out;
    if (ordered_candidates.empty()) {
        return out;
    }
    for (std::size_t i = 0; i < ordered_candidates.size(); ++i) {
        const auto& signal = ordered_candidates[i];
        const double multiplier = executionAwareSizeMultiplier(signal, policy);
        const double adjusted_size = std::clamp(signal.position_size * multiplier, 0.0, 1.0);
        if (adjusted_size < policy.min_position_size) {
            out.rejected_by_execution_cap++;
            continue;
        }
        out.selected_indices.push_back(i);
        out.adjusted_position_sizes.push_back(adjusted_size);
    }
    return out;
}

} // namespace phase4
} // namespace common
} // namespace autolife
