# Strict Gate Runbook (Roadmap Locked, 2026-02-27)

## Purpose
- This document is the operational single source of truth for Stage B to Stage E execution.
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

### Backtest
- Split manifest: `build/Release/logs/time_split_manifest_r21_prefix.json`
- Dataset root: `data/backtest_real` (6 markets)
- Prewarm: `168h`
- Execution/evaluation split: `execution_range = prewarm + core`, `evaluation_range = core`
- `split_applied=true` is mandatory with report evidence.

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

## 3) Stage Map (A -> E)
- Stage A: Consistency/gate/aggregation normalization (completed, recheck only).
- Stage B: Cost/label/edge semantics alignment (highest priority now).
- Stage C: First regime mode-switch (no new strategy).
- Stage D: Optional ranging-only strategy or model split.
- Stage E: Paper 24-72h promotion and Live readiness (after strict parity fix).

Current focus (2026-02-27): Stage A mostly complete. Stage B to Stage E pending.

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
- `C1-2 required_ev_add` (alternative):
  - Increase required EV in RANGING to suppress entry.
- `C1-3 hard disable` (last resort):
  - Fully disable RANGING entry.

### C2) Apply Single-Axis Round
- Apply exactly one mechanism only.
- Output artifact set:
  - `aC_mode_switch_config.json`
  - before/after reports

### C3) Paper Expansion
- First `4h` structural check.
- If stable, extend to `24h` then `72h`.

### Stage C Success Criteria
- Lower RANGING entry share.
- Realized expectancy improves toward/above zero.
- Stop-loss share decreases (especially in RANGING).

## 7) Stage D (Optional): RANGING Strategy or Model Split
- Only start Stage D after Stage B semantic lock and Stage C loss reduction confirmation.
- D1: Add RANGING mean-reversion strategy (last, highest complexity).
- D2: Train separate TRENDING and RANGING models (evaluate market-id feature in single-axis rounds).
- D3: Keep trend-focused architecture unchanged (safest implementation path).

## 8) Stage E: Paper -> Live Readiness (After Strict Parity Fix)
- E1: Paper 24-72h success repeated twice.
- E2: Execute `docs/strict_parity_fix_plan.md`.
  - Resolve `cli_strict_parity_pass_flag` mismatch if confirmed as real bug.
  - Live remains blocked until resolved.
- E3: Live promotion in steps:
  - `10% -> 30% -> 100%`
  - minimum `24h` stable at each step.

## 9) Round Output Format (Fixed)
Use the same report template every round:

### Hard Lock
- `code_hash`
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
1. Stage B start: document and lock edge semantics (`NET` vs `GROSS`).
   - Current gate4 training is NET (`-12bps`), so `B1-NET` is recommended.
2. Stage B execution: unify `cost_model.enabled` / `phase3.cost_model.enabled` to the selected semantic.
   - Avoid accidental `{}` defaults that break semantic intent.
   - If switching to gross, retraining labels must be changed first.
3. Stage C first round: apply budget multiplier in RANGING (no strategy addition).
   - A16 showed high RANGING share with loss structure; one mode-switch axis is the safest entry point.

## 11) Post-Stage-B Execution Order (Current Hard-Lock)
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

### Step 2) Baseline Repro Check (Required, Quarantine x1)
- Run `run_verification.py` with hard lock (`split manifest`, `prewarm=168h`, `execution=prewarm+core`, `evaluation=core`).
- Confirm:
  - `semantics_lock_report.json` is `OK`,
  - `runtime_cost_semantics_debug.json` has cost mean/max `0`,
  - core funnel and `total_trades_core_effective` are non-zero.

### Step 3) Paper10 Smoke (4-8h, Safety Locked)
- Keep `allow_live_orders=false`, `paper fixed capital=200000`.
- Run paper-equivalent 10% sizing (risk budget/sizing multiplier only).
- Track mandatory KPIs:
  - trades/hour,
  - realized pnl sum,
  - realized expectancy,
  - stop_loss / take_profit / strategy_exit ratio,
  - RANGING share,
  - edge_cal / `signal.expected_value` distribution samples.
- Stop conditions:
  - trades=0 for 2h+,
  - realized pnl <= `-3000 KRW`,
  - crash/assert/log integrity failure.

### Step 4) Strict Parity Fix Track (Parallel)
- Diagnose and resolve any `cli_strict_parity_pass_flag` mismatch path.
- Keep live blocked until strict parity hold is cleared.

### Step 5) Stage C Entry Condition
- Start Stage C only after Paper10 stability.
- Apply a single mode-switch axis only (no strategy proliferation).
