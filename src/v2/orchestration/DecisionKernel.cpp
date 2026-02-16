#include "v2/orchestration/DecisionKernel.h"

#include <iterator>
#include <utility>

namespace autolife {
namespace v2 {

namespace {

PolicyContext buildPolicyContext(
    const MarketSnapshot& market_snapshot,
    const KernelConfig& config
) {
    PolicyContext context;
    context.small_seed_mode = false;
    context.max_new_orders_per_cycle = config.max_new_orders_per_cycle;
    context.dominant_regime = market_snapshot.dominant_regime;
    context.ts_ms = market_snapshot.ts_ms;
    return context;
}

RiskContext buildRiskContext(const PortfolioSnapshot& snapshot) {
    RiskContext context;
    context.available_capital_krw = snapshot.available_capital_krw;
    context.invested_capital_krw = snapshot.invested_capital_krw;
    context.total_capital_krw = snapshot.total_capital_krw;
    context.daily_realized_pnl_krw = snapshot.daily_realized_pnl_krw;
    context.open_position_count = static_cast<int>(snapshot.positions.size());
    context.ts_ms = snapshot.ts_ms;
    return context;
}

const PositionSnapshot* findPosition(
    const PortfolioSnapshot& snapshot,
    const std::string& market
) {
    for (const auto& position : snapshot.positions) {
        if (position.market == market) {
            return &position;
        }
    }
    return nullptr;
}

ExecutionIntent buildIntent(
    const SignalCandidate& candidate,
    const PortfolioSnapshot& portfolio_snapshot
) {
    ExecutionIntent intent;
    intent.intent_id = candidate.signal_id.empty()
        ? (candidate.market + ":" + candidate.strategy_id)
        : candidate.signal_id;
    intent.market = candidate.market;
    intent.side = (candidate.direction == SignalDirection::EXIT)
        ? ExecutionSide::SELL
        : ExecutionSide::BUY;
    intent.limit_price = candidate.entry_price;
    intent.order_volume = 0.0;
    intent.order_notional_krw =
        portfolio_snapshot.available_capital_krw * candidate.position_fraction;
    intent.strategy_id = candidate.strategy_id;
    intent.source_signal_id = candidate.signal_id;
    intent.stop_loss = candidate.stop_loss;
    intent.take_profit_1 = candidate.take_profit_1;
    intent.take_profit_2 = candidate.take_profit_2;
    intent.ts_ms = candidate.ts_ms;
    return intent;
}

ExecutionEvent makeSyntheticSubmitEvent(const ExecutionIntent& intent, bool submitted) {
    ExecutionEvent event;
    event.event_id = intent.intent_id + ":submit";
    event.intent_id = intent.intent_id;
    event.order_id = submitted ? ("submitted:" + intent.intent_id) : "";
    event.market = intent.market;
    event.side = intent.side;
    event.status = submitted ? ExecutionStatus::SUBMITTED : ExecutionStatus::REJECTED;
    event.filled_volume = 0.0;
    event.order_volume = intent.order_volume;
    event.avg_price = intent.limit_price;
    event.terminal = !submitted;
    event.source = "decision_kernel";
    event.reason_code = submitted ? "submitted" : "submit_failed";
    event.ts_ms = intent.ts_ms;
    return event;
}

} // namespace

DecisionKernel::DecisionKernel(
    std::shared_ptr<IPolicyPlane> policy_plane,
    std::shared_ptr<IRiskPlane> risk_plane,
    std::shared_ptr<IExecutionPlane> execution_plane
)
    : policy_plane_(std::move(policy_plane))
    , risk_plane_(std::move(risk_plane))
    , execution_plane_(std::move(execution_plane)) {}

DecisionResult DecisionKernel::runCycle(
    const MarketSnapshot& market_snapshot,
    const PortfolioSnapshot& portfolio_snapshot,
    const KernelConfig& config
) {
    DecisionResult result;
    auto selected_candidates = market_snapshot.candidates;

    if (config.enable_policy_plane) {
        if (policy_plane_) {
            PolicyDecisionBatch batch =
                policy_plane_->selectCandidates(selected_candidates, buildPolicyContext(market_snapshot, config));
            selected_candidates = std::move(batch.selected_candidates);
            result.policy_decisions = std::move(batch.decisions);
        } else {
            result.warnings.push_back("policy_plane_enabled_but_unset");
            result.policy_decisions.reserve(selected_candidates.size());
            for (const auto& candidate : selected_candidates) {
                PolicyDecision decision;
                decision.signal_id = candidate.signal_id;
                decision.accepted = true;
                decision.reason_code = "policy_unset_passthrough";
                decision.adjusted_score = candidate.score;
                result.policy_decisions.push_back(std::move(decision));
            }
        }
    } else {
        result.policy_decisions.reserve(selected_candidates.size());
        for (const auto& candidate : selected_candidates) {
            PolicyDecision decision;
            decision.signal_id = candidate.signal_id;
            decision.accepted = true;
            decision.reason_code = "policy_bypass";
            decision.adjusted_score = candidate.score;
            result.policy_decisions.push_back(std::move(decision));
        }
    }

    result.policy_selected_candidates = selected_candidates;

    const RiskContext risk_context = buildRiskContext(portfolio_snapshot);
    for (const auto& candidate : selected_candidates) {
        if (candidate.direction == SignalDirection::NONE) {
            continue;
        }

        const ExecutionIntent intent = buildIntent(candidate, portfolio_snapshot);
        RiskCheck check;
        check.allowed = true;
        check.reason_code = "risk_bypass";
        check.max_position_fraction = 1.0;
        check.suggested_order_krw = intent.order_notional_krw;

        if (config.enable_risk_plane) {
            if (risk_plane_) {
                if (intent.side == ExecutionSide::SELL) {
                    const PositionSnapshot* position = findPosition(portfolio_snapshot, candidate.market);
                    if (position) {
                        check = risk_plane_->validateExit(intent, *position, risk_context);
                    } else {
                        check.allowed = false;
                        check.reason_code = "position_not_found_for_exit";
                    }
                } else {
                    check = risk_plane_->validateEntry(intent, candidate, risk_context);
                }
            } else {
                check.allowed = false;
                check.reason_code = "risk_plane_enabled_but_unset";
                result.warnings.push_back("risk_plane_enabled_but_unset");
            }
        }

        RiskCheckRecord check_record;
        check_record.signal_id = candidate.signal_id;
        check_record.market = candidate.market;
        check_record.check = check;
        result.risk_checks.push_back(std::move(check_record));

        if (!check.allowed) {
            continue;
        }

        result.accepted_intents.push_back(intent);
        if (config.enable_execution_plane && execution_plane_ && !config.dry_run) {
            const bool submitted = execution_plane_->submit(intent);
            result.execution_events.push_back(makeSyntheticSubmitEvent(intent, submitted));
        }
    }

    if (config.enable_execution_plane && execution_plane_ && !config.dry_run) {
        execution_plane_->poll();
        auto drained = execution_plane_->drainEvents();
        result.execution_events.insert(
            result.execution_events.end(),
            std::make_move_iterator(drained.begin()),
            std::make_move_iterator(drained.end())
        );
    }

    result.cycle_ok = true;
    return result;
}

void DecisionKernel::setPolicyPlane(std::shared_ptr<IPolicyPlane> policy_plane) {
    policy_plane_ = std::move(policy_plane);
}

void DecisionKernel::setRiskPlane(std::shared_ptr<IRiskPlane> risk_plane) {
    risk_plane_ = std::move(risk_plane);
}

void DecisionKernel::setExecutionPlane(std::shared_ptr<IExecutionPlane> execution_plane) {
    execution_plane_ = std::move(execution_plane);
}

} // namespace v2
} // namespace autolife
