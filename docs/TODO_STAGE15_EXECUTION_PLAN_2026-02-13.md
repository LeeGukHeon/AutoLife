# Stage 15 Execution TODO (Reset Baseline)

Last updated: 2026-02-19
Status: `RESET_BASELINE_ACTIVE`

## Reset Intent
- End log-heavy operation and rebuild validation reliability first.
- Prioritize validation integrity over repeated parameter tuning.
- Scope:
  - `run_realdata_candidate_loop`
  - `run_profitability_matrix`
  - `run_candidate_train_eval_cycle`
  - `walk_forward_validate`

## Archived History
- Full execution log moved to:
  - `docs/archive/TODO_STAGE15_EXECUTION_PLAN_2026-02-13_FULLLOG_2026-02-19.md`
- One-off large script audit docs moved to:
  - `docs/archive/SCRIPT_READING_TUNE_CANDIDATE_GATE_2026-02-19.md`
  - `docs/archive/TUNE_CANDIDATE_GATE_LINE_AUDIT_2026-02-19.md`

## Minimal Active Docs
- `docs/TODO_STAGE15_EXECUTION_PLAN_2026-02-13.md`
- `docs/ADAPTIVE_ENGINE_REBUILD_PLAN_2026-02-19.md`
- `docs/VALIDATION_METHOD_REVIEW_2026-02-19.md`
- `docs/VERIFICATION_RESET_BASELINE_2026-02-19.md`
- `docs/FOUNDATION_ENGINE_STRATEGY_REVIEW_2026-02-19.md`
- `docs/CHAPTER_CURRENT.md`
- `docs/CHAPTER_HISTORY_BRIEF.md`
- `docs/STRICT_GATE_RUNBOOK_2026-02-13.md`
- `docs/TARGET_ARCHITECTURE.md`
- `docs/FILE_USAGE_MAP.md`
- `docs/DELETE_WAVE_MANIFEST.md`

## Documentation Discipline
- At end of every work batch, update:
  - `docs/CHAPTER_CURRENT.md` with detailed completed items and remaining tasks.
  - `docs/CHAPTER_HISTORY_BRIEF.md` with a short chapter-level summary.
- Keep old details in archive docs only. Do not append long logs to active TODO docs.

## Current Batch Order (Enforced)
1. Documentation sync first.
  - update plan/todo/chapter docs before code changes
2. Runtime legacy-path disconnect hardening.
  - remove unreachable legacy selection logic from active C++ path
3. Verification alignment.
  - adaptive profile default
  - legacy gate only for compatibility checks
4. Build and smoke verification.
  - release build + adaptive smoke (+ optional legacy compatibility smoke)
5. Chapter close update.
  - completed items + remaining tasks only

## Non-Negotiable Rules
- Keep validation sequential: `--max-workers 1`.
- No coin hardcoding. Use pattern/regime level logic only.
- Re-run matrix validation after every tuning application.
- Use adaptive validation profile as primary (`--validation-profile adaptive`).
- Keep legacy threshold gate only for compatibility checks (`--validation-profile legacy_gate`).
- Use `--data-mode fixed` as gate baseline; `refresh_if_missing/refresh_force` only for robustness checks.
- Keep anti-overfitting safeguards:
  - dedicated latest holdout slice
  - walk-forward required before promotion
  - no promotion from single-dataset gain

## P0: Verification Integrity Recovery (Must Pass First)
1. Add post-tune matrix re-run in `run_realdata_candidate_loop`.
2. Pass explicit source/build config paths from loop to matrix script.
3. Separate promotion decision for `core_full` from combined overall gate logic.
4. Enforce latest-time holdout split in `run_candidate_train_eval_cycle`.
5. Add explicit adaptive state I/O mode control in `walk_forward_validate`.

Definition of Done (P0):
- Gate outputs are reproducible on repeated runs.
- Final gate report reflects tuned config state.
- Split and walk-forward modes are explicit in artifacts.

## P1: Structure Simplification
1. Split `tune_candidate_gate_trade_density.py` into clear phases.
2. Move objective and selector-veto policy into dedicated modules.
3. Freeze artifact schema contracts for JSON/CSV outputs.

Definition of Done (P1):
- Start removing 2k+ line single-function blocks.
- Enable phase-level isolated tests.

## P2: Strategy Improvement (After P0/P1)
1. Stabilize multi-TF signal combination (1m, 15m, 1h, 4h).
2. Validate risk-control paths by regime (downtrend, range, high-vol).
3. Run integrated shadow parity and dry-run before live promotion.

## Local Build Baseline
- CMake path:
  - `D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe`
- Build command:
```powershell
D:\MyApps\vcpkg\downloads\tools\cmake-3.31.10-windows\cmake-3.31.10-windows-x86_64\bin\cmake.exe --build build --config Release --target AutoLifeTrading
```

## Active Progress Board
- [x] R0.1 Doc-first batch applied + StrategyManager legacy select dead-path prune + README baseline normalization (2026-02-19)
- [x] R0.2 Runtime strategy registration hard-switched to foundation-only path (legacy pack disconnected) (2026-02-19)
- [x] R0.3 Legacy v2 strategy files compile-excluded and physically deleted from active tree (2026-02-19)
- [x] R0.4 `include/v2`, `src/v2` folder flatten 완료 (경로 재배치 + CMake/tests/scripts 반영 + smoke 검증) (2026-02-19)
- [x] R0.5 Remaining legacy shadow/v2 stack fully removed from runtime + CMake + tests/scripts (2026-02-19)
- [x] R0.6 V2 naming cleanup + residual legacy preset removal (`run_verification.py`, `legacy_fallback` 삭제, runtime `getPrimaryTakeProfit` 명명 정리) (2026-02-19)
- [x] P0.1 Fix tune-before-final-report mismatch (2026-02-19)
- [x] P0.2 Fix source config path wiring (2026-02-19)
- [ ] P0.3 Split promotion gate logic
- [ ] P0.4 Enforce latest-time holdout split
- [ ] P0.5 Add explicit walk-forward state I/O mode
- [ ] P1.1 Split tuner phases
- [ ] P1.2 Modularize objective and selector
- [ ] P1.3 Freeze artifact schema contracts

## Exit Criteria
- Distinguish whether `overall_gate_pass=false` is caused by strategy/data or by validation design.
- Do not run long tuning batches before P0 completion.
