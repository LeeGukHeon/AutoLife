# Validation Gates (Fail-Closed)
Last updated: 2026-02-23

## Gate 1: Data integrity (strict)
Command:
```powershell
python scripts/validate_probabilistic_feature_dataset.py --strict
```
Must be zero:
- schema mismatch
- timestamp order violations
- leakage label mismatches

Recommended:
- per-market minimum rows
- NaN rate thresholds
- monotonic timestamp checks per market

v2 strict additions:
- run with `--pipeline-version v2`
- manifest `version` must be `prob_features_v2_draft`
- feature contract `version` must be `v2_draft`
- manifest/contract pipeline and contract tags must be mutually consistent
- validation output `gate_profile` must be `v2_strict`

## Gate 2: Runtime bundle parity (strict)
Command:
```powershell
python scripts/validate_runtime_bundle_parity.py
```
Must pass:
- status `pass`
- `max_abs_diff <= epsilon`
- reproducibility metadata persisted (seed, row ids, summary)

v2 strict additions:
- runtime bundle `version` must be `probabilistic_runtime_bundle_v2_draft`
- `pipeline_version` must match `v2`
- `feature_contract_version` and `runtime_bundle_contract_version` must be `v2_draft`
- train/split inputs must be same pipeline version (`v2`)

## Gate 3: Strategy verification (strict)
Command:
```powershell
python scripts/run_verification.py
```
Must pass:
- no simultaneous degradation of PF + expectancy + DD vs baseline
- OOS-focused evaluation

v2 strict profile:
- run with `--pipeline-version v2`
- must satisfy all:
  - threshold gate pass
  - adaptive verdict pass
  - baseline comparison available
  - dataset set comparable with baseline
  - non-degradation contract applied + pass
- v2 gate fail returns non-zero exit code.

## Gate 4: Shadow run (live orders disabled)
Requirements:
- `allow_live_orders=false`
- compare decision logs against backtest using same bundle and same candles
- shadow report `pipeline_version` must match promotion target pipeline
- if shadow report exposes `gate_profile` in v2 flow, it must be `v2_strict`
- live runtime resets `logs/policy_decisions.jsonl` on engine start to avoid stale-scan contamination

Shadow report generation command:
```powershell
python scripts/generate_probabilistic_shadow_report.py `
  --live-decision-log-jsonl ".\build\Release\logs\policy_decisions.jsonl" `
  --backtest-decision-log-jsonl ".\build\Release\logs\policy_decisions_backtest.jsonl" `
  --runtime-bundle-json ".\config\model\probabilistic_runtime_bundle_v2.json" `
  --pipeline-version v2 `
  --output-json ".\build\Release\logs\probabilistic_shadow_report_latest.json" `
  --strict
```

Shadow report strict validation command:
```powershell
python scripts/validate_probabilistic_shadow_report.py `
  --shadow-report-json ".\build\Release\logs\probabilistic_shadow_report_latest.json" `
  --pipeline-version v2 `
  --strict
```

Readiness evaluator command (live_enable target requires shadow report):
```powershell
python scripts/evaluate_probabilistic_promotion_readiness.py `
  --target-stage live_enable `
  --pipeline-version v2 `
  --feature-validation-json ".\build\Release\logs\probabilistic_feature_validation_summary.json" `
  --parity-json ".\build\Release\logs\probabilistic_runtime_bundle_parity.json" `
  --verification-json ".\build\Release\logs\verification_report.json" `
  --shadow-report-json ".\build\Release\logs\probabilistic_shadow_report_latest.json" `
  --shadow-validation-json ".\build\Release\logs\probabilistic_shadow_report_validation_latest.json" `
  --runtime-config-json ".\build\Release\config\config.json"
```

Hybrid cycle integration:
- `scripts/run_probabilistic_hybrid_cycle.py` supports `--generate-shadow-report`.
- for `v2 + --promotion-target-stage live_enable`, shadow validation remains fail-closed and can be combined with generation in one run.
- `--generate-shadow-report` and `--validate-shadow-report` are only valid with `--evaluate-promotion-readiness` (fail-closed arg guard).

## Gate 5: Staged live enable
Requirements:
- only after Gate 4 pass
- start with minimal tier/caps
- increase caps gradually with monitoring and rollback path

Evaluator output:
- `status=pass` and `promotion_ready=true` are required before staged live enable.

## Global enforcement
- Unknown state = fail.
- Any mismatch = no-trade decision.
- No gate bypass in regular operation.
