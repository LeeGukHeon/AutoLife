#!/usr/bin/env python3
import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List

from _script_common import resolve_repo_path


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--feedback-json",
        "-FeedbackJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry_feedback.json",
    )
    parser.add_argument(
        "--policy-registry-json",
        "-PolicyRegistryJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_registry.json",
    )
    parser.add_argument(
        "--policy-decision-json",
        "-PolicyDecisionJson",
        default="./build/Release/logs/candidate_patch_action_override_policy_decision.json",
    )
    parser.add_argument("--template-id", "-TemplateId", default="")
    parser.add_argument("--required-keep-streak", "-RequiredKeepStreak", type=int, default=2)
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default="./build/Release/logs/candidate_patch_action_override_feedback_promotion_check.json",
    )
    return parser.parse_args(argv)


def _read_json(path_value: Path) -> Dict[str, Any]:
    if not path_value.exists():
        return {}
    try:
        payload = json.loads(path_value.read_text(encoding="utf-8-sig"))
        if isinstance(payload, dict):
            return payload
    except Exception:
        return {}
    return {}


def _build_recommended_ab_command(template_id: str, repeat_runs: int) -> str:
    rr = max(1, int(repeat_runs))
    template = str(template_id or "").strip()
    if not template:
        return ""
    return (
        "python scripts/run_patch_action_override_ab.py "
        f"--seed-patch-template {template} "
        f"--repeat-runs {rr} "
        "--require-handoff-applied "
        "--max-iterations 1 "
        "--max-runtime-minutes 60 "
        "--tune-max-scenarios 1 "
        "--tune-screen-dataset-limit 1 "
        "--tune-screen-top-k 1 "
        "--matrix-max-workers 1 "
        "--matrix-backtest-retry-count 1 "
        "--tune-holdout-suppression-hint-ratio-threshold 0.60 "
        "--real-data-only "
        "--require-higher-tf-companions "
        "--skip-core-vs-legacy-gate"
    )


def _collect_blockers(
    entry: Dict[str, Any],
    required_keep_streak: int,
    required_min_repeat_runs: int,
) -> List[str]:
    blockers: List[str] = []
    if not entry:
        blockers.append("template_feedback_missing")
        return blockers

    latest_decision = str(entry.get("latest_decision", "") or "")
    allow_keep = bool(entry.get("allow_keep_promotion", False))
    block_auto = bool(entry.get("block_auto_loop_consumption", False))
    current_repeat_runs = int(entry.get("last_repeat_runs", 0) or 0)
    current_keep_streak = int(entry.get("consecutive_keep_streak", 0) or 0)

    if latest_decision != "keep_override":
        blockers.append("latest_decision_not_keep_override")
    if not allow_keep:
        blockers.append("allow_keep_promotion_false")
    if block_auto:
        blockers.append("block_auto_loop_consumption_true")
    if current_repeat_runs < int(required_min_repeat_runs):
        blockers.append("repeat_runs_below_required")
    if current_keep_streak < int(required_keep_streak):
        blockers.append("keep_streak_below_required")

    return blockers


