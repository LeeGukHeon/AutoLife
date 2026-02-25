# CH-01 Phase 3 Context (Compression-Safe)
Last updated: 2026-02-25
Status: implementation complete (validation artifacts captured)

## Scope
- Implement CH-01 Phase 3 over Phase 2 gate stack without baseline regression:
  - frontier coupling
  - EV calibration
  - entry/exit/tail cost mode
  - adaptive EV blend
  - diagnostics v2
- Phase 2 closure baseline reference:
  - `docs/_codex/PHASE2_CLOSURE_2026-02-25.md`

## Invariants
- Live/Backtest/StrategyManager/FoundationRiskPipeline parity must hold.
- Deterministic inputs and rule ordering must hold.
- Feature flags default OFF.
- OFF path must reproduce legacy behavior.

## Source touch map
- Runtime inference + bundle parsing:
  - `include/analytics/ProbabilisticRuntimeModel.h`
  - `src/analytics/ProbabilisticRuntimeModel.cpp`
  - `scripts/export_probabilistic_runtime_bundle.py`
- Signal/runtime application:
  - `include/strategy/IStrategy.h`
  - `src/runtime/LiveTradingRuntime.cpp`
  - `src/runtime/BacktestRuntime.cpp`
- Frontier decision + diagnostics:
  - `include/strategy/FoundationRiskPipeline.h`
  - `src/strategy/FoundationRiskPipeline.cpp`
  - `src/strategy/StrategyManager.cpp`
- Verification report:
  - `scripts/run_verification.py`

## Required outputs
1. Bundle `phase3` keys + defaults
2. Diagnostics v2 counters and bucket slices
3. Verification report extensions:
   - funnel breakdown with frontier categories
   - bucket pass-rates
   - regressor-vs-fallback pass-rate
   - top3 bottleneck cause summary

## Gate commands
```powershell
python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity
python scripts/run_ci_operational_gate.py `
  --include-backtest `
  --strict-execution-parity `
  --run-should-exit-parity-analysis `
  --refresh-should-exit-audit-from-logs `
  --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" `
  --should-exit-audit-live-runtime-log-mode-filter exclude_backtest `
  --strict-should-exit-parity
python scripts/run_verification.py --pipeline-version auto `
  --data-dir .\build\Release\data\backtest_real_live `
  --dataset-names upbit_KRW_BTC_1m_live.csv
```

## Validation artifacts (latest)
- OFF gate/parity:
  - `build/Release/logs/operational_readiness_report.json`
  - `build/Release/logs/execution_parity_report.json`
- OFF verification sample:
  - `build/Release/logs/verification_report_phase3_off_btc1m_swapped_tmp.json`
- ON verification sample (temporary runtime bundle/config swap):
  - `build/Release/logs/verification_report_phase3_on_btc1m_swapped_tmp.json`
- ON runtime log evidence (adaptive blend changes):
  - `build/Release/logs/autolife.log` (`ev_blend=0.190`, `ev_blend=0.330`)
- Final delivery package:
  - `docs/_codex/CH01_PHASE3_FINAL_DELIVERY_2026-02-25.md`

## Completion checklist
- [x] Phase3 OFF no-regression evidence recorded
- [x] Phase3 ON diagnostics/behavior evidence recorded
- [x] Operational gate pass
- [x] Strict parity pass
- [x] Final delivery summary includes required 6 sections
