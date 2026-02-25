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
    parser.add_argument(
        "--pipeline-version",
        "--pipeline_version",
        choices=("v1", "v2"),
        default="v1",
        help="MODE switch. v1 exports active bundle contract; v2 exports draft v2 contract.",
    )
    return parser.parse_args(argv)


def utc_now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def infer_train_summary_pipeline_version(summary: Dict[str, Any]) -> str:
    explicit = str(summary.get("pipeline_version", "")).strip().lower()
    if explicit in ("v1", "v2"):
        return explicit
    summary_version = str(summary.get("version", "")).strip().lower()
    if "v2" in summary_version:
        return "v2"
    return "v1"


def default_phase3_bundle_config() -> Dict[str, Any]:
    return {
        "phase3_frontier_enabled": False,
        "phase3_ev_calibration_enabled": False,
        "phase3_cost_tail_enabled": False,
        "phase3_adaptive_ev_blend_enabled": False,
        "phase3_diagnostics_v2_enabled": False,
        "frontier": {
            "enabled": False,
            "k_margin": 0.0,
            "k_uncertainty": 0.0,
            "k_cost_tail": 0.0,
            "min_required_ev": -0.0002,
            "max_required_ev": 0.0050,
            "margin_floor": -1.0,
            "ev_confidence_floor": 0.0,
            "ev_confidence_penalty": 0.0,
            "cost_tail_penalty": 0.0,
            "cost_tail_reject_threshold_pct": 1.0,
        },
        "ev_calibration": {
            "enabled": False,
            "use_quantile_map": False,
            "min_bucket_samples": 64,
            "default_confidence": 1.0,
            "min_confidence": 0.10,
            "ood_penalty": 0.10,
            "buckets": [],
        },
        "cost_model": {
            "enabled": False,
            "mode": "mean_mode",
            "entry_multiplier": 0.50,
            "exit_multiplier": 0.50,
            "entry_add_bps": 0.0,
            "exit_add_bps": 0.0,
            "tail_markup_ratio": 0.35,
            "tail_add_bps": 0.0,
            "hybrid_lambda": 0.50,
        },
        "adaptive_ev_blend": {
            "enabled": False,
            "min": 0.05,
            "max": 0.40,
            "base": 0.20,
            "trend_bonus": 0.08,
            "ranging_penalty": 0.06,
            "hostile_penalty": 0.08,
            "high_confidence_bonus": 0.05,
            "low_confidence_penalty": 0.10,
            "cost_penalty": 0.06,
        },
        "diagnostics_v2": {"enabled": False},
    }


def merge_dicts(base: Dict[str, Any], override: Dict[str, Any]) -> Dict[str, Any]:
    out: Dict[str, Any] = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = merge_dicts(out[key], value)
        else:
            out[key] = value
    return out


def build_phase3_bundle_config(summary: Dict[str, Any]) -> Dict[str, Any]:
    defaults = default_phase3_bundle_config()
    raw = summary.get("phase3", {})
    if not isinstance(raw, dict):
        raw = {}
    merged = merge_dicts(defaults, raw)

    # Keep nested enabled fields aligned with top-level feature flags.
    merged["frontier"]["enabled"] = bool(
        merged.get("phase3_frontier_enabled", merged["frontier"].get("enabled", False))
    )
    merged["ev_calibration"]["enabled"] = bool(
        merged.get("phase3_ev_calibration_enabled", merged["ev_calibration"].get("enabled", False))
    )
    merged["cost_model"]["enabled"] = bool(
        merged.get("phase3_cost_tail_enabled", merged["cost_model"].get("enabled", False))
    )
    merged["adaptive_ev_blend"]["enabled"] = bool(
        merged.get(
            "phase3_adaptive_ev_blend_enabled",
            merged["adaptive_ev_blend"].get("enabled", False),
        )
    )
    merged["diagnostics_v2"]["enabled"] = bool(
        merged.get("phase3_diagnostics_v2_enabled", merged["diagnostics_v2"].get("enabled", False))
    )
    merged["phase3_frontier_enabled"] = bool(merged["frontier"]["enabled"])
    merged["phase3_ev_calibration_enabled"] = bool(merged["ev_calibration"]["enabled"])
    merged["phase3_cost_tail_enabled"] = bool(merged["cost_model"]["enabled"])
    merged["phase3_adaptive_ev_blend_enabled"] = bool(merged["adaptive_ev_blend"]["enabled"])
    merged["phase3_diagnostics_v2_enabled"] = bool(merged["diagnostics_v2"]["enabled"])
    return merged


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


