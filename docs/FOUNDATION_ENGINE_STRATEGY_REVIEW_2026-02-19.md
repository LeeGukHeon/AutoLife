# Foundation Review: Engine/Strategy/Main (2026-02-19)

## Why this review
- Verification accuracy cannot improve if core execution logic is structurally hard to reason about.
- Current bottleneck is not only tuning policy, but codebase coupling and oversized entry/runtime flows.

## Current structural facts
- `src/main.cpp`: reduced to 81 lines, responsibilities now mode selection + delegation only.
- delegated modules in `app/*`:
  - `BacktestReportFormatter` (result print formatting)
  - `BacktestCliHandler` (`--backtest` parsing/execution/json output)
  - `BacktestInteractiveHandler` (interactive backtest setup/execution)
  - `LiveModeHandler` (interactive live setup + engine bootstrap)
- shared market-data window policy added:
  - `include/common/MarketDataWindowPolicy.h`
  - scanner/live/backtest now share TF caps and live-equivalent minimum window checks
- Runtime files are very large:
  - `src/runtime/LiveTradingRuntime.cpp` 7500+ lines
  - `src/runtime/BacktestRuntime.cpp` 5500+ lines
- Verification scripts were also monolithic:
  - `scripts/run_candidate_auto_improvement_loop.py` 3200+ lines
  - `scripts/run_profitability_matrix.py` 1500+ lines

## Risk interpretation
1. Behavior drift risk:
- Small policy edits can cause hidden side effects due to high coupling.
2. Validation ambiguity risk:
- Output interpretation depends on many optional branches and flags.
3. Handoff risk:
- Context reset makes it expensive to recover exact progress state.

## Reset principles
1. Minimal deterministic verification path first (`verify_baseline.py` -> `run_verification.py`).
2. Main entrypoint thinning before algorithm tuning expansion.
3. Runtime decomposition by domain:
   - signal generation
   - risk gating
   - execution decision
   - reporting
4. Chapter-based progress docs for context reset resilience.

## Immediate actions started
- Added minimal verification entrypoint:
  - `scripts/verify_baseline.py`
  - `scripts/run_verification.py`
- Continued `main.cpp` responsibility split:
  - moved repeated backtest console reporting into `src/app/BacktestReportFormatter.cpp`
  - moved CLI backtest parsing/execution/json output into `src/app/BacktestCliHandler.cpp`
  - moved interactive backtest setup/execution into `src/app/BacktestInteractiveHandler.cpp`
  - moved interactive live setup/engine bootstrap into `src/app/LiveModeHandler.cpp`
- Added root-cause data parity guard:
  - unified TF window constants in scanner/live/backtest
  - live signal generation now rejects insufficient parity windows
  - strict live-equivalent parity mode in backtest when companion set is present
- Added chapter docs:
  - `docs/CHAPTER_CURRENT.md`
  - `docs/CHAPTER_HISTORY_BRIEF.md`
- Started strategy/risk rebuild from baseline:
  - added `FoundationRiskPipeline` (decomposed manager prefilter rules)
  - connected `StrategyManager::filterSignalsWithDiagnostics` to the new rule pipeline first

## Next structural actions
1. Runtime split phase-1:
   - extract entry-funnel/reporting serialization from runtime core logic
2. Verification migration:
   - keep legacy scripts frozen
   - route user-facing commands to baseline path
3. Remove dead legacy prefilter block after parity check:
   - keep only the new foundation pipeline path in `StrategyManager`

