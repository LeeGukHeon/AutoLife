#!/usr/bin/env python3
import argparse
import shutil
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List


@dataclass
class CleanupItem:
    path: Path
    reason: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Clean generated build artifacts/logs with safe defaults."
    )
    parser.add_argument(
        "--workspace",
        default=".",
        help="Workspace root (default: current directory).",
    )
    parser.add_argument(
        "--build-dir",
        default="build",
        help="Build directory relative to workspace.",
    )
    parser.add_argument(
        "--logs-dir",
        default="build/Release/logs",
        help="Logs directory relative to workspace.",
    )
    parser.add_argument(
        "--remove-old-logs-days",
        type=int,
        default=7,
        help="Delete generated logs older than N days (default: 7).",
    )
    parser.add_argument(
        "--remove-project-build-files",
        action="store_true",
        help="Also remove generated CMake/VS project files under build/.",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Apply deletion. Without this flag, dry-run only.",
    )
    return parser.parse_args()


def is_generated_log(path: Path) -> bool:
    name = path.name.lower()
    if name.endswith((".json", ".jsonl", ".csv", ".log", ".err.log", ".tmp", ".bak")):
        return True
    prefixes = (
        "auto_improvement_",
        "candidate_",
        "profitability_",
        "parity_",
        "fetch_test_",
        "upbit_compliance_",
    )
    return name.startswith(prefixes)


def iter_old_generated_logs(logs_dir: Path, age_days: int) -> Iterable[CleanupItem]:
    if not logs_dir.exists():
        return
    cutoff = time.time() - (age_days * 24 * 60 * 60)
    for path in logs_dir.rglob("*"):
        if not path.is_file():
            continue
        if not is_generated_log(path):
            continue
        if path.stat().st_mtime > cutoff:
            continue
        yield CleanupItem(path=path, reason=f"old_generated_log>{age_days}d")


def iter_build_project_files(build_dir: Path) -> Iterable[CleanupItem]:
    if not build_dir.exists():
        return

    root_files = (
        "*.sln",
        "*.vcxproj",
        "*.vcxproj.filters",
        "CMakeCache.txt",
        "cmake_install.cmake",
    )
    for pattern in root_files:
        for path in build_dir.glob(pattern):
            if path.exists():
                yield CleanupItem(path=path, reason="generated_project_file")

    root_dirs = (
        "CMakeFiles",
        ".cmake",
        "x64",
        "Debug",
    )
    for name in root_dirs:
        path = build_dir / name
        if path.exists():
            yield CleanupItem(path=path, reason="generated_project_dir")

    for path in build_dir.glob("*.dir"):
        if path.is_dir():
            yield CleanupItem(path=path, reason="generated_target_dir")


def apply_cleanup(items: List[CleanupItem], apply: bool) -> int:
    if not items:
        print("[cleanup] nothing to remove")
        return 0

    print(f"[cleanup] candidates={len(items)} apply={str(apply).lower()}")
    for item in items:
        print(f"- {item.path} ({item.reason})")

    if not apply:
        print("[cleanup] dry-run only. pass --apply to delete.")
        return 0

    removed = 0
    for item in items:
        try:
            if item.path.is_dir():
                shutil.rmtree(item.path)
            else:
                item.path.unlink(missing_ok=True)
            removed += 1
        except Exception as exc:  # pragma: no cover
            print(f"[cleanup] failed: {item.path} ({exc})")

    print(f"[cleanup] removed={removed}")
    return 0


def main() -> int:
    args = parse_args()
    workspace = Path(args.workspace).resolve()
    build_dir = (workspace / args.build_dir).resolve()
    logs_dir = (workspace / args.logs_dir).resolve()

    items: List[CleanupItem] = []
    items.extend(iter_old_generated_logs(logs_dir, args.remove_old_logs_days) or [])
    if args.remove_project_build_files:
        items.extend(iter_build_project_files(build_dir) or [])

    # deterministic output ordering
    items.sort(key=lambda x: str(x.path).lower())
    return apply_cleanup(items, apply=args.apply)


if __name__ == "__main__":
    raise SystemExit(main())

