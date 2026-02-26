#!/usr/bin/env python3
import argparse
import copy
import json
import math
import pathlib
import sys
from datetime import datetime, timezone
from typing import Any, Dict, List

from _script_common import dump_json, load_json_or_none, resolve_repo_path


def now_iso() -> str:
    return datetime.now(tz=timezone.utc).isoformat()


def now_epoch() -> int:
    return int(datetime.now(tz=timezone.utc).timestamp())


def f64(v: Any, d: float = 0.0) -> float:
    try:
        x = float(v)
    except Exception:
        return float(d)
    return float(d) if not math.isfinite(x) else x


def i64(v: Any, d: int = 0) -> int:
    try:
        return int(float(v))
    except Exception:
        return int(d)


def ng(payload: Any, path: List[str], default: Any) -> Any:
    cur = payload
    for key in path:
        if not isinstance(cur, dict):
            return default
        cur = cur.get(key)
    return default if cur is None else cur


def normalize_ops(ops: Dict[str, Any]) -> Dict[str, Any]:
    out = dict(ops)
    out["enabled"] = bool(out.get("enabled", False))
    out["mode"] = str(out.get("mode", "manual")).strip().lower() or "manual"
    out["required_ev_offset_min"] = f64(out.get("required_ev_offset_min", -0.003), -0.003)
    out["required_ev_offset_max"] = f64(out.get("required_ev_offset_max", 0.003), 0.003)
    if out["required_ev_offset_max"] < out["required_ev_offset_min"]:
        out["required_ev_offset_min"], out["required_ev_offset_max"] = out["required_ev_offset_max"], out["required_ev_offset_min"]
    out["required_ev_offset"] = min(out["required_ev_offset_max"], max(out["required_ev_offset_min"], f64(out.get("required_ev_offset", 0.0), 0.0)))

    out["k_margin_scale_min"] = max(0.0, f64(out.get("k_margin_scale_min", 0.5), 0.5))
    out["k_margin_scale_max"] = max(0.0, f64(out.get("k_margin_scale_max", 2.0), 2.0))
    if out["k_margin_scale_max"] < out["k_margin_scale_min"]:
        out["k_margin_scale_min"], out["k_margin_scale_max"] = out["k_margin_scale_max"], out["k_margin_scale_min"]
    out["k_margin_scale"] = min(out["k_margin_scale_max"], max(out["k_margin_scale_min"], f64(out.get("k_margin_scale", 1.0), 1.0)))

    out["ev_blend_scale_min"] = max(0.0, f64(out.get("ev_blend_scale_min", 0.5), 0.5))
    out["ev_blend_scale_max"] = max(0.0, f64(out.get("ev_blend_scale_max", 1.5), 1.5))
    if out["ev_blend_scale_max"] < out["ev_blend_scale_min"]:
        out["ev_blend_scale_min"], out["ev_blend_scale_max"] = out["ev_blend_scale_max"], out["ev_blend_scale_min"]
    out["ev_blend_scale"] = min(out["ev_blend_scale_max"], max(out["ev_blend_scale_min"], f64(out.get("ev_blend_scale", 1.0), 1.0)))

    out["max_step_per_update"] = min(1.0, max(0.0, f64(out.get("max_step_per_update", 0.05), 0.05)))
    out["min_update_interval_sec"] = max(0, i64(out.get("min_update_interval_sec", 3600), 3600))
    return out


def ensure_ops(bundle: Dict[str, Any]) -> Dict[str, Any]:
    phase3 = bundle.get("phase3", {})
    if not isinstance(phase3, dict):
        phase3 = {}
    ops = phase3.get("operations_control", {})
    if not isinstance(ops, dict):
        ops = {}
    defaults = {
        "enabled": False,
        "mode": "manual",
        "required_ev_offset": 0.0,
        "required_ev_offset_min": -0.003,
        "required_ev_offset_max": 0.003,
        "k_margin_scale": 1.0,
        "k_margin_scale_min": 0.5,
        "k_margin_scale_max": 2.0,
        "ev_blend_scale": 1.0,
        "ev_blend_scale_min": 0.5,
        "ev_blend_scale_max": 1.5,
        "max_step_per_update": 0.05,
        "min_update_interval_sec": 3600,
    }
    merged = dict(defaults)
    merged.update(ops)
    phase3["operations_control"] = normalize_ops(merged)
    bundle["phase3"] = phase3
    return bundle


