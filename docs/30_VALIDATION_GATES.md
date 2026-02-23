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

## Gate 3: Strategy verification (strict)
Command:
```powershell
python scripts/run_verification.py
```
Must pass:
- no simultaneous degradation of PF + expectancy + DD vs baseline
- OOS-focused evaluation

## Gate 4: Shadow run (live orders disabled)
Requirements:
- `allow_live_orders=false`
- compare decision logs against backtest using same bundle and same candles

## Gate 5: Staged live enable
Requirements:
- only after Gate 4 pass
- start with minimal tier/caps
- increase caps gradually with monitoring and rollback path

## Global enforcement
- Unknown state = fail.
- Any mismatch = no-trade decision.
- No gate bypass in regular operation.
