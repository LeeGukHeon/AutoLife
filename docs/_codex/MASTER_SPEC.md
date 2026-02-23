# CODEX MASTER SPEC — Probabilistic Execution Architecture (Baseline + Extensions)
Last updated: 2026-02-23  
Owner: repo maintainers  
Audience: Codex (context resets frequently)  
Goal: Deliver a robust, reproducible, policy-compliant trading research + execution system.  
Note: This spec is about engineering correctness and safety. NOT a profit guarantee.

## 0) NON-NEGOTIABLE CONSTRAINTS (LEGAL / POLICY / OPERATION)

### 0.1 Legal / ToS compliance (Upbit)
- Do NOT bypass or evade rate limits (no proxy rotation, no multi-IP spraying).
- Obey Remaining-Req headers and handle 429 with backoff.
- Do NOT include Origin header in server-side requests (Origin triggers stricter limits).
- Log safely (no API keys/JWT/signatures).
- Use only official Upbit endpoints (REST + Public WS) and follow published limits.

### 0.2 Live safety
- Default `allow_live_orders=false` always.
- Live enable requires: strict validation pass + parity pass + verification pass + shadow pass.
- Fail-closed: if anything mismatches or is unknown, do not trade.

### 0.3 Determinism & reproducibility
- Every dataset/model/bundle must have: hash, manifest, git sha, args hash.
- Baseline must remain identical when extensions are OFF.

## 1) DOCUMENT SPLIT PLAN (Single Source of Truth + Extensions)

Create / update these docs:

```text
docs/
  00_INDEX.md
  10_BASELINE_IMPLEMENTED.md
  11_BASELINE_RUNBOOK.md
  20_API_POLICY_UPBIT.md
  30_VALIDATION_GATES.md
  40_DATA_MANIFEST_CONTRACT.md
  41_FEATURE_CONTRACT.md
  42_RUNTIME_BUNDLE_CONTRACT.md
  50_EXTENSIONS_OVERVIEW.md
  51_EXT_PURGED_WALKFORWARD.md
  52_EXT_EVENT_SAMPLING.md
  53_EXT_UNCERTAINTY_ENSEMBLE.md
  54_EXT_CONDITIONAL_COST_MODEL.md
  55_EXT_REGIME_SPEC.md
  90_SECURITY_COMPLIANCE.md

docs/_codex/
  BOOTSTRAP.md
  CURRENT_STATE.md
  CONTEXT_REFRESH_PROTOCOL.md
  TICKET_TEMPLATE.md
  PR_CHECKLIST.md
  MASTER_SPEC.md
```

Key rule:
- Baseline docs define current behavior and must not change without explicit versioning.
- Extensions are opt-in via flags and must not affect baseline when OFF.

## 2) REPO ARCHITECTURE TARGET (Baseline preserved, v2 possible)

Support two modes:

### MODE A — Baseline-preserving (preferred first)
- Keep Anchor TF = 1m
- Context TFs = 5m, 15m, 60m, 240m
- Keep existing scripts/runtime structure
- Add extensions behind flags

### MODE B — Full restructure (optional later)
- Introduce feature contract v2 + runtime bundle v2
- Potentially change Anchor TF to 5m
- Keep 1m as micro-volatility inputs only
- Requires full migration plan + parity redefinition

Default:
- Implement MODE A first.
- MODE B is a separate milestone.

## 3) BASELINE (CURRENT IMPLEMENTED) — MUST REMAIN WORKING

### 3.1 Pipeline entrypoint (baseline)
`scripts/run_probabilistic_hybrid_cycle.py`
- fetch -> feature build -> validate(strict) -> split -> baseline -> global train -> export -> parity -> verification

### 3.2 Data collection (baseline)
- `scripts/fetch_probabilistic_training_bundle.py`
- `scripts/fetch_upbit_historical_candles.py`
- Single truth: `data/backtest_probabilistic/probabilistic_bundle_manifest.json`
- Collection windows:
  - 5/15/60/240m: listed_since ~ now
  - 1m: last 2 years ~ now (with universe-scoping extension)

