# Security and Compliance
Last updated: 2026-02-23

## Secrets handling
- Never commit credentials or generated auth tokens.
- Keep secrets in environment or secure local configuration.
- Redact secrets in logs and summaries.

## Logging policy
- Allowed:
  - endpoint names
  - sanitized params
  - status codes
  - rate limit headers
  - latency/retry/backoff metadata
- Forbidden:
  - API key
  - JWT
  - signatures
  - full auth headers

## ToS policy
- Follow Upbit published limits and official endpoints.
- No bypass/evasion behavior for throttling.
- No `Origin` header on server-side API requests.

## Live execution safety
- Default `allow_live_orders=false`.
- Live enable only after strict validation + parity + verification + shadow pass.
- Any unknown/mismatch condition must fail closed (no trade).

## Reproducibility and auditability
- Persist dataset/model/bundle manifests with hashes.
- Record git commit SHA and key arguments for each run.
- Keep gate outputs for audit trail.