def build_global_ensemble_members(
    summary: Dict[str, Any],
    fold_policy: str,
) -> List[Dict[str, Any]]:
    ensemble_node = summary.get("ensemble", {})
    if not isinstance(ensemble_node, dict) or not bool(ensemble_node.get("enabled", False)):
        return []
    members_raw = [x for x in (ensemble_node.get("members", []) or []) if isinstance(x, dict)]
    out_members: List[Dict[str, Any]] = []
    for member in members_raw:
        global_model = member.get("global_model", {})
        if not isinstance(global_model, dict):
            continue
        folds = [f for f in (global_model.get("folds", []) or []) if isinstance(f, dict)]
        if not folds:
            continue
        chosen = pick_fold(folds, fold_policy)
        if chosen is None:
            continue
        h1_path = str(chosen.get("model_artifacts", {}).get("h1_model", "")).strip()
        h5_path = str(chosen.get("model_artifacts", {}).get("h5_model", "")).strip()
        if not h1_path or not h5_path:
            continue
        h1_payload = joblib.load(h1_path)
        h5_payload = joblib.load(h5_path)
        runtime_entry = make_runtime_entry(chosen=chosen, h1_payload=h1_payload, h5_payload=h5_payload)
        out_members.append(
            {
                "member_index": int(member.get("member_index", 0) or 0),
                "random_state": int(member.get("random_state", 0) or 0),
                "selected_fold_id": int(runtime_entry.get("selected_fold_id", 0) or 0),
                "model_artifacts": {
                    "h1_model": h1_path,
                    "h5_model": h5_path,
                },
                "h1_model": runtime_entry.get("h1_model", {}),
                "h5_model": runtime_entry.get("h5_model", {}),
                "selected_fold_metrics": runtime_entry.get("selected_fold_metrics", {}),
            }
        )
    return out_members


def main(argv=None) -> int:
    args = parse_args(argv)
    train_summary_path = resolve_repo_path(args.train_summary_json)
    output_path = resolve_repo_path(args.output_json)
    pipeline_version = str(args.pipeline_version).strip().lower()

    summary = load_json_or_none(train_summary_path)
    if not isinstance(summary, dict):
        raise RuntimeError(f"invalid training summary: {train_summary_path}")
    summary_pipeline_version = infer_train_summary_pipeline_version(summary)
    if summary_pipeline_version != pipeline_version:
        raise RuntimeError(
            "pipeline version mismatch: "
            f"export={pipeline_version} train_summary={summary_pipeline_version}"
        )

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
    ensemble_members: List[Dict[str, Any]] = []
    ensemble_summary_meta: Dict[str, Any] = {}
    if str(args.export_mode) in ("global_only", "hybrid"):
        default_model = build_global_default_model(summary, str(args.fold_policy))
        if default_model is None:
            raise RuntimeError(
                "export_mode requires global_model in train summary, but global fold artifacts were not found"
            )
        ensemble_members = build_global_ensemble_members(summary, str(args.fold_policy))
        ensemble_node = summary.get("ensemble", {})
        ensemble_enabled_in_summary = isinstance(ensemble_node, dict) and bool(ensemble_node.get("enabled", False))
        if ensemble_enabled_in_summary and not ensemble_members:
            raise RuntimeError("ensemble enabled in training summary, but no valid ensemble member artifacts were found")
        if ensemble_members:
            default_model["ensemble_members"] = ensemble_members
            default_model["ensemble_member_count"] = int(len(ensemble_members))
            default_model["ensemble_model_artifacts"] = [
                x.get("model_artifacts", {}) for x in ensemble_members
            ]
            ensemble_summary_meta = {
                "enabled": True,
                "ensemble_k": int(
                    (ensemble_node.get("ensemble_k", len(ensemble_members)) if isinstance(ensemble_node, dict) else len(ensemble_members))
                    or len(ensemble_members)
                ),
                "member_count": int(len(ensemble_members)),
                "seed_step": int(
                    (ensemble_node.get("seed_step", 0) if isinstance(ensemble_node, dict) else 0) or 0
                ),
            }

    if str(args.export_mode) == "per_market" and not markets_out:
        raise RuntimeError("per_market export requested, but no valid market fold artifacts found")
    if str(args.export_mode) in ("global_only", "hybrid") and default_model is None:
        raise RuntimeError("global export requested, but default_model could not be created")

    global_fallback_enabled = bool(default_model is not None)
    prefer_default_model = bool(default_model is not None)
    phase3_bundle = build_phase3_bundle_config(summary)

    out = {
        "version": (
            "probabilistic_runtime_bundle_v1"
            if pipeline_version == "v1"
            else "probabilistic_runtime_bundle_v2_draft"
        ),
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
        "phase3": phase3_bundle,
        "phase3_frontier_enabled": bool(phase3_bundle.get("phase3_frontier_enabled", False)),
        "phase3_ev_calibration_enabled": bool(phase3_bundle.get("phase3_ev_calibration_enabled", False)),
        "phase3_cost_tail_enabled": bool(phase3_bundle.get("phase3_cost_tail_enabled", False)),
        "phase3_adaptive_ev_blend_enabled": bool(phase3_bundle.get("phase3_adaptive_ev_blend_enabled", False)),
        "phase3_diagnostics_v2_enabled": bool(phase3_bundle.get("phase3_diagnostics_v2_enabled", False)),
    }
    if pipeline_version != "v1":
        out["pipeline_version"] = str(pipeline_version)
        out["feature_contract_version"] = "v2_draft"
        out["runtime_bundle_contract_version"] = "v2_draft"
    if ensemble_summary_meta:
        out["ensemble"] = ensemble_summary_meta
    dump_json(output_path, out)

    print("[ExportRuntimeBundle] completed", flush=True)
    print(f"export_mode={args.export_mode}", flush=True)
    print(f"global_fallback_enabled={global_fallback_enabled}", flush=True)
    print(f"markets={len(markets_out)}", flush=True)
    print(f"pipeline_version={pipeline_version}", flush=True)
    print(f"output={output_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
