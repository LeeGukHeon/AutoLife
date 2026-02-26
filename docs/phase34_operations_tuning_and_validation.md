# Phase 3~4 Operations Tuning and Anti-Overfit Validation

## 1. Scope
This document defines the runtime operations method for Phase 3 and Phase 4:
- Phase 3: frontier, EV/cost calibration, adaptive EV blend, diagnostics v2.
- Phase 4: allocator, correlation/exposure control, risk budget, drawdown governor, execution-aware sizing.

Runtime tuning is treated as control with bounded changes, not online model learning.
All changes are managed as bundle releases.

## 2. 3-Knob Control Model
Bundle path: `phase3.operations_control`

Knobs:
- `required_ev_offset`: additive shift to frontier required EV.
- `k_margin_scale`: multiplicative scale to frontier `k_margin`.
- `ev_blend_scale`: multiplicative scale to adaptive EV blend result.

Runtime integration:
- Effective frontier k-margin is `frontier_k_margin * frontier_k_margin_scale`.
- Required EV includes `required_ev_offset`.
- Adaptive blend output is multiplied by `ev_blend_scale` and then clamped by blend min/max.

## 3. Operation Modes
### Mode A: Target-band control
Command:
```powershell
python scripts/run_phase34_operations_tuning.py control_a_update \
  --bundle-json config/model/probabilistic_runtime_bundle_v2.json \
  --verification-report-json build/Release/logs/verification_report.json \
  --output-bundle-json config/model/probabilistic_runtime_bundle_v2_phase34_ops_next.json
```

Behavior:
- Reads KPI snapshot from verification report.
- Updates 3 knobs with step/range/frequency limits.
- Writes release metadata and bundle diff summary.

### Mode B: Deterministic bundle tournament/bandit
Command:
```powershell
python scripts/run_phase34_operations_tuning.py bandit_b_select \
  --candidate-bundle-jsons config/model/bundle_conservative.json config/model/bundle_neutral.json config/model/bundle_aggressive.json \
  --state-json build/Release/logs/phase34_ops_state.json
```

Behavior:
- Keeps per-bundle reward stats in state file.
- Selects bundle by deterministic explore/exploit rule.
- Optional reward update from latest verification report.

## 4. Anti-Overfit Validation Protocol
Use development, validation, quarantine report triplet.

Command:
```powershell
python scripts/run_phase34_operations_tuning.py validate_protocol \
  --development-report-json build/Release/logs/verification_report_dev.json \
  --validation-report-json build/Release/logs/verification_report_val.json \
  --quarantine-report-json build/Release/logs/verification_report_quarantine.json \
  --purge-gap-minutes 240 \
  --strict-parity-pass
```

Lexicographic checks:
1. Survival/risk
2. Execution/alignment
3. Profitability
4. Throughput

All levels must pass in all three splits.

## 5. Paper to Live Promotion Gate
Command:
```powershell
python scripts/run_phase34_operations_tuning.py promotion_gate \
  --paper-report-json build/Release/logs/verification_report_paper.json \
  --current-stage paper \
  --strict-parity-pass
```

Behavior:
- Evaluates paper window with lexicographic checks.
- Advances staged rollout (`paper -> live10 -> live30 -> live100`) after required consecutive windows.
- On failure, returns rollback action to conservative mode.

## 6. Bundle Release and Diff
Diff command:
```powershell
python scripts/run_phase34_operations_tuning.py bundle_diff \
  --base-bundle-json config/model/probabilistic_runtime_bundle_v2.json \
  --candidate-bundle-json config/model/probabilistic_runtime_bundle_v2_phase34_ops_next.json
```

Bundle metadata:
- `release_meta.version`
- `release_meta.created_at`
- `release_meta.parent_version`
- `release_meta.change_summary`
- `release_meta.mode`

## 7. Rollback Policy
Rollback triggers (operations gate):
- drawdown threshold breach
- tail-loss concentration breach
- persistent quality degradation

Rollback action:
- switch to conservative bundle
- keep cooldown window
- produce reasoned report from mode outputs

## 8. Example Outputs
- `build/Release/logs/phase34_ops_control_update_report.json`
- `build/Release/logs/phase34_ops_bandit_report.json`
- `build/Release/logs/phase34_ops_validation_report.json`
- `build/Release/logs/phase34_ops_promotion_gate_report.json`
- `build/Release/logs/phase34_ops_bundle_diff.json`