def flatten(x: Any, p: str = "") -> Dict[str, Any]:
    out: Dict[str, Any] = {}
    if isinstance(x, dict):
        for k in sorted(x.keys()):
            cp = f"{p}.{k}" if p else str(k)
            out.update(flatten(x[k], cp))
        return out
    if isinstance(x, list):
        for i, v in enumerate(x):
            out.update(flatten(v, f"{p}[{i}]"))
        return out
    out[p] = x
    return out


def bundle_diff(lhs: Dict[str, Any], rhs: Dict[str, Any]) -> List[Dict[str, Any]]:
    a = flatten(lhs)
    b = flatten(rhs)
    rows = []
    for k in sorted(set(a.keys()) | set(b.keys())):
        if a.get(k) != b.get(k):
            rows.append({"key": k, "before": a.get(k), "after": b.get(k)})
    return rows


def kpis(report: Dict[str, Any]) -> Dict[str, Any]:
    agg = report.get("aggregates", {}) if isinstance(report.get("aggregates"), dict) else {}
    aad = ng(report, ["adaptive_validation", "aggregates"], {})
    if not isinstance(aad, dict):
        aad = {}
    lta = aad.get("loss_tail_aggregate", {})
    if not isinstance(lta, dict):
        lta = {}
    p3 = ng(report, ["diagnostics", "aggregate", "phase3_diagnostics_v2", "funnel_breakdown"], {})
    if not isinstance(p3, dict):
        p3 = {}
    p4 = ng(report, ["diagnostics", "aggregate", "phase4_portfolio_diagnostics", "selection_breakdown"], {})
    if not isinstance(p4, dict):
        p4 = {}
    return {
        "throughput_avg_trades": max(0.0, f64(agg.get("avg_total_trades", 0.0), 0.0)),
        "quality_risk_adjusted_score": f64(aad.get("avg_risk_adjusted_score", 0.0), 0.0),
        "quality_realized_edge_proxy_krw": f64(agg.get("avg_expectancy_krw", 0.0), 0.0),
        "risk_drawdown_pct": max(0.0, f64(agg.get("peak_max_drawdown_pct", 0.0), 0.0)),
        "risk_tail_loss_concentration": max(0.0, f64(lta.get("avg_top3_loss_concentration", 0.0), 0.0)),
        "risk_avg_fee_krw": max(0.0, f64(agg.get("avg_fee_krw", 0.0), 0.0)),
        "phase3_rejects": {
            "reject_margin_insufficient": max(0, i64(p3.get("reject_margin_insufficient", 0))),
            "reject_expected_value_fail": max(0, i64(p3.get("reject_expected_value_fail", 0))),
            "reject_frontier_fail": max(0, i64(p3.get("reject_frontier_fail", 0))),
            "reject_ev_confidence_low": max(0, i64(p3.get("reject_ev_confidence_low", 0))),
            "reject_cost_tail_fail": max(0, i64(p3.get("reject_cost_tail_fail", 0))),
        },
        "phase4_selection": {
            "selection_rate": max(0.0, f64(p4.get("selection_rate", 0.0), 0.0)),
            "rejected_by_drawdown_governor": max(0, i64(p4.get("rejected_by_drawdown_governor", 0))),
        },
    }


def eval_lex(report: Dict[str, Any], args: argparse.Namespace) -> Dict[str, Any]:
    ks = kpis(report)
    l1 = {
        "drawdown_ok": f64(ks["risk_drawdown_pct"]) <= float(args.max_drawdown_pct),
        "tail_ok": f64(ks["risk_tail_loss_concentration"]) <= float(args.max_tail_concentration),
    }
    l2 = {
        "adaptive_verdict_not_fail": str(ng(report, ["adaptive_validation", "verdict", "verdict"], "fail")).strip().lower() != "fail",
        "avg_fee_ok": f64(ks["risk_avg_fee_krw"]) <= float(args.max_avg_fee_krw),
        "strict_parity_ok": bool(args.strict_parity_pass),
    }
    agg = report.get("aggregates", {}) if isinstance(report.get("aggregates"), dict) else {}
    l3 = {
        "profit_factor_ok": f64(agg.get("avg_profit_factor", 0.0)) >= float(args.min_profit_factor),
        "expectancy_ok": f64(agg.get("avg_expectancy_krw", 0.0)) >= float(args.min_expectancy_krw),
    }
    l4 = {"throughput_ok": f64(ks["throughput_avg_trades"]) >= float(args.min_avg_trades)}
    p1 = all(l1.values())
    p2 = p1 and all(l2.values())
    p3 = p2 and all(l3.values())
    p4 = p3 and all(l4.values())
    return {"kpis": ks, "levels": {"p1": {"pass": p1, "checks": l1}, "p2": {"pass": p2, "checks": l2}, "p3": {"pass": p3, "checks": l3}, "p4": {"pass": p4, "checks": l4}}, "overall_pass": p4}


