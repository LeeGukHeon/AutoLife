# AutoLife Phase 3~4 Test and Tuning Execution Runbook

## 0. Objective
Run a single operational flow for Phase 3 and Phase 4 from:
- regression safety (OFF parity),
- staged enablement (Phase 3 then Phase 4),
- bounded candidate search,
- anti-overfit validation (Dev/Val/Quarantine + purge),
- paper promotion,
- live staged rollout,
- runtime tuning (control or bandit),
- automatic rollback.

Runtime tuning is control, not online re-training.
All policy changes are bundle-versioned releases.

Mandatory reference:
- `docs/BACKTEST_REPRODUCTION_CHECKLIST.md`

## 1. Required Inputs
- Base bundle (stable baseline release).
- Candidate bundle set:
  - conservative / neutral / aggressive, or
  - base + 3-knob variants (`required_ev_offset`, `k_margin_scale`, `ev_blend_scale`).
- Frozen verification datasets (`--data-mode fixed`) for deterministic comparison.
- Diagnostics and verification outputs enabled:
  - Phase 3 diagnostics v2,
  - Phase 4 portfolio diagnostics,
  - verification report JSON/CSV.

## 2. Fixed Execution Order
Do not reorder:
1. Step 1: Phase 3/4 OFF regression.
2. Step 2: Phase 3 ON behavior validation.
3. Step 3: Phase 4 ON allocator/constraint validation.
4. Step 4: bounded candidate generation (K budget).
5. Step 5: Dev/Val/Quarantine + purge + regime-cross filtering.
6. Step 6: Paper promotion gate (24h minimum, 48-72h preferred).
7. Step 7: Live staged promotion (10% -> 30% -> 100%).
8. Step 8: runtime tuning in Mode A or Mode B only.
9. Step 9: rollback and incident response.

## 3. Step 1 - Phase 3/4 OFF Regression (Mandatory)
Goal:
- OFF must preserve baseline behavior.

Execution:
```powershell
python scripts/run_ci_operational_gate.py --include-backtest --strict-execution-parity --run-should-exit-parity-analysis --refresh-should-exit-audit-from-logs --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" --should-exit-audit-live-runtime-log-mode-filter exclude_backtest --strict-should-exit-parity
```

Decision:
- If this fails, stop. Do not proceed to Phase 3/4 ON.

Output:
- Save this as OFF baseline report set under `build/Release/logs`.

## 4. Step 2 - Phase 3 ON Validation
Goal:
- Validate frontier/EV/cost/adaptive blend behavior before portfolio complexity.

Run profile:
- `phase3.*.enabled = true`
- `phase4.*.enabled = false`
- Start with `mean_mode` cost, then test tail mode.
- Evaluate only 1-2 policy candidates.

Execution:
```powershell
python scripts/run_verification.py --data-mode fixed --validation-profile adaptive --output-json build/Release/logs/verification_report_phase3_on.json --output-csv build/Release/logs/verification_matrix_phase3_on.csv
```

Required checks:
- `required_ev` moves with margin (frontier coupling active).
- Reject decomposition is visible:
  - margin fail / EV fail / frontier fail.
- `ev_blend` is dynamic (not constant).
- Realized slippage gap does not spike abnormally.

## 5. Step 3 - Phase 4 ON Validation
Goal:
- Validate allocator/constraints determinism and diagnostics quality.

Run profile:
- `phase4_portfolio_allocator_enabled = true`
- `phase4_risk_budget_enabled = true`
- Start with correlation control OFF, then ON.
- `drawdown_governor` starts conservative.

Execution:
```powershell
python scripts/run_verification.py --data-mode fixed --validation-profile adaptive --phase4-off-on-compare --output-json build/Release/logs/verification_report_phase4_compare.json --output-csv build/Release/logs/verification_matrix_phase4_compare.csv
```

Required checks:
- Gross/per-market cap violations must be zero.
- Selection reject reasons are populated.
- Deterministic replay gives same selection decisions for same inputs.
- Correlation/cluster control ON reduces simultaneous overlap in high-correlation states.

