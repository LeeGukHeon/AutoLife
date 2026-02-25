# Start-To-Exit Path Audit (Runtime-First)
Last updated: 2026-02-25

## Update (2026-02-25 cleanup wave B)
- Removed runtime direct probabilistic fallback path from both live/backtest.
- Removed foundation signal-supply fallback/minimal-probe path from base strategy.
- Removed manager soft-queue / probabilistic fastpass promotion path.
- Removed related config keys from `EngineConfig`/`Config.cpp`/`config/config.json`.

## Update (2026-02-25 cleanup wave C)
- Backtest manager prefilter path was aligned to live:
  - removed backtest-only hostile-market additive boost on manager `min_strength/min_expected_value`
  - removed backtest-only small-seed additive prefilter bump
- Hostility score computation was shared across live/backtest via `ExecutionGuardPolicyShared`.
- Foundation manager filter removed non-policy hardcoded relief branches
  (range-only supply relief / low-vol-low-liq relief / quality relief caps), keeping
  frontier + manager-policy-driven thresholds as primary.

## Scope
- Goal: verify actual runtime code path from startup to sell/risk-management before script-side work.
- Scope included: `src/main.cpp`, `src/app/*`, `src/runtime/*`, `src/strategy/*`, `src/risk/*`, `src/common/*`, `CMakeLists.txt`.
- Scope excluded: Python script behavior changes.

## Active binary boundary
- Primary runtime binary is `AutoLifeTrading` (`CMakeLists.txt:80`).
- Optional extra `.exe` targets are OFF by default:
  - `AUTOLIFE_BUILD_TOOL_BINARIES=OFF` (`CMakeLists.txt:29`)
  - `AUTOLIFE_BUILD_LIVE_PROBE_TOOL=OFF` (`CMakeLists.txt:30`)
  - `AUTOLIFE_BUILD_GATE_TESTS=OFF` (`CMakeLists.txt:31`)
  - `AUTOLIFE_BUILD_EXTRA_TESTS=OFF` (`CMakeLists.txt:32`)

## Live runtime flow (actual path)
1) Startup entry
- `main` -> CLI backtest fast path or interactive mode select (`src/main.cpp:41`, `src/main.cpp:59`, `src/main.cpp:87`, `src/main.cpp:90`)
- live interactive config build (`src/app/LiveModeHandler.cpp:76`)

2) Engine bootstrap
- ctor wiring (scanner/strategy/risk/order/core planes/model bundle) (`src/runtime/LiveTradingRuntime.cpp:1722`)
- start/load state/sync account/thread launch (`src/runtime/LiveTradingRuntime.cpp:1862`)
- main loop (`monitorPositions -> scanMarkets -> generateSignals -> learnOptimalFilterValue -> executeSignals`) (`src/runtime/LiveTradingRuntime.cpp:1935`)

3) Candidate generation path
- market prefilter scan (`src/runtime/LiveTradingRuntime.cpp:2185`)
- probabilistic snapshot infer + strategy collect + runtime adjustment + manager filter (`src/runtime/LiveTradingRuntime.cpp:2404`, `src/runtime/LiveTradingRuntime.cpp:2575`, `src/runtime/LiveTradingRuntime.cpp:2660`)
- probabilistic primary minimums and ranking:
  - minimum policy (`src/runtime/LiveTradingRuntime.cpp:998`, `src/runtime/LiveTradingRuntime.cpp:1121`)
  - ranking score (`src/runtime/LiveTradingRuntime.cpp:1048`)

4) Entry execution path
- execute gates + sizing + risk-manager admission + buy submit (`src/runtime/LiveTradingRuntime.cpp:2987`)
- buy execution path (`src/runtime/LiveTradingRuntime.cpp:3717`)

5) Sell and risk-management path
- position monitor (`src/runtime/LiveTradingRuntime.cpp:4137`)
- per position: update -> adaptive risk controls -> strategy exit -> TP1 partial -> risk-manager exit (`src/runtime/LiveTradingRuntime.cpp:4137`)
- full sell (`src/runtime/LiveTradingRuntime.cpp:4396`)
- partial sell (`src/runtime/LiveTradingRuntime.cpp:4603`)
- risk-manager core:
  - `shouldExitPosition` (`src/risk/RiskManager.cpp:384`)
  - `applyAdaptiveRiskControls` (`src/risk/RiskManager.cpp:662`)
  - `getAdaptivePartialExitRatio` (`src/risk/RiskManager.cpp:906`)

## Backtest runtime flow (actual path)
1) Init/load/run
- init (`src/runtime/BacktestRuntime.cpp:1450`)
- data load (`src/runtime/BacktestRuntime.cpp:1716`)
- run loop (`src/runtime/BacktestRuntime.cpp:1752`)

2) Candle processing and parity guard
- process candle (`src/runtime/BacktestRuntime.cpp:1841`)
- strict live-equivalent data window check path is explicit in backtest only (intended) (`src/runtime/BacktestRuntime.cpp:1894`)

3) Entry/exit parity path
- execution artifact lifecycle:
  - queue/submit (`src/runtime/BacktestRuntime.cpp:3073`)
  - fill/drain (`src/runtime/BacktestRuntime.cpp:3016`)

## Intentional Live/Backtest differences (keep)
- Backtest intrabar collision model (stop touched + TP touched same candle => conservative stop priority) (`src/runtime/BacktestRuntime.cpp:2094`)
- Backtest synthetic microstructure generation from candles for orderbook proxy (`src/runtime/BacktestRuntime.cpp:1979`)
- Backtest time override for deterministic replay (`src/runtime/BacktestRuntime.cpp:1843`)

These are expected differences for simulation correctness and should not be force-unified to live wall-clock behavior.

## Residual tuning inventory (active path)
1) Dynamic filter + hostility heuristic constants
- Live execution gate and scan profile still rely on many fixed constants (`src/runtime/LiveTradingRuntime.cpp:2987`, `src/runtime/LiveTradingRuntime.cpp:3042`, `src/runtime/LiveTradingRuntime.cpp:3174`, `src/runtime/LiveTradingRuntime.cpp:6316`).
- Backtest mirrors equivalent heuristic profile (`src/runtime/BacktestRuntime.cpp:2427`, `src/runtime/BacktestRuntime.cpp:2447`).

2) Legacy/pre-cat config surface
- `strategy_ev_pre_cat_*` surface has been removed from active config/runtime path:
  - parser no longer reads it
  - `config/config.json` no longer carries these keys

## Small cleanup applied during this audit
- removed unused local in risk pipeline:
  - `src/strategy/FoundationRiskPipeline.cpp`
- removed unused locals in live execution stage:
  - `src/runtime/LiveTradingRuntime.cpp`

## Verification after cleanup
- Build: `AutoLifeTrading` Release succeeded.
- Gate: `python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity --run-should-exit-parity-analysis --strict-should-exit-parity`
- Result:
  - `CIGate PASSED`
  - `ExecutionParity PASSED`
  - `ShouldExitParity verdict=pass`

## Recommended next cleanup order (if you want residual tuning removal next)
1) Move remaining hardcoded runtime thresholds into single bundle/policy source, then delete duplicated constants.
2) Continue trimming remaining hardcoded runtime constants in live/backtest scoring/profile gates.
