#!/usr/bin/env python3
import argparse
import datetime as dt
import json
from pathlib import Path
from typing import Any, Dict, List, Tuple

from _script_common import dump_json, resolve_repo_path, run_command


TEXT_SUFFIXES = {
    ".md",
    ".txt",
    ".json",
    ".yml",
    ".yaml",
    ".py",
    ".cpp",
    ".h",
    ".cmake",
    ".bat",
}

DEFAULT_TEST_EXES = [
    r".\build\Release\AutoLifeStateTest.exe",
    r".\build\Release\AutoLifeEventJournalTest.exe",
    r".\build\Release\AutoLifeExecutionStateTest.exe",
    r".\build\Release\AutoLifeRateLimiterComplianceTest.exe",
    r".\build\Release\AutoLifeTest.exe",
]

DEFAULT_HELP_COMMANDS = [
    [r"python", r"scripts/run_ci_operational_gate.py", "--help"],
    [r"python", r"scripts/run_realdata_candidate_loop.py", "--help"],
    [r"python", r"scripts/validate_operational_readiness.py", "--help"],
]

ACTIVE_REF_ROOTS = [
    "README.md",
    "docs",
    "scripts",
    ".github/workflows",
    "CMakeLists.txt",
    "src",
    "include",
    "tests",
    "config",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify Wave A cleanup state.")
    parser.add_argument(
        "--manifest-json",
        default=r".\legacy_archive\manifest.json",
        help="Wave A archive manifest path",
    )
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\wave_a_cleanup_verification.json",
        help="Verification report path",
    )
    parser.add_argument(
        "--run-tests",
        action="store_true",
        help="Run local test executables as part of verification",
    )
    parser.add_argument(
        "--run-help-checks",
        action="store_true",
        help="Run core operation scripts with --help",
    )
    return parser.parse_args()


def now_utc_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def load_manifest(path_value: Path) -> Dict[str, Any]:
    raw = json.loads(path_value.read_text(encoding="utf-8-sig"))
    if not isinstance(raw, dict):
        raise ValueError("manifest root must be an object")
    return raw


def check_moved_files(repo_root: Path, manifest: Dict[str, Any]) -> Tuple[bool, List[Dict[str, Any]]]:
    wave_a = (manifest.get("wave_a") or {}) if isinstance(manifest.get("wave_a"), dict) else {}
    moved = wave_a.get("moved_files")
    final_delete_executed = bool(wave_a.get("final_delete_executed"))
    if not isinstance(moved, list):
        return False, [{"error": "manifest.wave_a.moved_files is missing or invalid"}]

    rows: List[Dict[str, Any]] = []
    all_ok = True
    for item in moved:
        src = str((item or {}).get("source_path", "")).strip()
        arc = str((item or {}).get("archive_path", "")).strip()
        if not src or not arc:
            all_ok = False
            rows.append({"source_path": src, "archive_path": arc, "ok": False, "reason": "invalid_manifest_item"})
            continue

        src_abs = (repo_root / src).resolve()
        arc_abs = (repo_root / arc).resolve()
        source_exists = src_abs.exists()
        archive_exists = arc_abs.exists()
        ok = (not source_exists) and (archive_exists or final_delete_executed)
        if not ok:
            all_ok = False
        rows.append(
            {
                "source_path": src,
                "archive_path": arc,
                "source_exists": source_exists,
                "archive_exists": archive_exists,
                "final_delete_executed": final_delete_executed,
                "ok": ok,
            }
        )
    return all_ok, rows


def check_keep_files(repo_root: Path, manifest: Dict[str, Any]) -> Tuple[bool, List[Dict[str, Any]]]:
    keep = (
        (manifest.get("wave_a") or {}).get("explicit_keep")
        if isinstance(manifest.get("wave_a"), dict)
        else None
    )
    if not isinstance(keep, list):
        return False, [{"error": "manifest.wave_a.explicit_keep is missing or invalid"}]

    rows: List[Dict[str, Any]] = []
    all_ok = True
    for rel in keep:
        path_rel = str(rel).strip()
        exists = (repo_root / path_rel).exists()
        if not exists:
            all_ok = False
        rows.append({"path": path_rel, "exists": exists, "ok": exists})
    return all_ok, rows


def iter_text_files(repo_root: Path, roots: List[str]) -> List[Path]:
    files: List[Path] = []
    for root in roots:
        p = (repo_root / root).resolve()
        if not p.exists():
            continue
        if p.is_file():
            files.append(p)
            continue
        for candidate in p.rglob("*"):
            if not candidate.is_file():
                continue
            if "legacy_archive" in candidate.parts:
                continue
            if candidate.name == "CMakeLists.txt":
                files.append(candidate)
                continue
            if candidate.suffix.lower() in TEXT_SUFFIXES:
                files.append(candidate)
    return files


def find_non_archive_refs(repo_root: Path, old_path: str, files: List[Path]) -> List[Dict[str, Any]]:
    hits: List[Dict[str, Any]] = []
    marker = "legacy_archive/"
    for file_path in files:
        try:
            text = file_path.read_text(encoding="utf-8", errors="ignore")
        except Exception:
            continue
        start = 0
        while True:
            idx = text.find(old_path, start)
            if idx < 0:
                break
            prefix = text[max(0, idx - len(marker)) : idx]
            if prefix != marker:
                line_no = text.count("\n", 0, idx) + 1
                rel = file_path.relative_to(repo_root).as_posix()
                hits.append({"file": rel, "line": line_no})
                if len(hits) >= 20:
                    return hits
            start = idx + len(old_path)
    return hits


