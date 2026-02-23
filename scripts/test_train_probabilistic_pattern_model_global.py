#!/usr/bin/env python3
import argparse
import csv
import tempfile
import unittest
from pathlib import Path
from typing import Any, Dict, List

from train_probabilistic_pattern_model import FEATURE_COLUMNS
from train_probabilistic_pattern_model_global import stream_dataset_into_global_states


def _base_feature_row(timestamp: int) -> Dict[str, Any]:
    row: Dict[str, Any] = {
        "timestamp": str(int(timestamp)),
        "label_up_h1": "1",
        "label_up_h5": "1",
        "label_edge_bps_h5": "0.0",
    }
    for key in FEATURE_COLUMNS:
        if key == "rsi_14" or key.endswith("_rsi_14"):
            row[key] = "50.0"
        elif key.endswith("_age_min"):
            row[key] = "5.0"
        elif key.endswith("_sign"):
            row[key] = "1.0"
        else:
            row[key] = "0.01"
    return row


def _write_rows(path: Path, rows: List[Dict[str, Any]]) -> None:
    fieldnames: List[str] = []
    seen = set()
    for row in rows:
        for key in row.keys():
            if key in seen:
                continue
            seen.add(key)
            fieldnames.append(str(key))
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _empty_fold_state() -> Dict[str, Any]:
    return {
        "train_x": [],
        "train_y_h1": [],
        "train_y_h5": [],
        "train_edge": [],
        "infer": {
            "valid": {"x": [], "y_h1": [], "y_h5": [], "edge": []},
            "test": {"x": [], "y_h1": [], "y_h5": [], "edge": []},
        },
    }


class TrainProbabilisticPatternModelGlobalStreamTest(unittest.TestCase):
    def test_stream_uses_custom_target_columns(self):
        with tempfile.TemporaryDirectory() as td:
            csv_path = Path(td) / "d.csv"

            r1 = _base_feature_row(2)
            r1["tb_h1"] = "1"
            r1["tb_h5"] = "1"
            r1["tb_edge"] = "8.5"

            r2 = _base_feature_row(3)
            r2["tb_h1"] = "-1"
            r2["tb_h5"] = "-1"
            r2["tb_edge"] = "-4.0"

            r3 = _base_feature_row(4)
            r3["tb_h1"] = ""
            r3["tb_h5"] = ""
            r3["tb_edge"] = "3.0"

            _write_rows(csv_path, [r1, r2, r3])

            args = argparse.Namespace(
                h1_target_column="tb_h1",
                h5_target_column="tb_h5",
                edge_column="tb_edge",
                drop_neutral_target=True,
                clip_abs=8.0,
                batch_size=999_999,
                infer_batch_size=999_999,
                prob_eps=1e-6,
            )
            fold_windows = {
                1: {
                    "train_start": 1,
                    "train_end": 10,
                    "valid_start": 11,
                    "valid_end": 20,
                    "test_start": 21,
                    "test_end": 30,
                }
            }
            states = {1: _empty_fold_state()}

            out = stream_dataset_into_global_states(
                dataset_market="KRW-TEST",
                csv_path=csv_path,
                dataset_fold_windows=fold_windows,
                global_states=states,
                args=args,
            )

            self.assertEqual(3, int(out.get("rows_total", 0)))
            self.assertEqual(2, int(out.get("rows_used", 0)))
            self.assertEqual(1, int(out.get("rows_skipped", 0)))
            state = states[1]
            self.assertEqual([1, 0], list(state["train_y_h1"]))
            self.assertEqual([1, 0], list(state["train_y_h5"]))
            self.assertEqual([8.5, -4.0], list(state["train_edge"]))

    def test_stream_falls_back_to_default_edge_column(self):
        with tempfile.TemporaryDirectory() as td:
            csv_path = Path(td) / "d.csv"

            r1 = _base_feature_row(2)
            r1["custom_edge"] = ""
            r1["label_up_h1"] = "1"
            r1["label_up_h5"] = "1"
            r1["label_edge_bps_h5"] = "6.25"
            _write_rows(csv_path, [r1])

            args = argparse.Namespace(
                h1_target_column="label_up_h1",
                h5_target_column="label_up_h5",
                edge_column="custom_edge",
                drop_neutral_target=True,
                clip_abs=8.0,
                batch_size=999_999,
                infer_batch_size=999_999,
                prob_eps=1e-6,
            )
            fold_windows = {
                1: {
                    "train_start": 1,
                    "train_end": 10,
                    "valid_start": 11,
                    "valid_end": 20,
                    "test_start": 21,
                    "test_end": 30,
                }
            }
            states = {1: _empty_fold_state()}

            out = stream_dataset_into_global_states(
                dataset_market="KRW-TEST",
                csv_path=csv_path,
                dataset_fold_windows=fold_windows,
                global_states=states,
                args=args,
            )

            self.assertEqual(1, int(out.get("rows_used", 0)))
            self.assertEqual([6.25], list(states[1]["train_edge"]))

    def test_stream_can_keep_neutral_signed_labels(self):
        with tempfile.TemporaryDirectory() as td:
            csv_path = Path(td) / "d.csv"

            r1 = _base_feature_row(2)
            r1["tb_h1"] = "0"
            r1["tb_h5"] = "0"
            r1["tb_edge"] = "1.0"
            _write_rows(csv_path, [r1])

            args = argparse.Namespace(
                h1_target_column="tb_h1",
                h5_target_column="tb_h5",
                edge_column="tb_edge",
                drop_neutral_target=False,
                clip_abs=8.0,
                batch_size=999_999,
                infer_batch_size=999_999,
                prob_eps=1e-6,
            )
            fold_windows = {
                1: {
                    "train_start": 1,
                    "train_end": 10,
                    "valid_start": 11,
                    "valid_end": 20,
                    "test_start": 21,
                    "test_end": 30,
                }
            }
            states = {1: _empty_fold_state()}

            out = stream_dataset_into_global_states(
                dataset_market="KRW-TEST",
                csv_path=csv_path,
                dataset_fold_windows=fold_windows,
                global_states=states,
                args=args,
            )

            self.assertEqual(1, int(out.get("rows_used", 0)))
            self.assertEqual([0], list(states[1]["train_y_h1"]))
            self.assertEqual([0], list(states[1]["train_y_h5"]))


if __name__ == "__main__":
    unittest.main()
