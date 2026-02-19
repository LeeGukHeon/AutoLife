#!/usr/bin/env python3
import argparse
import py_compile
import subprocess
import sys
from pathlib import Path
from typing import List, Tuple


ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = ROOT / "scripts"


def list_python_scripts() -> List[Path]:
    return sorted([p for p in SCRIPTS_DIR.glob("*.py") if p.is_file()], key=lambda p: p.name.lower())


def has_main_guard(path_value: Path) -> bool:
    text = path_value.read_text(encoding="utf-8-sig")
    return "__main__" in text


def run_help(path_value: Path, timeout_sec: int) -> Tuple[bool, str]:
    proc = subprocess.run(
        [sys.executable, str(path_value), "--help"],
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=max(1, int(timeout_sec)),
    )
    if proc.returncode == 0:
        return True, ""
    err = proc.stderr.strip() or proc.stdout.strip() or f"exit={proc.returncode}"
    return False, err


def verify_flag_pair(path_value: Path, enable_flag: str, disable_flag: str) -> Tuple[bool, str]:
    text = path_value.read_text(encoding="utf-8-sig")
    if enable_flag in text and disable_flag not in text:
        return False, f"missing pair: {disable_flag}"
    return True, ""


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-help", action="store_true")
    parser.add_argument("--help-timeout-sec", type=int, default=20)
    args = parser.parse_args(argv)

    scripts = list_python_scripts()
    compile_failures: List[str] = []
    help_failures: List[str] = []
    pair_failures: List[str] = []

    for script_path in scripts:
        try:
            py_compile.compile(str(script_path), doraise=True)
        except Exception as exc:  # pragma: no cover - CLI-only path
            compile_failures.append(f"{script_path.name}: {exc}")

    if not bool(args.skip_help):
        for script_path in scripts:
            if not has_main_guard(script_path):
                continue
            ok, detail = run_help(script_path, timeout_sec=int(args.help_timeout_sec))
            if not ok:
                help_failures.append(f"{script_path.name}: {detail}")

    # Wrapper chain integrity checks for adaptive-threshold flag pairs.
    pair_checks = [
        (
            SCRIPTS_DIR / "run_realdata_candidate_loop.py",
            "--enable-hostility-adaptive-thresholds",
            "--disable-hostility-adaptive-thresholds",
        ),
        (
            SCRIPTS_DIR / "run_realdata_candidate_loop.py",
            "--enable-hostility-adaptive-trades-only",
            "--disable-hostility-adaptive-trades-only",
        ),
        (
            SCRIPTS_DIR / "run_candidate_train_eval_cycle.py",
            "--enable-hostility-adaptive-thresholds",
            "--disable-hostility-adaptive-thresholds",
        ),
        (
            SCRIPTS_DIR / "run_candidate_train_eval_cycle.py",
            "--enable-hostility-adaptive-trades-only",
            "--disable-hostility-adaptive-trades-only",
        ),
        (
            SCRIPTS_DIR / "tune_candidate_gate_trade_density.py",
            "--enable-hostility-adaptive-thresholds",
            "--disable-hostility-adaptive-thresholds",
        ),
        (
            SCRIPTS_DIR / "tune_candidate_gate_trade_density.py",
            "--enable-hostility-adaptive-trades-only",
            "--disable-hostility-adaptive-trades-only",
        ),
    ]
    for script_path, enable_flag, disable_flag in pair_checks:
        ok, detail = verify_flag_pair(script_path, enable_flag=enable_flag, disable_flag=disable_flag)
        if not ok:
            pair_failures.append(f"{script_path.name}: {detail}")

    print(f"scripts_total={len(scripts)}")
    print(f"compile_failures={len(compile_failures)}")
    print(f"help_failures={len(help_failures)}")
    print(f"flag_pair_failures={len(pair_failures)}")

    for item in compile_failures:
        print(f"[compile][FAIL] {item}")
    for item in help_failures:
        print(f"[help][FAIL] {item}")
    for item in pair_failures:
        print(f"[pair][FAIL] {item}")

    if compile_failures or help_failures or pair_failures:
        return 1
    print("VERIFY_SCRIPT_SUITE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