def check_reference_integrity(repo_root: Path, manifest: Dict[str, Any]) -> Tuple[bool, List[Dict[str, Any]]]:
    moved = (
        (manifest.get("wave_a") or {}).get("moved_files")
        if isinstance(manifest.get("wave_a"), dict)
        else None
    )
    if not isinstance(moved, list):
        return False, [{"error": "manifest.wave_a.moved_files is missing or invalid"}]

    files = iter_text_files(repo_root, ACTIVE_REF_ROOTS)
    rows: List[Dict[str, Any]] = []
    all_ok = True
    for item in moved:
        old_path = str((item or {}).get("source_path", "")).strip()
        if not old_path:
            continue
        refs = find_non_archive_refs(repo_root, old_path, files)
        ok = len(refs) == 0
        if not ok:
            all_ok = False
        rows.append({"source_path": old_path, "ok": ok, "hits": refs})
    return all_ok, rows


def run_test_executables(repo_root: Path) -> Tuple[bool, List[Dict[str, Any]]]:
    rows: List[Dict[str, Any]] = []
    all_ok = True
    for exe in DEFAULT_TEST_EXES:
        cmd = [str((repo_root / exe).resolve())]
        result = run_command(cmd, cwd=repo_root)
        ok = result.exit_code == 0
        if not ok:
            all_ok = False
        rows.append(
            {
                "command": cmd,
                "exit_code": result.exit_code,
                "ok": ok,
                "stdout_tail": result.stdout.splitlines()[-5:],
                "stderr_tail": result.stderr.splitlines()[-5:],
            }
        )
    return all_ok, rows


def run_help_checks(repo_root: Path) -> Tuple[bool, List[Dict[str, Any]]]:
    rows: List[Dict[str, Any]] = []
    all_ok = True
    for cmd in DEFAULT_HELP_COMMANDS:
        result = run_command(cmd, cwd=repo_root)
        ok = result.exit_code == 0
        if not ok:
            all_ok = False
        rows.append(
            {
                "command": cmd,
                "exit_code": result.exit_code,
                "ok": ok,
                "stdout_tail": result.stdout.splitlines()[-5:],
                "stderr_tail": result.stderr.splitlines()[-5:],
            }
        )
    return all_ok, rows


def main() -> int:
    args = parse_args()
    repo_root = Path.cwd().resolve()
    manifest_path = resolve_repo_path(args.manifest_json)
    output_path = resolve_repo_path(args.output_json)

    report: Dict[str, Any] = {
        "generated_at_utc": now_utc_iso(),
        "repo_root": str(repo_root),
        "manifest_json": str(manifest_path),
        "checks": {},
    }

    if not manifest_path.exists():
        report["overall_pass"] = False
        report["error"] = f"manifest not found: {manifest_path}"
        dump_json(output_path, report)
        print(f"[WaveA] FAIL: manifest missing: {manifest_path}")
        print(f"[WaveA] report_json={output_path}")
        return 1

    try:
        manifest = load_manifest(manifest_path)
    except Exception as exc:
        report["overall_pass"] = False
        report["error"] = f"manifest parse failed: {exc}"
        dump_json(output_path, report)
        print(f"[WaveA] FAIL: manifest parse failed: {exc}")
        print(f"[WaveA] report_json={output_path}")
        return 1

    moved_ok, moved_rows = check_moved_files(repo_root, manifest)
    keep_ok, keep_rows = check_keep_files(repo_root, manifest)
    refs_ok, ref_rows = check_reference_integrity(repo_root, manifest)

    report["checks"]["moved_files"] = {"ok": moved_ok, "items": moved_rows}
    report["checks"]["keep_files"] = {"ok": keep_ok, "items": keep_rows}
    report["checks"]["reference_integrity"] = {"ok": refs_ok, "items": ref_rows}

    overall_pass = moved_ok and keep_ok and refs_ok

    if args.run_tests:
        tests_ok, test_rows = run_test_executables(repo_root)
        report["checks"]["runtime_tests"] = {"ok": tests_ok, "items": test_rows}
        overall_pass = overall_pass and tests_ok

    if args.run_help_checks:
        help_ok, help_rows = run_help_checks(repo_root)
        report["checks"]["help_checks"] = {"ok": help_ok, "items": help_rows}
        overall_pass = overall_pass and help_ok

    report["overall_pass"] = overall_pass
    dump_json(output_path, report)

    status = "PASS" if overall_pass else "FAIL"
    print(f"[WaveA] {status}: moved={moved_ok}, keep={keep_ok}, refs={refs_ok}")
    if args.run_tests:
        print(f"[WaveA] tests={report['checks']['runtime_tests']['ok']}")
    if args.run_help_checks:
        print(f"[WaveA] help_checks={report['checks']['help_checks']['ok']}")
    print(f"[WaveA] report_json={output_path}")

    return 0 if overall_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
