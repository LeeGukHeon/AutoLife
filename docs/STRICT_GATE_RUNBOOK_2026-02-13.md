# Strict Gate Runbook (Roadmap Locked, 2026-02-27)

## Purpose
- This document is the operational single source of truth for Stage B to Stage F execution.
- Keep only the actionable roadmap content needed for repeatable execution across contexts.
- Historical A1-A22 implementation logs are intentionally trimmed from this runbook.

## Build Path Lock (Windows)
- Use CMake from `D:\MyApps\vcpkg` explicitly.
- Locked CMake path:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe`
- Release build command:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release --target AutoLifeTrading`
- Build evidence outputs:
  - `build/Release/logs/build_log_release.txt`
  - `build/Release/logs/binary_version_stamp.txt`
  - `build/Release/logs/binary_smoke_60s.log`

## 0) Hard Lock (Always-On)

### Runtime Config
- Runtime source of truth (single source): `build/Release/config/config.json`
- Root config must be synchronized every run:
  - `config/config.json == build/Release/config/config.json`

### Bundle
- `config/model/probabilistic_runtime_bundle_v2_a19_margin_observe_only_step1.json` (default Paper/Backtest bundle)
- `edge_semantics="net"` is required at bundle root.
- Stage C v2 lock:
  - `phase3.regime_entry_disable.RANGING=true`
  - non-RANGING regimes remain `false`
- Stage F10 lock:
  - `phase3.exit.strategy_exit_mode="disabled"`
  - `phase3.exit.be_after_partial_tp_delay_sec=120`
  - `phase3.exit.tp_distance_trending_multiplier=1.10` (TRENDING_UP/DOWN only, Stage F10)
  - `phase3.risk.enabled=true`
  - `phase3.risk.stop_loss_trending_multiplier=1.15` (F8 baseline restored in F10)

### Backtest
- Split manifest: `build/Release/logs/time_split_manifest_r21_prefix.json`
- Dataset root: `data/backtest_real` (6 markets)
- Prewarm: `168h`
- Execution/evaluation split: `execution_range = prewarm + core`, `evaluation_range = core`
- `split_applied=true` is mandatory with report evidence.

### Backtest Dataset Lock (Regression + Fresh)
- Purpose:
  - Keep `data/backtest_real` as immutable regression baseline.
  - Generate separate fresh datasets for drift checks.
  - Always run `Regression -> Fresh` in the same config/bundle and compare directional consistency.
- Dataset tiers:
  - Regression (immutable): `data/backtest_real`
  - Fresh primary (immutable snapshot): `data/backtest_fresh_14d`
  - Fresh auxiliary (hotfix-only): `data/backtest_fresh_5d`
- Folder layout (fixed):
  - `data/backtest_real/KRW-*/ohlcv_1m.csv`
  - `data/backtest_fresh_14d/meta.json`
  - `data/backtest_fresh_14d/KRW-*/ohlcv_1m.csv`
  - `data/backtest_fresh_5d/meta.json`
  - `data/backtest_fresh_5d/KRW-*/ohlcv_1m.csv`
- Market set lock:
  - Fresh must use the same market list as regression.
  - Default set: `KRW-BTC, KRW-ETH, KRW-XRP, KRW-SOL, KRW-DOGE, KRW-ADA`.
  - Changing market set invalidates regression-vs-fresh comparability.
- File format lock:
  - Filename: `ohlcv_1m.csv` per market directory.
  - Required columns: `ts_ms, open, high, low, close, volume`.
  - Timestamp unit: milliseconds (`ts_ms`), not seconds.
  - Ordering: `ts_ms` ascending, no duplicate `ts_ms` (if duplicated, keep the latest one).
  - Missing candles are not synthesized (no forward-fill insertion).
- Fresh generation window lock:
  - Anchor time is KST midnight at generation date.
  - `end_ts = today(KST) 00:00:00`.
  - `start_ts = end_ts - window_days` (`14d` or `5d`).
  - After generation, snapshot folder is immutable for reproducibility.
- Fresh metadata (`meta.json`) is mandatory:
  - Required keys: `dataset_name, created_at_kst, window_days, start_ts_ms, end_ts_ms, markets, granularity, schema, source, hashes`.
  - `hashes` must include per-market `rows` and `sha256`.
  - Example schema:
```json
{
  "dataset_name": "backtest_fresh_14d",
  "created_at_kst": "2026-02-28T00:05:00+09:00",
  "window_days": 14,
  "start_ts_ms": 1769990400000,
  "end_ts_ms": 1771200000000,
  "markets": ["KRW-BTC", "KRW-ETH", "KRW-XRP", "KRW-SOL", "KRW-DOGE", "KRW-ADA"],
  "granularity": "1m",
  "schema": ["ts_ms", "open", "high", "low", "close", "volume"],
  "source": "upbit_ohlcv",
  "hashes": {
    "KRW-BTC": {"rows": 20160, "sha256": "<...>"},
    "KRW-ETH": {"rows": 20160, "sha256": "<...>"}
  }
}
```
- Hash/version lock:
  - If any market hash changes, treat as a new snapshot.
  - Use a new folder name when needed, for example `backtest_fresh_14d_YYYYMMDD`.
- Split and eval lock (common):
  - Keep split manifest `time_split_manifest_r21_prefix.json`.
  - Keep `prewarm=168h`, `execution_range=prewarm+core`, `evaluation_range=core`.
  - If fresh core is too short and causes zero trades, extend only core evaluation window. Do not tune policy/threshold for this.
- Fixed execution order:
  - Run 1: Regression (`data/backtest_real`).
  - Run 2: Fresh (`data/backtest_fresh_14d`) with identical bundle/config.
  - Compare directionality of `expectancy/PF`. Opposite direction must be recorded as drift/overfit warning.
- Prohibited operations:
  - Never overwrite `data/backtest_real`.
  - Never refresh fresh dataset in place for every run.
  - Never change market set between regression and fresh.
  - Never change timestamp unit.
  - Never insert missing candles via forward fill.

### Safety
- `allow_live_orders=false`
- `live_paper_use_fixed_initial_capital=true`
- `live_paper_fixed_initial_capital_krw=200000`

### Semantics
- `edge_semantics = net`
- throughput source: `core_filled` (`protocol_split.total_trades_core_effective`)
- negative EV block: ON (`signal.expected_value < 0`)
- strict parity mode: `warning_hold`

### Code Version Policy
- Commit hash is excluded from hard lock.
- Code version is managed by `HEAD at run time + runbook snapshot`.
- Commit hash is recorded only in experiment logs/artifacts.
- Current semantic code tag: `CODE_VERSION: StageF10_tp_distance_trending_1p10_sl_restore_1p15`
- Optional tracking:
  - semantic code tag (for example, `CODE_VERSION: StageB_semantics_lock`)
  - `git describe --tags` in logs (not a hard-lock condition)

## 1) Target State (Final)
- G1: Promotion gate remains `rollback=false` + `overall_pass=true`.
  - Paper allowed with strict parity `warning_hold`.
  - Live is blocked until strict parity is resolved.
- G2: Paper 24-72h realized expectancy converges to around `>= 0`.
  - Must be reproducible at least 2 times on different days.
- G3: Regime separation is implemented as a mode switch (or smooth budget multiplier), not strategy proliferation.
  - Keep the single-axis-change principle to prevent overfitting.

## 2) Execution Loop Rules (Absolute)
- R0: One round changes exactly one axis.
  - No multi-axis tuning in one round.
  - Candidate count must stay small (`K<=2`, max `K<=3` for sweep).
- R1: If baseline/hard lock is broken, stop and reset immediately.
  - Split/dataset/bundle/config mismatch invalidates the round.
- R2: Quarantine is evaluation-only.
  - Distribution/limits/history estimation must come from Dev/Val only.
- R3: Invariance checks are mandatory.
  - Diagnostic tracks (A8-0/A9-0 and similar) must not alter `candidate_total`/`total_trades`.

## 2.1) Legacy Cleanup Principles (Refactor-Only Rounds)
- Do not mix feature changes with refactor changes.
  - Stage B rounds permit behavior-preserving edits only.
- Refactor rounds allow move/rename/organization only.
  - Logic/threshold/default numeric changes are next rounds.
- Deletion is allowed only when usage count is proven zero.
  - Verify with grep/report/telemetry evidence first.
- Legacy removal uses kill-switch first, then deletion after 1-2 safe rounds.
- Cleanup scope is limited to one module per round (max 1-2 files).

### Cleanup Checklist
- hard lock (`code/bundle/config/dataset/split`) unchanged
- baseline repro run1/run2 key counters unchanged
- stage funnel (`S0~S5`) unchanged
- promotion gate result unchanged (`hold/rollback/top_failed_reasons`)
- `semantics_lock_report.json` is `OK`
- commit note explicitly states `refactor-only` and no numeric logic change

