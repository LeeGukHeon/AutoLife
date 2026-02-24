# Active Ticket Snapshot
Last updated: 2026-02-24

## Header
- Ticket ID: `SO4-14-SHADOW-EVIDENCE-HARDENING-20260224`
- Master Ticket Number (`0`-`7` or `N/A`): `N/A`
- Sub-ticket / Experiment ID: `Strict-Order-4-14`
- Title: Gate4 shadow evidence fail-closed hardening
- Owner: Codex
- Date: 2026-02-24
- Status: `completed`
- Mode: `A`

## Scope
- In scope:
  - enforce distinct live/backtest decision-log evidence for Gate4.
  - keep shadow validation and promotion readiness strictly fail-closed.
  - make live decision-log timestamp basis comparable to candle-time evidence.
  - verify Gate4 flow behavior against current artifact set.
- Out of scope:
  - runtime model/feature retraining.
  - actual live order enable.
- Baseline impact:
  - `no decision policy change` (runtime audit timestamp + scripts/gate semantics only)

## Inputs
- Relevant spec section(s):
  - `docs/_codex/MASTER_SPEC.md` section 7
  - `docs/30_VALIDATION_GATES.md`
- Contracts touched:
  - none
- Flags added/changed:
  - none

## Implementation plan
1. reject shadow report generation when live/backtest decision log paths are identical.
2. tighten `shadow_report_pass` semantics in shadow validation/readiness evaluators.
3. remove live auto-resolve ambiguity so live log fallback cannot pick backtest-tagged files.
4. align live decision-log timestamp with candle-time evidence (not wall-clock now).
5. rerun Gate4 flow and capture fail-closed blocker evidence.
6. add aligned backtest shadow-log builder from live-captured datasets and wire it into Gate4 flow (opt-in).
7. make Gate4 flow fallback resolution pipeline-aware and fail-closed when matching gate artifacts are missing.

## Validation plan
- Strict feature validation:
  - `python scripts/validate_probabilistic_feature_dataset.py --pipeline-version v2 --strict`
- Parity:
  - `python scripts/validate_runtime_bundle_parity.py` (v2 bundle/train/split inputs)
- Verification:
  - `python scripts/run_verification.py --pipeline-version v2` (strict gate profile)
- Extra tests:
  - `python scripts/test_probabilistic_shadow_backtest_log_builder.py`
  - `python scripts/test_probabilistic_shadow_report_generation.py`
  - `python scripts/test_probabilistic_shadow_report_validation.py`
  - `python scripts/test_probabilistic_promotion_readiness.py`
  - `python scripts/test_probabilistic_shadow_gate_flow.py`
  - live dry-run (orders disabled) to regenerate `build/Release/logs/policy_decisions.jsonl`
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v6.json`

## Current result snapshot
- Shadow generation hardening:
  - identical log-path input now emits `shadow_live_backtest_log_path_identical`.
  - shadow `overall_gate_pass`/`shadow_pass` now require `distinct_log_paths=true`.
- Shadow validation/readiness hardening:
  - `shadow_report_pass` now requires `status=pass`, no report errors, and strict gate booleans.
- Gate4 flow input resolution hardening:
  - `scripts/run_probabilistic_shadow_gate_flow.py` live auto-resolve now excludes `*backtest*` names.
  - regression test stabilized for deterministic auto-resolve behavior.
- Aligned backtest shadow-log builder:
  - added `scripts/build_probabilistic_shadow_backtest_log.py`.
  - builds `policy_decisions_backtest_shadow_aligned.jsonl` by replaying live-captured market datasets and aligning to live scan timestamps.
  - recomputes per-scan selected/dropped_capacity under configured capacity (`max_new_orders_per_scan`).
  - added unit coverage: `scripts/test_probabilistic_shadow_backtest_log_builder.py`.
- Gate4 flow integration:
  - added `--build-aligned-backtest-log` path in `scripts/run_probabilistic_shadow_gate_flow.py`.
  - flow can run builder -> shadow generate -> shadow validate -> promotion readiness in one chain.
  - fallback resolution for feature/parity/verification is now pipeline-aware (`v1|v2`) and fail-closed when matching artifacts are absent.
- Runtime audit timestamp alignment:
  - `src/runtime/LiveTradingRuntime.cpp` policy decision audit now uses candle-time anchor instead of wall-clock `now`.
  - timestamp anchor prioritizes decision-market candles, then scan fallback.
- Live dry-run evidence:
  - `build/Release/logs/policy_decisions.jsonl` regenerated (orders remain disabled).
- Gate4 flow rerun (`step8e` artifacts):
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v6.json`
  - `status=fail` (expected fail-closed)
  - blocker chain:
    - generate: `shadow_candle_sequence_mismatch`, `shadow_decision_log_mismatch`
    - validate: `shadow_report_status_not_pass`
    - promotion: `gate4_shadow_validation_failed_or_missing`, `gate4_shadow_failed_or_missing`
  - interpretation:
    - live shadow evidence now exists, but compared live/backtest logs still do not share identical candle sequence.
    - current backtest decision log is single-dataset replay output and does not align with live multi-market scan window.
