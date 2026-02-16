#include "core/adapters/PolicyLearningPlaneAdapter.h"
#include "core/execution/ExecutionUpdateSchema.h"
#include "engine/AdaptivePolicyController.h"
#include "v2/adapters/LegacyPolicyPlaneAdapter.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using autolife::OrderSide;
using autolife::OrderStatus;
using autolife::analytics::MarketRegime;
using autolife::core::PolicyLearningPlaneAdapter;
using autolife::core::PolicyContext;
using autolife::engine::AdaptivePolicyController;
using autolife::strategy::Signal;
using autolife::strategy::SignalType;
using autolife::v2::ExecutionEvent;
using autolife::v2::ExecutionSide;
using autolife::v2::ExecutionStatus;
using autolife::v2::LegacyPolicyPlaneAdapter;
using autolife::v2::SignalCandidate;
using autolife::v2::SignalDirection;

Signal makeLegacySignal(
    const std::string& market,
    const std::string& strategy_name,
    double strength,
    double score,
    double expected_value,
    double liquidity_score,
    double volatility,
    long long ts_ms
) {
    Signal signal;
    signal.type = SignalType::BUY;
    signal.market = market;
    signal.strategy_name = strategy_name;
    signal.strength = strength;
    signal.score = score;
    signal.expected_value = expected_value;
    signal.liquidity_score = liquidity_score;
    signal.volatility = volatility;
    signal.market_regime = MarketRegime::TRENDING_UP;
    signal.timestamp = ts_ms;
    signal.entry_archetype = "MOMENTUM_PULLBACK";
    signal.used_preloaded_tf_5m = true;
    signal.used_preloaded_tf_1h = true;
    signal.used_resampled_tf_fallback = false;
    return signal;
}

SignalCandidate makeV2Candidate(const Signal& signal, const std::string& signal_id) {
    SignalCandidate candidate;
    candidate.signal_id = signal_id;
    candidate.market = signal.market;
    candidate.strategy_id = signal.strategy_name;
    candidate.direction = SignalDirection::LONG;
    candidate.score = signal.score;
    candidate.strength = signal.strength;
    candidate.expected_edge_pct = signal.expected_value;
    candidate.reward_risk = (signal.expected_risk_pct > 1e-9)
        ? (signal.expected_return_pct / signal.expected_risk_pct)
        : 0.0;
    candidate.entry_price = signal.entry_price;
    candidate.stop_loss = signal.stop_loss;
    candidate.take_profit_1 = signal.take_profit_1;
    candidate.take_profit_2 = signal.take_profit_2;
    candidate.position_fraction = signal.position_size;
    candidate.regime = autolife::v2::MarketRegime::TRENDING_UP;
    candidate.ts_ms = signal.timestamp;
    return candidate;
}

std::set<std::string> selectedCoreKeys(const std::vector<Signal>& selected) {
    std::set<std::string> out;
    for (const auto& signal : selected) {
        out.insert(signal.market + ":" + signal.strategy_name);
    }
    return out;
}

std::set<std::string> selectedV2Keys(const std::vector<SignalCandidate>& selected) {
    std::set<std::string> out;
    for (const auto& candidate : selected) {
        out.insert(candidate.market + ":" + candidate.strategy_id);
    }
    return out;
}

std::map<std::string, int> coreReasonCounts(
    const std::vector<autolife::engine::PolicyDecisionRecord>& decisions
) {
    std::map<std::string, int> out;
    for (const auto& row : decisions) {
        out[row.reason] += 1;
    }
    return out;
}

std::map<std::string, int> v2ReasonCounts(
    const std::vector<autolife::v2::PolicyDecision>& decisions
) {
    std::map<std::string, int> out;
    for (const auto& row : decisions) {
        out[row.reason_code] += 1;
    }
    return out;
}

std::set<std::string> mapKeys(const std::map<std::string, int>& values) {
    std::set<std::string> keys;
    for (const auto& item : values) {
        keys.insert(item.first);
    }
    return keys;
}

int reasonMismatchCount(
    const std::map<std::string, int>& left,
    const std::map<std::string, int>& right
) {
    std::set<std::string> all_reasons = mapKeys(left);
    const std::set<std::string> right_keys = mapKeys(right);
    all_reasons.insert(right_keys.begin(), right_keys.end());

    int mismatch_total = 0;
    for (const auto& reason : all_reasons) {
        const int left_count = (left.count(reason) > 0) ? left.at(reason) : 0;
        const int right_count = (right.count(reason) > 0) ? right.at(reason) : 0;
        mismatch_total += std::abs(left_count - right_count);
    }
    return mismatch_total;
}