## 3) Stage Map (A -> F)
- Stage A: Consistency/gate/aggregation normalization (completed, recheck only).
- Stage B: Cost/label/edge semantics alignment (highest priority now).
- Stage C: First regime mode-switch (no new strategy).
- Stage D: Optional ranging-only strategy or model split.
- Stage E: Paper 24-72h promotion and Live readiness (after strict parity fix).
- Stage F: Entry/exit structure deep-dive and single-axis exit hardening.

Current focus (2026-03-01): Stage F10 (`TRENDING TP distance multiplier 1.10`, stop-loss multiplier restored to 1.15 baseline).

## Stage F10 Work Log (2026-03-01)
- Baseline restore (hard-lock recovery): `phase3.risk.stop_loss_trending_multiplier 1.25 -> 1.15`.
- Single-axis experiment: `phase3.exit.tp_distance_trending_multiplier 1.00 -> 1.10`.
- Validation + Quarantine rerun with same Stage F8 output format; comparison baseline remains Stage F8.

## Stage F9 Work Log (2026-03-01)
- Single-axis change only: `phase3.risk.stop_loss_trending_multiplier 1.15 -> 1.25`.
- No change to entry/EV/gates/partial/trailing/BE/strategy_exit/semantics policies.
- Validation + Quarantine rerun required with same F8 artifact format, compared against F8.

## 4) Stage A Recheck (Already Implemented)
- A1 split real separation applied in `run_verification` main loop.
- A2 execution/evaluation split fixed (`prewarm+core` vs `core`).
- A3 A8-0 coverage=0 resolved (`candle_series` fallback).
- A4 A9-1 negative EV block.
- A5 A10-3 EV SSoT (`mismatch=0`).
- A6 A13 throughput dynamic gate.
- A7 A15 strict parity `warning_hold`.
- A8 A20/A21 trade aggregation consistency + throughput source `core_filled`.

### Stage A Required Artifacts
- `baseline_metrics_summary.json`
- `baseline_repro_check.json`
- `promotion_gate_result_after_a21.json`
- `a21_throughput_source_debug.json`

### Stage A Pass Criteria
- Promotion gate:
  - `action=hold_for_strict_parity`
  - `rollback=false`
  - `top_failed_reasons=[]`
- Funnel:
  - backtest core `S4/S5 (orders/trades) > 0`

## 5) Stage B (Top Priority): Cost/Label/Edge Semantics Alignment

### Confirmed Background
- Training label `label_edge_bps_h5` currently uses:
  - `close_t -> close_(t+5)` return minus `12bps` roundtrip cost (gate4).
- Runtime had `root.cost_model.enabled=false` in A15 path.
- Runtime logs use `expected_edge_calibrated`.
- If runtime cost is additionally subtracted without semantic lock, double subtraction risk appears.

### Stage B Goal
- Lock one semantic definition for edge regressor output: gross or net.
- Align label cost model and runtime cost model as isomorphic semantics.
- Only after semantic lock, proceed to regime mode-switch decisions.

### B1) Semantic Decision (Document and Lock One)
- Option `B1-NET` (recommended, minimum change):
  - Edge is NET (cost included).
  - Training label: `return - cost`.
  - Runtime: treat `expected_edge_calibrated` as NET; do not subtract extra runtime cost.
- Option `B1-GROSS`:
  - Edge is GROSS (cost excluded).
  - Training label: raw return.
  - Runtime: subtract runtime cost from calibrated edge.

Current recommendation: choose `B1-NET` because gate4 training is already NET (`-12bps`).

### B2) Isomorphic Runtime Application (Single Axis)
- Match `cost_model.enabled` and `phase3.cost_model.enabled` behavior to the selected semantic.
- Ensure `c_entry/c_exit/c_tail` traces in A16/A19-style runs follow the locked definition.
- Output artifact: `semantics_lock_report.json`

### B3) Retrain Decision (Optional)
- If persistent negative edge bias remains after semantic lock:
  - likely drift between training period (2024-02~03) and 2026 runtime.
- Retrain must still follow one-axis rule:
  - either expand training period OR add market-id feature, not both in one round.

### Stage B Required Artifacts
- `semantics_lock_report.json`
- `runtime_cost_semantics_debug.json` (A16/A19 baseline, 50 samples)
- `retrain_plan.md` (only if retraining is needed)

