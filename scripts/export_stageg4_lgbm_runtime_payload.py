#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import joblib

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Stage G4: export LightGBM txt model + bundle backend payload (lgbm path/sha)."
    )
    p.add_argument(
        "--train-summary-json",
        default=r".\build\Release\logs\train_summary_global_label_tp1H20_123.json",
    )
    p.add_argument(
        "--bundle-json",
        default=r".\config\model\probabilistic_runtime_bundle_v2_a19_margin_observe_only_step1.json",
    )
    p.add_argument(
        "--bundle-json-mirror",
        default=r".\build\Release\config\model\probabilistic_runtime_bundle_v2_a19_margin_observe_only_step1.json",
    )
    p.add_argument(
        "--output-model-txt",
        default=r".\config\model\lgbm_tp1H20_123.txt",
    )
    p.add_argument(
        "--output-model-sha",
        default=r".\config\model\lgbm_tp1H20_123.sha256",
    )
    p.add_argument(
        "--report-json",
        default=r".\build\Release\logs\stageG4_bundle_export_report.json",
    )
    p.add_argument(
        "--fold-policy",
        choices=("best_valid_auc", "latest_fold"),
        default="best_valid_auc",
    )
    return p.parse_args()


def _safe_float(value: Any, default: float = float("-inf")) -> float:
    try:
        out = float(value)
    except Exception:
        return default
    if out != out:  # NaN check
        return default
    return out


def choose_fold(folds: List[Dict[str, Any]], policy: str) -> Dict[str, Any]:
    if not folds:
        raise RuntimeError("train summary has no folds")
    if policy == "latest_fold":
        return max(folds, key=lambda x: int(x.get("fold_id", 0) or 0))
    # best_valid_auc
    return max(
        folds,
        key=lambda x: _safe_float(
            (((x.get("metrics") or {}).get("h5") or {}).get("valid") or {}).get("auc"),
            default=float("-inf"),
        ),
    )


