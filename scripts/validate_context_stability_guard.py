#!/usr/bin/env python3
"""Fast regression checks for context-stability guard transitions."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
from typing import Any, Dict, Tuple


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "scripts" / "tune_candidate_gate_trade_density.py"


def load_tune_module():
    spec = importlib.util.spec_from_file_location("tune_candidate_gate_trade_density", MODULE_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load module spec: {MODULE_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_case(
    module: Any,
    *,
    case_name: str,
    context: Dict[str, Any],
    state: Dict[str, Any],
    threshold: int,
    expect_ctx: Dict[str, Any],
    expect_state: Dict[str, Any],
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    updated_ctx, next_state = module.apply_context_stability_guard(
        context=context,
        state=state,
        enabled=True,
        min_consecutive_to_flip=threshold,
    )
    for key, value in expect_ctx.items():
        actual = updated_ctx.get(key)
        require(
            actual == value,
            f"[{case_name}] context mismatch: {key} expected={value!r}, actual={actual!r}",
        )
    for key, value in expect_state.items():
        actual = next_state.get(key)
        require(
            actual == value,
            f"[{case_name}] state mismatch: {key} expected={value!r}, actual={actual!r}",
        )
    return updated_ctx, next_state


def main() -> int:
    module = load_tune_module()
    threshold = 3
    require(
        module.canonical_rr_adaptive_focus_branch("entry_quality_rr_base_test")
        == "entry_quality_rr_base",
        "canonical mapping failed for entry_quality_rr_base",
    )
    require(
        module.canonical_rr_adaptive_focus_branch("entry_quality_rr_adaptive_history_test")
        == "entry_quality_rr_adaptive_history",
        "canonical mapping failed for entry_quality_rr_adaptive_history",
    )
    require(
        module.canonical_rr_adaptive_focus_branch("second_stage_confirmation_hostile_history_severe_flag")
        == "second_stage_confirmation_hostile_history_severe",
        "canonical mapping failed for second_stage_confirmation_hostile_history_severe",
    )

    case1_ctx = {"risk_gate_focus": "entry_quality_rr_adaptive_mixed"}
    case1_state: Dict[str, Any] = {}
    _, state_1 = run_case(
        module,
        case_name="first_managed_branch",
        context=case1_ctx,
        state=case1_state,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_mixed",
            "context_stability_guard_reason": "first_managed_branch",
            "context_stability_guard_override_applied": False,
            "context_stability_guard_flip_candidate_focus": "",
            "context_stability_guard_flip_candidate_count": 0,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_mixed",
            "applied_focus_streak": 1,
            "flip_candidate_focus": "",
            "flip_candidate_count": 0,
            "last_override_applied": False,
            "last_reason": "first_managed_branch",
            "runs_total": 1,
        },
    )

    case2_ctx = {
        "risk_gate_focus": "entry_quality_rr_adaptive_regime",
        "holdout_recommendation": "hold_candidate_calibrate_risk_gate_rr_adaptive_regime_adders",
        "risk_gate_focus_source": "holdout_recommendation_override",
    }
    _, state_2 = run_case(
        module,
        case_name="flip_candidate_1_below_threshold",
        context=case2_ctx,
        state=state_1,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_mixed",
            "risk_gate_focus_source": "context_stability_guard_override",
            "holdout_recommendation": "hold_candidate_calibrate_risk_gate_rr_adaptive_mixed_adders",
            "holdout_recommendation_source": "context_stability_guard_override",
            "context_stability_guard_reason": "flip_candidate_below_threshold",
            "context_stability_guard_override_applied": True,
            "context_stability_guard_flip_candidate_focus": "entry_quality_rr_adaptive_regime",
            "context_stability_guard_flip_candidate_count": 1,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_mixed",
            "applied_focus_streak": 2,
            "flip_candidate_focus": "entry_quality_rr_adaptive_regime",
            "flip_candidate_count": 1,
            "last_override_applied": True,
            "last_reason": "flip_candidate_below_threshold",
            "runs_total": 2,
        },
    )

    case3_ctx = {
        "risk_gate_focus": "entry_quality_rr_adaptive_regime",
        "holdout_recommendation": "hold_candidate_calibrate_risk_gate_rr_adaptive_regime_adders",
    }
    _, state_3 = run_case(
        module,
        case_name="flip_candidate_2_below_threshold",
        context=case3_ctx,
        state=state_2,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_mixed",
            "context_stability_guard_reason": "flip_candidate_below_threshold",
            "context_stability_guard_override_applied": True,
            "context_stability_guard_flip_candidate_focus": "entry_quality_rr_adaptive_regime",
            "context_stability_guard_flip_candidate_count": 2,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_mixed",
            "applied_focus_streak": 3,
            "flip_candidate_focus": "entry_quality_rr_adaptive_regime",
            "flip_candidate_count": 2,
            "last_override_applied": True,
            "last_reason": "flip_candidate_below_threshold",
            "runs_total": 3,
        },
    )

    case4_ctx = {"risk_gate_focus": "entry_quality_rr_adaptive_regime"}
    _, state_4 = run_case(
        module,
        case_name="flip_accepted_after_threshold",
        context=case4_ctx,
        state=state_3,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_regime",
            "context_stability_guard_reason": "flip_accepted_after_threshold",
            "context_stability_guard_override_applied": False,
            "context_stability_guard_flip_candidate_focus": "",
            "context_stability_guard_flip_candidate_count": 0,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_regime",
            "applied_focus_streak": 1,
            "flip_candidate_focus": "",
            "flip_candidate_count": 0,
            "last_override_applied": False,
            "last_reason": "flip_accepted_after_threshold",
            "runs_total": 4,
        },
    )

    case5_ctx = {"risk_gate_focus": "entry_quality_rr_adaptive_regime"}
    _, state_5 = run_case(
        module,
        case_name="same_branch",
        context=case5_ctx,
        state=state_4,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_regime",
            "context_stability_guard_reason": "same_branch",
            "context_stability_guard_override_applied": False,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_regime",
            "applied_focus_streak": 2,
            "last_reason": "same_branch",
            "runs_total": 5,
        },
    )

    case6_ctx = {
        "risk_gate_focus": "entry_quality_rr_base",
        "holdout_recommendation": "hold_candidate_calibrate_risk_gate_rr_baseline_floor",
    }
    _, state_6 = run_case(
        module,
        case_name="rr_base_flip_candidate_1",
        context=case6_ctx,
        state=state_5,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_regime",
            "context_stability_guard_reason": "flip_candidate_below_threshold",
            "context_stability_guard_override_applied": True,
            "context_stability_guard_flip_candidate_focus": "entry_quality_rr_base",
            "context_stability_guard_flip_candidate_count": 1,
            "holdout_recommendation": "hold_candidate_calibrate_risk_gate_rr_adaptive_regime_adders",
            "holdout_recommendation_source": "context_stability_guard_override",
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_regime",
            "flip_candidate_focus": "entry_quality_rr_base",
            "flip_candidate_count": 1,
            "last_reason": "flip_candidate_below_threshold",
            "runs_total": 6,
        },
    )

    case7_ctx = {"risk_gate_focus": "entry_quality_rr_base"}
    _, state_7 = run_case(
        module,
        case_name="rr_base_flip_candidate_2",
        context=case7_ctx,
        state=state_6,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_adaptive_regime",
            "context_stability_guard_reason": "flip_candidate_below_threshold",
            "context_stability_guard_override_applied": True,
            "context_stability_guard_flip_candidate_focus": "entry_quality_rr_base",
            "context_stability_guard_flip_candidate_count": 2,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_adaptive_regime",
            "flip_candidate_focus": "entry_quality_rr_base",
            "flip_candidate_count": 2,
            "last_reason": "flip_candidate_below_threshold",
            "runs_total": 7,
        },
    )

    case8_ctx = {"risk_gate_focus": "entry_quality_rr_base"}
    run_case(
        module,
        case_name="rr_base_flip_accepted_after_threshold",
        context=case8_ctx,
        state=state_7,
        threshold=threshold,
        expect_ctx={
            "risk_gate_focus": "entry_quality_rr_base",
            "context_stability_guard_reason": "flip_accepted_after_threshold",
            "context_stability_guard_override_applied": False,
            "context_stability_guard_flip_candidate_focus": "",
            "context_stability_guard_flip_candidate_count": 0,
        },
        expect_state={
            "applied_focus": "entry_quality_rr_base",
            "flip_candidate_focus": "",
            "flip_candidate_count": 0,
            "last_reason": "flip_accepted_after_threshold",
            "runs_total": 8,
        },
    )

    print("[ValidateContextStabilityGuard] PASS")
    print(f"module={MODULE_PATH}")
    print(f"threshold={threshold}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
