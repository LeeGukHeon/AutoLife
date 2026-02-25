# Phase 2 Closure Checkpoint (Parity-First)
Last updated: 2026-02-25

## Decision
- Phase 2 is closed for parity-readiness.
- Profitability optimization is explicitly deferred until Phase 3/4 completion and migration cleanup.

## Closure Criteria (Locked)
1. `operational gate` passes with backtest included.
2. `strict execution parity` passes.
3. `should-exit parity` passes under explicit parity boundary rules.
4. Live/backtest differences are classified as:
   - must-match decision semantics
   - allowed runtime/environment differences

## Locked Parity Boundary
- Must match:
  - `STOP_LOSS`, `TAKE_PROFIT_*`, `STRATEGY_OR_RISK_EXIT` decision surface.
  - normalized execution lifecycle surface (`side/event/status`).
- Allowed:
  - `BACKTEST_EOD` (backtest-only session close).
  - runtime transport differences (`ts_ms`, `source`, `order_id`).
- Additional TP handling:
  - `TAKE_PROFIT_2` and `TAKE_PROFIT_FULL_DUE_TO_MIN_ORDER` are treated as the same final take-profit family.
  - If live has `TAKE_PROFIT_FULL_DUE_TO_MIN_ORDER` while backtest has `TAKE_PROFIT_1` + `TAKE_PROFIT_FINAL`, TP1 is treated as collapsed to full exit under min-order constraints.

## Repro Command (Phase 2 Baseline)
```powershell
python scripts/run_ci_operational_gate.py `
  --include-backtest `
  --strict-execution-parity `
  --run-should-exit-parity-analysis `
  --refresh-should-exit-audit-from-logs `
  --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" `
  --should-exit-audit-live-runtime-log-mode-filter exclude_backtest `
  --strict-should-exit-parity
```

## Latest Evidence (2026-02-25)
- Command run completed: `CIGate PASSED`
- Artifacts:
  - `build/Release/logs/operational_readiness_report.json`
    - `generated_at=2026-02-25T07:32:06.869282+00:00`
    - all readiness checks `true` with `strict_execution_parity=true`.
  - `build/Release/logs/execution_parity_report.json`
    - `generated_at=2026-02-25T07:32:03.516174+00:00`
    - normalized comparison ready.
  - `build/Release/logs/strategyless_exit_audit_5set_20260225_v25_skip_primary.json`
    - `generated_at_utc=2026-02-25T07:32:03.495244+00:00`
    - live snapshot observed exits: `stop_loss=4`, `take_profit_full_due_to_min_order=2`.
  - `build/Release/logs/should_exit_parity_report.json`
    - `generated_at_utc=2026-02-25T07:32:03.508188+00:00`
    - `verdict=pass`
    - `critical_findings=0`
    - allowed findings include:
      - `allowed_reason_difference` (`BACKTEST_EOD`)
      - `allowed_tp1_collapsed_to_full_due_to_min_order`

## Handoff
- Phase 3 start point:
  - finalize OFF/ON evidence package and required 6-section delivery summary.
- Phase 4 prep:
  - portfolio-level capital allocation/exposure controls behind default-OFF flags.