## 6) Stage C: Regime Split (Mode Switch v1, No New Strategy)

### Confirmed Background
- A16: RANGING had about `P~0.51`, margin around `+1%p`, `edge_cal` negative `35/35`, realized PnL negative.
- Regime split is allowed, but strategy proliferation is prohibited.

### Stage C Goal
- Reduce directional trading exposure in RANGING through the safest minimal intervention.
- Preserve single-axis round rule.

### C1) Select One Mode-Switch Mechanism
- `C1-1 budget multiplier` (recommended first):
  - RANGING `capital_multiplier = 0.0~0.3`
  - TRENDING `capital_multiplier = 1.0`
  - Stage C v1 lock:
    - `phase4.risk_budget.regime_budget_multipliers.RANGING = 0.30`
    - `phase4.risk_budget.regime_budget_multipliers.TRENDING_UP = 1.00`
    - `phase4.risk_budget.regime_budget_multipliers.TRENDING_DOWN = 1.00`
    - `phase4.risk_budget.regime_budget_multipliers.HIGH_VOLATILITY = 1.00`
    - `phase4.risk_budget.regime_budget_multipliers.UNKNOWN = 1.00`
- `C1-2 required_ev_add` (alternative):
  - Increase required EV in RANGING to suppress entry.
- `C1-3 hard disable` (last resort):
  - Fully disable RANGING entry.

### C2) Apply Single-Axis Round
- Apply exactly one mechanism only.
- Output artifact set:
  - `stageC_mode_switch_config.json`
  - before/after reports

### C3) Paper Expansion
- First `4h` structural check.
- If stable, extend to `24h` then `72h`.

### Stage C Success Criteria
- Lower RANGING entry share.
- Realized expectancy improves toward/above zero.
- Stop-loss share decreases (especially in RANGING).

## 7) Stage D v1 (Single Axis): RANGING Shadow Mode
- Keep `RANGING` real entry disabled (risk 0) but log deterministic shadow events per candidate.
- No strategy addition and no gate threshold retuning in this round.
- Required outputs:
  - `build/Release/logs/ranging_shadow_signals.jsonl`
  - `build/Release/logs/ranging_shadow_run_meta.json`
  - `build/Release/logs/verification_report_stageD_v1_quarantine_exec_eval.json`
  - `build/Release/logs/stageD_v1_effect_summary.json`
- Required telemetry:
  - `shadow_count_total`
  - `shadow_count_by_regime` (`RANGING` only expected in v1)
  - `shadow_count_by_market`
  - `shadow_would_pass_frontier_count`
  - `shadow_would_pass_execution_guard_count`
  - optional: `shadow_edge_neg_count`, `shadow_edge_pos_count`
- Validation/paper follow-up is separate:
  - Validation x1 (recommended)
  - Paper 20m smoke (recommended, still `allow_live_orders=false`)
- Stage D v2 remains locked until v1 evidence is accumulated.

## 8) Stage E Operations Mode (Integrated)

### E0) Decision Lock (Must Stay Fixed in Runbook)
- Stage D v2 decision is locked as `NO_GO_KEEP_OFF`.
- Policy lock:
  - RANGING real orders stay OFF.
  - RANGING shadow logging stays ON.
  - TRENDING_UP/TRENDING_DOWN are the only real-order path candidates.
- Required evidence:
  - `build/Release/logs/stageD_v2_shadow_eval/stageD_v2_choice.json`
  - explicit runbook statement for RANGING OFF policy (this section).

### E1) Backtest Validation + Quarantine (1 run each, required)
- Purpose:
  - verify that real trades can still happen under TRENDING-only execution path,
  - verify split hard lock evidence remains intact.
- Required checks:
  - `split_applied=true`,
  - `total_trades_core_effective > 0` preferred (if 0, treat as market mostly RANGING and move to single-axis easing round),
  - shadow counters continue increasing while RANGING real entry remains blocked.
- Required artifacts:
  - `build/Release/logs/verification_report_E1_validation.json`
  - `build/Release/logs/verification_report_E1_quarantine.json`
  - `build/Release/logs/E1_effect_summary.json`

### E2) Paper TRENDING-only Soak (4-8h, recommended)
- Keep safety lock fixed:
  - `allow_live_orders=false`,
  - `live_paper_fixed_initial_capital_krw=200000`.