### 3.3 Feature dataset (baseline)
- `scripts/build_probabilistic_feature_dataset.py`
- Contract: `config/model/probabilistic_feature_contract_v1.json`
- Contract summary:
  - 36 feature cols
  - 43 total cols incl meta/labels
- Labels:
  - `label_up_h1`
  - `label_up_h5`
  - `label_edge_bps_h5`
- Optional:
  - triple-barrier labels (default OFF)

### 3.4 Training (baseline)
- `scripts/train_probabilistic_pattern_model_global.py`
- `scripts/train_probabilistic_pattern_model.py`
- Outputs:
  - model artifact (joblib)
  - training summary json

### 3.5 Runtime bundle (baseline)
- `scripts/export_probabilistic_runtime_bundle.py`
- Modes:
  - global_only (default today)
  - hybrid
  - per_market
- Parity:
  - `scripts/validate_runtime_bundle_parity.py`
- Bundle:
  - `config/model/probabilistic_runtime_bundle_v1.json`

### 3.6 Execution engine (baseline)
- Live: `src/runtime/LiveTradingRuntime.cpp`
- Backtest: `src/runtime/BacktestRuntime.cpp`
- Model: `src/analytics/ProbabilisticRuntimeModel.cpp`
- Flow:
  - scan -> probabilistic prefilter -> enrichment -> ranking -> sizing -> post-entry control

## 4) API POLICY IMPLEMENTATION (Upbit-compliant)

### 4.1 Throttling rule (mandatory)
- Parse `Remaining-Req` header.
- Use `sec` to throttle (`min` may be deprecated/fixed).
- If `sec == 0`: sleep to next second boundary + jitter (`<=50ms`).
- If HTTP 429: bounded exponential backoff and stop pressure immediately.
- Never evade limits through distributed IP/proxy tactics.

### 4.2 Origin header rule (mandatory)
- Server-side requests must not attach `Origin`.
- If library auto-adds `Origin`, remove it.

### 4.3 Candle endpoint constraints (mandatory)
- Minute units: `1,3,5,10,15,30,60,240`
- `count` max `200`
- `to` is exclusive "candles before time"
- Missing intervals are possible when no trades occurred

### 4.4 WebSocket (recommended for live)
- Use WS for real-time feeds where possible.
- Respect WS rate limits.
- Implement WS send limiter for subscribe messages.

### 4.5 Logging (mandatory)
- Log endpoint, redacted params, status, Remaining-Req, latency, retry/backoff.
- Never log API key/JWT/signatures.

## 5) CRITICAL UPGRADE: DYNAMIC UNIVERSE + BUFFER

Problem:
- Runtime scans top25 by liquidity and this changes over time.
- Collecting 1m for all markets is expensive.
- Collecting 1m for fixed top25 is unstable and can fail-closed.

Solution:
- Dynamic universe with hysteresis buffers:
  - Active set: topN by recent traded value (`acc_trade_price_24h`)
  - Maintenance set: markets active within last K days + buffer topM
  - `final_1m_markets = Active ∪ Maintenance`

Implementation:

### 5.1 Add script
- `scripts/build_dynamic_universe.py`
- Inputs:
  - Upbit ticker endpoint (24h metrics)
  - market filters (default KRW)
- Args:
  - `--active_size N` default 25
  - `--buffer_size M` default 50
  - `--membership_ttl_days K` default 7
- Outputs:
  - `config/universe/runtime_universe.json`
- Schema:
```json
{
  "generated_at": "...",
  "active_markets": ["..."],
  "maintenance_markets": ["..."],
  "final_1m_markets": ["..."],
  "params": {"active_size": 25, "buffer_size": 50, "membership_ttl_days": 7},
  "source_snapshot": {"ticker_ts": "...", "sha256": "..."}
}
```