def cmd_control(args: argparse.Namespace) -> int:
    bpath = resolve_repo_path(args.bundle_json)
    rpath = resolve_repo_path(args.verification_report_json)
    opath = resolve_repo_path(args.output_bundle_json)
    repath = resolve_repo_path(args.output_report_json)
    spath = resolve_repo_path(args.state_json)
    bundle = load_json_or_none(bpath)
    report = load_json_or_none(rpath)
    if not isinstance(bundle, dict) or not isinstance(report, dict):
        raise RuntimeError("bundle/report load failed")
    state = load_json_or_none(spath)
    if not isinstance(state, dict):
        state = {}
    before = copy.deepcopy(bundle)
    bundle = ensure_ops(bundle)
    ops = bundle["phase3"]["operations_control"]
    ks = kpis(report)
    p3r = ks["phase3_rejects"]
    max_step = max(0.0, f64(ops.get("max_step_per_update", 0.05), 0.05))
    requested_step = f64(args.step_size, -1.0)
    if requested_step <= 0.0:
        # Default behavior uses policy max_step_per_update when CLI step is omitted.
        step = max_step
    else:
        step = max(0.0, min(requested_step, max_step))
    cs = state.get("control_mode_a", {}) if isinstance(state.get("control_mode_a"), dict) else {}
    elapsed = max(0, now_epoch() - i64(cs.get("last_update_ts", 0)))
    interval_ok = elapsed >= i64(ops.get("min_update_interval_sec", 3600))
    acts = []

    def apply_delta(key: str, delta: float) -> None:
        old = f64(ops.get(key, 0.0))
        new = old + delta
        if key == "required_ev_offset":
            new = min(f64(ops["required_ev_offset_max"]), max(f64(ops["required_ev_offset_min"]), new))
        elif key == "k_margin_scale":
            new = min(f64(ops["k_margin_scale_max"]), max(f64(ops["k_margin_scale_min"]), new))
        elif key == "ev_blend_scale":
            new = min(f64(ops["ev_blend_scale_max"]), max(f64(ops["ev_blend_scale_min"]), new))
        if abs(new - old) > 1e-12:
            ops[key] = new
            acts.append({"key": key, "old": old, "new": new, "delta": delta})

    if interval_ok and step > 0.0:
        t = f64(ks["throughput_avg_trades"])
        q = f64(ks["quality_risk_adjusted_score"])
        d = f64(ks["risk_drawdown_pct"])
        tc = f64(ks["risk_tail_loss_concentration"])
        evf = i64(p3r["reject_expected_value_fail"]) + i64(p3r["reject_frontier_fail"]) + i64(p3r["reject_ev_confidence_low"]) + i64(p3r["reject_cost_tail_fail"])
        mf = i64(p3r["reject_margin_insufficient"])
        if t < float(args.target_throughput_min):
            if evf >= mf:
                apply_delta("required_ev_offset", -step)
            else:
                apply_delta("k_margin_scale", +step)
        elif t > float(args.target_throughput_max):
            apply_delta("required_ev_offset", +step)
        if d > float(args.risk_drawdown_max_pct) or tc > float(args.risk_tail_concentration_max):
            apply_delta("required_ev_offset", +step)
            apply_delta("ev_blend_scale", -step)
        if q < float(args.target_quality_min_score):
            apply_delta("required_ev_offset", +(step * 0.5))
            apply_delta("ev_blend_scale", -(step * 0.5))

    bundle["phase3"]["operations_control"] = normalize_ops(ops)
    meta = bundle.get("release_meta", {}) if isinstance(bundle.get("release_meta"), dict) else {}
    parent = str(meta.get("version", bundle.get("version", ""))).strip()
    meta.update({"version": (str(args.release_version).strip() or f"phase34_ops_{now_epoch()}"), "created_at": now_iso(), "parent_version": parent, "mode": "phase34_operations_control_a", "change_summary": [f"{x['key']}:{x['old']}->{x['new']}" for x in acts]})
    bundle["release_meta"] = meta

    drows = bundle_diff(before, bundle)
    dump_json(opath, bundle)
    state["control_mode_a"] = {"last_update_ts": now_epoch(), "last_actions": acts, "last_kpis": ks, "current_knobs": {"required_ev_offset": bundle["phase3"]["operations_control"]["required_ev_offset"], "k_margin_scale": bundle["phase3"]["operations_control"]["k_margin_scale"], "ev_blend_scale": bundle["phase3"]["operations_control"]["ev_blend_scale"]}}
    dump_json(spath, state)
    dump_json(repath, {"mode": "control_mode_a", "generated_at": now_iso(), "interval_ok": interval_ok, "elapsed_sec": elapsed, "actions": acts, "kpis": ks, "bundle_diff_count": len(drows), "bundle_diff_top": drows[:50], "bundle_output": str(opath), "state_path": str(spath)})
    print(f"[Phase34Ops] mode=control_a actions={len(acts)} diff={len(drows)} interval_ok={str(interval_ok).lower()}", flush=True)
    return 0


