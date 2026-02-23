# Feature Contract (v1)
Last updated: 2026-02-23

## Canonical contract file
- `config/model/probabilistic_feature_contract_v1.json`

## Optional draft (not active)
- `config/model/probabilistic_feature_contract_v2.json` (MODE B kickoff draft)

## Dataset builder
- `scripts/build_probabilistic_feature_dataset.py`

## Validator
- `scripts/validate_probabilistic_feature_dataset.py`

## Baseline schema summary
- Anchor timeframe: `1m`
- Context timeframes: `5m,15m,60m,240m`
- Core feature count: 36
- Total columns (features + meta + labels): 43
- Primary labels:
  - `label_up_h1`
  - `label_up_h5`
  - `label_edge_bps_h5`

## Optional labels
- Triple-barrier labels exist behind flag and default to OFF.

## Ticket 1 strictness rule (universe-aware missing 1m)
- If market is in `final_1m_markets` and 1m anchor data is missing:
  - strict fail
- If market is not in `final_1m_markets` and 1m anchor data is missing:
  - skip market with warning

## Baseline compatibility requirement
- With all extension flags OFF:
  - row selection and schema must remain baseline-compatible.
