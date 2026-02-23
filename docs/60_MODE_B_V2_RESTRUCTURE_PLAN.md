# MODE B / v2 Restructure Plan (Optional)
Last updated: 2026-02-23
Status: Draft kickoff only (no runtime activation)

## Goal
- Prepare optional MODE B migration with explicit v2 contracts.
- Keep baseline MODE A (`v1`) as the default active path until full parity + verification is redefined and passed.

## Added draft artifacts
- `config/model/probabilistic_feature_contract_v2.json`
- `config/model/probabilistic_runtime_bundle_v2.json`

## Guardrails
- v2 is not active by default.
- No existing v1 script/runtime path may switch behavior implicitly.
- Any v2 run must be explicit and produce separate manifests/bundles.

## Planned phases
1. Contract freeze:
   - finalize v2 column contract and transforms.
   - finalize v2 bundle fields and provenance.
2. Pipeline branching:
   - add explicit `v1|v2` switches in build/train/export scripts.
   - keep `v1` default.
3. Runtime compatibility:
   - add safe v2 parsing with strict version checks.
   - fail closed on unknown/mixed contracts.
4. Gate redefinition:
   - define parity/verification criteria specifically for v2.
   - keep v1 gate rules unchanged.
5. Promotion:
   - run shadow + staged live with v2 bundle only after v2 gates pass.

## Exit criteria for full Ticket 7 completion
- v2 end-to-end pipeline passes strict validation/parity/verification.
- v1 remains fully supported and reproducible.