def reward(report: Dict[str, Any], args: argparse.Namespace) -> float:
    ks = kpis(report)
    return (f64(ks["quality_realized_edge_proxy_krw"]) * float(args.reward_expectancy_weight) - f64(ks["risk_drawdown_pct"]) * float(args.reward_drawdown_weight) - f64(ks["risk_tail_loss_concentration"]) * float(args.reward_tail_weight) + f64(ks["throughput_avg_trades"]) * float(args.reward_throughput_weight))


def cmd_bandit(args: argparse.Namespace) -> int:
    spath = resolve_repo_path(args.state_json)
    opath = resolve_repo_path(args.output_json)
    state = load_json_or_none(spath)
    if not isinstance(state, dict):
        state = {}
    arms = [str(resolve_repo_path(x)) for x in args.candidate_bundle_jsons if str(x).strip()]
    if not arms:
        raise RuntimeError("candidate bundles required")
    b = state.get("bandit_mode_b", {}) if isinstance(state.get("bandit_mode_b"), dict) else {}
    bd = b.get("arms", {}) if isinstance(b.get("arms"), dict) else {}
    for arm in arms:
        cur = bd.get(arm, {}) if isinstance(bd.get(arm), dict) else {}
        bd[arm] = {"plays": max(0, i64(cur.get("plays", 0))), "total_reward": f64(cur.get("total_reward", 0.0)), "mean_reward": f64(cur.get("mean_reward", 0.0)), "last_reward": cur.get("last_reward", None)}
    last = str(b.get("last_selected", "")).strip()
    updated = None
    if str(args.update_reward_report_json).strip() and last in bd:
        r = load_json_or_none(resolve_repo_path(args.update_reward_report_json))
        if isinstance(r, dict):
            updated = reward(r, args)
            bd[last]["plays"] = int(bd[last]["plays"]) + 1
            bd[last]["total_reward"] = float(bd[last]["total_reward"]) + float(updated)
            bd[last]["mean_reward"] = float(bd[last]["total_reward"]) / float(max(1, bd[last]["plays"]))
            bd[last]["last_reward"] = float(updated)
    rnd = max(0, i64(b.get("round", 0))) + 1
    n = max(1, i64(args.explore_every_n, 4))
    explore = (rnd % n) == 0
    if explore:
        sel = sorted(arms, key=lambda x: (i64(bd[x]["plays"]), x))[0]
        why = "explore_least_played"
    else:
        sel = sorted(arms, key=lambda x: (-f64(bd[x]["mean_reward"]), i64(bd[x]["plays"]), x))[0]
        why = "exploit_best_mean_reward"
    rank = sorted([{"bundle": x, "plays": i64(bd[x]["plays"]), "mean_reward": f64(bd[x]["mean_reward"]), "last_reward": bd[x]["last_reward"]} for x in arms], key=lambda x: (-f64(x["mean_reward"]), i64(x["plays"]), str(x["bundle"])))
    b.update({"round": int(rnd), "last_selected": sel, "last_selection_reason": why, "last_updated_at": now_iso(), "arms": bd})
    state["bandit_mode_b"] = b
    dump_json(spath, state)
    out = {"mode": "bandit_mode_b", "generated_at": now_iso(), "round": int(rnd), "selected_bundle": sel, "selection_reason": why, "should_explore": bool(explore), "updated_reward": updated, "ranking": rank, "state_path": str(spath)}
    if str(args.output_selected_bundle_json).strip():
        sel_payload = load_json_or_none(pathlib.Path(sel))
        if isinstance(sel_payload, dict):
            dump_json(resolve_repo_path(args.output_selected_bundle_json), sel_payload)
            out["output_selected_bundle_json"] = str(resolve_repo_path(args.output_selected_bundle_json))
    dump_json(opath, out)
    print(f"[Phase34Ops] mode=bandit_b round={rnd} selected={sel} reason={why}", flush=True)
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    dev = load_json_or_none(resolve_repo_path(args.development_report_json))
    val = load_json_or_none(resolve_repo_path(args.validation_report_json))
    qua = load_json_or_none(resolve_repo_path(args.quarantine_report_json))
    if not isinstance(dev, dict) or not isinstance(val, dict) or not isinstance(qua, dict):
        raise RuntimeError("dev/val/quarantine reports required")
    de = eval_lex(dev, args)
    ve = eval_lex(val, args)
    qe = eval_lex(qua, args)
    ok = bool(de.get("overall_pass", False) and ve.get("overall_pass", False) and qe.get("overall_pass", False))
    dump_json(resolve_repo_path(args.output_json), {"mode": "validation_protocol", "generated_at": now_iso(), "purge_gap_minutes": int(args.purge_gap_minutes), "split_meta": {"development_tag": str(args.development_tag), "validation_tag": str(args.validation_tag), "quarantine_tag": str(args.quarantine_tag)}, "development": de, "validation": ve, "quarantine": qe, "overall_pass": ok})
    print(f"[Phase34Ops] mode=validate overall_pass={str(ok).lower()}", flush=True)
    return 0 if ok else 2


