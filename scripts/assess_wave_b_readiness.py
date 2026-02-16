#!/usr/bin/env python3
import argparse
import datetime as dt
from pathlib import Path
import re
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

from _script_common import dump_json, load_json_or_none, resolve_repo_path, run_command, tail_strings


BUILD_TEXT_SUFFIXES: Set[str] = {
    ".cpp",
    ".cc",
    ".cxx",
    ".h",
    ".hpp",
    ".cmake",
}

AUX_TEXT_SUFFIXES: Set[str] = {
    ".md",
    ".txt",
    ".json",
    ".yml",
    ".yaml",
    ".py",
    ".bat",
    ".ps1",
}

BUILD_REF_ROOTS = [
    "CMakeLists.txt",
    "src",
    "include",
    "tests",
]

AUX_REF_ROOTS = [
    "README.md",
    "docs",
    "scripts",
    ".github/workflows",
    "config",
]

B1_FILES = [
    "include/core/adapters/LegacyExecutionPlaneAdapter.h",
    "include/core/adapters/LegacyPolicyLearningPlaneAdapter.h",
    "src/core/adapters/LegacyExecutionPlaneAdapter.cpp",
    "src/core/adapters/LegacyPolicyLearningPlaneAdapter.cpp",
]

B1_REPLACEMENT_SIGNALS = [
    "include/v2/adapters/LegacyExecutionPlaneAdapter.h",
    "include/v2/adapters/LegacyPolicyPlaneAdapter.h",
    "src/v2/adapters/LegacyExecutionPlaneAdapter.cpp",
    "src/v2/adapters/LegacyPolicyPlaneAdapter.cpp",
    "include/v2/orchestration/DecisionKernel.h",
    "src/v2/orchestration/DecisionKernel.cpp",
]

