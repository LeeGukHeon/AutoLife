#!/usr/bin/env python3
import importlib.util
import pathlib
import unittest


def load_split_module():
    module_path = pathlib.Path(__file__).resolve().parent / "generate_probabilistic_split_manifest.py"
    spec = importlib.util.spec_from_file_location("generate_probabilistic_split_manifest", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load module: {module_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class PurgedWalkForwardOverlapTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_split_module()

    def test_purged_windows_have_no_overlap(self):
        folds, removed = self.mod.build_fold_windows_purged(
            start_ts=1700000000000,
            end_ts=1700000000000 + (60 * 24 * 60 * 1000),
            train_ratio=0.7,
            valid_ratio=0.15,
            test_ratio=0.15,
            folds=3,
            unit_min=1,
            h1_bars=1,
            h5_bars=5,
            purge_bars=5,
            embargo_bars=1,
        )
        self.assertTrue(len(folds) >= 1)
        self.assertGreaterEqual(int(removed), 0)
        for fold in folds:
            pe = fold.get("purge_embargo", {})
            self.assertIsInstance(pe, dict)
            self.assertFalse(bool(pe.get("overlap_check", {}).get("train_valid_overlap", True)))
            self.assertFalse(bool(pe.get("overlap_check", {}).get("valid_test_overlap", True)))

    def test_zero_purge_embargo_raises_overlap(self):
        with self.assertRaises(RuntimeError):
            self.mod.build_fold_windows_purged(
                start_ts=1700000000000,
                end_ts=1700000000000 + (30 * 24 * 60 * 1000),
                train_ratio=0.7,
                valid_ratio=0.15,
                test_ratio=0.15,
                folds=2,
                unit_min=1,
                h1_bars=1,
                h5_bars=5,
                purge_bars=0,
                embargo_bars=0,
            )


if __name__ == "__main__":
    unittest.main()
