# Codex Bootstrap (Read First)
Last updated: 2026-02-23

## Session startup order
1. Read `docs/_codex/CURRENT_STATE.md`.
2. Read `docs/_codex/MASTER_SPEC.md`.
3. Read active ticket content using `docs/_codex/TICKET_TEMPLATE.md`.
4. Check `docs/_codex/PR_CHECKLIST.md` before finalizing changes.

## Immediate commands
```powershell
git rev-parse HEAD
python scripts/validate_probabilistic_feature_dataset.py --strict
python scripts/validate_runtime_bundle_parity.py
python scripts/run_verification.py
```

## Baseline reminder
- Baseline behavior is authoritative and must remain unchanged unless explicitly versioned.
- Extensions are opt-in and default OFF.
- Live trading remains disabled unless all gates pass.
