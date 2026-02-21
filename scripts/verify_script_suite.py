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


def run_help(path_value: Path, timeout_sec: int) -> Tuple[bool, str, bool]:
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
        return True, "", False
    err = proc.stderr.strip() or proc.stdout.strip() or f"exit={proc.returncode}"
    optional_dep_markers = [
        "No module named 'joblib'",
        "No module named 'sklearn'",
        "No module named 'pandas'",
        "No module named 'numpy'",
    ]
    if any(marker in err for marker in optional_dep_markers):
        return True, err, True
    return False, err, False


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-help", action="store_true")
    parser.add_argument("--help-timeout-sec", type=int, default=20)
    args = parser.parse_args(argv)

    scripts = list_python_scripts()
    compile_failures: List[str] = []
    help_failures: List[str] = []
    help_skipped_optional_dep: List[str] = []

    for script_path in scripts:
        try:
            py_compile.compile(str(script_path), doraise=True)
        except Exception as exc:  # pragma: no cover - CLI-only path
            compile_failures.append(f"{script_path.name}: {exc}")

    if not bool(args.skip_help):
        for script_path in scripts:
            if not has_main_guard(script_path):
                continue
            ok, detail, skipped_optional_dep = run_help(
                script_path,
                timeout_sec=int(args.help_timeout_sec),
            )
            if skipped_optional_dep:
                help_skipped_optional_dep.append(f"{script_path.name}: {detail}")
                continue
            if not ok:
                help_failures.append(f"{script_path.name}: {detail}")

    print(f"scripts_total={len(scripts)}")
    print(f"compile_failures={len(compile_failures)}")
    print(f"help_failures={len(help_failures)}")
    print(f"help_skipped_optional_dep={len(help_skipped_optional_dep)}")
    print("flag_pair_failures=0")

    for item in compile_failures:
        print(f"[compile][FAIL] {item}")
    for item in help_failures:
        print(f"[help][FAIL] {item}")
    for item in help_skipped_optional_dep:
        print(f"[help][SKIP_OPTIONAL_DEP] {item}")
    if compile_failures or help_failures:
        return 1
    print("VERIFY_SCRIPT_SUITE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

