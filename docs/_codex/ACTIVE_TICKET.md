# Active Ticket Snapshot
Last updated: 2026-02-25

## Header
- Ticket ID: `CH01-PHASE4-PORTFOLIO-ENGINE-20260225`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `CH01-P4-KICKOFF`
- Title: Phase 4 Portfolio Engine (Allocator/Exposure/Risk Budget)
- Owner: Codex
- Date: 2026-02-25
- Status: `in_progress`
- Mode: `A` (baseline-preserving)

## Objective
Promote Phase 3 signal quality into a portfolio-level engine for multi-market selection, allocation, and risk control.

## Scope (In)
- Portfolio Candidate Snapshot normalization
- Portfolio Allocator (score/rank/top-k/budgeted selection)
- Exposure control (cluster/correlation cap, deterministic)
- Risk budget + drawdown governor
- Execution-aware sizing
- Portfolio diagnostics/report + OFF/ON comparison

## Scope (Out)
- New model training/tuning logic
- Live order enable policy changes
- Phase 5+ auto-tuning framework

## Non-negotiables
- Preserve Live/Backtest parity
- Deterministic behavior (same input -> same output)
- Bundle-first policy with safe fallback
- All Phase 4 flags default OFF (OFF must preserve current behavior)

## Feature Flags (default OFF)
- `phase4_portfolio_allocator_enabled=false`
- `phase4_correlation_control_enabled=false`
- `phase4_risk_budget_enabled=false`
- `phase4_drawdown_governor_enabled=false`
- `phase4_execution_aware_sizing_enabled=false`
- `phase4_portfolio_diagnostics_enabled=false`

## Implementation Order
1. Candidate snapshot + diagnostics skeleton (no behavior change)
2. Allocator core (score, top-k, deterministic tie-break)
3. Per-market cap / gross cap / risk budget
4. Drawdown governor
5. Correlation/cluster cap
6. Execution-aware sizing
7. Verification report extension (OFF/ON deltas)

## Acceptance Criteria
### Phase 4 OFF
- operational gate pass
- strict parity pass
- no behavior change

### Phase 4 ON
- parity still preserved
- portfolio diagnostics emitted
- selection/rejection reasons and exposure/budget enforcement visible in reports

## Current Step
- Step 1 complete: candidate snapshot + portfolio diagnostics skeleton wired (no behavior change).
- Step 2 complete: allocator core wired (phase4 flag ON -> allocator score ranking + deterministic tie-break + top_k/min_score budget filter; OFF -> existing behavior).
- Step 3 complete: risk budget filter wired (phase4 risk_budget ON -> per_market_cap + gross_cap + risk_budget_cap with deterministic prefix selection; OFF -> existing behavior).
- Step 4 complete: drawdown governor wired (phase4 drawdown_governor ON -> DD-based budget multiplier and dedicated reject split).
- Step 5 complete: correlation/cluster cap wired (phase4 correlation_control ON -> cluster exposure cap and dedicated reject counter).
- Step 6 complete: execution-aware sizing wired (phase4 execution_aware_sizing ON -> liquidity/tail-cost size scaling + min-size rejection).
- Step 7 complete: `run_verification.py --phase4-off-on-compare` now runs forced Phase4 OFF/ON backtest passes on the same dataset set and emits delta block in report (`phase4_off_on_comparison`).

## Verification Commands (baseline)
```powershell
python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity --run-should-exit-parity-analysis --refresh-should-exit-audit-from-logs --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" --should-exit-audit-live-runtime-log-mode-filter exclude_backtest --strict-should-exit-parity
python scripts/run_verification.py --pipeline-version v2
python scripts/run_verification.py --pipeline-version v2 --phase4-off-on-compare --data-dir data/backtest_recent_7d_20260225 --dataset-names upbit_KRW_BTC_1m_full.csv --output-json build/Release/logs/verification_report_phase4_step7_smoke.json
```

## Expected Touchpoints
- Runtime: `src/runtime/LiveTradingRuntime.cpp`, `src/runtime/BacktestRuntime.cpp`
- Strategy/Risk: `src/strategy/StrategyManager.cpp`, `src/strategy/FoundationRiskPipeline.cpp`
- Model policy surface: `src/analytics/ProbabilisticRuntimeModel.cpp`
- Config schema: `include/engine/EngineConfig.h`, `src/common/Config.cpp`, `config/config.json`, `config/model/probabilistic_runtime_bundle_v2.json`
- Verification: `scripts/run_verification.py` and portfolio diagnostics-related scripts\n
