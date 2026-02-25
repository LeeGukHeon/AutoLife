#!/usr/bin/env python3
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_source_encoding as encoding_check
import evaluate_v22_guard_promotion as v22_eval


def _write_json(path_value: Path, payload):
    path_value.parent.mkdir(parents=True, exist_ok=True)
    path_value.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


class V22GuardPromotionEvaluatorTest(unittest.TestCase):
    def test_pass_case(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ver_off = root / "ver_off.json"
            ver_on = root / "ver_on.json"
            daily_off = root / "daily_off.json"
            daily_on = root / "daily_on.json"
            workflow = root / "workflow.json"
            out_json = root / "out.json"

            _write_json(ver_off, {"aggregates": {"avg_profit_factor": 1.0229, "avg_expectancy_krw": -0.6964, "avg_total_trades": 14.0}})
            _write_json(ver_on, {"aggregates": {"avg_profit_factor": 1.0229, "avg_expectancy_krw": -0.6964, "avg_total_trades": 14.0}})
            _write_json(daily_off, {"status": "pass", "aggregates": {"total_profit_sum": 118.672413, "nonpositive_day_count": 23}})
            _write_json(daily_on, {"status": "pass", "aggregates": {"total_profit_sum": 269.508805, "nonpositive_day_count": 22}})
            _write_json(workflow, {"status": "pass", "summary": {"v16_fail_reasons": []}})

            rc = v22_eval.main(
                [
                    "--verification-off-json",
                    str(ver_off),
                    "--verification-on-json",
                    str(ver_on),
                    "--daily-off-json",
                    str(daily_off),
                    "--daily-on-json",
                    str(daily_on),
                    "--workflow-json",
                    str(workflow),
                    "--output-json",
                    str(out_json),
                ]
            )
            self.assertEqual(0, rc)
            payload = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual("pass", payload.get("status"))

    def test_fail_case(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            ver_off = root / "ver_off.json"
            ver_on = root / "ver_on.json"
            daily_off = root / "daily_off.json"
            daily_on = root / "daily_on.json"
            workflow = root / "workflow.json"
            out_json = root / "out.json"

            _write_json(ver_off, {"aggregates": {"avg_profit_factor": 1.0, "avg_expectancy_krw": 0.0, "avg_total_trades": 10.0}})
            _write_json(ver_on, {"aggregates": {"avg_profit_factor": 1.1, "avg_expectancy_krw": 0.0, "avg_total_trades": 10.0}})
            _write_json(daily_off, {"status": "pass", "aggregates": {"total_profit_sum": 100.0, "nonpositive_day_count": 5}})
            _write_json(daily_on, {"status": "fail", "aggregates": {"total_profit_sum": 90.0, "nonpositive_day_count": 7}})
            _write_json(workflow, {"status": "fail", "summary": {"v16_fail_reasons": ["nontarget_positive_trade_delta"]}})

            rc = v22_eval.main(
                [
                    "--verification-off-json",
                    str(ver_off),
                    "--verification-on-json",
                    str(ver_on),
                    "--daily-off-json",
                    str(daily_off),
                    "--daily-on-json",
                    str(daily_on),
                    "--workflow-json",
                    str(workflow),
                    "--output-json",
                    str(out_json),
                ]
            )
            self.assertNotEqual(0, rc)
            payload = json.loads(out_json.read_text(encoding="utf-8"))
            self.assertEqual("fail", payload.get("status"))
            self.assertTrue(payload.get("failed_checks"))


class SourceEncodingCheckTest(unittest.TestCase):
    def setUp(self):
        self.repo_root = Path(__file__).resolve().parent.parent
        self.tmp_root = self.repo_root / "build" / "Release" / "tmp_encoding_unittest"
        if self.tmp_root.exists():
            shutil.rmtree(self.tmp_root)
        self.tmp_root.mkdir(parents=True, exist_ok=True)

    def tearDown(self):
        if self.tmp_root.exists():
            shutil.rmtree(self.tmp_root)

    def test_pass_without_bom(self):
        sample_dir = self.tmp_root / "pass"
        sample_dir.mkdir(parents=True, exist_ok=True)
        sample_file = sample_dir / "sample.cpp"
        sample_file.write_text("int main(){return 0;}\n", encoding="utf-8")
        output_json = self.tmp_root / "pass_report.json"

        rc = encoding_check.main(
            [
                "--roots",
                str(sample_dir.relative_to(self.repo_root)).replace("\\", "/"),
                "--extra-files",
                "",
                "--exclude-dirs",
                "",
                "--output-json",
                str(output_json.relative_to(self.repo_root)).replace("\\", "/"),
            ]
        )
        self.assertEqual(0, rc)
        payload = json.loads(output_json.read_text(encoding="utf-8"))
        self.assertEqual("pass", payload.get("status"))

    def test_fail_with_bom(self):
        sample_dir = self.tmp_root / "fail"
        sample_dir.mkdir(parents=True, exist_ok=True)
        sample_file = sample_dir / "sample.py"
        sample_file.write_bytes(b"\xef\xbb\xbfprint('x')\n")
        output_json = self.tmp_root / "fail_report.json"

        rc = encoding_check.main(
            [
                "--roots",
                str(sample_dir.relative_to(self.repo_root)).replace("\\", "/"),
                "--extra-files",
                "",
                "--exclude-dirs",
                "",
                "--output-json",
                str(output_json.relative_to(self.repo_root)).replace("\\", "/"),
            ]
        )
        self.assertNotEqual(0, rc)
        payload = json.loads(output_json.read_text(encoding="utf-8"))
        self.assertEqual("fail", payload.get("status"))
        self.assertGreater(payload.get("summary", {}).get("bom_violation_count", 0), 0)


if __name__ == "__main__":
    unittest.main()
