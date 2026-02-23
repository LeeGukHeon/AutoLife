#!/usr/bin/env python3
import argparse
import pathlib
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional

import joblib
import numpy as np

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Export trained probabilistic model artifacts to runtime-friendly JSON bundle."
    )
    parser.add_argument(
        "--train-summary-json",
        default=r".\build\Release\logs\probabilistic_model_train_summary_full_20260220.json",
    )
    parser.add_argument(
        "--output-json",
        default=r".\config\model\probabilistic_runtime_bundle_v1.json",
    )
    parser.add_argument(
        "--export-mode",
        choices=("global_only", "hybrid", "per_market"),
        default="global_only",
        help=(
            "global_only: export only global fallback(default_model), "
            "hybrid: export global fallback + per-market entries, "
            "per_market: export legacy per-market entries only."
        ),
    )
    parser.add_argument(
        "--fold-policy",
        choices=("latest", "best_h5_logloss"),
        default="latest",
        help="Which fold to export per market.",
    )
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def extract_linear(payload: Dict[str, Any]) -> Dict[str, Any]:
    model = payload.get("model")
    if model is None:
        raise RuntimeError("model missing in payload")
    coef = np.asarray(model.coef_, dtype=np.float64).reshape(-1).tolist()
    intercept = float(np.asarray(model.intercept_, dtype=np.float64).reshape(-1)[0])
    return {
        "coef": [float(x) for x in coef],
        "intercept": float(intercept),
    }


def extract_edge_regressor(payload: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    node = payload.get("edge_regressor")
    if not isinstance(node, dict):
        return None
    linear = node.get("linear")
    if not isinstance(linear, dict):
        return None
    coef_raw = linear.get("coef")
    if not isinstance(coef_raw, list) or not coef_raw:
        return None
    try:
        coef = [float(x) for x in coef_raw]
        intercept = float(linear.get("intercept", 0.0) or 0.0)
        clip_abs_bps = float(node.get("clip_abs_bps", 250.0) or 250.0)
    except Exception:
        return None
    if not np.isfinite(intercept):
        return None
    if not np.isfinite(clip_abs_bps):
        clip_abs_bps = 250.0
    clip_abs_bps = float(np.clip(clip_abs_bps, 10.0, 5000.0))
    return {
        "linear": {
            "coef": coef,
            "intercept": intercept,
        },
        "clip_abs_bps": clip_abs_bps,
    }


def pick_fold(folds: List[Dict[str, Any]], policy: str) -> Optional[Dict[str, Any]]:
    if not folds:
        return None
    if policy == "latest":
        return max(folds, key=lambda x: int(x.get("fold_id", 0) or 0))
    if policy == "best_h5_logloss":
        cand = []
        for f in folds:
            ll = (
                f.get("metrics", {})
                .get("h5", {})
                .get("test_calibrated", {})
                .get("logloss", None)
            )
            if ll is None:
                continue
            try:
                ll_f = float(ll)
            except Exception:
                continue
            cand.append((ll_f, f))
        if cand:
            cand.sort(key=lambda x: x[0])
            return cand[0][1]
    return max(folds, key=lambda x: int(x.get("fold_id", 0) or 0))


def make_runtime_entry(
    *,
    chosen: Dict[str, Any],
    h1_payload: Dict[str, Any],
    h5_payload: Dict[str, Any],
) -> Dict[str, Any]:
    h1_lin = extract_linear(h1_payload)
    h5_lin = extract_linear(h5_payload)
    h5_edge_regressor = extract_edge_regressor(h5_payload)

    h1_cal = chosen.get("calibration", {}).get("h1", {}) or {}
    h5_cal = chosen.get("calibration", {}).get("h5", {}) or {}
    h5_thr = (
        chosen.get("metrics", {})
        .get("h5", {})
        .get("threshold_selection", {})
        .get("threshold", 0.6)
    )
    h5_edge_profile = h5_payload.get("edge_profile", {}) if isinstance(h5_payload, dict) else {}
    if not isinstance(h5_edge_profile, dict):
        h5_edge_profile = {}
    return {
        "selected_fold_id": int(chosen.get("fold_id", 0) or 0),
        "h1_model": {
            "linear": h1_lin,
            "calibration": {
                "a": float(h1_cal.get("a", 1.0) or 1.0),
                "b": float(h1_cal.get("b", 0.0) or 0.0),
            },
        },
        "h5_model": {
            "linear": h5_lin,
            "calibration": {
                "a": float(h5_cal.get("a", 1.0) or 1.0),
                "b": float(h5_cal.get("b", 0.0) or 0.0),
            },
            "selection_threshold": float(h5_thr),
            "edge_regressor": h5_edge_regressor if h5_edge_regressor is not None else None,
            "edge_profile": {
                "sample_count": int(h5_edge_profile.get("sample_count", 0) or 0),
                "win_count": int(h5_edge_profile.get("win_count", 0) or 0),
                "loss_count": int(h5_edge_profile.get("loss_count", 0) or 0),
                "neutral_count": int(h5_edge_profile.get("neutral_count", 0) or 0),
                "win_mean_edge_bps": float(h5_edge_profile.get("win_mean_edge_bps", 0.0) or 0.0),
                "loss_mean_edge_bps": float(h5_edge_profile.get("loss_mean_edge_bps", 0.0) or 0.0),
                "neutral_mean_edge_bps": float(h5_edge_profile.get("neutral_mean_edge_bps", 0.0) or 0.0),
            },
        },
        "selected_fold_metrics": {
            "h5_test_calibrated_logloss": float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_calibrated", {})
                .get("logloss", float("nan"))
            ),
            "h5_test_calibrated_brier": float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_calibrated", {})
                .get("brier", float("nan"))
            ),
            "h5_test_trade_selected_coverage": float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_trade_metrics", {})
                .get("coverage", float("nan"))
            ),
            "h5_test_trade_selected_mean_edge_bps": float(
                chosen.get("metrics", {})
                .get("h5", {})
                .get("test_trade_metrics", {})
                .get("mean_edge_bps", float("nan"))
            ),
        },
    }


