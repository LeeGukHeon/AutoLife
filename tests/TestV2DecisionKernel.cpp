#include "v2/orchestration/DecisionKernel.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using autolife::v2::DecisionKernel;
using autolife::v2::DecisionResult;
using autolife::v2::ExecutionEvent;
using autolife::v2::ExecutionIntent;
using autolife::v2::ExecutionSide;
using autolife::v2::ExecutionStatus;
using autolife::v2::IExecutionPlane;
using autolife::v2::IPolicyPlane;
using autolife::v2::IRiskPlane;
using autolife::v2::KernelConfig;
using autolife::v2::MarketRegime;
using autolife::v2::MarketSnapshot;
using autolife::v2::PolicyContext;
using autolife::v2::PolicyDecision;
using autolife::v2::PolicyDecisionBatch;
using autolife::v2::PortfolioSnapshot;
using autolife::v2::RiskCheck;
using autolife::v2::RiskContext;
using autolife::v2::SignalCandidate;
using autolife::v2::SignalDirection;

namespace {

class StubPolicyPlane final : public IPolicyPlane {
public:
    PolicyDecisionBatch selectCandidates(
        const std::vector<SignalCandidate>& candidates,
        const PolicyContext& context
    ) override {
        last_context = context;

        PolicyDecisionBatch batch;
        batch.decisions.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            PolicyDecision decision;
            decision.signal_id = candidate.signal_id;
            decision.accepted = candidate.score >= 0.5;
            decision.reason_code = decision.accepted ? "accepted_for_smoke" : "dropped_low_score";
            decision.adjusted_score = candidate.score;
            batch.decisions.push_back(decision);

            if (decision.accepted) {
                batch.selected_candidates.push_back(candidate);
            } else {
                batch.dropped_count += 1;
            }
        }
        return batch;
    }

    PolicyContext last_context;
};

class StubRiskPlane final : public IRiskPlane {
public:
    RiskCheck validateEntry(
        const ExecutionIntent& intent,
        const SignalCandidate& candidate,
        const RiskContext& context
    ) override {
        (void)candidate;
        last_context = context;
        RiskCheck check;
        check.allowed = (intent.market == "KRW-BTC");
        check.reason_code = check.allowed ? "ok" : "market_blocked_for_smoke";
        check.max_position_fraction = 0.2;
        check.suggested_order_krw = intent.order_notional_krw;
        return check;
    }

    RiskCheck validateExit(
        const ExecutionIntent& intent,
        const autolife::v2::PositionSnapshot& position,
        const RiskContext& context
    ) override {
        (void)intent;
        (void)position;
        (void)context;
        RiskCheck check;
        check.allowed = true;
        check.reason_code = "ok";
        return check;
    }

    RiskContext last_context;
};

class StubExecutionPlane final : public IExecutionPlane {
public:
    bool submit(const ExecutionIntent& intent) override {
        submitted.push_back(intent);
        return true;
    }

    bool cancel(const std::string& order_id) override {
        (void)order_id;
        return true;
    }

    void poll() override {
        poll_called = true;
    }

    std::vector<ExecutionEvent> drainEvents() override {
        std::vector<ExecutionEvent> events;
        events.reserve(submitted.size());
        for (const auto& intent : submitted) {
            ExecutionEvent event;
            event.event_id = intent.intent_id + ":fill";
            event.intent_id = intent.intent_id;
            event.order_id = "filled:" + intent.intent_id;
            event.market = intent.market;
            event.side = intent.side;
            event.status = ExecutionStatus::FILLED;
            event.filled_volume = intent.order_volume;
            event.order_volume = intent.order_volume;
            event.avg_price = intent.limit_price;
            event.terminal = true;
            event.source = "stub_execution_plane";
            event.reason_code = "filled";
            event.ts_ms = intent.ts_ms;
            events.push_back(event);
        }
        return events;
    }

    bool poll_called = false;
    std::vector<ExecutionIntent> submitted;
};

SignalCandidate makeCandidate(
    const std::string& signal_id,
    const std::string& market,
    const std::string& strategy_id,
    double score,
    double position_fraction,
    double entry_price
) {
    SignalCandidate candidate;
    candidate.signal_id = signal_id;
    candidate.market = market;
    candidate.strategy_id = strategy_id;
    candidate.direction = SignalDirection::LONG;
    candidate.score = score;
    candidate.position_fraction = position_fraction;
    candidate.entry_price = entry_price;
    candidate.ts_ms = 1700000000000LL;
    candidate.regime = MarketRegime::TRENDING_UP;
    return candidate;
}

