# Codex Bootstrap (Read First)
Last updated: 2026-02-25

## Session startup order
1. Read `docs/_codex/CURRENT_STATE.md`.
2. Read `docs/_codex/MASTER_SPEC.md`.
3. Read active ticket:
   - `docs/_codex/ACTIVE_TICKET.md` (preferred)
   - if missing, infer from PR title/body using `docs/_codex/TICKET_TEMPLATE.md`
4. Check `docs/_codex/PR_CHECKLIST.md` before finalizing changes.

## Immediate commands
```powershell
git branch --show-current
git rev-parse HEAD
python scripts/run_codex_context_refresh_checks.py `
  --touched-areas feature,model,runtime `
  --pipeline-version auto `
  --skip-missing
python scripts/check_source_encoding.py `
  --output-json .\build\Release\logs\source_encoding_check_bootstrap.json
```

## Baseline reminder
- Baseline behavior is authoritative and must remain unchanged unless explicitly versioned.
- Extensions are opt-in and default OFF.
- Live trading remains disabled unless all gates pass.
- Source/script/docs/config text files must be `UTF-8 (No BOM)`.
- CSV files are the only exception and use `UTF-8 with BOM`.