## 6. Step 4 - Candidate Generation (Bounded Budget)
Goal:
- No unlimited search.

Rules:
- Candidate budget `K` is 3-10 max.
- Runtime adjustable controls are only:
  - `required_ev_offset`,
  - `k_margin_scale`,
  - `ev_blend_scale`.
- All other policy surfaces remain frozen per release round.

Examples:
```powershell
python scripts/run_phase34_operations_tuning.py control_a_update --bundle-json config/model/probabilistic_runtime_bundle_v2.json --verification-report-json build/Release/logs/verification_report_phase3_on.json --output-bundle-json config/model/probabilistic_runtime_bundle_v2_candidate_a.json --release-version candidate_a
python scripts/run_phase34_operations_tuning.py bundle_diff --base-bundle-json config/model/probabilistic_runtime_bundle_v2.json --candidate-bundle-json config/model/probabilistic_runtime_bundle_v2_candidate_a.json --output-json build/Release/logs/phase34_bundle_diff_candidate_a.json
```

## 7. Step 5 - Anti-Overfit Candidate Selection
Goal:
- Select candidates by robust protocol, not single-split luck.

Protocol:
- Development split: evaluate all candidates.
- Validation split: compare only (no tuning).
- Quarantine split: final approval only (strict no-tune).
- Apply purge gap according to holding period leakage risk.
- Require regime/vol/liquidity cross analysis and Phase 4 exposure diagnostics.

Execution:
```powershell
python scripts/run_phase34_operations_tuning.py validate_protocol --development-report-json build/Release/logs/verification_report_dev.json --validation-report-json build/Release/logs/verification_report_val.json --quarantine-report-json build/Release/logs/verification_report_quarantine.json --purge-gap-minutes 240 --strict-parity-pass --output-json build/Release/logs/phase34_ops_validation_report.json
```

Lexicographic decision order:
1. Risk/safety and constraint violations.
2. Execution alignment (slippage gap, EV vs realized drift).
3. Profitability.
4. Throughput and regime coverage.

Any Quarantine failure at level 1 or 2 is immediate reject.

## 8. Step 6 - Paper Promotion Gate
Goal:
- Final safety gate before live capital.

Window:
- Minimum 24h, recommended 48-72h.

Execution:
```powershell
python scripts/run_phase34_operations_tuning.py promotion_gate --paper-report-json build/Release/logs/verification_report_paper.json --current-stage paper --min-consecutive-windows 2 --strict-parity-pass --output-json build/Release/logs/phase34_ops_promotion_gate_report.json
```

Pass criteria should include:
- tail exceed below threshold,
- slippage gap below threshold,
- stable reject structure,
- zero cap violations,
- no persistent negative edge collapse.

Failure path:
- automatic rollback action + reason report.

## 9. Step 7 - Live Staged Promotion
Goal:
- Increase exposure only after stage stability.

Stages:
1. 10% size (>=24h stable)
2. 30% size (>=24-48h stable)
3. 100% size

Per-stage checks:
- cost/slippage gap,
- DD proxy and loss streak,
- cap violations = 0,
- regime pass-rate stability,
- cluster exposure skew.

## 10. Step 8 - Runtime Tuning (Only Two Modes)
### Mode A - 3-Knob Control
Use KPI windows (1h/6h/24h) to update only the three knobs with:
- step clamp,
- update interval clamp,
- absolute min/max clamp.

Execution:
```powershell
python scripts/run_phase34_operations_tuning.py control_a_update --bundle-json config/model/probabilistic_runtime_bundle_v2.json --verification-report-json build/Release/logs/verification_report.json --output-bundle-json config/model/probabilistic_runtime_bundle_v2_phase34_ops_next.json --output-report-json build/Release/logs/phase34_ops_control_update_report.json --state-json build/Release/logs/phase34_ops_state.json
```

### Mode B - Bundle Bandit
Select among 2-3 candidate bundles in fixed time blocks.

