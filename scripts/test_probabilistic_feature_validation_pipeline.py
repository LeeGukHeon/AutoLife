#!/usr/bin/env python3
import csv
import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from validate_probabilistic_feature_dataset import BASELINE_COLUMN_ORDER, main


def write_json(path: Path, payload) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def to_iso(ts_ms: int) -> str:
    return datetime.fromtimestamp(float(ts_ms) / 1000.0, tz=timezone.utc).isoformat()


def round6(value: float) -> float:
    return round(float(value), 6)


def write_valid_feature_csv(path: Path, market: str, rows: int = 12, roundtrip_cost_bps: float = 12.0) -> None:
    closes = [100.0 + float(i) for i in range(rows)]
    base_ts = 1700000000000
    with path.open("w", encoding="utf-8", newline="\n") as fp:
        writer = csv.DictWriter(fp, fieldnames=BASELINE_COLUMN_ORDER)
        writer.writeheader()
        for i in range(rows):
            close = closes[i]
            h1_label = 1 if (i + 1 < rows and closes[i + 1] > close) else 0
            h5_label = 1 if (i + 5 < rows and closes[i + 5] > close) else 0
            if i + 5 < rows:
                edge = round6((((closes[i + 5] / close) - 1.0) * 10000.0) - float(roundtrip_cost_bps))
            else:
                edge = 0.0

            row = {
                "timestamp": int(base_ts + (i * 60000)),
                "timestamp_utc": to_iso(int(base_ts + (i * 60000))),
                "market": market,
                "close": close,
                "ret_1m": 0.001,
                "ret_5m": 0.002,
                "ret_20m": 0.003,
                "ema_gap_12_26": 0.001,
                "rsi_14": 55.0,
                "atr_pct_14": 0.01,
                "bb_width_20": 0.02,
                "vol_ratio_20": 1.0,
                "notional_ratio_20": 1.0,
                "ctx_5m_age_min": 0.0,
                "ctx_5m_ret_3": 0.001,
                "ctx_5m_ret_12": 0.002,
                "ctx_5m_ema_gap_20": 0.001,
                "ctx_5m_rsi_14": 55.0,
                "ctx_5m_atr_pct_14": 0.01,
                "ctx_15m_age_min": 0.0,
                "ctx_15m_ret_3": 0.001,
                "ctx_15m_ret_12": 0.002,
                "ctx_15m_ema_gap_20": 0.001,
                "ctx_15m_rsi_14": 55.0,
                "ctx_15m_atr_pct_14": 0.01,
                "ctx_60m_age_min": 0.0,
                "ctx_60m_ret_3": 0.001,
                "ctx_60m_ret_12": 0.002,
                "ctx_60m_ema_gap_20": 0.001,
                "ctx_60m_rsi_14": 55.0,
                "ctx_60m_atr_pct_14": 0.01,
                "ctx_240m_age_min": 0.0,
                "ctx_240m_ret_3": 0.001,
                "ctx_240m_ret_12": 0.002,
                "ctx_240m_ema_gap_20": 0.001,
                "ctx_240m_rsi_14": 55.0,
                "ctx_240m_atr_pct_14": 0.01,
                "regime_trend_60_sign": 1,
                "regime_trend_240_sign": 1,
                "regime_vol_60_atr_pct": 0.01,
                "label_up_h1": h1_label,
                "label_up_h5": h5_label,
                "label_edge_bps_h5": edge,
            }
            writer.writerow(row)


def make_contract_payload(version: str) -> dict:
    return {
        "version": version,
        "row_schema": {
            "column_count": len(BASELINE_COLUMN_ORDER),
            "column_order": list(BASELINE_COLUMN_ORDER),
            "label_columns": ["label_up_h1", "label_up_h5", "label_edge_bps_h5"],
        },
        "leakage_rules": {
            "forbidden_fields": ["future_high", "future_low", "future_close"],
        },
    }


class FeatureValidationPipelineTest(unittest.TestCase):
    def test_v1_strict_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            csv_path = root / "prob_features_KRW_BTC_1m_v1.csv"
            manifest = root / "manifest_v1.json"
            contract = root / "contract_v1.json"
            out = root / "out_v1.json"
            write_valid_feature_csv(csv_path, "KRW-BTC")
            write_json(contract, make_contract_payload("v1"))
            write_json(
                manifest,
                {
                    "version": "prob_features_v1",
                    "roundtrip_cost_bps": 12.0,
                    "cost_model": {"enabled": False},
                    "jobs": [{"market": "KRW-BTC", "status": "built", "output_path": str(csv_path)}],
                },
            )
            rc = main(
                [
                    "--dataset-manifest-json",
                    str(manifest),
                    "--contract-json",
                    str(contract),
                    "--pipeline-version",
                    "v1",
                    "--output-json",
                    str(out),
                    "--strict",
                ]
            )
            self.assertEqual(rc, 0)
            result = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(result.get("status"), "pass")
            self.assertEqual(result.get("gate_profile"), "v1")

    def test_v2_strict_fail_on_contract_mismatch(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            csv_path = root / "prob_features_KRW_BTC_1m_v1.csv"
            manifest = root / "manifest_v2.json"
            contract = root / "contract_v1_wrong.json"
            out = root / "out_v2_fail.json"
            write_valid_feature_csv(csv_path, "KRW-BTC")
            write_json(contract, make_contract_payload("v1"))
            write_json(
                manifest,
                {
                    "version": "prob_features_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "roundtrip_cost_bps": 12.0,
                    "cost_model": {"enabled": False},
                    "jobs": [{"market": "KRW-BTC", "status": "built", "output_path": str(csv_path)}],
                },
            )
            rc = main(
                [
                    "--dataset-manifest-json",
                    str(manifest),
                    "--contract-json",
                    str(contract),
                    "--pipeline-version",
                    "v2",
                    "--output-json",
                    str(out),
                    "--strict",
                ]
            )
            self.assertEqual(rc, 2)
            result = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(result.get("status"), "fail")
            self.assertIn("contract_version_matches_pipeline", result.get("preflight_errors", []))

    def test_v2_strict_pass(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            csv_path = root / "prob_features_KRW_BTC_1m_v1.csv"
            manifest = root / "manifest_v2.json"
            contract = root / "contract_v2.json"
            out = root / "out_v2_pass.json"
            write_valid_feature_csv(csv_path, "KRW-BTC")
            write_json(contract, make_contract_payload("v2_draft"))
            write_json(
                manifest,
                {
                    "version": "prob_features_v2_draft",
                    "pipeline_version": "v2",
                    "feature_contract_version": "v2_draft",
                    "roundtrip_cost_bps": 12.0,
                    "cost_model": {"enabled": False},
                    "jobs": [{"market": "KRW-BTC", "status": "built", "output_path": str(csv_path)}],
                },
            )
            rc = main(
                [
                    "--dataset-manifest-json",
                    str(manifest),
                    "--contract-json",
                    str(contract),
                    "--pipeline-version",
                    "v2",
                    "--output-json",
                    str(out),
                    "--strict",
                ]
            )
            self.assertEqual(rc, 0)
            result = json.loads(out.read_text(encoding="utf-8"))
            self.assertEqual(result.get("status"), "pass")
            self.assertEqual(result.get("gate_profile"), "v2_strict")


if __name__ == "__main__":
    unittest.main()
