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

Readiness evaluator command (live_enable target requires shadow report):
```powershell
python scripts/evaluate_probabilistic_promotion_readiness.py `
  --target-stage live_enable `
  --pipeline-version v2 `
  --feature-validation-json ".\build\Release\logs\probabilistic_feature_validation_summary.json" `
  --parity-json ".\build\Release\logs\probabilistic_runtime_bundle_parity.json" `
  --verification-json ".\build\Release\logs\verification_report.json" `
  --shadow-report-json ".\build\Release\logs\probabilistic_shadow_report_latest.json" `
  --runtime-config-json ".\build\Release\config\config.json"
```

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