### 5.2 Modify fetch script
- `scripts/fetch_probabilistic_training_bundle.py`
- New arg:
  - `--universe-file config/universe/runtime_universe.json`
- Behavior:
  - `5/15/60/240m`: keep configured market scope
  - `1m`: collect/update only `final_1m_markets`

### 5.3 Modify manifest contract
- `data/backtest_probabilistic/probabilistic_bundle_manifest.json`
- Add:
  - `markets_scope: "all" | "universe"`
  - `universe_file_path`
  - `universe_file_hash`
- Completion rule:
  - `run_state=completed` means completed for declared scope.

### 5.4 Feature builder strictness
- If market in `final_1m_markets` and 1m missing: strict fail.
- If market not in `final_1m_markets` and 1m missing: skip with warning.

DoD:
- Pipeline completes with universe-scoped 1m.
- Runtime warmup gates remain stable even when top25 changes.

## 6) EXTENSIONS (ALL DEFAULT OFF)

### EXT-51 Purged Walk-forward + Embargo
- Add purge/embargo split controls.
- Ensure no overlap leakage between train and eval horizons.

### EXT-52 Event sampling
- Add sampling modes (`time|dollar|volatility`) for training rows only.
- Apply post-feature-compute to keep schema stable.

### EXT-53 Uncertainty ensemble
- Train K-model ensemble and export all artifacts.
- Runtime uses `prob_mean`, `prob_std` for sizing attenuation.

### EXT-54 Conditional cost model
- Replace fixed cost with variable cost estimate in labels/runtime edge.
- Python and C++ functional isomorphism required.

### EXT-55 Regime hardening
- Explicit `normal|volatile|hostile` states.
- Deterministic no-lookahead transitions and action policy.

## 7) VALIDATION & TESTING (FAIL-CLOSED GATES)

Gate 1:
- strict feature data integrity (`validate_probabilistic_feature_dataset.py --strict`)

Gate 2:
- strict bundle parity (`validate_runtime_bundle_parity.py`)

Gate 3:
- strategy verification (`run_verification.py`), OOS-focused

Gate 4:
- shadow run with live orders disabled

Gate 5:
- staged live enable only after shadow pass

## 8) IMPLEMENTATION TICKETS (ORDER)

0. Codex bootstrap reliability pack (mandatory docs)
1. Dynamic universe + buffer + scope-aware manifest
2. Purged walk-forward + embargo (EXT-51)
3. Conditional cost model (EXT-54)
4. Regime spec (EXT-55)
5. Uncertainty ensemble (EXT-53)
6. Event sampling (EXT-52)
7. Optional full restructure MODE B (v2 contracts)

## 9) CODEX CONTEXT RESET SOLUTION (MANDATORY WORKFLOW)

At session start/reset:
1. Read `docs/_codex/BOOTSTRAP.md`
2. Read `docs/_codex/CURRENT_STATE.md`
3. Identify active ticket (PR + template)
4. Run minimal checks for touched area
5. Update `CURRENT_STATE.md` in each PR

PR rule:
- No PR accepted without `PR_CHECKLIST` completion.

## 10) REFERENCES (Upbit official docs)
- Rate limits and Remaining-Req:
  - https://docs.upbit.com/kr/reference/rate-limits
- User request guide:
  - https://docs.upbit.com/kr/v1.5.9/docs/user-request-guide
- REST API best practice:
  - https://docs.upbit.com/kr/docs/rest-api-best-practice
- Minute candles endpoint:
  - https://docs.upbit.com/kr/reference/list-candles-minutes
- Origin header stricter limits changelog:
  - https://docs.upbit.com/kr/changelog/origin_rate_limit
- WebSocket best practices:
  - https://global-docs.upbit.com/docs/websocket-best-practice
- WebSocket trade subscription format:
  - https://docs.upbit.com/kr/reference/websocket-trade
