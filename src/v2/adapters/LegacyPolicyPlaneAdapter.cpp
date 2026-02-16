#include "v2/adapters/LegacyPolicyPlaneAdapter.h"

#include <utility>

namespace autolife {
namespace v2 {
namespace {

analytics::MarketRegime toLegacyRegime(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TRENDING_UP: return analytics::MarketRegime::TRENDING_UP;
        case MarketRegime::TRENDING_DOWN: return analytics::MarketRegime::TRENDING_DOWN;
        case MarketRegime::RANGING: return analytics::MarketRegime::RANGING;
        case MarketRegime::HIGH_VOLATILITY: return analytics::MarketRegime::HIGH_VOLATILITY;
        case MarketRegime::UNKNOWN:
        default:
            return analytics::MarketRegime::UNKNOWN;
    }
}

MarketRegime fromLegacyRegime(analytics::MarketRegime regime) {
    switch (regime) {
        case analytics::MarketRegime::TRENDING_UP: return MarketRegime::TRENDING_UP;
        case analytics::MarketRegime::TRENDING_DOWN: return MarketRegime::TRENDING_DOWN;
        case analytics::MarketRegime::RANGING: return MarketRegime::RANGING;
        case analytics::MarketRegime::HIGH_VOLATILITY: return MarketRegime::HIGH_VOLATILITY;
        case analytics::MarketRegime::UNKNOWN:
        default:
            return MarketRegime::UNKNOWN;
    }
}

strategy::SignalType toLegacySignalType(SignalDirection direction) {
    switch (direction) {
        case SignalDirection::LONG: return strategy::SignalType::BUY;
        case SignalDirection::EXIT: return strategy::SignalType::SELL;
        case SignalDirection::NONE:
        default:
            return strategy::SignalType::NONE;
    }
}

SignalDirection fromLegacySignalType(strategy::SignalType type) {
    switch (type) {
        case strategy::SignalType::STRONG_BUY:
        case strategy::SignalType::BUY:
            return SignalDirection::LONG;
        case strategy::SignalType::SELL:
        case strategy::SignalType::STRONG_SELL:
            return SignalDirection::EXIT;
        case strategy::SignalType::HOLD:
        case strategy::SignalType::NONE:
        default:
            return SignalDirection::NONE;
    }
}

strategy::Signal toLegacySignal(const SignalCandidate& candidate) {
    strategy::Signal out;
    out.type = toLegacySignalType(candidate.direction);
    out.market = candidate.market;
    out.strategy_name = candidate.strategy_id;
    out.strength = candidate.strength;
    out.entry_price = candidate.entry_price;
    out.stop_loss = candidate.stop_loss;
    out.take_profit_1 = candidate.take_profit_1;
    out.take_profit_2 = candidate.take_profit_2;
    out.position_size = candidate.position_fraction;
    out.expected_value = candidate.expected_edge_pct;
    out.score = candidate.score;
    out.market_regime = toLegacyRegime(candidate.regime);
    out.timestamp = candidate.ts_ms;
    return out;
}

SignalCandidate fromLegacySignal(const strategy::Signal& signal) {
    SignalCandidate out;
    out.signal_id = signal.market + ":" + signal.strategy_name + ":" + std::to_string(signal.timestamp);
    out.market = signal.market;
    out.strategy_id = signal.strategy_name;
    out.direction = fromLegacySignalType(signal.type);
    out.score = signal.score;
    out.strength = signal.strength;
    out.expected_edge_pct = signal.expected_value;
    out.reward_risk = (signal.expected_risk_pct > 1e-9)
        ? (signal.expected_return_pct / signal.expected_risk_pct)
        : 0.0;
    out.entry_price = signal.entry_price;
    out.stop_loss = signal.stop_loss;
    out.take_profit_1 = signal.take_profit_1;
    out.take_profit_2 = signal.take_profit_2;
    out.position_fraction = signal.position_size;
    out.regime = fromLegacyRegime(signal.market_regime);
    out.ts_ms = signal.timestamp;
    return out;
}

} // namespace

LegacyPolicyPlaneAdapter::LegacyPolicyPlaneAdapter(
    engine::AdaptivePolicyController& controller,
    const engine::PerformanceStore* performance_store
)
    : controller_(controller)
    , performance_store_(performance_store) {}

void LegacyPolicyPlaneAdapter::setPerformanceStore(const engine::PerformanceStore* performance_store) {
    performance_store_ = performance_store;
}

PolicyDecisionBatch LegacyPolicyPlaneAdapter::selectCandidates(
    const std::vector<SignalCandidate>& candidates,
    const PolicyContext& context
) {
    engine::PolicyInput input;
    input.candidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        input.candidates.push_back(toLegacySignal(candidate));
    }
    input.small_seed_mode = context.small_seed_mode;
    input.max_new_orders_per_scan = context.max_new_orders_per_cycle;
    input.dominant_regime = toLegacyRegime(context.dominant_regime);

    if (performance_store_ != nullptr) {
        input.strategy_stats = &performance_store_->byStrategy();
        input.bucket_stats = &performance_store_->byBucket();
    }

    engine::PolicyOutput output = controller_.selectCandidates(input);

    PolicyDecisionBatch batch;
    batch.selected_candidates.reserve(output.selected_candidates.size());
    for (const auto& selected : output.selected_candidates) {
        batch.selected_candidates.push_back(fromLegacySignal(selected));
    }
    batch.dropped_count = output.dropped_by_policy;
    batch.decisions.reserve(output.decisions.size());
    for (const auto& decision : output.decisions) {
        PolicyDecision out;
        out.signal_id = decision.market + ":" + decision.strategy_name;
        out.accepted = decision.selected;
        out.reason_code = decision.reason;
        out.adjusted_score = decision.policy_score;
        batch.decisions.push_back(std::move(out));
    }
    return batch;
}

} // namespace v2
} // namespace autolife

