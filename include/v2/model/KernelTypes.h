#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace autolife {
namespace v2 {

enum class MarketRegime {
    UNKNOWN,
    TRENDING_UP,
    TRENDING_DOWN,
    RANGING,
    HIGH_VOLATILITY
};

enum class SignalDirection {
    NONE,
    LONG,
    EXIT
};

enum class ExecutionSide {
    BUY,
    SELL
};

enum class ExecutionStatus {
    PENDING,
    SUBMITTED,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED,
    REJECTED
};

struct SignalCandidate {
    std::string signal_id;
    std::string market;
    std::string strategy_id;
    SignalDirection direction = SignalDirection::LONG;
    double score = 0.0;
    double strength = 0.0;
    double expected_edge_pct = 0.0;
    double reward_risk = 0.0;
    double entry_price = 0.0;
    double stop_loss = 0.0;
    double take_profit_1 = 0.0;
    double take_profit_2 = 0.0;
    double position_fraction = 0.0;
    MarketRegime regime = MarketRegime::UNKNOWN;
    long long ts_ms = 0;
    std::unordered_map<std::string, double> features;
};

struct PolicyContext {
    bool small_seed_mode = false;
    int max_new_orders_per_cycle = 1;
    MarketRegime dominant_regime = MarketRegime::UNKNOWN;
    long long ts_ms = 0;
};

struct PolicyDecision {
    std::string signal_id;
    bool accepted = false;
    std::string reason_code;
    double adjusted_score = 0.0;
};

struct PolicyDecisionBatch {
    std::vector<SignalCandidate> selected_candidates;
    std::vector<PolicyDecision> decisions;
    int dropped_count = 0;
};

struct PositionSnapshot {
    std::string position_id;
    std::string market;
    std::string strategy_id;
    double quantity = 0.0;
    double entry_price = 0.0;
    double stop_loss = 0.0;
    double take_profit_1 = 0.0;
    double take_profit_2 = 0.0;
    long long opened_ts_ms = 0;
};

struct PortfolioSnapshot {
    std::vector<PositionSnapshot> positions;
    double available_capital_krw = 0.0;
    double invested_capital_krw = 0.0;
    double total_capital_krw = 0.0;
    double daily_realized_pnl_krw = 0.0;
    long long ts_ms = 0;
};

struct RiskContext {
    double available_capital_krw = 0.0;
    double invested_capital_krw = 0.0;
    double total_capital_krw = 0.0;
    double daily_realized_pnl_krw = 0.0;
    int open_position_count = 0;
    long long ts_ms = 0;
};

struct MarketSnapshot {
    std::vector<SignalCandidate> candidates;
    MarketRegime dominant_regime = MarketRegime::UNKNOWN;
    long long ts_ms = 0;
};

struct RiskCheck {
    bool allowed = true;
    std::string reason_code = "ok";
    double max_position_fraction = 1.0;
    double suggested_order_krw = 0.0;
};

struct RiskCheckRecord {
    std::string signal_id;
    std::string market;
    RiskCheck check;
};

struct ExecutionIntent {
    std::string intent_id;
    std::string market;
    ExecutionSide side = ExecutionSide::BUY;
    double limit_price = 0.0;
    double order_volume = 0.0;
    double order_notional_krw = 0.0;
    std::string strategy_id;
    std::string source_signal_id;
    double stop_loss = 0.0;
    double take_profit_1 = 0.0;
    double take_profit_2 = 0.0;
    long long ts_ms = 0;
};

struct ExecutionEvent {
    std::string event_id;
    std::string intent_id;
    std::string order_id;
    std::string market;
    ExecutionSide side = ExecutionSide::BUY;
    ExecutionStatus status = ExecutionStatus::PENDING;
    double filled_volume = 0.0;
    double order_volume = 0.0;
    double avg_price = 0.0;
    bool terminal = false;
    std::string source;
    std::string reason_code;
    long long ts_ms = 0;
};

struct KernelConfig {
    bool enable_policy_plane = true;
    bool enable_risk_plane = true;
    bool enable_execution_plane = true;
    int max_new_orders_per_cycle = 1;
    bool dry_run = false;
};

struct DecisionResult {
    bool cycle_ok = true;
    std::vector<SignalCandidate> policy_selected_candidates;
    std::vector<PolicyDecision> policy_decisions;
    std::vector<RiskCheckRecord> risk_checks;
    std::vector<ExecutionIntent> accepted_intents;
    std::vector<ExecutionEvent> execution_events;
    std::vector<std::string> warnings;
};

struct LearningStateSnapshot {
    int schema_version = 1;
    long long saved_at_ms = 0;
    std::unordered_map<std::string, double> scalar_params;
    std::unordered_map<std::string, double> bucket_expectancy_krw;
    std::unordered_map<std::string, double> bucket_win_rate;
};

} // namespace v2
} // namespace autolife