def build_global_default_model(
    summary: Dict[str, Any],
    fold_policy: str,
) -> Optional[Dict[str, Any]]:
    global_node = summary.get("global_model", {})
    if not isinstance(global_node, dict):
        return None
    folds = [f for f in (global_node.get("folds", []) or []) if isinstance(f, dict)]
    if not folds:
        return None
    chosen = pick_fold(folds, fold_policy)
    if chosen is None:
        return None
    h1_path = str(chosen.get("model_artifacts", {}).get("h1_model", "")).strip()
    h5_path = str(chosen.get("model_artifacts", {}).get("h5_model", "")).strip()
    if not h1_path or not h5_path:
        return None
    h1_payload = joblib.load(h1_path)
    h5_payload = joblib.load(h5_path)
    out = make_runtime_entry(chosen=chosen, h1_payload=h1_payload, h5_payload=h5_payload)
    out["source"] = "global_cross_market"
    return out


def main(argv=None) -> int:
    args = parse_args(argv)
    train_summary_path = resolve_repo_path(args.train_summary_json)
    output_path = resolve_repo_path(args.output_json)

    summary = load_json_or_none(train_summary_path)
    if not isinstance(summary, dict):
        raise RuntimeError(f"invalid training summary: {train_summary_path}")

    feature_columns = list(summary.get("feature_columns", []) or [])
    if not feature_columns:
        raise RuntimeError("feature_columns missing in training summary")

    markets_out = []
    if str(args.export_mode) in ("hybrid", "per_market"):
        for ds in summary.get("datasets", []) or []:
            if not isinstance(ds, dict):
                continue
            market = str(ds.get("market", "")).strip()
            folds = [f for f in (ds.get("folds", []) or []) if isinstance(f, dict)]
            if not market or not folds:
                continue
            chosen = pick_fold(folds, str(args.fold_policy))
            if chosen is None:
                continue

            h1_path = str(chosen.get("model_artifacts", {}).get("h1_model", "")).strip()
            h5_path = str(chosen.get("model_artifacts", {}).get("h5_model", "")).strip()
            if not h1_path or not h5_path:
                continue
            h1_payload = joblib.load(h1_path)
            h5_payload = joblib.load(h5_path)

            row = make_runtime_entry(chosen=chosen, h1_payload=h1_payload, h5_payload=h5_payload)
            row["market"] = market
            markets_out.append(row)

    default_model = None
    if str(args.export_mode) in ("global_only", "hybrid"):
        default_model = build_global_default_model(summary, str(args.fold_policy))
        if default_model is None:
            raise RuntimeError(
                "export_mode requires global_model in train summary, but global fold artifacts were not found"
            )

    if str(args.export_mode) == "per_market" and not markets_out:
        raise RuntimeError("per_market export requested, but no valid market fold artifacts found")
    if str(args.export_mode) in ("global_only", "hybrid") and default_model is None:
        raise RuntimeError("global export requested, but default_model could not be created")

    global_fallback_enabled = bool(default_model is not None)
    prefer_default_model = bool(default_model is not None)

    out = {
        "version": "probabilistic_runtime_bundle_v1",
        "generated_at_utc": utc_now_iso(),
        "source_train_summary_json": str(train_summary_path),
        "selection_policy": str(args.fold_policy),
        "export_mode": str(args.export_mode),
        "feature_columns": feature_columns,
        "feature_transform_contract": {
            "rsi_center_scale": {"center": 50.0, "scale": 50.0},
            "age_min_cap": 240.0,
            "log_transform_columns": ["vol_ratio_20", "notional_ratio_20"],
            "percent_scale_rules": ["ret_*", "*_ret_*", "*atr_pct*", "*bb_width*", "*gap*"],
            "clip_abs": 8.0,
        },
        "global_fallback_enabled": bool(global_fallback_enabled),
        "prefer_default_model": bool(prefer_default_model),
        "default_model": default_model if default_model is not None else None,
        "markets": markets_out,
        "cost_model": summary.get("cost_model", {}),
    }
    dump_json(output_path, out)

    print("[ExportRuntimeBundle] completed", flush=True)
    print(f"export_mode={args.export_mode}", flush=True)
    print(f"global_fallback_enabled={global_fallback_enabled}", flush=True)
    print(f"markets={len(markets_out)}", flush=True)
    print(f"output={output_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