PortfolioSnapshot makePortfolio() {
    PortfolioSnapshot snapshot;
    snapshot.available_capital_krw = 1000000.0;
    snapshot.invested_capital_krw = 250000.0;
    snapshot.total_capital_krw = 1250000.0;
    snapshot.daily_realized_pnl_krw = 1200.0;
    snapshot.ts_ms = 1700000000000LL;
    return snapshot;
}

void testExecutionEnabledPath() {
    auto policy_plane = std::make_shared<StubPolicyPlane>();
    auto risk_plane = std::make_shared<StubRiskPlane>();
    auto execution_plane = std::make_shared<StubExecutionPlane>();

    DecisionKernel kernel(policy_plane, risk_plane, execution_plane);

    MarketSnapshot market_snapshot;
    market_snapshot.dominant_regime = MarketRegime::TRENDING_UP;
    market_snapshot.ts_ms = 1700000000000LL;
    market_snapshot.candidates.push_back(
        makeCandidate("sig-btc", "KRW-BTC", "momentum", 0.9, 0.10, 100000000.0)
    );
    market_snapshot.candidates.push_back(
        makeCandidate("sig-eth", "KRW-ETH", "mean_reversion", 0.2, 0.10, 5000000.0)
    );

    const PortfolioSnapshot portfolio_snapshot = makePortfolio();
    const KernelConfig config;

    const DecisionResult result = kernel.runCycle(market_snapshot, portfolio_snapshot, config);
    assert(result.cycle_ok);
    assert(result.warnings.empty());

    assert(policy_plane->last_context.max_new_orders_per_cycle == config.max_new_orders_per_cycle);
    assert(policy_plane->last_context.dominant_regime == market_snapshot.dominant_regime);

    assert(result.policy_decisions.size() == 2);
    assert(result.policy_selected_candidates.size() == 1);
    assert(result.policy_selected_candidates.front().market == "KRW-BTC");

    assert(result.risk_checks.size() == 1);
    assert(result.risk_checks.front().check.allowed);

    assert(result.accepted_intents.size() == 1);
    assert(result.accepted_intents.front().market == "KRW-BTC");
    assert(result.accepted_intents.front().side == ExecutionSide::BUY);
    assert(std::abs(result.accepted_intents.front().order_notional_krw - 100000.0) < 1e-6);

    assert(execution_plane->submitted.size() == 1);
    assert(execution_plane->poll_called);

    bool has_submit_event = false;
    bool has_fill_event = false;
    for (const auto& event : result.execution_events) {
        if (event.status == ExecutionStatus::SUBMITTED) {
            has_submit_event = true;
        }
        if (event.status == ExecutionStatus::FILLED) {
            has_fill_event = true;
        }
    }
    assert(has_submit_event);
    assert(has_fill_event);
}

void testDryRunPath() {
    auto policy_plane = std::make_shared<StubPolicyPlane>();
    auto risk_plane = std::make_shared<StubRiskPlane>();
    auto execution_plane = std::make_shared<StubExecutionPlane>();

    DecisionKernel kernel(policy_plane, risk_plane, execution_plane);

    MarketSnapshot market_snapshot;
    market_snapshot.dominant_regime = MarketRegime::RANGING;
    market_snapshot.ts_ms = 1700000001000LL;
    market_snapshot.candidates.push_back(
        makeCandidate("sig-dry-run", "KRW-BTC", "scalping", 0.8, 0.05, 98000000.0)
    );

    const PortfolioSnapshot portfolio_snapshot = makePortfolio();
    KernelConfig config;
    config.dry_run = true;

    const DecisionResult result = kernel.runCycle(market_snapshot, portfolio_snapshot, config);
    assert(result.cycle_ok);
    assert(result.accepted_intents.size() == 1);
    assert(result.execution_events.empty());
    assert(execution_plane->submitted.empty());
    assert(!execution_plane->poll_called);
}

} // namespace

int main() {
    testExecutionEnabledPath();
    testDryRunPath();

    std::cout << "[TEST] V2DecisionKernel PASSED\n";
    return 0;
}
