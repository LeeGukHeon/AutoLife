# Data Manifest Contract
Last updated: 2026-02-23
Contract version: v1 (+ universe scope extension)

## Canonical file
- `data/backtest_probabilistic/probabilistic_bundle_manifest.json`

## Required top-level fields
- `generated_at_utc`: ISO datetime
- `run_state`: `in_progress|completed|completed_with_budget_stop|stopped_disk_budget|...`
- `history_window.start_utc`
- `history_window.end_utc`
- `market_groups.major[]`
- `market_groups.alt[]`
- `market_groups.all_unique[]`
- `timeframes_min[]`
- `planned_job_count`
- `completed_job_count`
- `success_count`
- `failed_count`
- `jobs[]`

## Scope extension fields (Ticket 1)
- `markets_scope`: `all|universe`
- `universe_file_path`: string (empty when `markets_scope=all`)
- `universe_file_hash`: sha256 hex (empty when `markets_scope=all`)
- `universe_final_1m_markets[]`: optional explicit resolved set used in this run

## Job record fields
- `market`
- `unit_min`
- `output_path`
- `status`: `planned|fetched|failed|skipped_existing|blocked_disk_budget|estimated`
- `rows`
- `file_size_bytes`
- `from_utc`
- `to_utc`
- `elapsed_sec` (when fetched)

## Completion semantics
- `run_state=completed` means fetch completed for the declared scope:
  - `markets_scope=all`: all configured markets/timeframes
  - `markets_scope=universe`: timeframe set where `1m` is universe-scoped and others follow configured market list

## Reproducibility notes
- Preserve manifest per run.
- Persist related summary JSON under `build/Release/logs`.
- Include hash-stamped universe file when scope is `universe`.
