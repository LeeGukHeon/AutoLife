#!/usr/bin/env python3
import json
import pathlib
import subprocess
from dataclasses import dataclass
from typing import Any, Iterable, List, Optional


@dataclass
class CommandResult:
    exit_code: int
    stdout: str
    stderr: str


def resolve_repo_path(path_value: str) -> pathlib.Path:
    p = pathlib.Path(path_value)
    if p.is_absolute():
        return p
    return (pathlib.Path.cwd() / p).resolve()


def ensure_parent_directory(path_value: pathlib.Path) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)


def load_json_or_none(path_value: pathlib.Path) -> Optional[Any]:
    if not path_value.exists():
        return None
    try:
        return json.loads(path_value.read_text(encoding="utf-8-sig"))
    except Exception:
        return None


def dump_json(path_value: pathlib.Path, payload: Any) -> None:
    ensure_parent_directory(path_value)
    path_value.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_nonempty_lines(path_value: pathlib.Path) -> List[str]:
    if not path_value.exists():
        return []
    with path_value.open("r", encoding="utf-8", errors="ignore") as fh:
        return [line.rstrip("\r\n") for line in fh if line.strip()]


def parse_last_json_line(text: str) -> Optional[dict]:
    lines = text.splitlines()
    for line in reversed(lines):
        t = line.strip()
        if t.startswith("{") and t.endswith("}"):
            try:
                value = json.loads(t)
            except Exception:
                continue
            if isinstance(value, dict):
                return value
    return None


def tail_strings(values: Iterable[Any], count: int = 3) -> List[str]:
    items = [str(v) for v in values]
    if count <= 0:
        return []
    return items[-count:]


def find_latest_log(log_dir: pathlib.Path, pattern: str = "autolife*.log") -> Optional[pathlib.Path]:
    if not log_dir.exists():
        return None
    items = [p for p in log_dir.glob(pattern) if p.is_file()]
    if not items:
        return None
    return max(items, key=lambda p: p.stat().st_mtime)


def run_command(command: List[str], cwd: Optional[pathlib.Path] = None) -> CommandResult:
    proc = subprocess.run(
        command,
        cwd=str(cwd) if cwd else None,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
    )
    return CommandResult(exit_code=int(proc.returncode), stdout=proc.stdout or "", stderr=proc.stderr or "")
