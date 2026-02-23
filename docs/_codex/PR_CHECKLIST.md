# PR Checklist (Mandatory)
Last updated: 2026-02-23

## Scope and baseline
- [ ] Ticket scope is explicit and matches `TICKET_TEMPLATE`.
- [ ] Baseline behavior is unchanged unless explicitly versioned.
- [ ] New functionality is behind flag(s) when required.

## Compliance and safety
- [ ] Upbit API policy respected (`Remaining-Req`, 429 backoff, no Origin header on server-side).
- [ ] No secrets logged or committed.
- [ ] Live defaults remain fail-closed (`allow_live_orders=false` unless explicitly staged).

## Reproducibility
- [ ] Relevant manifests/summaries include hashes and args metadata.
- [ ] Contract and bundle changes recorded with updated hashes.
- [ ] `docs/_codex/CURRENT_STATE.md` updated.

## Validation gates
- [ ] Strict feature validation pass (if applicable).
- [ ] Runtime bundle parity pass (if applicable).
- [ ] Verification pass (if decision/runtime logic touched).
- [ ] Tests added/updated for new behavior.

## Documentation
- [ ] Docs index and touched spec docs updated.
- [ ] Extension docs updated for extension changes.
- [ ] Any new script has usage notes and defaults documented.