B2_FILES = [
    "include/engine/TradingEngine.h",
    "src/engine/TradingEngine.cpp",
    "include/backtest/BacktestEngine.h",
    "src/backtest/BacktestEngine.cpp",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Assess Wave B deletion readiness.")
    parser.add_argument(
        "--output-json",
        default=r".\build\Release\logs\wave_b_readiness_report.json",
        help="Output report path",
    )
    parser.add_argument(
        "--wave-a-report-json",
        default=r".\build\Release\logs\wave_a_cleanup_verification.json",
        help="Wave A verification report path",
    )
    parser.add_argument(
        "--v2-shadow-report-json",
        default=r".\build\Release\logs\v2_shadow_parity_report.json",
        help="v2 shadow parity report path",
    )
    parser.add_argument(
        "--operational-report-json",
        default=r".\build\Release\logs\operational_readiness_report.json",
        help="Operational readiness report path",
    )
    parser.add_argument(
        "--run-refresh-checks",
        action="store_true",
        help="Refresh prerequisite reports before assessment",
    )
    parser.add_argument(
        "--max-hit-samples",
        type=int,
        default=15,
        help="Maximum per-file reference samples to include",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Return non-zero when no batch is ready for any deletion",
    )
    return parser.parse_args()


def now_utc_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def iter_text_files(repo_root: Path, roots: Iterable[str], suffixes: Set[str]) -> List[Path]:
    files: List[Path] = []
    for root in roots:
        path_value = (repo_root / root).resolve()
        if not path_value.exists():
            continue
        if path_value.is_file():
            files.append(path_value)
            continue
        for candidate in path_value.rglob("*"):
            if not candidate.is_file():
                continue
            if "legacy_archive" in candidate.parts:
                continue
            if "build" in candidate.parts:
                continue
            if candidate.name == "CMakeLists.txt" or candidate.suffix.lower() in suffixes:
                files.append(candidate)
    return files


def to_rel(path_value: Path, repo_root: Path) -> str:
    try:
        return path_value.relative_to(repo_root).as_posix()
    except Exception:
        return str(path_value)


def build_patterns(target_rel: str) -> List[str]:
    rel = target_rel.replace("\\", "/")
    parts = rel.split("/")
    patterns = {
        rel,
        rel.replace("/", "\\"),
    }
    if parts and parts[0] in {"include", "src"} and len(parts) > 1:
        short = "/".join(parts[1:])
        patterns.add(short)
        patterns.add(short.replace("/", "\\"))
    return sorted(p for p in patterns if p)


def contains_path_token(line: str, pattern: str) -> bool:
    escaped = re.escape(pattern)
    # Require token-ish boundaries so "v2/strategy/Foo.h" does not match
    # the shorter "strategy/Foo.h" candidate.
    return bool(
        re.search(
            rf"(?<![A-Za-z0-9_./\\\\-]){escaped}(?![A-Za-z0-9_./\\\\-])",
            line,
        )
    )


def scan_references(
    repo_root: Path,
    target_rel: str,
    files: List[Path],
    max_samples: int,
) -> Tuple[int, List[Dict[str, Any]]]:
    target_abs = (repo_root / target_rel).resolve()
    patterns = build_patterns(target_rel)
    total_hits = 0
    samples: List[Dict[str, Any]] = []

    for file_path in files:
        if file_path.resolve() == target_abs:
            continue
        try:
            lines = file_path.read_text(encoding="utf-8", errors="ignore").splitlines()
        except Exception:
            continue
        for idx, line in enumerate(lines, start=1):
            matched: Optional[str] = None
            for pattern in patterns:
                if contains_path_token(line, pattern):
                    matched = pattern
                    break
            if matched is None:
                continue
            total_hits += 1
            if len(samples) < max_samples:
                samples.append(
                    {
                        "file": to_rel(file_path, repo_root),
                        "line": idx,
                        "matched_pattern": matched,
                    }
                )
    return total_hits, samples


def as_dict(value: Any) -> Dict[str, Any]:
    return value if isinstance(value, dict) else {}


def as_list(value: Any) -> List[Any]:
    return value if isinstance(value, list) else []


def as_bool(value: Any) -> bool:
    return bool(value is True)


def as_int(value: Any) -> int:
    try:
        return int(value)
    except Exception:
        return 0


def evaluate_gate_reports(
    wave_a_report: Optional[Dict[str, Any]],
    v2_shadow_report: Optional[Dict[str, Any]],
    operational_report: Optional[Dict[str, Any]],
) -> Dict[str, Any]:
    wave_a = as_dict(wave_a_report)
    wave_a_ok = as_bool(wave_a.get("overall_pass"))

    v2 = as_dict(v2_shadow_report)
    v2_checks = as_dict(v2.get("checks"))
    v2_metrics = as_dict(v2.get("metrics"))
    v2_errors = as_list(v2.get("errors"))
    v2_base_check_names = [
        "policy_selected_set_equal",
        "policy_dropped_count_equal",
        "rejection_taxonomy_equal",
        "execution_schema_compatible",
        "execution_side_valid",
        "execution_status_valid",
    ]
    v2_base_checks_ok = all(as_bool(v2_checks.get(name)) for name in v2_base_check_names)
    v2_runtime_backtest_ok = (
        as_bool(v2_checks.get("runtime_shadow_all_entries_passed"))
        and as_int(v2_metrics.get("runtime_entry_count")) > 0
    )
    v2_runtime_live_ok = (
        as_bool(v2_checks.get("runtime_live_shadow_all_entries_passed"))
        and as_int(v2_metrics.get("runtime_live_entry_count")) > 0
    )
    v2_ok = not v2_errors and v2_base_checks_ok and v2_runtime_backtest_ok and v2_runtime_live_ok

    operational = as_dict(operational_report)
    operational_checks = as_dict(operational.get("checks"))
    operational_errors = as_list(operational.get("errors"))
    op_check_names = [
        "recovery_e2e_passed",
        "replay_reconcile_diff_passed",
        "execution_parity_passed",
        "v2_shadow_parity_passed",
    ]
    op_checks_ok = all(as_bool(operational_checks.get(name)) for name in op_check_names)
    operational_ok = not operational_errors and op_checks_ok

    global_ready = wave_a_ok and v2_ok and operational_ok
    return {
        "wave_a_ok": wave_a_ok,
        "v2_shadow_ok": v2_ok,
        "operational_ok": operational_ok,
        "v2_shadow_base_checks_ok": v2_base_checks_ok,
        "v2_runtime_backtest_ok": v2_runtime_backtest_ok,
        "v2_runtime_live_ok": v2_runtime_live_ok,
        "global_ready": global_ready,
        "missing_or_failed_reasons": [
            reason
            for reason, passed in [
                ("wave_a_not_ready", wave_a_ok),
                ("v2_shadow_not_ready", v2_ok),
                ("operational_not_ready", operational_ok),
            ]
            if not passed
        ],
    }


def evaluate_file_delete_readiness(
    repo_root: Path,
    target_rel: str,
    build_files: List[Path],
    aux_files: List[Path],
    max_hit_samples: int,
) -> Dict[str, Any]:
    abs_path = (repo_root / target_rel).resolve()
    exists = abs_path.exists()
    build_ref_count, build_ref_samples = scan_references(
        repo_root=repo_root,
        target_rel=target_rel,
        files=build_files,
        max_samples=max_hit_samples,
    )
    aux_ref_count, aux_ref_samples = scan_references(
        repo_root=repo_root,
        target_rel=target_rel,
        files=aux_files,
        max_samples=max_hit_samples,
    )
    reference_clear = (not exists) or (build_ref_count == 0)
    return {
        "path": target_rel,
        "exists": exists,
        "build_ref_count": build_ref_count,
        "aux_ref_count": aux_ref_count,
        "build_ref_samples": build_ref_samples,
        "aux_ref_samples": aux_ref_samples,
        "reference_clear_for_delete": reference_clear,
    }


def evaluate_static_batch(
    repo_root: Path,
    batch_id: str,
    files: List[str],
    build_files: List[Path],
    aux_files: List[Path],
    global_gate_ready: bool,
    replacement_signals: Optional[List[str]],
    max_hit_samples: int,
) -> Dict[str, Any]:
    file_rows = [
        evaluate_file_delete_readiness(
            repo_root=repo_root,
            target_rel=target,
            build_files=build_files,
            aux_files=aux_files,
            max_hit_samples=max_hit_samples,
        )
        for target in files
    ]
    file_refs_clear = all(bool(row.get("reference_clear_for_delete")) for row in file_rows)

    replacements: List[Dict[str, Any]] = []
    replacements_ok = True
    if replacement_signals:
        for rel in replacement_signals:
            exists = (repo_root / rel).exists()
            replacements.append({"path": rel, "exists": exists})
            replacements_ok = replacements_ok and exists

    blocking_reasons: List[str] = []
    if not global_gate_ready:
        blocking_reasons.append("global_gate_not_ready")
    if not file_refs_clear:
        blocking_reasons.append("target_files_still_referenced_in_build_path")
    if replacement_signals and not replacements_ok:
        blocking_reasons.append("replacement_signal_missing")

    ready = global_gate_ready and file_refs_clear and replacements_ok
    return {
        "batch_id": batch_id,
        "ready": ready,
        "global_gate_ready": global_gate_ready,
        "file_refs_clear": file_refs_clear,
        "replacement_signals": replacements,
        "replacement_signals_ok": replacements_ok,
        "files": file_rows,
        "blocking_reasons": blocking_reasons,
    }


def collect_strategy_units(repo_root: Path) -> List[Dict[str, Any]]:
    headers = {
        p.stem: p.relative_to(repo_root).as_posix()
        for p in (repo_root / "include/strategy").glob("*.h")
        if p.is_file()
    }
    sources = {
        p.stem: p.relative_to(repo_root).as_posix()
        for p in (repo_root / "src/strategy").glob("*.cpp")
        if p.is_file()
    }
    names = sorted(set(sources.keys()) | set(headers.keys()))
    units: List[Dict[str, Any]] = []
    for name in names:
        files: List[str] = []
        if name in headers:
            files.append(headers[name])
        if name in sources:
            files.append(sources[name])
        units.append(
            {
                "strategy_unit": name,
                "files": files,
                "replacement_candidates": [
                    f"include/v2/strategy/{name}.h",
                    f"src/v2/strategy/{name}.cpp",
                ],
            }
        )
    return units


def evaluate_strategy_batch(
    repo_root: Path,
    build_files: List[Path],
    aux_files: List[Path],
    global_gate_ready: bool,
    max_hit_samples: int,
) -> Dict[str, Any]:
    units = collect_strategy_units(repo_root)
    unit_rows: List[Dict[str, Any]] = []
    ready_any = False
    ready_all = True

    if not units:
        return {
            "batch_id": "B3",
            "ready_any": bool(global_gate_ready),
            "ready_all": bool(global_gate_ready),
            "global_gate_ready": global_gate_ready,
            "strategy_units": [],
            "note": "no_remaining_v1_strategy_units_detected",
        }

    for unit in units:
        file_rows = [
            evaluate_file_delete_readiness(
                repo_root=repo_root,
                target_rel=target,
                build_files=build_files,
                aux_files=aux_files,
                max_hit_samples=max_hit_samples,
            )
            for target in unit["files"]
        ]
        file_refs_clear = all(bool(row.get("reference_clear_for_delete")) for row in file_rows)

        replacement_rows: List[Dict[str, Any]] = []
        replacement_ok = True
        for candidate in unit["replacement_candidates"]:
            exists = (repo_root / candidate).exists()
            replacement_rows.append({"path": candidate, "exists": exists})
            replacement_ok = replacement_ok and exists

        blocking_reasons: List[str] = []
        if not global_gate_ready:
            blocking_reasons.append("global_gate_not_ready")
        if not file_refs_clear:
            blocking_reasons.append("strategy_files_still_referenced_in_build_path")
        if not replacement_ok:
            blocking_reasons.append("v2_strategy_replacement_missing")

        unit_ready = global_gate_ready and file_refs_clear and replacement_ok
        if unit_ready:
            ready_any = True
        else:
            ready_all = False

        unit_rows.append(
            {
                "strategy_unit": unit["strategy_unit"],
                "ready": unit_ready,
                "file_refs_clear": file_refs_clear,
                "replacement_signals_ok": replacement_ok,
                "files": file_rows,
                "replacement_signals": replacement_rows,
                "blocking_reasons": blocking_reasons,
            }
        )

    return {
        "batch_id": "B3",
        "ready_any": ready_any,
        "ready_all": ready_all,
        "global_gate_ready": global_gate_ready,
        "strategy_units": unit_rows,
    }


def find_b2_replacement_evidence(repo_root: Path) -> List[str]:
    evidence: List[str] = []
    roots = [repo_root / "include/v2", repo_root / "src/v2"]
    for root in roots:
        if not root.exists():
            continue
        for pattern in ("*Engine*.h", "*Engine*.cpp", "*Backtest*.h", "*Backtest*.cpp"):
            for path_value in root.rglob(pattern):
                if path_value.is_file():
                    evidence.append(path_value.relative_to(repo_root).as_posix())
    return sorted(set(evidence))


def run_refresh(repo_root: Path) -> List[Dict[str, Any]]:
    checks: List[Dict[str, Any]] = []
    commands = [
        ["python", "scripts/verify_cleanup_wave_a.py", "--run-tests", "--run-help-checks"],
        [
            "python",
            "scripts/run_ci_operational_gate.py",
            "--include-v2-shadow-parity",
            "--strict-v2-shadow-parity",
            "--check-runtime-v2-shadow-parity",
            "--check-runtime-live-v2-shadow-parity",
        ],
    ]
    for command in commands:
        result = run_command(command, cwd=repo_root)
        checks.append(
            {
                "command": command,
                "exit_code": result.exit_code,
                "ok": result.exit_code == 0,
                "stdout_tail": tail_strings(result.stdout.splitlines(), 10),
                "stderr_tail": tail_strings(result.stderr.splitlines(), 10),
            }
        )
    return checks


def main() -> int:
    args = parse_args()
    repo_root = Path.cwd().resolve()
    output_path = resolve_repo_path(args.output_json)
    wave_a_path = resolve_repo_path(args.wave_a_report_json)
    v2_shadow_path = resolve_repo_path(args.v2_shadow_report_json)
    operational_path = resolve_repo_path(args.operational_report_json)

    report: Dict[str, Any] = {
        "generated_at_utc": now_utc_iso(),
        "repo_root": str(repo_root),
        "inputs": {
            "wave_a_report_json": str(wave_a_path),
            "v2_shadow_report_json": str(v2_shadow_path),
            "operational_report_json": str(operational_path),
        },
        "refresh_checks": [],
    }

    if args.run_refresh_checks:
        report["refresh_checks"] = run_refresh(repo_root)

    wave_a_report = load_json_or_none(wave_a_path)
    v2_shadow_report = load_json_or_none(v2_shadow_path)
    operational_report = load_json_or_none(operational_path)

    gate_eval = evaluate_gate_reports(
        wave_a_report=wave_a_report if isinstance(wave_a_report, dict) else None,
        v2_shadow_report=v2_shadow_report if isinstance(v2_shadow_report, dict) else None,
        operational_report=operational_report if isinstance(operational_report, dict) else None,
    )
    report["global_gates"] = gate_eval

    build_files = iter_text_files(repo_root, BUILD_REF_ROOTS, BUILD_TEXT_SUFFIXES)
    aux_files = iter_text_files(repo_root, AUX_REF_ROOTS, AUX_TEXT_SUFFIXES)

    batch_b1 = evaluate_static_batch(
        repo_root=repo_root,
        batch_id="B1",
        files=B1_FILES,
        build_files=build_files,
        aux_files=aux_files,
        global_gate_ready=bool(gate_eval.get("global_ready")),
        replacement_signals=B1_REPLACEMENT_SIGNALS,
        max_hit_samples=max(1, args.max_hit_samples),
    )

    b2_replacement_evidence = find_b2_replacement_evidence(repo_root)
    batch_b2 = evaluate_static_batch(
        repo_root=repo_root,
        batch_id="B2",
        files=B2_FILES,
        build_files=build_files,
        aux_files=aux_files,
        global_gate_ready=bool(gate_eval.get("global_ready")),
        replacement_signals=b2_replacement_evidence if b2_replacement_evidence else [],
        max_hit_samples=max(1, args.max_hit_samples),
    )
    if not b2_replacement_evidence:
        batch_b2["replacement_signals_ok"] = False
        batch_b2["blocking_reasons"] = list(dict.fromkeys(batch_b2["blocking_reasons"] + ["v2_engine_backtest_replacement_evidence_missing"]))

    batch_b3 = evaluate_strategy_batch(
        repo_root=repo_root,
        build_files=build_files,
        aux_files=aux_files,
        global_gate_ready=bool(gate_eval.get("global_ready")),
        max_hit_samples=max(1, args.max_hit_samples),
    )

    report["batches"] = {
        "B1": batch_b1,
        "B2": batch_b2,
        "B3": batch_b3,
    }

    ready_any = bool(batch_b1.get("ready")) or bool(batch_b2.get("ready")) or bool(batch_b3.get("ready_any"))
    ready_full = bool(batch_b1.get("ready")) and bool(batch_b2.get("ready")) and bool(batch_b3.get("ready_all"))
    report["summary"] = {
        "ready_for_any_wave_b_delete": ready_any,
        "ready_for_full_wave_b": ready_full,
        "ready_batches": [
            batch_id
            for batch_id, ready in [
                ("B1", bool(batch_b1.get("ready"))),
                ("B2", bool(batch_b2.get("ready"))),
                ("B3_any", bool(batch_b3.get("ready_any"))),
                ("B3_all", bool(batch_b3.get("ready_all"))),
            ]
            if ready
        ],
    }

    dump_json(output_path, report)

    print(
        "[WaveB] global_ready={} | B1={} | B2={} | B3_any={} | B3_all={} | report={}".format(
            gate_eval.get("global_ready"),
            batch_b1.get("ready"),
            batch_b2.get("ready"),
            batch_b3.get("ready_any"),
            batch_b3.get("ready_all"),
            output_path,
        )
    )

    if args.strict and not ready_any:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
