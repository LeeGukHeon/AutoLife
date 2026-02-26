# AutoLife Backtest Reproducibility Checklist

## Goal
Use this checklist to prevent "good-looking backtest illusion" and keep only results that are reproducible in live execution.

## 1. Data Integrity
1. No look-ahead leakage.
- Do not use future bars in feature generation.
- In multi-timeframe joins, use only confirmed higher-timeframe candles.
- Ensure label horizon does not overlap train ranges across splits.

2. Live/backtest parity.
- Use identical feature calculation path.
- Use identical cost model.
- Use identical frontier policy.
- Do not add backtest-only decision compensation.

3. Purge gap required.
- Apply purge gap near split boundaries based on average/max holding duration.
- Remove label overlap between adjacent signals.

4. Missing/outlier handling consistency.
- Missing-value handling must match live behavior.
- Abnormal candle and thin-volume filtering rules must be identical.

## 2. Cost and Fill Model
5. Always include fee and slippage.
- Use measured exchange fee assumptions.
- Include slippage proxy (spread/ATR/liquidity).
- Test both mean and tail-safe modes.

6. Avoid optimistic fill assumptions.
- Do not assume close-price fills always.
- Do not assume 100% limit-fill success.
- Include partial-fill and unfilled-order risk.

7. Track estimated vs realized cost gap.
- Persist estimated cost and realized cost gap.
- Monitor tail exceed rate explicitly.

## 3. Parameter Search Discipline
8. Search budget cap.
- Candidate count per round must stay in `K=3..10`.
- No unlimited grid search.
- No repeated tuning on the same split until protocol cycle is complete.

9. Triple split isolation.
- Development: exploration allowed.
- Validation: selection only.
- Quarantine holdout: strictly no tuning.

10. Regime-cross validation.
- Evaluate by trend/range/high-vol/low-vol and liquidity buckets.
- Reject policies that perform only in narrow slices.

## 4. Performance Interpretation
11. No single-metric optimization.
- Evaluation order:
  - 1) drawdown/risk,
  - 2) execution and cost alignment,
  - 3) EV/profitability,
  - 4) throughput/coverage.
- Do not decide by Sharpe only.

12. Win-rate illusion guard.
- High win-rate with weak EV is reject.
- Check average win vs average loss ratio.

13. Tail risk review.
- Maximum single-loss,
- loss streak,
- stressed volatility segment performance.

## 5. Phase 4 Portfolio Guardrails
14. Correlation awareness mandatory.
- Check simultaneous directional overlap across correlated symbols.
- Check cluster-cap compliance.

15. Gross/per-market cap violations must stay zero.
- Any cap violation in backtest is immediate reject.

16. Concurrent position behavior must be compared.
- Compare Phase 4 OFF vs ON.
- Verify overlap reduction in high-correlation symbols.

## 6. Live Readiness
17. Backtest vs paper consistency required.
- Paper behavior should align with backtest in edge, cost, and slippage profile.

18. Real-time drift monitoring required.
- Monitor EV calibration drift.
- Monitor probability calibration drift.

19. No immediate live scale-up after parameter change.
- Require paper pass window (minimum 24h, recommended 48-72h) before staged promotion.

## 7. Overfitting Warning Signals
- Performance jumps only when candidate count grows.
- Profit concentrated in a few narrow periods.
- Small parameter change causes large performance collapse.
- Quarantine performance degrades sharply.
- Persistent live realized-edge shortfall vs expected edge.

If any signal appears:
- Stop exploration immediately.
- Reassess structure, not just thresholds.

## 8. Pre-Close Checklist
- Purge gap applied.
- Fee/slippage applied.
- Dev/Val/Quarantine isolation verified.
- Regime-cross evaluation completed.
- Cap violations zero.
- Tail risk reviewed.
- Backtest vs paper alignment reviewed.
- Bundle diff recorded.

## Core Principle
Backtesting is not a tool to prove profit.
Backtesting is a safety filter to increase survival probability under live uncertainty.
