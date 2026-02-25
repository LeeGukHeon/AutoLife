#!/usr/bin/env python3
"""Fail-closed source encoding check: UTF-8 without BOM."""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Sequence


DEFAULT_ROOTS = ("include", "src", "scripts", "config")
DEFAULT_EXTRA_FILES = (".editorconfig", ".gitattributes", "CMakeLists.txt")
DEFAULT_EXCLUDE_DIRS = (
    ".git",
    ".github",
    ".vscode",
    ".venv",
    "build",
    "data",
    "external",
    "third_party",
    "vcpkg",
    "__pycache__",
)
TARGET_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".py",
    ".ps1",
    ".sh",
    ".bat",
    ".cmd",
    ".cmake",
    ".md",
    ".txt",
    ".json",
    ".yaml",
    ".yml",
    ".toml",
    ".ini",
    ".cfg",
    ".conf",
    ".env",
    ".sql",
    ".xml",
    ".html",
    ".css",
    ".js",
    ".ts",
}
TARGET_NAMES = {"CMakeLists.txt", "Makefile", "README", "README.md", "AGENTS.md"}


def _parse_csv(raw: str) -> List[str]:
    values: List[str] = []
    seen = set()
    for token in str(raw).split(","):
        value = token.strip()
        if not value or value in seen:
            continue
        seen.add(value)
        values.append(value)
    return values


def _is_excluded(path_value: Path, exclude_dirs: Sequence[str]) -> bool:
    parts = {x.lower() for x in path_value.parts}
    for name in exclude_dirs:
        if str(name).strip().lower() in parts:
            return True
    return False


def _is_target_file(path_value: Path) -> bool:
    if path_value.name in TARGET_NAMES:
        return True
    return path_value.suffix.lower() in TARGET_SUFFIXES


def _iter_candidates(repo_root: Path, roots: Sequence[str], extra_files: Sequence[str], exclude_dirs: Sequence[str]):
    seen = set()
    for root_name in roots:
        root_path = (repo_root / root_name).resolve()
        if not root_path.exists():
            continue
        if root_path.is_file():
            if root_path not in seen:
                seen.add(root_path)
                yield root_path
            continue
        for item in root_path.rglob("*"):
            if not item.is_file():
                continue
            resolved = item.resolve()
            if resolved in seen:
                continue
            if _is_excluded(resolved, exclude_dirs):
                continue
            if not _is_target_file(resolved):
                continue
            seen.add(resolved)
            yield resolved

    for item_name in extra_files:
        path_value = (repo_root / item_name).resolve()
        if not path_value.exists() or not path_value.is_file():
            continue
        if path_value in seen:
            continue
        if _is_excluded(path_value, exclude_dirs):
            continue
        seen.add(path_value)
        yield path_value


def _rel(path_value: Path, repo_root: Path) -> str:
    try:
        return str(path_value.relative_to(repo_root)).replace("\\", "/")
    except Exception:
        return str(path_value).replace("\\", "/")


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Check source/script/config files are UTF-8 without BOM.")
    parser.add_argument("--roots", default=",".join(DEFAULT_ROOTS), help="Comma-separated roots to scan.")
    parser.add_argument("--extra-files", default=",".join(DEFAULT_EXTRA_FILES), help="Comma-separated additional files.")
    parser.add_argument("--exclude-dirs", default=",".join(DEFAULT_EXCLUDE_DIRS), help="Comma-separated directory names to skip.")
    parser.add_argument("--output-json", default=r".\build\Release\logs\source_encoding_check.json")
    return parser.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    repo_root = Path(__file__).resolve().parent.parent
    output_json = (repo_root / args.output_json).resolve() if not Path(args.output_json).is_absolute() else Path(args.output_json).resolve()
    output_json.parent.mkdir(parents=True, exist_ok=True)

    roots = _parse_csv(args.roots)
    extra_files = _parse_csv(args.extra_files)
    exclude_dirs = _parse_csv(args.exclude_dirs)

    bom_violations: List[Dict[str, str]] = []
    decode_violations: List[Dict[str, str]] = []
    scanned_count = 0

    for path_value in _iter_candidates(repo_root, roots, extra_files, exclude_dirs):
        scanned_count += 1
        raw = path_value.read_bytes()
        rel = _rel(path_value, repo_root)
        if raw.startswith(b"\xef\xbb\xbf"):
            bom_violations.append({"path": rel, "reason": "utf8_bom_detected"})
            continue
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            decode_violations.append({"path": rel, "reason": f"utf8_decode_error:{exc.start}"})

    status = "pass" if (not bom_violations and not decode_violations) else "fail"
    payload = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "status": status,
        "policy": "UTF-8 without BOM for source/script/config text files",
        "inputs": {
            "roots": roots,
            "extra_files": extra_files,
            "exclude_dirs": exclude_dirs,
        },
        "summary": {
            "scanned_file_count": scanned_count,
            "bom_violation_count": len(bom_violations),
            "decode_violation_count": len(decode_violations),
        },
        "violations": {
            "bom": bom_violations,
            "decode": decode_violations,
        },
    }
    with output_json.open("w", encoding="utf-8", newline="\n") as fp:
        json.dump(payload, fp, ensure_ascii=False, indent=2)

    print(
        "[SourceEncodingCheck] "
        f"status={status} scanned={scanned_count} "
        f"bom={len(bom_violations)} decode={len(decode_violations)} "
        f"report={output_json}"
    )
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