def cmd_promotion(args: argparse.Namespace) -> int:
    rep = load_json_or_none(resolve_repo_path(args.paper_report_json))
    if not isinstance(rep, dict):
        raise RuntimeError("paper report required")
    spath = resolve_repo_path(args.state_json)
    state = load_json_or_none(spath)
    if not isinstance(state, dict):
        state = {}
    ps = state.get("promotion_gate", {}) if isinstance(state.get("promotion_gate"), dict) else {}
    ev = eval_lex(rep, args)
    passed = bool(ev.get("overall_pass", False))
    stages = ["paper", "live10", "live30", "live100"]
    cur = str(args.current_stage).strip().lower()
    if cur not in stages:
        cur = "paper"
    c = max(0, i64(ps.get("consecutive_ok", 0)))
    c = c + 1 if passed else 0
    req = max(1, i64(args.min_consecutive_windows, 2))
    action = "hold"
    nxt = cur
    rollback = False
    if not passed:
        rollback = True
        nxt = "paper"
        action = "rollback_to_conservative_bundle"
    elif c >= req:
        idx = stages.index(cur)
        if idx < len(stages) - 1:
            nxt = stages[idx + 1]
            action = f"promote_{cur}_to_{nxt}"
            c = 0
        else:
            action = "hold_live100"
    ps.update({"last_updated_at": now_iso(), "current_stage": cur, "next_stage": nxt, "action": action, "rollback": rollback, "consecutive_ok": c, "min_consecutive_windows": req, "last_pass": passed})
    state["promotion_gate"] = ps
    dump_json(spath, state)
    dump_json(resolve_repo_path(args.output_json), {"mode": "promotion_gate", "generated_at": now_iso(), "current_stage": cur, "next_stage": nxt, "action": action, "rollback": rollback, "consecutive_ok": c, "evaluation": ev, "state_path": str(spath)})
    print(f"[Phase34Ops] mode=promotion current={cur} next={nxt} action={action}", flush=True)
    return 0 if not rollback else 2