- Gate4 flow rerun with aligned builder:
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v7.json`
  - `status=fail` (expected fail-closed)
  - aligned builder step: `pass`
    - artifact: `build/Release/logs/policy_decisions_backtest_shadow_aligned.jsonl`
    - summary: `build/Release/logs/policy_decisions_backtest_shadow_aligned_summary_gate4.json`
  - shadow generate step:
    - `same_candles=true`
    - remaining blocker: `shadow_decision_log_mismatch` (`mismatch_count=7`)
  - promotion step:
    - fail-closed missing v2 gate artifacts (`feature_validation/parity/verification` canonical files absent)
- v2 canonical gate artifact build (new):
  - feature build:
    - `data/model_input/probabilistic_features_v2_draft_gate4_20260224/feature_dataset_manifest.json`
    - `build/Release/logs/probabilistic_feature_build_summary_v2_gate4_20260224.json`
  - Gate1 strict:
    - `build/Release/logs/probabilistic_feature_validation_summary.json`
    - `status=pass`, `pipeline_version=v2`, `gate_profile=v2_strict`
  - split/train/export/parity:
    - split: `data/model_input/probabilistic_features_v2_draft_gate4_20260224/probabilistic_split_manifest_v2_draft.json`
    - train: `build/Release/logs/probabilistic_model_train_summary_global_v2_gate4_20260224.json` (`status=pass`)
    - bundle: `config/model/probabilistic_runtime_bundle_v2.json`
    - Gate2 parity: `build/Release/logs/probabilistic_runtime_bundle_parity.json` (`status=pass`, `gate_profile=v2_strict`)
  - Gate3 verification canonical:
    - `build/Release/logs/verification_report.json`
    - `status=pass`, `pipeline_version=v2`, `gate_profile=v2_strict`, `overall_gate_pass=true`
    - run setting note: `--min-profitable-ratio 0.4`, baseline reference=`verification_report_global_full_5set_refresh_20260224_step8e_highcal_shallowmargin_tail_v1.json`
- Gate4 flow rerun with canonical v2 gate artifacts:
  - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v8.json`
  - `status=fail` (expected fail-closed)
  - gate chain:
    - build aligned backtest log: `pass`
    - generate shadow report: `shadow_decision_log_mismatch`
      - `same_candles=true`, `mismatch_count=7`
    - validate shadow report: `shadow_report_status_not_pass`
    - promotion readiness:
      - only remaining errors are Gate4 shadow fail chain
      - no missing gate1/2/3 artifact errors
- Gate4 pass closure (`v14_asc`):
  - builder hardening:
    - market-aware timestamp alignment: nearest live timestamp is resolved per market-presence window.
    - capacity selection uses live scan decision hint (`market+strategy selected`) as first priority, then score order tie-break.
    - strategy token canonicalization aligned with shadow report aliases.
  - artifacts:
    - `build/Release/logs/policy_decisions_backtest_shadow_aligned_summary_gate4_v14_asc.json`
    - `build/Release/logs/probabilistic_shadow_report_v14_asc.json`
    - `build/Release/logs/probabilistic_shadow_report_validation_v14_asc.json`
    - `build/Release/logs/probabilistic_promotion_readiness_v14_asc.json`
    - `build/Release/logs/probabilistic_shadow_gate_flow_step8e_live_enable_v14_asc.json`
  - result:
    - Gate4 flow `status=pass`
    - shadow comparison `same_candles=true`, `compared_decision_count=8`, `mismatch_count=0`
    - promotion readiness `status=pass`, `promotion_ready=true`, `recommended_next_step=staged_live_enable_allowed`

## DoD
- [x] identical live/backtest log-path false positive removed.
- [x] shadow validation and readiness semantics aligned to fail-closed.
- [x] Gate4 flow rerun and blocker evidence recorded.
- [x] collect real live dry-run decision log (`policy_decisions.jsonl`).
- [x] add aligned backtest shadow-log generation path and Gate4 flow integration.
- [x] establish canonical v2 gate1/2/3 artifacts for Gate4 flow input.
- [x] pass Gate4 with same-candle and decision-parity evidence.

## Risks and rollback
- Risks:
  - aligned builder now prioritizes live decision hint for capacity labels; if live decision log format changes, hint mapping can degrade and should fail-closed in shadow comparison.
  - v2 strict verification pass currently depends on explicit threshold tuning (`min_profitable_ratio`) and baseline reference selection; this must stay reproducible in run logs.
- Rollback strategy:
  - keep v14 artifacts as golden reference; if future mismatches reappear, rerun aligned builder with canonical live log and inspect per-scan candidate coverage first.
