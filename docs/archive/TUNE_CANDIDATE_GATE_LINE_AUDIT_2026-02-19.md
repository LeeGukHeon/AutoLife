# tune_candidate_gate_trade_density.py Line Audit (2026-02-19)

- Target: `scripts/tune_candidate_gate_trade_density.py`
- Total top-level functions: 75
- Method: AST line-range extraction + call-chain reachability (all 75 reachable from `main`) + latest smoke (`stage2306`) activity markers.
- Important: reachable != always necessary. This audit classifies by operational necessity for current pipeline.

## Summary
- REQUIRED: 22 functions, 11388 lines (49.05%)
- CONDITIONAL_EXPERIMENT: 9 functions, 3989 lines (17.18%)
- CONDITIONAL_POLICY: 18 functions, 2907 lines (12.52%)
- SUPPORT: 26 functions, 4934 lines (21.25%)

## Full Function Inventory
| Function | Lines | Range | Class | Note |
|---|---:|---|---|---|
| `read_live_signal_funnel_snapshot` | 83 | `scripts\tune_candidate_gate_trade_density.py:134` / `scripts\tune_candidate_gate_trade_density.py:216` | SUPPORT |  |
| `read_train_eval_holdout_context` | 480 | `scripts\tune_candidate_gate_trade_density.py:219` / `scripts\tune_candidate_gate_trade_density.py:698` | SUPPORT |  |
| `build_effective_bottleneck_context` | 321 | `scripts\tune_candidate_gate_trade_density.py:701` / `scripts\tune_candidate_gate_trade_density.py:1021` | SUPPORT |  |
| `canonical_rr_adaptive_focus_branch` | 13 | `scripts\tune_candidate_gate_trade_density.py:1024` / `scripts\tune_candidate_gate_trade_density.py:1036` | SUPPORT |  |
| `canonical_focus_branch_from_risk_component_reason` | 17 | `scripts\tune_candidate_gate_trade_density.py:1039` / `scripts\tune_candidate_gate_trade_density.py:1055` | SUPPORT |  |
| `load_context_stability_state` | 28 | `scripts\tune_candidate_gate_trade_density.py:1058` / `scripts\tune_candidate_gate_trade_density.py:1085` | SUPPORT |  |
| `save_context_stability_state` | 7 | `scripts\tune_candidate_gate_trade_density.py:1088` / `scripts\tune_candidate_gate_trade_density.py:1094` | SUPPORT |  |
| `apply_context_stability_guard` | 121 | `scripts\tune_candidate_gate_trade_density.py:1097` / `scripts\tune_candidate_gate_trade_density.py:1217` | CONDITIONAL_POLICY | inactive_in_stage2306 |
| `compute_combo_bottleneck_priority_score` | 327 | `scripts\tune_candidate_gate_trade_density.py:1220` / `scripts\tune_candidate_gate_trade_density.py:1546` | CONDITIONAL_POLICY |  |
| `prioritize_combo_specs_for_bottleneck` | 21 | `scripts\tune_candidate_gate_trade_density.py:1549` / `scripts\tune_candidate_gate_trade_density.py:1569` | SUPPORT |  |
| `_clamp` | 2 | `scripts\tune_candidate_gate_trade_density.py:1572` / `scripts\tune_candidate_gate_trade_density.py:1573` | SUPPORT |  |
| `apply_hint_impact_guardrail` | 49 | `scripts\tune_candidate_gate_trade_density.py:1576` / `scripts\tune_candidate_gate_trade_density.py:1624` | CONDITIONAL_POLICY |  |
| `apply_holdout_failure_family_suppression` | 103 | `scripts\tune_candidate_gate_trade_density.py:1627` / `scripts\tune_candidate_gate_trade_density.py:1729` | CONDITIONAL_POLICY |  |
| `expand_post_suppression_quality_exit_candidates` | 99 | `scripts\tune_candidate_gate_trade_density.py:1732` / `scripts\tune_candidate_gate_trade_density.py:1830` | CONDITIONAL_EXPERIMENT |  |
| `expand_walk_forward_profit_sum_recovery_candidates` | 310 | `scripts\tune_candidate_gate_trade_density.py:1833` / `scripts\tune_candidate_gate_trade_density.py:2142` | CONDITIONAL_EXPERIMENT |  |
| `expand_walk_forward_expectancy_repair_candidates` | 209 | `scripts\tune_candidate_gate_trade_density.py:2145` / `scripts\tune_candidate_gate_trade_density.py:2353` | CONDITIONAL_EXPERIMENT |  |
| `expand_walk_forward_window_feature_recovery_candidates` | 526 | `scripts\tune_candidate_gate_trade_density.py:2356` / `scripts\tune_candidate_gate_trade_density.py:2881` | CONDITIONAL_EXPERIMENT |  |
| `expand_holdout_expectancy_lift_candidates` | 547 | `scripts\tune_candidate_gate_trade_density.py:2884` / `scripts\tune_candidate_gate_trade_density.py:3430` | CONDITIONAL_EXPERIMENT |  |
| `expand_generalization_walk_forward_bridge_candidates` | 415 | `scripts\tune_candidate_gate_trade_density.py:3433` / `scripts\tune_candidate_gate_trade_density.py:3847` | CONDITIONAL_EXPERIMENT |  |
| `expand_oos_profitability_remediation_candidates` | 851 | `scripts\tune_candidate_gate_trade_density.py:3850` / `scripts\tune_candidate_gate_trade_density.py:4700` | CONDITIONAL_EXPERIMENT |  |
| `expand_manager_ev_edge_joint_remediation_candidates` | 462 | `scripts\tune_candidate_gate_trade_density.py:4703` / `scripts\tune_candidate_gate_trade_density.py:5164` | CONDITIONAL_EXPERIMENT | inactive_in_stage2306 |
| `expand_rr_adaptive_history_mixed_oos_recovery_candidates` | 570 | `scripts\tune_candidate_gate_trade_density.py:5167` / `scripts\tune_candidate_gate_trade_density.py:5736` | CONDITIONAL_EXPERIMENT |  |
| `adapt_combo_specs_for_bottleneck` | 3434 | `scripts\tune_candidate_gate_trade_density.py:5739` / `scripts\tune_candidate_gate_trade_density.py:9172` | SUPPORT |  |
| `resolve_or_throw` | 7 | `scripts\tune_candidate_gate_trade_density.py:9175` / `scripts\tune_candidate_gate_trade_density.py:9181` | SUPPORT |  |
| `ensure_parent_directory` | 2 | `scripts\tune_candidate_gate_trade_density.py:9184` / `scripts\tune_candidate_gate_trade_density.py:9185` | SUPPORT |  |
| `set_or_add_property` | 2 | `scripts\tune_candidate_gate_trade_density.py:9188` / `scripts\tune_candidate_gate_trade_density.py:9189` | SUPPORT |  |
| `ensure_strategy_node` | 5 | `scripts\tune_candidate_gate_trade_density.py:9192` / `scripts\tune_candidate_gate_trade_density.py:9196` | SUPPORT |  |
| `apply_candidate_combo_to_config` | 220 | `scripts\tune_candidate_gate_trade_density.py:9199` / `scripts\tune_candidate_gate_trade_density.py:9418` | REQUIRED |  |
| `has_higher_tf_companions` | 12 | `scripts\tune_candidate_gate_trade_density.py:9421` / `scripts\tune_candidate_gate_trade_density.py:9432` | SUPPORT |  |
| `market_label_from_dataset_name` | 11 | `scripts\tune_candidate_gate_trade_density.py:9435` / `scripts\tune_candidate_gate_trade_density.py:9445` | SUPPORT |  |
| `summarize_market_loss_concentration_from_matrix` | 62 | `scripts\tune_candidate_gate_trade_density.py:9448` / `scripts\tune_candidate_gate_trade_density.py:9509` | SUPPORT |  |
| `classify_wf_window_loss_feature` | 60 | `scripts\tune_candidate_gate_trade_density.py:9512` / `scripts\tune_candidate_gate_trade_density.py:9571` | SUPPORT |  |
| `summarize_walk_forward_window_loss_context` | 287 | `scripts\tune_candidate_gate_trade_density.py:9574` / `scripts\tune_candidate_gate_trade_density.py:9860` | SUPPORT |  |
| `get_dataset_list` | 17 | `scripts\tune_candidate_gate_trade_density.py:9863` / `scripts\tune_candidate_gate_trade_density.py:9879` | REQUIRED |  |
| `resolve_explicit_dataset_list` | 35 | `scripts\tune_candidate_gate_trade_density.py:9882` / `scripts\tune_candidate_gate_trade_density.py:9916` | REQUIRED |  |
| `run_dataset_quality_gate` | 60 | `scripts\tune_candidate_gate_trade_density.py:9919` / `scripts\tune_candidate_gate_trade_density.py:9978` | REQUIRED |  |
| `new_combo_variant` | 6 | `scripts\tune_candidate_gate_trade_density.py:9981` / `scripts\tune_candidate_gate_trade_density.py:9986` | SUPPORT |  |
| `build_combo_specs` | 274 | `scripts\tune_candidate_gate_trade_density.py:9989` / `scripts\tune_candidate_gate_trade_density.py:10262` | REQUIRED |  |
| `select_evenly_spaced_datasets` | 17 | `scripts\tune_candidate_gate_trade_density.py:10265` / `scripts\tune_candidate_gate_trade_density.py:10281` | CONDITIONAL_POLICY |  |
| `combo_tunable_material` | 12 | `scripts\tune_candidate_gate_trade_density.py:10284` / `scripts\tune_candidate_gate_trade_density.py:10295` | SUPPORT |  |
| `combo_fingerprint` | 4 | `scripts\tune_candidate_gate_trade_density.py:10298` / `scripts\tune_candidate_gate_trade_density.py:10301` | REQUIRED |  |
| `dedupe_combos` | 10 | `scripts\tune_candidate_gate_trade_density.py:10304` / `scripts\tune_candidate_gate_trade_density.py:10313` | REQUIRED |  |
| `dataset_signature` | 12 | `scripts\tune_candidate_gate_trade_density.py:10316` / `scripts\tune_candidate_gate_trade_density.py:10327` | REQUIRED |  |
| `stable_json_hash` | 3 | `scripts\tune_candidate_gate_trade_density.py:10330` / `scripts\tune_candidate_gate_trade_density.py:10332` | SUPPORT |  |
| `stable_base_config_hash` | 18 | `scripts\tune_candidate_gate_trade_density.py:10335` / `scripts\tune_candidate_gate_trade_density.py:10352` | REQUIRED |  |
| `file_cache_signature` | 14 | `scripts\tune_candidate_gate_trade_density.py:10355` / `scripts\tune_candidate_gate_trade_density.py:10368` | SUPPORT |  |
| `load_eval_cache` | 10 | `scripts\tune_candidate_gate_trade_density.py:10371` / `scripts\tune_candidate_gate_trade_density.py:10380` | REQUIRED |  |
| `save_eval_cache` | 3 | `scripts\tune_candidate_gate_trade_density.py:10383` / `scripts\tune_candidate_gate_trade_density.py:10385` | REQUIRED |  |
| `compute_validation_coupled_two_head_override_cap` | 37 | `scripts\tune_candidate_gate_trade_density.py:10388` / `scripts\tune_candidate_gate_trade_density.py:10424` | CONDITIONAL_POLICY |  |
| `compute_objective_feasibility_deadlock_penalty_scale` | 70 | `scripts\tune_candidate_gate_trade_density.py:10427` / `scripts\tune_candidate_gate_trade_density.py:10496` | CONDITIONAL_POLICY |  |
| `compute_combo_objective` | 617 | `scripts\tune_candidate_gate_trade_density.py:10499` / `scripts\tune_candidate_gate_trade_density.py:11115` | REQUIRED |  |
| `parse_metric_field_csv` | 12 | `scripts\tune_candidate_gate_trade_density.py:11118` / `scripts\tune_candidate_gate_trade_density.py:11129` | REQUIRED |  |
| `row_metric_as_float` | 5 | `scripts\tune_candidate_gate_trade_density.py:11132` / `scripts\tune_candidate_gate_trade_density.py:11136` | REQUIRED |  |
| `row_metric_as_optional_float` | 12 | `scripts\tune_candidate_gate_trade_density.py:11139` / `scripts\tune_candidate_gate_trade_density.py:11150` | REQUIRED |  |
| `dominates_row` | 23 | `scripts\tune_candidate_gate_trade_density.py:11153` / `scripts\tune_candidate_gate_trade_density.py:11175` | SUPPORT |  |
| `compute_pareto_front_rows` | 21 | `scripts\tune_candidate_gate_trade_density.py:11178` / `scripts\tune_candidate_gate_trade_density.py:11198` | CONDITIONAL_POLICY |  |
| `row_metric_as_bool` | 7 | `scripts\tune_candidate_gate_trade_density.py:11201` / `scripts\tune_candidate_gate_trade_density.py:11207` | REQUIRED |  |
| `is_baseline_combo_id` | 9 | `scripts\tune_candidate_gate_trade_density.py:11210` / `scripts\tune_candidate_gate_trade_density.py:11218` | SUPPORT |  |
| `select_baseline_row` | 36 | `scripts\tune_candidate_gate_trade_density.py:11221` / `scripts\tune_candidate_gate_trade_density.py:11256` | CONDITIONAL_POLICY |  |
| `extract_row_comparison_metrics` | 13 | `scripts\tune_candidate_gate_trade_density.py:11259` / `scripts\tune_candidate_gate_trade_density.py:11271` | SUPPORT |  |
| `build_candidate_vs_baseline_snapshot` | 100 | `scripts\tune_candidate_gate_trade_density.py:11274` / `scripts\tune_candidate_gate_trade_density.py:11373` | REQUIRED |  |
| `build_candidate_vs_baseline_readiness` | 105 | `scripts\tune_candidate_gate_trade_density.py:11376` / `scripts\tune_candidate_gate_trade_density.py:11480` | REQUIRED |  |
| `apply_baseline_regression_guard_to_rows` | 98 | `scripts\tune_candidate_gate_trade_density.py:11483` / `scripts\tune_candidate_gate_trade_density.py:11580` | CONDITIONAL_POLICY |  |
| `apply_selector_baseline_readiness_veto_to_rows` | 709 | `scripts\tune_candidate_gate_trade_density.py:11583` / `scripts\tune_candidate_gate_trade_density.py:12291` | CONDITIONAL_POLICY |  |
| `apply_split_gap_screen_occupancy_boost_to_rows` | 222 | `scripts\tune_candidate_gate_trade_density.py:12294` / `scripts\tune_candidate_gate_trade_density.py:12515` | CONDITIONAL_POLICY |  |
| `compute_generalization_wf_bridge_objective_bonus` | 115 | `scripts\tune_candidate_gate_trade_density.py:12518` / `scripts\tune_candidate_gate_trade_density.py:12632` | CONDITIONAL_POLICY |  |
| `compute_manager_ev_edge_joint_objective_bonus` | 154 | `scripts\tune_candidate_gate_trade_density.py:12635` / `scripts\tune_candidate_gate_trade_density.py:12788` | CONDITIONAL_POLICY | inactive_in_stage2306 |
| `compute_rr_adaptive_history_mixed_oos_objective_bonus` | 366 | `scripts\tune_candidate_gate_trade_density.py:12791` / `scripts\tune_candidate_gate_trade_density.py:13156` | CONDITIONAL_POLICY |  |
| `compute_recent_performance_objective_penalty` | 224 | `scripts\tune_candidate_gate_trade_density.py:13159` / `scripts\tune_candidate_gate_trade_density.py:13382` | CONDITIONAL_POLICY |  |
| `compute_consistency_selector_score` | 52 | `scripts\tune_candidate_gate_trade_density.py:13385` / `scripts\tune_candidate_gate_trade_density.py:13436` | CONDITIONAL_POLICY |  |
| `select_row_with_objective_deadlock_escape` | 186 | `scripts\tune_candidate_gate_trade_density.py:13439` / `scripts\tune_candidate_gate_trade_density.py:13624` | CONDITIONAL_POLICY |  |
| `select_best_row_with_selector` | 313 | `scripts\tune_candidate_gate_trade_density.py:13627` / `scripts\tune_candidate_gate_trade_density.py:13939` | REQUIRED |  |
| `get_effective_objective_thresholds` | 24 | `scripts\tune_candidate_gate_trade_density.py:13942` / `scripts\tune_candidate_gate_trade_density.py:13965` | REQUIRED |  |
| `evaluate_combo` | 517 | `scripts\tune_candidate_gate_trade_density.py:13968` / `scripts\tune_candidate_gate_trade_density.py:14484` | REQUIRED |  |
| `main` | 9013 | `scripts\tune_candidate_gate_trade_density.py:14487` / `scripts\tune_candidate_gate_trade_density.py:23499` | REQUIRED |  |

## Necessity Verdict
- Immediate hard-delete candidates: none (no dead top-level function found).
- High-value split targets (too large and policy-coupled):
  - `main` (`scripts\tune_candidate_gate_trade_density.py:14487`)
  - `adapt_combo_specs_for_bottleneck` (`scripts\tune_candidate_gate_trade_density.py:5739`)
  - `compute_combo_objective` (`scripts\tune_candidate_gate_trade_density.py:10499`)
- Conditional families can be moved behind explicit modules/feature toggles without affecting core matrix execution path:
  - `expand_*` families (total 3,989 lines)
  - selector/guard policy helpers (`apply_*` + `compute_*` subset)
- Latest smoke inactive marker observed: manager-ev-edge-joint family (kept but candidate for temporary freeze).