def cmd_diff(args: argparse.Namespace) -> int:
    a = load_json_or_none(resolve_repo_path(args.base_bundle_json))
    b = load_json_or_none(resolve_repo_path(args.candidate_bundle_json))
    if not isinstance(a, dict) or not isinstance(b, dict):
        raise RuntimeError("base/candidate bundles required")
    rows = bundle_diff(a, b)
    dump_json(resolve_repo_path(args.output_json), {"mode": "bundle_diff", "generated_at": now_iso(), "base_bundle_json": str(resolve_repo_path(args.base_bundle_json)), "candidate_bundle_json": str(resolve_repo_path(args.candidate_bundle_json)), "change_count": len(rows), "changes": rows})
    print(f"[Phase34Ops] mode=bundle_diff changes={len(rows)}", flush=True)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Phase3/4 operations tuning and anti-overfit validation toolkit.")
    s = p.add_subparsers(dest="cmd", required=True)

    a = s.add_parser("control_a_update")
    a.add_argument("--bundle-json", default=r".\config\model\probabilistic_runtime_bundle_v2.json")
    a.add_argument("--verification-report-json", default=r".\build\Release\logs\verification_report.json")
    a.add_argument("--output-bundle-json", default=r".\config\model\probabilistic_runtime_bundle_v2_phase34_ops_next.json")
    a.add_argument("--output-report-json", default=r".\build\Release\logs\phase34_ops_control_update_report.json")
    a.add_argument("--state-json", default=r".\build\Release\logs\phase34_ops_state.json")
    a.add_argument("--release-version", default="")
    a.add_argument("--step-size", type=float, default=0.0)
    a.add_argument("--target-throughput-min", type=float, default=8.0)
    a.add_argument("--target-throughput-max", type=float, default=80.0)
    a.add_argument("--target-quality-min-score", type=float, default=-0.10)
    a.add_argument("--risk-drawdown-max-pct", type=float, default=12.0)
    a.add_argument("--risk-tail-concentration-max", type=float, default=0.70)

    b = s.add_parser("bandit_b_select")
    b.add_argument("--candidate-bundle-jsons", nargs="+", required=True)
    b.add_argument("--state-json", default=r".\build\Release\logs\phase34_ops_state.json")
    b.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_bandit_report.json")
    b.add_argument("--output-selected-bundle-json", default="")
    b.add_argument("--update-reward-report-json", default="")
    b.add_argument("--explore-every-n", type=int, default=4)
    b.add_argument("--reward-expectancy-weight", type=float, default=1.0)
    b.add_argument("--reward-drawdown-weight", type=float, default=2.0)
    b.add_argument("--reward-tail-weight", type=float, default=50.0)
    b.add_argument("--reward-throughput-weight", type=float, default=0.2)

    v = s.add_parser("validate_protocol")
    v.add_argument("--development-report-json", required=True)
    v.add_argument("--validation-report-json", required=True)
    v.add_argument("--quarantine-report-json", required=True)
    v.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_validation_report.json")
    v.add_argument("--development-tag", default="development")
    v.add_argument("--validation-tag", default="validation")
    v.add_argument("--quarantine-tag", default="quarantine")
    v.add_argument("--purge-gap-minutes", type=int, default=0)
    v.add_argument("--strict-parity-pass", action="store_true")
    v.add_argument("--max-drawdown-pct", type=float, default=12.0)
    v.add_argument("--max-tail-concentration", type=float, default=0.70)
    v.add_argument("--max-avg-fee-krw", type=float, default=5000.0)
    v.add_argument("--min-profit-factor", type=float, default=1.0)
    v.add_argument("--min-expectancy-krw", type=float, default=0.0)
    v.add_argument("--min-avg-trades", type=float, default=8.0)

    g = s.add_parser("promotion_gate")
    g.add_argument("--paper-report-json", required=True)
    g.add_argument("--state-json", default=r".\build\Release\logs\phase34_ops_state.json")
    g.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_promotion_gate_report.json")
    g.add_argument("--current-stage", default="paper")
    g.add_argument("--min-consecutive-windows", type=int, default=2)
    g.add_argument("--strict-parity-pass", action="store_true")
    g.add_argument("--max-drawdown-pct", type=float, default=12.0)
    g.add_argument("--max-tail-concentration", type=float, default=0.70)
    g.add_argument("--max-avg-fee-krw", type=float, default=5000.0)
    g.add_argument("--min-profit-factor", type=float, default=1.0)
    g.add_argument("--min-expectancy-krw", type=float, default=0.0)
    g.add_argument("--min-avg-trades", type=float, default=8.0)

    d = s.add_parser("bundle_diff")
    d.add_argument("--base-bundle-json", required=True)
    d.add_argument("--candidate-bundle-json", required=True)
    d.add_argument("--output-json", default=r".\build\Release\logs\phase34_ops_bundle_diff.json")
    return p


def main(argv: List[str] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.cmd == "control_a_update":
        return cmd_control(args)
    if args.cmd == "bandit_b_select":
        return cmd_bandit(args)
    if args.cmd == "validate_protocol":
        return cmd_validate(args)
    if args.cmd == "promotion_gate":
        return cmd_promotion(args)
    if args.cmd == "bundle_diff":
        return cmd_diff(args)
    raise RuntimeError(f"unsupported cmd: {args.cmd}")


if __name__ == "__main__":
    sys.exit(main())
