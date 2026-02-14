#!/usr/bin/env python3
import argparse
import csv
import math
import random
from datetime import datetime, timezone

from _script_common import resolve_repo_path


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-path", "-OutputPath", default="./data/backtest_curated/regime_mix_12000_1m.csv")
    parser.add_argument("--candles", "-Candles", type=int, default=12000)
    parser.add_argument("--start-price", "-StartPrice", type=float, default=1_000_000.0)
    parser.add_argument("--seed", "-Seed", type=int, default=20260213)
    parser.add_argument("--profile", "-Profile", choices=["mixed", "range_bias", "trend_down_shock"], default="mixed")
    return parser.parse_args(argv)


def pick_regime(rng: random.Random, candidates):
    total = sum(float(x["weight"]) for x in candidates)
    x = rng.random() * total
    acc = 0.0
    for item in candidates:
        acc += float(item["weight"])
        if x <= acc:
            return item
    return candidates[0]


def main(argv=None) -> int:
    args = parse_args(argv)
    if args.candles < 1000:
        raise RuntimeError("Candles must be >= 1000 for regime-mix test data.")
    if args.start_price <= 0:
        raise RuntimeError("StartPrice must be > 0.")

    output_path = resolve_repo_path(args.output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    rng = random.Random(args.seed)
    now_ms = int(datetime.now(tz=timezone.utc).timestamp() * 1000)
    ts = now_ms - (int(args.candles) * 60000)
    price = float(args.start_price)

    regimes = [
        {"name": "TRENDING_UP", "drift": 0.00035, "sigma": 0.0018, "volMul": 1.2, "weight": 1.0},
        {"name": "TRENDING_DOWN", "drift": -0.00035, "sigma": 0.0020, "volMul": 1.25, "weight": 1.0},
        {"name": "RANGING", "drift": 0.0, "sigma": 0.0012, "volMul": 0.95, "weight": 1.0},
        {"name": "HIGH_VOL", "drift": 0.0, "sigma": 0.0038, "volMul": 1.8, "weight": 1.0},
    ]
    if args.profile == "range_bias":
        regimes = [
            {"name": "RANGING", "drift": 0.0, "sigma": 0.0010, "volMul": 0.90, "weight": 4.0},
            {"name": "MEANREV_NOISE", "drift": 0.0, "sigma": 0.0014, "volMul": 1.00, "weight": 2.0},
            {"name": "TRENDING_UP", "drift": 0.00020, "sigma": 0.0015, "volMul": 1.05, "weight": 1.0},
            {"name": "TRENDING_DOWN", "drift": -0.00020, "sigma": 0.0016, "volMul": 1.05, "weight": 1.0},
        ]
    elif args.profile == "trend_down_shock":
        regimes = [
            {"name": "TRENDING_DOWN", "drift": -0.00045, "sigma": 0.0024, "volMul": 1.35, "weight": 4.0},
            {"name": "HIGH_VOL", "drift": -0.00010, "sigma": 0.0045, "volMul": 2.0, "weight": 2.0},
            {"name": "RANGING", "drift": 0.0, "sigma": 0.0015, "volMul": 1.0, "weight": 1.0},
            {"name": "TRENDING_UP", "drift": 0.00020, "sigma": 0.0018, "volMul": 1.1, "weight": 1.0},
        ]

    rows = []
    i = 0
    while i < args.candles:
        regime = pick_regime(rng, regimes)
        block_len = rng.randint(180, 719)
        if i + block_len > args.candles:
            block_len = args.candles - i

        for _ in range(block_len):
            open_price = price
            ret = rng.gauss(regime["drift"], regime["sigma"])

            shock_prob = 0.008
            if args.profile == "trend_down_shock":
                shock_prob = 0.018
            elif args.profile == "range_bias":
                shock_prob = 0.006
            if rng.random() < shock_prob:
                shock = rng.gauss(0.0, max(0.0025, float(regime["sigma"]) * 2.8))
                ret += shock

            close = open_price * (1.0 + ret)
            if close <= 0:
                close = max(1.0, open_price * 0.98)

            abs_ret = abs(ret)
            wick_base = max(0.0004, float(regime["sigma"]) * 0.9)
            wick_u = open_price * (wick_base + (rng.random() * wick_base))
            wick_d = open_price * (wick_base + (rng.random() * wick_base))
            high = max(open_price, close) + wick_u
            low = min(open_price, close) - wick_d
            if low <= 0:
                low = max(0.1, min(open_price, close) * 0.995)

            base_vol = 30.0 + (rng.random() * 40.0)
            vol = base_vol * float(regime["volMul"]) * (1.0 + (abs_ret * 140.0))
            if rng.random() < 0.03:
                vol *= (1.3 + (rng.random() * 1.7))

            rows.append(
                {
                    "timestamp": int(ts),
                    "open": round(open_price, 6),
                    "high": round(high, 6),
                    "low": round(low, 6),
                    "close": round(close, 6),
                    "volume": round(vol, 6),
                }
            )
            price = close
            ts += 60000
            i += 1

    with output_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=["timestamp", "open", "high", "low", "close", "volume"])
        writer.writeheader()
        writer.writerows(rows)

    print(f"generated_file={output_path}")
    print(f"rows={args.candles}")
    print("timestamp_unit=ms")
    print(f"profile={args.profile}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
