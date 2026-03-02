# Gate vNext Full Rebuild (RND Track)
Last updated: 2026-03-02

## Scope
- Replace legacy gate/EV coupling with Gate vNext as the only baseline.
- Fresh14 backtest is mandatory first; Paper is the final step only.
- This document is the top-priority implementation order.

## 0) Non-negotiable Rules
- Legacy code is physically removed after Gate vNext replacement is stable.
- No legacy fallback mode is allowed in final state.
- Gate vNext is the only execution path:
  - Quality Selection (rank + TopK=5)
  - Risk Sizing + Execution Gate
- Selection uses rank-based ordering only.
  - No absolute probability hard cut as primary selector.
- EV is sizing-first.
  - Minimal rule allowed: `EV < 0 -> size=0`.
- Validation order is fixed:
  - Fresh14 backtest pass first
  - Paper last

## 1) Mandatory Safety Checks
- Run output isolation:
  - Every run must use `--run-dir`.
  - No mixed outputs under one shared logs directory.
- Provenance hard evidence:
  - `config_path`, `backend_request/effective`, model sha are required.
  - Backend mismatch is immediate fail.
- Stage funnel required in all runs:
  - `S0 snapshots_valid`
  - `S1 selected_topk`
  - `S2 sized_count`
  - `S3 exec_gate_pass`
  - `S4 submitted`
  - `S5 filled`
- Matrix automation is required:
  - At minimum: SGD vs LGBM
  - Optional expansion: platt variants
- Baseline params dump is required:
  - `build/Release/logs/run_params_dump.json`
  - If missing, run is invalid.

## 2) Target Code Layout
- New modules:
  - `include/gate_vnext/GateVNext.h`
  - `src/gate_vnext/GateVNext.cpp`
  - `include/gate_vnext/CandidateSnapshot.h`
  - `include/gate_vnext/QualitySelector.h`
  - `include/gate_vnext/RiskSizer.h`
  - `include/gate_vnext/ExecutionGate.h`
  - `include/gate_vnext/Telemetry.h`
- Runtime integration principle:
  - Live/Backtest runtime should call `GateVNext::Run()`.
  - Selection/gate logic migrates from runtime files into GateVNext modules.

## 3) CandidateSnapshot (Stage0)
- Required fields:
  - market, ts_ms
  - regime
  - atr_pct_14, adx
  - spread_pct, notional, volume_surge, imbalance, drop proxies
  - model outputs: p_calibrated, threshold, margin, edge_bps
  - validity flags and fail reason
- Feature build failure handling:
  - `snapshot_valid=false`
  - Count at Stage0 only
  - Track execution-scope and core-scope separately

## 4) QualitySelector (Stage1)
- v1 fixed rule:
  - `quality_key = margin` descending
  - `TopK = 5` fixed for all backends
- No absolute hard margin cut in this selector stage.
- Telemetry:
  - `topk_effective`
  - `margin_stats_topk` (mean/std/p10/p50/p90)

## 5) RiskSizer (Stage2)
- v1 expected value:
  - `expected_value_vnext_bps = edge_bps` (NET semantics)
  - if missing edge, use `0`
- v1 sizing:
  - if `expected_value_vnext_bps < 0`: `size=0`
  - else: `size_fraction = base_size * clamp(ev/ev_scale_bps, 0, 1)`
- v1 fixed parameters:
  - `ev_scale_bps = 10`
  - `base_size`: one single source value
- Telemetry:
  - EV mean/median/p90
  - size fraction mean/median/p90
  - negative-EV drop count

## 6) ExecutionGate (Stage3)
- Keep existing spread/imbalance/rapid_drop veto logic.
- Standardize reject reason taxonomy.
- Track pass/reject counts and top veto reasons.

## 7) Stage4/5 (Submit/Fill)
- Reuse existing submit/fill path.
- Funnel logging remains mandatory.

## 8) Legacy Removal Scope
- Remove legacy paths after replacement:
  - composite-score Top20 legacy branch
  - prefilter hard margin gate duplication
  - frontier hard margin duplication
  - runtime hard margin duplication
  - legacy expected_value pass hard gate duplication
  - legacy multi-blend EV branches
  - legacy mode/fallback branches
- Deletion flow:
  1. Replace with GateVNext path
  2. Build + Fresh14 matrix pass
  3. Physically delete legacy files/branches

## 9) Fresh14 Backtest Matrix (Required)
- Same split/prewarm for all runs.
- Minimum runs:
  - `backend=sgd`
  - `backend=lgbm`
- Required outputs:
  - S0~S5 funnel
  - trades_core
  - expectancy/PF
  - stop_loss/partial/tp_full ratios
  - topk evidence (`topk_effective=5`)
  - provenance
- PASS criteria:
  - LGBM no longer explodes in trades
  - stop_loss ratio does not explode to 90% range
  - expectancy does not collapse
- FAIL next action:
  - v2 single-axis: selector key change or TopK tightening

## 10) Paper Is Last
- Paper runs only after Fresh14 pass is reproduced twice.
- Paper runs must use `--run-dir`.

## 11) Immediate Execution Order
1. Complete `--run-dir` enforcement.
2. Add GateVNext scaffolding + stage funnel + provenance locks.
3. Integrate snapshot builder.
4. Implement QualitySelector TopK=5.
5. Implement RiskSizer (`EV<0` drop + sizing).
6. Connect ExecutionGate.
7. Remove legacy call paths.
8. Add fixed Fresh14 2-run matrix script + compare report.
9. Promote new baseline only after repeated pass.