- Required KPIs:
  - total trades, trades/hour,
  - realized pnl, realized expectancy,
  - exit reason breakdown,
  - regime distribution (`RANGING real trades=0`),
  - shadow count growth continuity.
- Required artifacts:
  - `build/Release/logs/paper_run_summary_E2_8h.json`
  - `build/Release/logs/paper_gate_funnel_breakdown_E2.json`
  - `build/Release/logs/paper_top_loss_markets_cells_E2.json`
  - `build/Release/logs/ranging_shadow_signals.jsonl` (append mode)

### E3) Strict Parity Fix Track (Live prerequisite)
- Current lock remains `strict_parity=warning_hold` for Paper.
- Live promotion is blocked until mismatch is resolved.
- Required artifacts:
  - `docs/strict_parity_fix_plan.md`
  - `build/Release/logs/strict_parity_diagnosis_after_fix.json`

### E4) Live Promotion Path (only after E3 pass)
- Step-up schedule:
  - Live 10% for 24h,
  - Live 30% for 24h,
  - Live 100%.
- Keep `RANGING real-entry OFF` and shadow accumulation ON during all promotion steps.

### E5) Long-horizon Re-evaluation (Phase D v3 gate)
- While RANGING shadow mean edge remains negative, keep RANGING real-entry OFF.
- Run Stage D2-0 shadow evaluation periodically (recommended 6-12 week cadence).
- Re-open RANGING strategy discussion only when:
  - mean edge approaches non-negative, or
  - stable positive cluster appears in limited bucket/market scope.

## 9) Round Output Format (Fixed)
Use the same report template every round:

### Hard Lock
- `bundle_path`
- `runtime_config_path`
- `dataset_root`
- `split_manifest`
- `prewarm_hours`

### Before / After
- Core funnel `S0~S5`
- `trades/hour` (Paper) or `total_trades_core_effective` (Backtest)
- `avg_expectancy_krw`, `PF`, `tail_concentration`
- `top_loss_markets/cells`
- Promotion gate `top_failed_reasons` Top3

### Artifacts
- `verification_report_*.json`
- `promotion_gate_result_*.json`
- `effect_summary_*.json`
- runbook update (`docs/STRICT_GATE_RUNBOOK_2026-02-13.md`)

## 10) Immediate Next 3 TODO (Current State)
1. Finalize E1 summary from latest Validation/Quarantine runs.
   - Confirm `split_applied=true` in both reports.
   - Confirm RANGING shadow accumulation and `RANGING real-entry OFF` evidence.
2. Run E2 Paper TRENDING-only soak (`4-8h`) under fixed safety lock.
   - Collect KPI artifacts and verify no RANGING real orders.
3. Continue strict parity fix track in parallel.
   - Keep Live blocked until hold is resolved.

## 11) Stage E Execution Order (Current Hard-Lock)
### Step 1) Build/Link Verification (Required)
- Build Release with locked CMake path from `D:\MyApps\vcpkg`.
- Confirm:
  - build succeeds,
  - `build/Release/AutoLifeTrading.exe` exists,
  - 60s smoke run exits without crash/assert.
- Required artifacts:
  - `build/Release/logs/build_log_release.txt`
  - `build/Release/logs/binary_version_stamp.txt`
  - `build/Release/logs/binary_smoke_60s.log`

### Step 2) E1 Backtest Pair (Validation + Quarantine, Required)
- Run exactly one `validation` and one `quarantine` with split hard lock.
- Confirm:
  - `split_applied=true`,
  - split execution/evaluation ranges match manifest core rules,
  - `E1_effect_summary.json` is updated with pass/fail notes and counters.

### Step 3) E2 Paper TRENDING-only Soak (4-8h)
- Keep `allow_live_orders=false` and fixed paper capital.
- Verify:
  - RANGING real orders remain `0`,
  - shadow counts continue to grow,
  - TRENDING real-trade flow is observable (if market allows).

### Step 4) Strict Parity Fix Track (Parallel, Live Blocker)
- Diagnose and resolve `cli_strict_parity_pass_flag` mismatch.
- Live remains blocked until strict parity hold is cleared.

### Step 5) Live Promotion (After Step 4 Pass Only)
- Promotion sequence: `10% -> 30% -> 100%`, minimum `24h` stability each.
- Keep Stage E policy lock (`RANGING OFF + shadow ON`) unchanged during promotion.