def main(argv=None) -> int:
    args = parse_args(argv)
    feedback_json = resolve_repo_path(args.feedback_json)
    policy_registry_json = resolve_repo_path(args.policy_registry_json)
    policy_decision_json = resolve_repo_path(args.policy_decision_json)
    output_json = resolve_repo_path(args.output_json)
    output_json.parent.mkdir(parents=True, exist_ok=True)

    feedback_payload = _read_json(feedback_json)
    policy_registry_payload = _read_json(policy_registry_json)
    policy_decision_payload = _read_json(policy_decision_json)

    feedback_template_map = feedback_payload.get("template_feedbacks") or {}
    if not isinstance(feedback_template_map, dict):
        feedback_template_map = {}

    template_id = str(args.template_id or "").strip()
    if not template_id:
        template_id = str(feedback_payload.get("latest_template_id", "") or "").strip()
    if not template_id:
        template_id = str(policy_registry_payload.get("latest_template_id", "") or "").strip()

    feedback_entry = feedback_template_map.get(template_id) if template_id else {}
    if not isinstance(feedback_entry, dict):
        feedback_entry = {}

    required_keep_streak = max(1, int(args.required_keep_streak))
    required_min_repeat_runs = int(feedback_entry.get("recommended_min_repeat_runs", 0) or 0)
    if required_min_repeat_runs <= 0:
        required_min_repeat_runs = 1

    blockers = _collect_blockers(
        feedback_entry,
        required_keep_streak=required_keep_streak,
        required_min_repeat_runs=required_min_repeat_runs,
    )
    if not template_id:
        blockers.append("template_id_missing")

    promotion_ready = len(blockers) == 0
    current_repeat_runs = int(feedback_entry.get("last_repeat_runs", 0) or 0)
    suggested_repeat_runs = max(required_min_repeat_runs, current_repeat_runs if current_repeat_runs > 0 else 1)
    if suggested_repeat_runs < required_min_repeat_runs:
        suggested_repeat_runs = required_min_repeat_runs

    report = {
        "generated_at": datetime.now(tz=timezone.utc).isoformat(),
        "inputs": {
            "feedback_json": str(feedback_json),
            "policy_registry_json": str(policy_registry_json),
            "policy_decision_json": str(policy_decision_json),
            "template_id": str(template_id),
            "required_keep_streak": int(required_keep_streak),
        },
        "feedback_snapshot": {
            "exists": bool(feedback_json.exists()),
            "latest_template_id": str(feedback_payload.get("latest_template_id", "") or ""),
            "latest_decision": str(feedback_payload.get("latest_decision", "") or ""),
            "matched_template": bool(template_id and bool(feedback_entry)),
            "entry": dict(feedback_entry or {}),
        },
        "policy_registry_snapshot": {
            "exists": bool(policy_registry_json.exists()),
            "latest_template_id": str(policy_registry_payload.get("latest_template_id", "") or ""),
            "latest_decision": str(policy_registry_payload.get("latest_decision", "") or ""),
        },
        "policy_decision_snapshot": {
            "exists": bool(policy_decision_json.exists()),
            "template_id": str(policy_decision_payload.get("template_id", "") or ""),
            "decision": str(policy_decision_payload.get("decision", "") or ""),
            "reason_code": str(policy_decision_payload.get("reason_code", "") or ""),
            "repeat_runs": int(policy_decision_payload.get("repeat_runs", 0) or 0),
        },
        "promotion_check": {
            "promotion_ready": bool(promotion_ready),
            "required_min_repeat_runs": int(required_min_repeat_runs),
            "required_keep_streak": int(required_keep_streak),
            "current_repeat_runs": int(current_repeat_runs),
            "current_keep_streak": int(feedback_entry.get("consecutive_keep_streak", 0) or 0),
            "latest_decision": str(feedback_entry.get("latest_decision", "") or ""),
            "allow_keep_promotion": bool(feedback_entry.get("allow_keep_promotion", False)),
            "block_auto_loop_consumption": bool(feedback_entry.get("block_auto_loop_consumption", False)),
            "blocker_reason_codes": list(blockers if blockers else ["ready"]),
        },
        "next_experiment": {
            "recommended_seed_template": str(template_id),
            "recommended_repeat_runs": int(suggested_repeat_runs),
            "recommended_command": _build_recommended_ab_command(template_id, suggested_repeat_runs),
        },
    }
    output_json.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("[PatchActionFeedbackPromotionCheck] 완료")
    print(f"output_json={output_json}")
    print(f"template_id={template_id or 'none'}")
    print(f"promotion_ready={promotion_ready}")
    print(
        "blockers="
        f"{','.join(report['promotion_check']['blocker_reason_codes'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