std::string sideToString(ExecutionSide side) {
    return (side == ExecutionSide::SELL) ? "SELL" : "BUY";
}

std::string statusToString(ExecutionStatus status) {
    switch (status) {
        case ExecutionStatus::PENDING: return "PENDING";
        case ExecutionStatus::SUBMITTED: return "SUBMITTED";
        case ExecutionStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case ExecutionStatus::FILLED: return "FILLED";
        case ExecutionStatus::CANCELLED: return "CANCELLED";
        case ExecutionStatus::REJECTED:
        default:
            return "REJECTED";
    }
}

nlohmann::json toExecutionSchemaJson(
    const ExecutionEvent& event,
    const std::string& event_name,
    const std::string& strategy_name
) {
    nlohmann::json row;
    row["ts_ms"] = event.ts_ms;
    row["source"] = event.source;
    row["event"] = event_name;
    row["order_id"] = event.order_id;
    row["market"] = event.market;
    row["side"] = sideToString(event.side);
    row["status"] = statusToString(event.status);
    row["filled_volume"] = event.filled_volume;
    row["order_volume"] = event.order_volume;
    row["avg_price"] = event.avg_price;
    row["strategy_name"] = strategy_name;
    row["terminal"] = event.terminal;
    return row;
}

std::string keySignature(const nlohmann::json& row) {
    std::vector<std::string> keys;
    for (auto it = row.begin(); it != row.end(); ++it) {
        keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end());

    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += keys[i];
    }
    return out;
}

nlohmann::json setToJsonArray(const std::set<std::string>& values) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

nlohmann::json countMapToJson(const std::map<std::string, int>& values) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& item : values) {
        out[item.first] = item.second;
    }
    return out;
}

} // namespace

