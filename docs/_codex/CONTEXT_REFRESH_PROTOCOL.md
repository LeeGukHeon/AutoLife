# Context Refresh Protocol
Last updated: 2026-02-23

## Trigger
Use this protocol whenever a Codex session starts fresh or after context reset.

## Required sequence
1. Read `docs/_codex/BOOTSTRAP.md`.
2. Read `docs/_codex/CURRENT_STATE.md`.
3. Read active ticket (PR title/body + `docs/_codex/TICKET_TEMPLATE.md` format).
4. Read only the minimum files needed for the ticket.
5. Run minimal mandatory checks for touched areas:
   - feature/data changes: strict validation
   - model/bundle changes: parity
   - runtime decision changes: verification
   - helper script:
     - `python scripts/run_codex_context_refresh_checks.py --touched-areas feature,model,runtime --pipeline-version auto --skip-missing`
6. Update `docs/_codex/CURRENT_STATE.md` in the same PR:
   - contract hashes
   - last gate status
   - known issues delta

## Fail-closed rule
- If context is ambiguous, do not proceed with live-impacting changes until clarified.

## PR acceptance rule
- PR is not complete until `docs/_codex/PR_CHECKLIST.md` is satisfied.