## 12) Progress Snapshot (2026-03-02)
- Completed:
  - `--run-dir` support and startup provenance (`run_provenance.json`, `run_params_dump.json`)
  - `GateVNext` scaffolding module added (`include/gate_vnext/*`, `src/gate_vnext/GateVNext.cpp`)
  - Live path partial integration (TopK/margin telemetry path present)
  - Backtest path integration for Stage0~Stage3 selection/sizing gate
  - Backtest legacy path physical removal (Step 7):
    - removed legacy manager/frontier/policy/phase4 candidate filtering block from runtime execution path
    - Backtest candidate flow is now vnext-only (snapshot -> topk -> sizing -> execution)
    - `gate_vnext_enabled` runtime switch removed in Backtest path
  - Backtest `entry_funnel` now exports:
    - `gate_system_version_effective`
    - `quality_topk_effective`
    - `stage_funnel_vnext` (S0..S5 + drop_ev_negative_count)
  - Fresh14 matrix automation:
    - `scripts/run_gate_vnext_fresh14_matrix.py`
    - summary csv/json with backend provenance + gate vnext fields
  - Live path legacy branch physical removal (2026-03-02):
    - removed legacy manager/frontier/primary-minimum/phase4 candidate-selection blocks from `generateSignals()`
    - removed legacy policy-controller/phase4 execution filtering blocks from `executeSignals()`
    - removed phase4 live artifact writer/reset path and phase4 diagnostics report block
    - Live runtime now uses vnext top-k selection output directly for execution candidates
  - Legacy file physical deletion:
    - deleted `include/common/Phase4PortfolioAllocator.h` (no remaining runtime references)
    - deleted `include/strategy/FoundationRiskPipeline.h`
    - deleted `src/strategy/FoundationRiskPipeline.cpp`
  - Additional physical deletion pass (2026-03-02):
    - removed `StrategyManager::filterSignalsWithDiagnostics` and related legacy frontier diagnostics structs
    - removed backtest `frontier_filter_backtest.jsonl` artifact writer/reset path
    - removed runtime prefilter hard-margin reject branch in both Live/Backtest snapshot inference
    - removed runtime hard-margin reject branch (`probabilistic_runtime_hard_gate`) in both Live/Backtest adjustment path
  - Additional physical deletion pass (2026-03-02, late):
    - removed remaining legacy phase scripts/files:
      - `scripts/build_trending_availability_audit.py` (deleted)
      - `scripts/run_phase34_operations_tuning.py` (deleted)
      - `docs/phase34_operations_tuning_and_validation.md` (deleted)
    - removed legacy frontier/manager blocks from bundle exporter:
      - `scripts/export_probabilistic_runtime_bundle.py` no longer emits `phase3.frontier` / `phase3.manager_filter`
    - removed legacy naming from code/scripts (`legacy_fixed`, `fallback_legacy`, `filtered_out_by_manager`, `reject_frontier_fail`, etc.)
    - updated verification vocabulary to vnext terminology (`quality_filter`, `filtered_out_by_sizing`, `would_pass_quality_selection`)
    - full token sweep evidence generated:
      - `build/Release/logs/gate_vnext_legacy_sweep_report.json`
      - `include/src/scripts` counts for
        `legacy|frontier|manager_filter|expected_value_pass|filtered_out_by_manager|probabilistic_runtime_hard_gate|probabilistic_runtime_scan_prefilter`
        are all `0`
  - Additional physical deletion pass (2026-03-02, final):
    - removed live scan prefilter path (`computeLiveScanPrefilterThresholds`) from runtime path
      - deleted `LiveScanPrefilterThresholds` interface from `ExecutionGuardPolicyShared`
      - removed implementation in `src/common/ExecutionGuardPolicyShared.cpp`
      - removed runtime call path from `LiveTradingRuntime::scanMarkets()`
    - removed legacy `top20_*` naming from live telemetry/report payload
      - replaced with `quality_*` fields (`quality_sort_mode`, `quality_selected_candidates`, `quality_margin_stats`)
    - physically deleted remaining Phase34/frontier bundle artifacts:
      - `config/model/probabilistic_runtime_bundle_v2_a18_frontier_relax_step1.json`
      - `config/model/probabilistic_runtime_bundle_v2_phase34_*.json`
    - physically deleted legacy diagnostics doc:
      - `docs/56_EXT_PHASE3_FRONTIER_EV_COST_DIAGNOSTICS.md`

- Current experiment bundles/config:
  - `config/model/EXP_RND_GATENEXT_SGD.json`
  - `config/model/EXP_RND_GATENEXT_LGBM.json`
  - local run configs (`config_rnd_vnext_*.json`) are generated for matrix execution

- Latest replay tag:
  - `gatevnext_vnextonly_20260302`
  - output summary:
    - `build/Release/logs/gatevnext_vnextonly_20260302_summary.json`
    - `build/Release/logs/gatevnext_vnextonly_20260302_summary.csv`

- Interpretation:
  - vnext activation/provenance is now visible in reports.
  - performance gap (SGD vs LGBM) is still unresolved (`SGD 35 trades / -36.9 KRW`, `LGBM 81 trades / -135.3 KRW` on Fresh14 quarantine core).
  - next step is cleaning remaining Phase4/legacy diagnostics payload structs to finish full physical-deletion scope.
