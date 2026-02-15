#!/usr/bin/env python3
import argparse
import json
import pathlib
import sys
from typing import Any, Dict


def ensure_parent_directory(path_value: pathlib.Path) -> None:
    path_value.parent.mkdir(parents=True, exist_ok=True)


def load_json(path_value: pathlib.Path) -> Dict[str, Any]:
    with path_value.open("r", encoding="utf-8-sig") as f:
        return json.load(f)


def dump_json(path_value: pathlib.Path, payload: Any) -> None:
    ensure_parent_directory(path_value)
    with path_value.open("w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, ensure_ascii=False, indent=4)


def merge_json_object(target: Dict[str, Any], patch: Dict[str, Any]) -> Dict[str, Any]:
    for k, v in patch.items():
        if isinstance(v, dict):
            tv = target.get(k)
            if not isinstance(tv, dict):
                tv = {}
                target[k] = tv
            merge_json_object(tv, v)
        else:
            target[k] = v
    return target


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", "-Preset", choices=["safe", "active"], default="safe")
    parser.add_argument("--preset-path", "-PresetPath", default="")
    parser.add_argument("--config-path", "-ConfigPath", default=r".\build\Release\config\config.json")
    parser.add_argument("--source-config-path", "-SourceConfigPath", default=r".\config\config.json")
    args = parser.parse_args()

    preset_path = pathlib.Path(args.preset_path) if args.preset_path else pathlib.Path(f"./config/presets/{args.preset}.json")
    config_path = pathlib.Path(args.config_path)
    source_config_path = pathlib.Path(args.source_config_path)

    if not preset_path.is_absolute():
        preset_path = (pathlib.Path.cwd() / preset_path).resolve()
    if not config_path.is_absolute():
        config_path = (pathlib.Path.cwd() / config_path).resolve()
    if not source_config_path.is_absolute():
        source_config_path = (pathlib.Path.cwd() / source_config_path).resolve()

    if not preset_path.exists():
        raise FileNotFoundError(f"Preset file not found: {preset_path}")

    if not config_path.exists():
        if not source_config_path.exists():
            raise FileNotFoundError(
                f"Config not found and source config missing: {config_path} / {source_config_path}"
            )
        ensure_parent_directory(config_path)
        config_path.write_text(source_config_path.read_text(encoding="utf-8-sig"), encoding="utf-8", newline="\n")

    config = load_json(config_path)
    preset_root = load_json(preset_path)

    patch: Dict[str, Any] = {}
    if "trading" in preset_root:
        patch["trading"] = preset_root["trading"]
    if "strategies" in preset_root:
        patch["strategies"] = preset_root["strategies"]

    merge_json_object(config, patch)
    dump_json(config_path, config)

    preset_name = str(preset_root.get("preset_name", args.preset))
    print(f"[ApplyTradingPreset] Applied preset: {preset_name}")
    print(f"config_path={config_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
