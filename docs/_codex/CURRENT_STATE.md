# Current State
Last updated: 2026-02-24

## Repository
- Branch: `main`
- Commit snapshot: `2fe30ab3b901bf4d7d702a7e110904f3d96225c8`

## Active ticket
- Source of truth: `docs/_codex/ACTIVE_TICKET.md`
- Current: `SO4-12-DAILY-OOS-20260224` (`Strict-Order-4-12`, Mode `A`, status `done`)

## Master ticket progress (0-7)
- Ticket 0 (Codex bootstrap pack): implemented; this turn is alignment follow-up.
- Ticket 1 (dynamic universe + scope-aware 1m): implemented.
- Ticket 2 (purged walk-forward + embargo): implemented.
- Ticket 3 (conditional cost model): implemented.
- Ticket 4 (regime spec hardening): implemented.
- Ticket 5 (uncertainty ensemble): implemented.
- Ticket 6 (event sampling): implemented.
- Ticket 7 (MODE B / v2): partially implemented behind strict pipeline guards.

## Contract hashes
- `config/model/probabilistic_feature_contract_v1.json`
  - `c0c8f87df95be43ce6dd9526ed9c31b674e211bf187b365f2022439e68f1084e`
- `config/model/probabilistic_feature_contract_v2.json`
  - `8dc014f9a52305dc26bce0b6c8cc7a10cb3047ddfeb725aad09f62a8140704f6`
- `config/model/probabilistic_runtime_bundle_v1.json`
  - `fddbfe3514e1e2d726c5c6cd58b6105fda50459c038ae283acb5af14aacdba84`
- `config/model/probabilistic_runtime_bundle_v2.json`
  - `d9f2749382e931488f4d0e2fa03f367db4effc180f846d232a83816c7fbfdc3e`
- `data/backtest_probabilistic/probabilistic_bundle_manifest.json`
  - `6c6568cbfc5349332e1899fb9ac154d3f372de09ccd51ad8d24e91a75591f89a`

## Latest gate snapshot (v1)
- Verification report:
  - `build/Release/logs/verification_report_global_full_5set_refresh_20260224_step6l_downtrend_rebound_imbalance_relax_v1.json`
- Result:
  - `overall_gate_pass=true`
  - `adaptive_verdict=pass`
  - `avg_profit_factor=3.0789`
  - `avg_expectancy_krw=18.5147`
  - `avg_total_trades=10.0`
  - `candidate_generation.no_signal_generated share=0.7019`
  - `baseline_comparison.non_degradation_contract.all_pass=true`

## This turn additions
- Added day-sliced OOS helper:
  - `scripts/run_daily_oos_stability.py`
  - `scripts/test_daily_oos_stability.py`
- Documentation wiring:
  - `docs/30_VALIDATION_GATES.md` (Gate3 supplement command + pass criteria)
  - `docs/11_BASELINE_RUNBOOK.md` (verification-stage command)
- Smoke artifact:
  - `build/Release/logs/daily_oos_stability_report_smoke_20260224.json`
  - `build/Release/logs/daily_oos_stability_windows_smoke_20260224.csv`
  - result: `status=fail`, blocker=`max_nonpositive_day_ratio`, `evaluated_day_count=5`
- Multi-market evidence run:
  - `build/Release/logs/daily_oos_stability_report_3m_7d_20260224_fix2.json`
  - `build/Release/logs/daily_oos_stability_windows_3m_7d_20260224_fix2.csv`
  - result:
    - `status=fail` (fail-closed)
    - `evaluated_day_count=15`
    - `nonpositive_day_ratio=1.0` (threshold `0.45` fail)
    - `total_profit_sum=-5118.106841` (positive-sum fail)
    - `peak_day_drawdown_pct=5.483792` (threshold `12.0` pass)
  - loss-tail concentration (`aggregate_loss_cells`):
    - `RANGING|CORE_RESCUE_SHOULD_ENTER`
    - `TRENDING_UP|CORE_RESCUE_SHOULD_ENTER`
    - `RANGING|PROBABILISTIC_PRIMARY_RUNTIME`

## Live safety status
- `allow_live_orders=false` default maintained.
- Gate4 shadow report and staged live enable remain required before any live enable attempt.

## Known gaps
- Need loss-tail mitigation on dominant cells before daily OOS guard can pass (`max_nonpositive_day_ratio`, `positive_profit_sum`).
- `CURRENT_STATE.md` must stay concise; detailed experiment history belongs in stage TODO/history docs.

## Detailed history references
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/PROBABILISTIC_EXECUTION_ROADMAP_2026-02-21.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
