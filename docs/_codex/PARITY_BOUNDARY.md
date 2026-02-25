# Live/Backtest Parity Boundary
Last updated: 2026-02-25

## Goal
- Keep decision-surface parity between live and backtest.
- Allow runtime transport differences that do not change decision semantics.

## Must Match
- Entry/exit/`should_exit` decision semantics.
- Core exit reason surface on overlap markets:
  - `STOP_LOSS`
  - `TAKE_PROFIT_*`
  - `STRATEGY_OR_RISK_EXIT`
- Execution lifecycle shape (`side`, `event`, `status`) under normalized comparison.

## Allowed Differences
- `BACKTEST_EOD` (backtest-only session close).
- Runtime transport details (`ts_ms`, `source`, `order_id`).
- Low-sample/no-overlap states (`inconclusive`).

## TP1 Handling (2026-02-25)
- If TP1 is unobservable in live logs, validator supports two non-blocking paths.
- Path A (automatic): `allowed_tp1_collapsed_to_full_due_to_min_order`
  - live has `TAKE_PROFIT_FULL_DUE_TO_MIN_ORDER`
  - backtest has `TAKE_PROFIT_1` + `TAKE_PROFIT_FINAL`
  - interpreted as TP1 collapse under min-order constraints.
- Path B (policy override): `--tp1-unobservable-policy pass`
  - use only when you need strict gate progress before TP1-observable logs are available.

## Commands
```powershell
python scripts/validate_execution_parity.py -Strict

python scripts/compare_execution_event_distribution.py `
  --output-json build/Release/logs/execution_event_distribution_comparison_ci.json

python scripts/validate_should_exit_parity.py `
  --audit-json build/Release/logs/strategyless_exit_audit_latest.json `
  --output-json build/Release/logs/should_exit_parity_report.json

python scripts/validate_should_exit_parity.py `
  --audit-json build/Release/logs/strategyless_exit_audit_latest.json `
  --output-json build/Release/logs/should_exit_parity_report.json `
  --tp1-unobservable-policy pass

python scripts/run_ci_operational_gate.py `
  --include-backtest `
  --strict-execution-parity `
  --run-should-exit-parity-analysis `
  --refresh-should-exit-audit-from-logs `
  --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" `
  --should-exit-audit-live-runtime-log-mode-filter exclude_backtest `
  --strict-should-exit-parity

python scripts/run_ci_operational_gate.py `
  --include-backtest `
  --strict-execution-parity `
  --run-should-exit-parity-analysis `
  --refresh-should-exit-audit-from-logs `
  --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" `
  --should-exit-audit-live-runtime-log-mode-filter exclude_backtest `
  --should-exit-tp1-unobservable-policy pass `
  --strict-should-exit-parity
```

## Artifacts
- `build/Release/logs/execution_parity_report.json`
- `build/Release/logs/execution_event_distribution_comparison_ci.json`
- `build/Release/logs/strategyless_exit_audit_latest.json`
- `build/Release/logs/should_exit_parity_report.json`
