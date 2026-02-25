# Codex Bootstrap (Read First)
Last updated: 2026-02-25

## Session startup order
1. Read `docs/_codex/CURRENT_STATE.md`.
2. Read `docs/_codex/MASTER_SPEC.md`.
3. Read active ticket:
   - `docs/_codex/ACTIVE_TICKET.md` (preferred)
   - if missing, infer from PR title/body using `docs/_codex/TICKET_TEMPLATE.md`
4. If ticket is CH-01 Phase 3 work, read:
   - `docs/_codex/CH01_PHASE3_CONTEXT.md`
   - `docs/_codex/CH01_PHASE3_FINAL_DELIVERY_2026-02-25.md` (final 6-section handoff)
5. If ticket touches live/backtest parity, read:
   - `docs/_codex/PARITY_BOUNDARY.md`
   - `docs/_codex/PHASE2_CLOSURE_2026-02-25.md` (current Phase 2 closure baseline)
6. Check `docs/_codex/PR_CHECKLIST.md` before finalizing changes.

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
python scripts/run_ci_operational_gate.py `
  --include-backtest `
  --strict-execution-parity `
  --run-should-exit-parity-analysis `
  --refresh-should-exit-audit-from-logs `
  --should-exit-audit-live-runtime-log-glob "build/Release/logs/live_probe_stdout*.txt" `
  --should-exit-audit-live-runtime-log-mode-filter exclude_backtest `
  --strict-should-exit-parity
```

## Baseline reminder
- Baseline behavior is authoritative and must remain unchanged unless explicitly versioned.
- Extensions are opt-in and default OFF.
- Live trading remains disabled unless all gates pass.
- Source/script/docs/config text files must be `UTF-8 (No BOM)`.
- CSV files are the only exception and use `UTF-8 with BOM`.