int main() {
    AdaptivePolicyController controller;
    PolicyLearningPlaneAdapter core_plane(controller, nullptr);
    LegacyPolicyPlaneAdapter v2_plane(controller, nullptr);

    std::vector<Signal> legacy_candidates;
    legacy_candidates.push_back(makeLegacySignal("KRW-BTC", "momentum", 0.82, 0.88, 0.0032, 72.0, 2.1, 1700000001000LL));
    legacy_candidates.push_back(makeLegacySignal("KRW-BTC", "mean_reversion", 0.76, 0.80, 0.0021, 68.0, 2.2, 1700000001001LL));
    legacy_candidates.push_back(makeLegacySignal("KRW-ETH", "breakout", 0.20, 0.71, 0.0036, 70.0, 2.0, 1700000001002LL));
    legacy_candidates.push_back(makeLegacySignal("KRW-XRP", "scalping", 0.79, 0.84, 0.0028, 74.0, 1.9, 1700000001003LL));
    legacy_candidates.push_back(makeLegacySignal("KRW-SOL", "trend_pullback", 0.74, 0.83, 0.0027, 69.0, 2.3, 1700000001004LL));

    std::vector<SignalCandidate> v2_candidates;
    v2_candidates.reserve(legacy_candidates.size());
    for (size_t i = 0; i < legacy_candidates.size(); ++i) {
        v2_candidates.push_back(makeV2Candidate(legacy_candidates[i], "sig-" + std::to_string(i + 1)));
    }

    PolicyContext core_context;
    core_context.small_seed_mode = false;
    core_context.max_new_orders_per_scan = 2;
    core_context.dominant_regime = MarketRegime::TRENDING_UP;

    autolife::v2::PolicyContext v2_context;
    v2_context.small_seed_mode = false;
    v2_context.max_new_orders_per_cycle = 2;
    v2_context.dominant_regime = autolife::v2::MarketRegime::TRENDING_UP;
    v2_context.ts_ms = 1700000001000LL;

    const auto core_batch = core_plane.selectCandidates(legacy_candidates, core_context);
    const auto v2_batch = v2_plane.selectCandidates(v2_candidates, v2_context);

    const std::set<std::string> core_selected = selectedCoreKeys(core_batch.selected_candidates);
    const std::set<std::string> v2_selected = selectedV2Keys(v2_batch.selected_candidates);

    std::set<std::string> selected_only_core;
    std::set<std::string> selected_only_v2;
    std::set_difference(
        core_selected.begin(),
        core_selected.end(),
        v2_selected.begin(),
        v2_selected.end(),
        std::inserter(selected_only_core, selected_only_core.begin())
    );
    std::set_difference(
        v2_selected.begin(),
        v2_selected.end(),
        core_selected.begin(),
        core_selected.end(),
        std::inserter(selected_only_v2, selected_only_v2.begin())
    );

    const auto core_reason_counts = coreReasonCounts(core_batch.decisions);
    const auto v2_reason_counts = v2ReasonCounts(v2_batch.decisions);

    const int selection_symmetric_diff_count =
        static_cast<int>(selected_only_core.size() + selected_only_v2.size());
    const int taxonomy_mismatch_count = reasonMismatchCount(core_reason_counts, v2_reason_counts);

    const auto core_update = autolife::core::execution::makeExecutionUpdate(
        "shadow_core",
        "filled",
        "order-1",
        "KRW-BTC",
        OrderSide::BUY,
        OrderStatus::FILLED,
        0.01,
        0.01,
        101000000.0,
        "momentum",
        true,
        1700000002000LL
    );
    const nlohmann::json core_execution_schema_row = autolife::core::execution::toJson(core_update);

    ExecutionEvent v2_event;
    v2_event.event_id = "evt-1";
    v2_event.intent_id = "intent-1";
    v2_event.order_id = "order-1";
    v2_event.market = "KRW-BTC";
    v2_event.side = ExecutionSide::BUY;
    v2_event.status = ExecutionStatus::FILLED;
    v2_event.filled_volume = 0.01;
    v2_event.order_volume = 0.01;
    v2_event.avg_price = 101000000.0;
    v2_event.terminal = true;
    v2_event.source = "shadow_v2";
    v2_event.reason_code = "filled";
    v2_event.ts_ms = 1700000002000LL;

    const nlohmann::json v2_execution_schema_row = toExecutionSchemaJson(v2_event, "filled", "momentum");

    const std::string core_signature = keySignature(core_execution_schema_row);
    const std::string v2_signature = keySignature(v2_execution_schema_row);

    const std::set<std::string> allowed_sides = {"BUY", "SELL"};
    const std::set<std::string> allowed_statuses = {
        "PENDING",
        "SUBMITTED",
        "FILLED",
        "PARTIALLY_FILLED",
        "CANCELLED",
        "REJECTED",
    };
    const std::string v2_side = std::string(v2_execution_schema_row.value("side", ""));
    const std::string v2_status = std::string(v2_execution_schema_row.value("status", ""));
    const bool v2_side_valid = allowed_sides.count(v2_side) > 0;
    const bool v2_status_valid = allowed_statuses.count(v2_status) > 0;

    const bool policy_selected_set_equal = selection_symmetric_diff_count == 0;
    const bool policy_dropped_count_equal = core_batch.dropped_by_policy == v2_batch.dropped_count;
    const bool rejection_taxonomy_equal = taxonomy_mismatch_count == 0;
    const bool execution_schema_compatible = core_signature == v2_signature;

    const int execution_schema_mismatch_count = execution_schema_compatible ? 0 : 1;

    nlohmann::json report;
    report["checks"] = {
        {"policy_selected_set_equal", policy_selected_set_equal},
        {"policy_dropped_count_equal", policy_dropped_count_equal},
        {"rejection_taxonomy_equal", rejection_taxonomy_equal},
        {"execution_schema_compatible", execution_schema_compatible},
        {"execution_side_valid", v2_side_valid},
        {"execution_status_valid", v2_status_valid},
    };
    report["metrics"] = {
        {"selection_symmetric_diff_count", selection_symmetric_diff_count},
        {"taxonomy_mismatch_count", taxonomy_mismatch_count},
        {"execution_schema_mismatch_count", execution_schema_mismatch_count},
        {"core_selected_count", static_cast<int>(core_selected.size())},
        {"v2_selected_count", static_cast<int>(v2_selected.size())},
        {"core_dropped_count", core_batch.dropped_by_policy},
        {"v2_dropped_count", v2_batch.dropped_count},
    };
    report["core"] = {
        {"selected", setToJsonArray(core_selected)},
        {"reason_counts", countMapToJson(core_reason_counts)},
        {"execution_key_signature", core_signature},
    };
    report["v2"] = {
        {"selected", setToJsonArray(v2_selected)},
        {"reason_counts", countMapToJson(v2_reason_counts)},
        {"execution_key_signature", v2_signature},
    };
    report["mismatches"] = {
        {"selected_only_core", setToJsonArray(selected_only_core)},
        {"selected_only_v2", setToJsonArray(selected_only_v2)},
    };

    const bool all_pass =
        policy_selected_set_equal &&
        policy_dropped_count_equal &&
        rejection_taxonomy_equal &&
        execution_schema_compatible &&
        v2_side_valid &&
        v2_status_valid;

    report["pass"] = all_pass;

    std::cout << report.dump() << "\n";

    if (!all_pass) {
        std::cerr << "[TEST] V2ShadowParity FAILED\n";
        return 1;
    }

    std::cout << "[TEST] V2ShadowParity PASSED\n";
    return 0;
}