Execution:
```powershell
python scripts/run_phase34_operations_tuning.py bandit_b_select --candidate-bundle-jsons config/model/bundle_conservative.json config/model/bundle_neutral.json config/model/bundle_aggressive.json --state-json build/Release/logs/phase34_ops_state.json --output-json build/Release/logs/phase34_ops_bandit_report.json
```

Hard prohibitions:
- tick/minute parameter shaking,
- unlimited candidate expansion,
- large unvalidated live changes.

## 11. Step 9 - Rollback and Incident Response
Immediate rollback triggers:
- tail exceed surge,
- DD proxy breach,
- persistent loss streak,
- cap violations,
- realized edge collapse with worsening slippage gap.

Actions:
- switch to conservative bundle,
- block new entries during cooldown,
- emit top3 root-cause report,
- patch only root-cause parameters in next release candidate.

## 12. Operating Cadence
- Daily:
  - diagnostics summary and bottleneck top3.
- Twice per week:
  - cost/slippage gap review and slow calibration decision.
- Weekly:
  - bounded candidate search (`K=3..5`) and protocol evaluation.
- Monthly:
  - refresh correlation/cluster reference set for Phase 4 constraints.

## 13. Common Failure Patterns
- Candidate budget violation (too many variants).
- Quarantine contamination (tuning on holdout).
- Ignoring cost/slippage gap while optimizing EV.
- Fast repeated live retuning causing instability.

## 14. Primary Output Artifacts
- `build/Release/logs/verification_report*.json`
- `build/Release/logs/verification_matrix*.csv`
- `build/Release/logs/phase34_ops_control_update_report.json`
- `build/Release/logs/phase34_ops_bandit_report.json`
- `build/Release/logs/phase34_ops_validation_report.json`
- `build/Release/logs/phase34_ops_promotion_gate_report.json`
- `build/Release/logs/phase34_ops_bundle_diff.json`

## 15. Backtest Caution Enforcement
Before closing any Phase 3/4 tuning cycle, run the checklist:
- `docs/BACKTEST_REPRODUCTION_CHECKLIST.md`

Hard gate:
- If any checklist item fails, freeze promotion and return to candidate revision.

## 16. Execution Log (2026-02-26, R2.1/R3.1/R4.1)
- Scope:
  - R2.1 protocol redesign (time split + purge preserved) with cumulative prefix split for Val/Quarantine sample recovery.
  - R3.1 Phase3 2-knob sweep (`required_ev_offset`, `k_margin_scale`) with `K=5`.
  - R4.1 Phase4 cap relaxation sweep with `K=4` (`off + cap_low + cap_mid + cap_high`).
- R2.1 result:
  - `time_split_applied=true`, `purge_gap_applied=true (240m)`, `leak_proximity_rows_remaining=0`.
  - Core sample sufficiency (diff basis):
    - validation core candidate total: `1627` (>=100)
    - quarantine core candidate total: `1405` (>=100)
- R3.1 result (core pass-rate, diff basis):
  - neutral: val `0.061%`, qua `0.356%`
  - step1: val `0.742%`, qua `3.524%`
  - step2: val `4.523%`, qua `12.999%`
  - step3: val `16.357%`, qua `27.878%`
  - step4: val `2.539%`, qua `10.180%`
  - Note: 5%-15% target not jointly satisfied yet (step2/step4 are nearest trade-off points).
- R4.1 result (all_purged ON/OFF comparison):
  - cap_low selection rate: `0.3387`
  - cap_mid selection rate: `0.4393`
  - cap_high selection rate: `0.6798` (target 0.6-0.9 satisfied)
  - filtering active with non-zero rejects (`rejected_by_budget`), no full wipeout.
- Reference artifacts:
  - `build/Release/logs/phase34_round_next_action_report_r21_r31_r41.json`
  - `build/Release/logs/time_split_manifest_r21_prefix.json`
  - `build/Release/logs/r31_candidate_summary.json`
  - `build/Release/logs/r41b_phase4_caps_summary.json`
  - `build/Release/logs/phase4_on_off_comparison_r41b_cap_high.json`
