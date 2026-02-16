#include "v2/backtest/BacktestEngineV2.h"
#include "v2/engine/TradingEngineV2.h"

#include <iostream>

namespace {

autolife::v2::MarketSnapshot makeMarketSnapshot() {
    autolife::v2::MarketSnapshot snapshot;
    snapshot.dominant_regime = autolife::v2::MarketRegime::TRENDING_UP;
    snapshot.ts_ms = 1700000000000LL;

    autolife::v2::SignalCandidate candidate;
    candidate.signal_id = "test:signal";
    candidate.market = "KRW-BTC";
    candidate.strategy_id = "scaffold";
    candidate.direction = autolife::v2::SignalDirection::LONG;
    candidate.score = 0.8;
    candidate.strength = 0.7;
    candidate.expected_edge_pct = 0.002;
    candidate.reward_risk = 1.4;
    candidate.entry_price = 100.0;
    candidate.stop_loss = 98.0;
    candidate.take_profit_1 = 102.0;
    candidate.take_profit_2 = 103.0;
    candidate.position_fraction = 0.1;
    candidate.regime = autolife::v2::MarketRegime::TRENDING_UP;
    candidate.ts_ms = snapshot.ts_ms;
    snapshot.candidates.push_back(candidate);
    return snapshot;
}

autolife::v2::PortfolioSnapshot makePortfolioSnapshot() {
    autolife::v2::PortfolioSnapshot snapshot;
    snapshot.available_capital_krw = 100000.0;
    snapshot.invested_capital_krw = 0.0;
    snapshot.total_capital_krw = 100000.0;
    snapshot.daily_realized_pnl_krw = 0.0;
    snapshot.ts_ms = 1700000000000LL;
    return snapshot;
}

} // namespace

int main() {
    using autolife::v2::DecisionResult;
    using autolife::v2::backtest::BacktestEngineV2;
    using autolife::v2::backtest::BacktestEngineV2Config;
    using autolife::v2::engine::TradingEngineV2;
    using autolife::v2::engine::TradingEngineV2Config;

    const auto market_snapshot = makeMarketSnapshot();
    const auto portfolio_snapshot = makePortfolioSnapshot();

    TradingEngineV2Config runtime_cfg;
    runtime_cfg.enable_policy_plane = false;
    runtime_cfg.enable_risk_plane = false;
    runtime_cfg.enable_execution_plane = false;
    runtime_cfg.dry_run = true;
    runtime_cfg.max_new_orders_per_cycle = 2;

    TradingEngineV2 runtime_engine(nullptr, nullptr, nullptr, runtime_cfg);
    DecisionResult runtime_result = runtime_engine.runCycle(market_snapshot, portfolio_snapshot);
    if (!runtime_result.cycle_ok) {
        std::cerr << "[TEST] runtime cycle_ok=false\n";
        return 1;
    }
    if (runtime_engine.cycleCount() != 1) {
        std::cerr << "[TEST] runtime cycle_count mismatch\n";
        return 1;
    }

    BacktestEngineV2Config backtest_cfg;
    backtest_cfg.runtime_config = runtime_cfg;
    backtest_cfg.collect_cycle_results = true;
    BacktestEngineV2 backtest_engine(nullptr, nullptr, nullptr, backtest_cfg);

    DecisionResult backtest_result = backtest_engine.runCycle(market_snapshot, portfolio_snapshot);
    if (!backtest_result.cycle_ok) {
        std::cerr << "[TEST] backtest cycle_ok=false\n";
        return 1;
    }
    if (backtest_engine.cycleCount() != 1) {
        std::cerr << "[TEST] backtest cycle_count mismatch\n";
        return 1;
    }
    if (backtest_engine.cycleResults().size() != 1) {
        std::cerr << "[TEST] backtest collected_result_count mismatch\n";
        return 1;
    }

    std::cout << "[TEST] V2EngineBacktestScaffold PASSED\n";
    return 0;
}