def sha256_file(path_value: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path_value.open("rb") as fh:
        while True:
            block = fh.read(1024 * 1024)
            if not block:
                break
            h.update(block)
    return h.hexdigest().lower()


def relative_or_absolute(target: pathlib.Path, base: pathlib.Path) -> str:
    try:
        return str(target.resolve().relative_to(base.resolve())).replace("\\", "/")
    except Exception:
        return str(target.resolve())


def update_bundle(
    *,
    bundle_path: pathlib.Path,
    model_txt_path: pathlib.Path,
    model_sha256: str,
    selected_fold_id: int,
    calibration: Dict[str, Any],
    train_summary_path: pathlib.Path,
) -> Dict[str, Any]:
    payload = load_json_or_none(bundle_path)
    if not isinstance(payload, dict):
        raise RuntimeError(f"invalid bundle json: {bundle_path}")

    bundle_dir = bundle_path.parent
    model_path_for_bundle = relative_or_absolute(model_txt_path, bundle_dir)
    payload["prob_model_backend"] = "lgbm"
    payload["lgbm_model_path"] = model_path_for_bundle
    payload["lgbm_model_sha256"] = str(model_sha256).strip().lower()
    payload["lgbm_h5_calibration"] = {
        "a": float(calibration.get("a", 1.0) or 1.0),
        "b": float(calibration.get("b", 0.0) or 0.0),
    }
    payload["lgbm_selected_fold_id"] = int(selected_fold_id)
    payload["lgbm_training_summary_json"] = str(train_summary_path)

    bundle_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return {
        "bundle_json": str(bundle_path),
        "prob_model_backend": payload.get("prob_model_backend", ""),
        "lgbm_model_path": payload.get("lgbm_model_path", ""),
        "lgbm_model_sha256": payload.get("lgbm_model_sha256", ""),
        "lgbm_selected_fold_id": int(payload.get("lgbm_selected_fold_id", 0) or 0),
    }


def main() -> int:
    args = parse_args()
    train_summary_path = resolve_repo_path(args.train_summary_json)
    bundle_path = resolve_repo_path(args.bundle_json)
    bundle_mirror_path = resolve_repo_path(args.bundle_json_mirror) if str(args.bundle_json_mirror).strip() else None
    output_model_txt = resolve_repo_path(args.output_model_txt)
    output_model_sha = resolve_repo_path(args.output_model_sha)
    report_json = resolve_repo_path(args.report_json)

    summary = load_json_or_none(train_summary_path)
    if not isinstance(summary, dict):
        raise RuntimeError(f"invalid training summary json: {train_summary_path}")
    global_model = summary.get("global_model", {})
    if not isinstance(global_model, dict):
        raise RuntimeError("training summary missing global_model")
    folds = [x for x in (global_model.get("folds", []) or []) if isinstance(x, dict)]
    chosen = choose_fold(folds, str(args.fold_policy))
    fold_id = int(chosen.get("fold_id", 0) or 0)
    h5_model_path_raw = str((chosen.get("model_artifacts") or {}).get("h5_model", "")).strip()
    if not h5_model_path_raw:
        raise RuntimeError("chosen fold missing model_artifacts.h5_model")

    h5_model_path = pathlib.Path(h5_model_path_raw)
    if not h5_model_path.is_absolute():
        h5_model_path = (train_summary_path.parent / h5_model_path).resolve()
    package = joblib.load(h5_model_path)
    if not isinstance(package, dict):
        raise RuntimeError(f"unexpected model package type: {type(package)}")
    model = package.get("model")
    if model is None:
        raise RuntimeError("model package missing `model`")
    booster = model.booster_
    best_iteration = int(package.get("best_iteration", 0) or 0)

    output_model_txt.parent.mkdir(parents=True, exist_ok=True)
    booster.save_model(
        str(output_model_txt),
        num_iteration=(best_iteration if best_iteration > 0 else -1),
    )
    model_sha = sha256_file(output_model_txt)
    output_model_sha.parent.mkdir(parents=True, exist_ok=True)
    output_model_sha.write_text(model_sha + "\n", encoding="utf-8")

    calibration = package.get("calibration", {}) if isinstance(package.get("calibration", {}), dict) else {}
    bundle_update_main = update_bundle(
        bundle_path=bundle_path,
        model_txt_path=output_model_txt,
        model_sha256=model_sha,
        selected_fold_id=fold_id,
        calibration=calibration,
        train_summary_path=train_summary_path,
    )

    bundle_update_mirror: Optional[Dict[str, Any]] = None
    if bundle_mirror_path is not None and bundle_mirror_path.resolve() != bundle_path.resolve():
        if bundle_mirror_path.exists():
            mirror_model_txt = bundle_mirror_path.parent / output_model_txt.name
            mirror_model_sha = bundle_mirror_path.parent / output_model_sha.name
            mirror_model_txt.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(output_model_txt, mirror_model_txt)
            mirror_model_sha.parent.mkdir(parents=True, exist_ok=True)
            mirror_model_sha.write_text(model_sha + "\n", encoding="utf-8")
            bundle_update_mirror = update_bundle(
                bundle_path=bundle_mirror_path,
                model_txt_path=mirror_model_txt,
                model_sha256=model_sha,
                selected_fold_id=fold_id,
                calibration=calibration,
                train_summary_path=train_summary_path,
            )

    report = {
        "generated_at_utc": utc_now_iso(),
        "status": "pass",
        "fold_policy": str(args.fold_policy),
        "selected_fold_id": int(fold_id),
        "source_train_summary_json": str(train_summary_path),
        "source_joblib_model_path": str(h5_model_path),
        "backend": "lgbm",
        "output_model_txt": str(output_model_txt),
        "output_model_sha_file": str(output_model_sha),
        "output_model_sha256": model_sha,
        "best_iteration": int(best_iteration),
        "bundle_update": bundle_update_main,
        "bundle_update_mirror": bundle_update_mirror,
    }
    dump_json(report_json, report)
    print(f"[StageG4-Export] fold_id={fold_id}")
    print(f"[StageG4-Export] model_txt={output_model_txt}")
    print(f"[StageG4-Export] model_sha256={model_sha}")
    print(f"[StageG4-Export] bundle={bundle_path}")
    if bundle_update_mirror:
        print(f"[StageG4-Export] bundle_mirror={bundle_mirror_path}")
    print(f"[StageG4-Export] report={report_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
