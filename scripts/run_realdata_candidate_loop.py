#!/usr/bin/env python3
import argparse
import json
import pathlib
import subprocess
import sys
from typing import Any, Dict, List

from _script_common import verification_lock


def ensure_directory(path_value: pathlib.Path) -> None:
    path_value.mkdir(parents=True, exist_ok=True)


def parse_reason_counts_json(value: Any) -> Dict[str, int]:
    payload: Any = {}
    if isinstance(value, dict):
        payload = value
    elif isinstance(value, str):
        text = value.strip()
        if text:
            try:
                payload = json.loads(text)
            except json.JSONDecodeError:
                payload = {}
    if not isinstance(payload, dict):
        return {}
    out: Dict[str, int] = {}
    for key, raw in payload.items():
        name = str(key)
        if not name:
            continue
        try:
            out[name] = int(raw or 0)
        except (TypeError, ValueError):
            continue
    return out


def build_realdata_file_path(output_dir: pathlib.Path, market: str, tf_unit: str, row_count: int) -> pathlib.Path:
    safe_market = market.replace("-", "_")
    return output_dir / f"upbit_{safe_market}_{tf_unit}m_{row_count}.csv"


def has_higher_tf_companions(primary_path: pathlib.Path) -> bool:
    if not primary_path.exists():
        return False
    stem = primary_path.stem.lower()
    if not stem.startswith("upbit_") or "_1m_" not in stem:
        return False
    pivot = stem.index("_1m_")
    if pivot <= 6:
        return False
    market_token = stem[6:pivot]
    for tf in ("5m", "60m", "240m"):
        if not list(primary_path.parent.glob(f"upbit_{market_token}_{tf}_*.csv")):
            return False
    return True


def get_dataset_files(dirs: List[pathlib.Path], only_real_data: bool, require_higher_tf: bool) -> List[pathlib.Path]:
    items: List[pathlib.Path] = []
    for d in dirs:
        if not d.exists():
            continue
        is_real = "backtest_real" in str(d).lower()
        if only_real_data and not is_real:
            continue
        for f in sorted(d.glob("*.csv"), key=lambda x: x.name.lower()):
            if is_real:
                if "_1m_" not in f.name.lower():
                    continue
                if require_higher_tf and not has_higher_tf_companions(f):
                    continue
            elif only_real_data:
                continue
            items.append(f.resolve())
    uniq = sorted(set(items), key=lambda x: str(x).lower())
    return uniq


def resolve_explicit_dataset_files(
    dataset_names: List[str],
    default_dirs: List[pathlib.Path],
    require_higher_tf: bool,
    only_real_data: bool,
) -> List[pathlib.Path]:
    resolved: List[pathlib.Path] = []
    search_dirs = [pathlib.Path.cwd().resolve(), *default_dirs]
    for raw in dataset_names:
        token = str(raw).strip()
        if not token:
            continue
        cand = pathlib.Path(token)
        found = None
        if cand.is_absolute() and cand.exists():
            found = cand.resolve()
        else:
            for base in search_dirs:
                probe = (base / cand).resolve()
                if probe.exists():
                    found = probe
                    break
        if found is None:
            raise FileNotFoundError(f"Dataset not found: {token}")
        is_real = "backtest_real" in str(found.parent).lower() or "_1m_" in found.name.lower()
        if only_real_data and not is_real:
            continue
        if is_real and "_1m_" not in found.name.lower():
            continue
        if require_higher_tf and is_real and not has_higher_tf_companions(found):
            raise RuntimeError(f"Missing higher TF companions for dataset: {found}")
        resolved.append(found)
    uniq = sorted(set(resolved), key=lambda x: str(x).lower())
    return uniq


def run_fetch_script(
    fetch_script: pathlib.Path,
    market: str,
    unit: str,
    candles: int,
    chunk_size: int,
    sleep_ms: int,
    output_path: pathlib.Path,
) -> None:
    cmd = [
        sys.executable,
        str(fetch_script),
        "-Market",
        market,
        "-Unit",
        unit,
        "-Candles",
        str(candles),
        "-ChunkSize",
        str(chunk_size),
        "-SleepMs",
        str(sleep_ms),
        "-OutputPath",
        str(output_path),
    ]
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"fetch script failed: script={fetch_script}, market={market}, unit={unit}")


def derive_variant_path(base_path: pathlib.Path, variant: str) -> pathlib.Path:
    return base_path.with_name(f"{base_path.stem}_{variant}{base_path.suffix}")


def remove_deprecated_variant_artifacts(*base_paths: pathlib.Path) -> None:
    for base_path in base_paths:
        for variant in ("strict", "adaptive"):
            stale = derive_variant_path(base_path, variant)
            if stale.exists():
                stale.unlink(missing_ok=True)


def run_profitability_matrix(
    matrix_script: pathlib.Path,
    datasets: List[pathlib.Path],
    gate_min_avg_trades: int,
    output_matrix_csv: pathlib.Path,
    output_profile_csv: pathlib.Path,
    output_report_json: pathlib.Path,
    matrix_max_workers: int,
    matrix_backtest_retry_count: int,
    enable_hostility_adaptive_thresholds: bool,
    enable_hostility_adaptive_trades_only: bool,
    require_higher_tf_companions: bool,
    enable_adaptive_state_io: bool,
    skip_core_vs_legacy_gate: bool,
) -> Dict[str, Any]:
    ensure_directory(output_matrix_csv.parent)
    ensure_directory(output_profile_csv.parent)
    ensure_directory(output_report_json.parent)

    matrix_cmd = [
        sys.executable,
        str(matrix_script),
        "--dataset-names",
        *[str(x) for x in datasets],
        "--exclude-low-trade-runs-for-gate",
        "--min-trades-per-run-for-gate",
        "1",
        "--min-avg-trades",
        str(int(gate_min_avg_trades)),
        "--output-csv",
        str(output_matrix_csv),
        "--output-profile-csv",
        str(output_profile_csv),
        "--output-json",
        str(output_report_json),
        "--max-workers",
        str(max(1, int(matrix_max_workers))),
        "--backtest-retry-count",
        str(max(1, int(matrix_backtest_retry_count))),
    ]
    if enable_hostility_adaptive_thresholds:
        matrix_cmd.append("--enable-hostility-adaptive-thresholds")
    if enable_hostility_adaptive_trades_only:
        matrix_cmd.append("--enable-hostility-adaptive-trades-only")
    if require_higher_tf_companions:
        matrix_cmd.append("--require-higher-tf-companions")
    if enable_adaptive_state_io:
        matrix_cmd.append("--enable-adaptive-state-io")
    if skip_core_vs_legacy_gate:
        matrix_cmd.append("--skip-core-vs-legacy-gate")

    proc = subprocess.run(matrix_cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"run_profitability_matrix.py failed (exit={proc.returncode})")
    return json.loads(output_report_json.read_text(encoding="utf-8-sig"))


def run_parity_invariant_report(
    parity_script: pathlib.Path,
    datasets: List[pathlib.Path],
    output_json: pathlib.Path,
    fail_on_invariant: bool,
) -> Dict[str, Any]:
    ensure_directory(output_json.parent)
    cmd = [
        sys.executable,
        str(parity_script),
        "--dataset-names",
        *[str(x) for x in datasets],
        "--output-json",
        str(output_json),
    ]
    if fail_on_invariant:
        cmd.append("--fail-on-invariant")
    proc = subprocess.run(cmd)
    if proc.returncode != 0:
        raise RuntimeError(f"generate_parity_invariant_report.py failed (exit={proc.returncode})")
    return json.loads(output_json.read_text(encoding="utf-8-sig"))


def print_report_snapshot(prefix: str, report: Dict[str, Any]) -> None:
    core_full = next((x for x in report.get("profile_summaries", []) if x.get("profile_id") == "core_full"), None)
    if core_full is not None:
        print(f"[RealDataLoop] {prefix}.core_full.avg_profit_factor={core_full.get('avg_profit_factor')}")
        print(f"[RealDataLoop] {prefix}.core_full.avg_total_trades={core_full.get('avg_total_trades')}")
        print(f"[RealDataLoop] {prefix}.core_full.avg_expectancy_krw={core_full.get('avg_expectancy_krw')}")
        print(
            f"[RealDataLoop] {prefix}.core_full.entry_rejection_top="
            f"{core_full.get('top_entry_rejection_reason')}:"
            f"{core_full.get('top_entry_rejection_count')}"
        )
        print(
            f"[RealDataLoop] {prefix}.core_full.entry_rejection_total="
            f"{core_full.get('entry_rejection_total')}"
        )
        top_risk_reason = str(core_full.get("top_entry_risk_gate_component_reason") or "")
        top_risk_count = int(core_full.get("top_entry_risk_gate_component_count") or 0)
        if not top_risk_reason:
            risk_breakdown = parse_reason_counts_json(core_full.get("entry_risk_gate_breakdown_json"))
            risk_components = {
                k: int(v)
                for k, v in risk_breakdown.items()
                if str(k)
                not in {
                    "blocked_risk_gate_total",
                    "blocked_risk_gate_entry_quality",
                    "blocked_risk_gate_entry_quality_rr",
                    "blocked_risk_gate_entry_quality_rr_adaptive",
                    "blocked_risk_gate_entry_quality_rr_edge",
                    "blocked_risk_gate_entry_quality_rr_edge_adaptive",
                }
            }
            if risk_components:
                top_risk_reason, top_risk_count = max(
                    risk_components.items(),
                    key=lambda kv: (kv[1], kv[0]),
                )
        if top_risk_reason:
            print(
                f"[RealDataLoop] {prefix}.core_full.risk_gate_component_top="
                f"{top_risk_reason}:{top_risk_count}"
            )

    threshold_info = report.get("thresholds", {})
    hostility_bundle = threshold_info.get("hostility_adaptive", {})
    effective = hostility_bundle.get("effective", {})
    if effective:
        print(
            f"[RealDataLoop] {prefix}.effective_thresholds="
            f"min_pf:{effective.get('min_profit_factor')}, "
            f"min_exp:{effective.get('min_expectancy_krw')}, "
            f"min_ratio:{effective.get('min_profitable_ratio')}, "
            f"min_win:{effective.get('min_avg_win_rate_pct')}, "
            f"min_trades:{effective.get('min_avg_trades')}"
        )
        hostility = hostility_bundle.get("hostility", {})
        quality = hostility_bundle.get("quality", {})
        blended = hostility_bundle.get("blended_context", {})
        print(
            f"[RealDataLoop] {prefix}.hostility="
            f"{blended.get('blended_hostility_level', hostility.get('hostility_level'))} "
            f"(score={blended.get('blended_adversarial_score', hostility.get('avg_adversarial_score'))}, "
            f"quality={quality.get('quality_level', 'unknown')}, "
            f"q_score={quality.get('avg_quality_risk_score', 0.0)})"
        )
    core_vs_legacy = report.get("core_vs_legacy") or {}
    if core_vs_legacy:
        print(
            f"[RealDataLoop] {prefix}.core_vs_legacy="
            f"gate_pass={core_vs_legacy.get('gate_pass')}, "
            f"skipped={core_vs_legacy.get('gate_skipped', False)}"
        )
    print(f"[RealDataLoop] {prefix}.overall_gate_pass={report.get('overall_gate_pass')}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fetch-script", "-FetchScript", default=r".\scripts\fetch_upbit_historical_candles.py")
    parser.add_argument("--matrix-script", "-MatrixScript", default=r".\scripts\run_profitability_matrix.py")
    parser.add_argument("--tune-script", "-TuneScript", default=r".\scripts\tune_candidate_gate_trade_density.py")
    parser.add_argument(
        "--entry-rejection-script",
        "-EntryRejectionScript",
        default=r".\scripts\analyze_entry_rejections.py",
    )
    parser.add_argument(
        "--parity-invariant-script",
        "-ParityInvariantScript",
        default=r".\scripts\generate_parity_invariant_report.py",
    )
    parser.add_argument(
        "--strategy-rejection-taxonomy-script",
        "-StrategyRejectionTaxonomyScript",
        default=r".\scripts\generate_strategy_rejection_taxonomy_report.py",
    )
    parser.add_argument(
        "--loss-contributor-script",
        "-LossContributorScript",
        default=r".\scripts\analyze_loss_contributors.py",
    )
    parser.add_argument("--real-data-dir", "-RealDataDir", default=r".\data\backtest_real")
    parser.add_argument("--backtest-data-dir", "-BacktestDataDir", default=r".\data\backtest")
    parser.add_argument("--curated-data-dir", "-CuratedDataDir", default=r".\data\backtest_curated")
    parser.add_argument(
        "--markets",
        "-Markets",
        nargs="*",
        default=[
            "KRW-BTC", "KRW-ETH", "KRW-XRP", "KRW-SOL", "KRW-DOGE", "KRW-ADA",
            "KRW-AVAX", "KRW-LINK", "KRW-DOT", "KRW-TRX", "KRW-SUI", "KRW-BCH",
        ],
    )
    parser.add_argument("--unit", "-Unit", default="1")
    parser.add_argument("--candles", "-Candles", type=int, default=12000)
    parser.add_argument("--candles5m", "-Candles5m", type=int, default=4000)
    parser.add_argument("--candles1h", "-Candles1h", type=int, default=1200)
    parser.add_argument("--candles4h", "-Candles4h", type=int, default=600)
    parser.add_argument("--chunk-size", "-ChunkSize", type=int, default=200)
    parser.add_argument("--sleep-ms", "-SleepMs", type=int, default=120)
    parser.add_argument("--output-matrix-csv", "-OutputMatrixCsv", default=r".\build\Release\logs\profitability_matrix_realdata.csv")
    parser.add_argument("--output-profile-csv", "-OutputProfileCsv", default=r".\build\Release\logs\profitability_profile_summary_realdata.csv")
    parser.add_argument("--output-report-json", "-OutputReportJson", default=r".\build\Release\logs\profitability_gate_report_realdata.json")
    parser.add_argument(
        "--entry-rejection-output-json",
        "-EntryRejectionOutputJson",
        default=r".\build\Release\logs\entry_rejection_summary_realdata.json",
    )
    parser.add_argument(
        "--entry-rejection-profile-csv",
        "-EntryRejectionProfileCsv",
        default=r".\build\Release\logs\entry_rejection_by_profile_realdata.csv",
    )
    parser.add_argument(
        "--entry-rejection-dataset-csv",
        "-EntryRejectionDatasetCsv",
        default=r".\build\Release\logs\entry_rejection_by_dataset_realdata.csv",
    )
    parser.add_argument(
        "--strategy-rejection-taxonomy-output-json",
        "-StrategyRejectionTaxonomyOutputJson",
        default=r".\build\Release\logs\strategy_rejection_taxonomy_report.json",
    )
    parser.add_argument(
        "--live-signal-funnel-taxonomy-json",
        "-LiveSignalFunnelTaxonomyJson",
        default=r".\build\Release\logs\live_signal_funnel_taxonomy_report.json",
    )
    parser.add_argument(
        "--parity-invariant-output-json",
        "-ParityInvariantOutputJson",
        default=r".\build\Release\logs\parity_invariant_report.json",
    )
    parser.add_argument(
        "--loss-contributor-market-csv",
        "-LossContributorMarketCsv",
        default=r".\build\Release\logs\loss_contributor_by_market.csv",
    )
    parser.add_argument(
        "--loss-contributor-strategy-csv",
        "-LossContributorStrategyCsv",
        default=r".\build\Release\logs\loss_contributor_by_strategy.csv",
    )
    parser.add_argument("--loss-contributor-max-workers", "-LossContributorMaxWorkers", type=int, default=1)
    parser.add_argument("--gate-min-avg-trades", "-GateMinAvgTrades", type=int, default=8)
    parser.add_argument("--matrix-max-workers", "-MatrixMaxWorkers", type=int, default=1)
    parser.add_argument("--matrix-backtest-retry-count", "-MatrixBacktestRetryCount", type=int, default=2)
    parser.add_argument(
        "--enable-hostility-adaptive-thresholds",
        "-EnableHostilityAdaptiveThresholds",
        dest="enable_hostility_adaptive_thresholds",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-thresholds",
        dest="enable_hostility_adaptive_thresholds",
        action="store_false",
    )
    parser.add_argument(
        "--enable-hostility-adaptive-trades-only",
        "-EnableHostilityAdaptiveTradesOnly",
        dest="enable_hostility_adaptive_trades_only",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--disable-hostility-adaptive-trades-only",
        dest="enable_hostility_adaptive_trades_only",
        action="store_false",
    )
    parser.add_argument("--skip-fetch", "-SkipFetch", action="store_true")
    parser.add_argument("--skip-higher-tf-fetch", "-SkipHigherTfFetch", action="store_true")
    parser.add_argument("--skip-tune", "-SkipTune", action="store_true")
    parser.add_argument("--skip-entry-rejection-analysis", "-SkipEntryRejectionAnalysis", action="store_true")
    parser.add_argument("--skip-strategy-rejection-taxonomy", "-SkipStrategyRejectionTaxonomy", action="store_true")
    parser.add_argument("--skip-loss-contributor-analysis", "-SkipLossContributorAnalysis", action="store_true")
    parser.add_argument("--skip-parity-invariant", "-SkipParityInvariant", action="store_true")
    parser.add_argument("--fail-on-parity-invariant", "-FailOnParityInvariant", action="store_true")
    parser.add_argument("--real-data-only", "-RealDataOnly", action="store_true")
    parser.add_argument(
        "--dataset-names",
        "-DatasetNames",
        nargs="*",
        default=[],
        help="Explicit primary datasets (.csv). When provided, auto-discovery is bypassed.",
    )
    parser.add_argument(
        "--require-higher-tf-companions",
        "-RequireHigherTfCompanions",
        dest="require_higher_tf_companions",
        action="store_true",
        default=True,
    )
    parser.add_argument(
        "--allow-missing-higher-tf-companions",
        dest="require_higher_tf_companions",
        action="store_false",
    )
    parser.add_argument(
        "--run-both-hostility-modes",
        "-RunBothHostilityModes",
        action="store_true",
        help="Deprecated compatibility flag. Strict/adaptive split is removed; a single hostility-driven gate run is used.",
    )
    parser.add_argument(
        "--skip-core-vs-legacy-gate",
        "-SkipCoreVsLegacyGate",
        action="store_true",
        help="Skip legacy comparison gate and evaluate core profile gates only (migration mode).",
    )
    parser.add_argument(
        "--enable-adaptive-state-io",
        "-EnableAdaptiveStateIo",
        action="store_true",
        help="Enable adaptive strategy state I/O during matrix backtests.",
    )
    parser.add_argument("--verification-lock-path", "-VerificationLockPath", default=r".\build\Release\logs\verification_run.lock")
    parser.add_argument("--verification-lock-timeout-sec", "-VerificationLockTimeoutSec", type=int, default=1800)
    parser.add_argument("--verification-lock-stale-sec", "-VerificationLockStaleSec", type=int, default=14400)
    args = parser.parse_args(argv)

    fetch_script = pathlib.Path(args.fetch_script).resolve()
    matrix_script = pathlib.Path(args.matrix_script).resolve()
    tune_script = pathlib.Path(args.tune_script).resolve()
    entry_rejection_script = pathlib.Path(args.entry_rejection_script).resolve()
    parity_invariant_script = pathlib.Path(args.parity_invariant_script).resolve()
    strategy_rejection_taxonomy_script = pathlib.Path(args.strategy_rejection_taxonomy_script).resolve()
    loss_contributor_script = pathlib.Path(args.loss_contributor_script).resolve()
    real_data_dir = pathlib.Path(args.real_data_dir).resolve()
    backtest_data_dir = pathlib.Path(args.backtest_data_dir).resolve()
    curated_data_dir = pathlib.Path(args.curated_data_dir).resolve()
    output_matrix_csv = pathlib.Path(args.output_matrix_csv).resolve()
    output_profile_csv = pathlib.Path(args.output_profile_csv).resolve()
    output_report_json = pathlib.Path(args.output_report_json).resolve()
    entry_rejection_output_json = pathlib.Path(args.entry_rejection_output_json).resolve()
    entry_rejection_profile_csv = pathlib.Path(args.entry_rejection_profile_csv).resolve()
    entry_rejection_dataset_csv = pathlib.Path(args.entry_rejection_dataset_csv).resolve()
    strategy_rejection_taxonomy_output_json = pathlib.Path(args.strategy_rejection_taxonomy_output_json).resolve()
    live_signal_funnel_taxonomy_json = pathlib.Path(args.live_signal_funnel_taxonomy_json).resolve()
    parity_invariant_output_json = pathlib.Path(args.parity_invariant_output_json).resolve()
    loss_contributor_market_csv = pathlib.Path(args.loss_contributor_market_csv).resolve()
    loss_contributor_strategy_csv = pathlib.Path(args.loss_contributor_strategy_csv).resolve()

    ensure_directory(real_data_dir)

    if not args.skip_fetch:
        for market in args.markets:
            if not market.strip():
                continue
            target_1m = build_realdata_file_path(real_data_dir, market, args.unit, args.candles)
            print(f"[RealDataLoop] Fetching market={market}, unit={args.unit}m, candles={args.candles}")
            run_fetch_script(fetch_script, market, args.unit, args.candles, args.chunk_size, args.sleep_ms, target_1m)

            if not args.skip_higher_tf_fetch:
                target_5m = build_realdata_file_path(real_data_dir, market, "5", args.candles5m)
                print(f"[RealDataLoop] Fetching market={market}, unit=5m, candles={args.candles5m}")
                run_fetch_script(fetch_script, market, "5", args.candles5m, args.chunk_size, args.sleep_ms, target_5m)

                target_1h = build_realdata_file_path(real_data_dir, market, "60", args.candles1h)
                print(f"[RealDataLoop] Fetching market={market}, unit=60m, candles={args.candles1h}")
                run_fetch_script(fetch_script, market, "60", args.candles1h, args.chunk_size, args.sleep_ms, target_1h)

                target_4h = build_realdata_file_path(real_data_dir, market, "240", args.candles4h)
                print(f"[RealDataLoop] Fetching market={market}, unit=240m, candles={args.candles4h}")
                run_fetch_script(fetch_script, market, "240", args.candles4h, args.chunk_size, args.sleep_ms, target_4h)

    dataset_dirs = [real_data_dir] if args.real_data_only else [backtest_data_dir, curated_data_dir, real_data_dir]
    if args.dataset_names:
        datasets = resolve_explicit_dataset_files(
            dataset_names=list(args.dataset_names),
            default_dirs=dataset_dirs,
            require_higher_tf=bool(args.require_higher_tf_companions),
            only_real_data=bool(args.real_data_only),
        )
    else:
        datasets = get_dataset_files(dataset_dirs, args.real_data_only, args.require_higher_tf_companions)
    if not datasets:
        raise RuntimeError("No datasets found after fetch step.")

    print(
        f"[RealDataLoop] dataset_mode={'realdata_only' if args.real_data_only else 'mixed'}, "
        f"require_higher_tf={bool(args.require_higher_tf_companions)}, dataset_count={len(datasets)}"
    )

    def run_verification_pipeline() -> None:
        if not args.skip_parity_invariant:
            print(f"[RealDataLoop] Running parity invariant report with datasets={len(datasets)}")
            parity_report = run_parity_invariant_report(
                parity_script=parity_invariant_script,
                datasets=datasets,
                output_json=parity_invariant_output_json,
                fail_on_invariant=bool(args.fail_on_parity_invariant),
            )
            summary = parity_report.get("summary") or {}
            print(
                f"[RealDataLoop] parity_invariant="
                f"overall_pass={summary.get('overall_invariant_pass')}, "
                f"fail_count={summary.get('invariant_fail_count')}"
            )

        print(f"[RealDataLoop] Running profitability matrix with datasets={len(datasets)}")
        if args.run_both_hostility_modes:
            print(
                "[RealDataLoop] run_both_hostility_modes is deprecated. "
                "Using a single hostility-driven gate run."
            )

        remove_deprecated_variant_artifacts(output_matrix_csv, output_profile_csv, output_report_json)

        selected_adaptive = bool(args.enable_hostility_adaptive_thresholds)
        selected_trades_only = (not selected_adaptive) and bool(args.enable_hostility_adaptive_trades_only)
        report = run_profitability_matrix(
            matrix_script=matrix_script,
            datasets=datasets,
            gate_min_avg_trades=args.gate_min_avg_trades,
            output_matrix_csv=output_matrix_csv,
            output_profile_csv=output_profile_csv,
            output_report_json=output_report_json,
            matrix_max_workers=args.matrix_max_workers,
            matrix_backtest_retry_count=args.matrix_backtest_retry_count,
            enable_hostility_adaptive_thresholds=bool(args.enable_hostility_adaptive_thresholds),
            enable_hostility_adaptive_trades_only=selected_trades_only,
            require_higher_tf_companions=bool(args.require_higher_tf_companions),
            enable_adaptive_state_io=bool(args.enable_adaptive_state_io),
            skip_core_vs_legacy_gate=bool(args.skip_core_vs_legacy_gate),
        )
        print_report_snapshot("selected", report)

        if not args.skip_entry_rejection_analysis:
            print("[RealDataLoop] Analyzing entry rejection reasons from matrix report")
            rejection_cmd = [
                sys.executable,
                str(entry_rejection_script),
                "--report-json",
                str(output_report_json),
                "--output-json",
                str(entry_rejection_output_json),
                "--output-profile-csv",
                str(entry_rejection_profile_csv),
                "--output-dataset-csv",
                str(entry_rejection_dataset_csv),
            ]
            rej_proc = subprocess.run(rejection_cmd)
            if rej_proc.returncode != 0:
                raise RuntimeError(f"analyze_entry_rejections.py failed (exit={rej_proc.returncode})")

        if not args.skip_strategy_rejection_taxonomy:
            if args.skip_entry_rejection_analysis and not entry_rejection_output_json.exists():
                raise RuntimeError(
                    "strategy rejection taxonomy requested but entry rejection summary is missing: "
                    f"{entry_rejection_output_json}"
                )
            taxonomy_cmd = [
                sys.executable,
                str(strategy_rejection_taxonomy_script),
                "--entry-rejection-summary-json",
                str(entry_rejection_output_json),
                "--output-json",
                str(strategy_rejection_taxonomy_output_json),
                "--live-signal-funnel-taxonomy-json",
                str(live_signal_funnel_taxonomy_json),
            ]
            tax_proc = subprocess.run(taxonomy_cmd)
            if tax_proc.returncode != 0:
                raise RuntimeError(
                    f"generate_strategy_rejection_taxonomy_report.py failed (exit={tax_proc.returncode})"
                )

        if not args.skip_loss_contributor_analysis:
            print("[RealDataLoop] Analyzing loss contributors by market/strategy")
            loss_cmd = [
                sys.executable,
                str(loss_contributor_script),
                "--gate-report-json",
                str(output_report_json),
                "--data-dirs",
                str(backtest_data_dir),
                str(curated_data_dir),
                str(real_data_dir),
                "--output-market-csv",
                str(loss_contributor_market_csv),
                "--output-strategy-csv",
                str(loss_contributor_strategy_csv),
                "--max-workers",
                str(max(1, int(args.loss_contributor_max_workers))),
            ]
            loss_proc = subprocess.run(loss_cmd)
            if loss_proc.returncode != 0:
                raise RuntimeError(f"analyze_loss_contributors.py failed (exit={loss_proc.returncode})")

        if not args.skip_tune:
            print("[RealDataLoop] Running candidate trade-density tuning with real datasets included")
            tune_cmd = [
                sys.executable,
                str(tune_script),
                "--data-dir",
                str(backtest_data_dir),
                "--curated-data-dir",
                str(curated_data_dir),
                "--extra-data-dirs",
                str(real_data_dir),
                "--matrix-max-workers",
                str(max(1, int(args.matrix_max_workers))),
                "--matrix-backtest-retry-count",
                str(max(1, int(args.matrix_backtest_retry_count))),
            ]
            if args.real_data_only:
                tune_cmd.append("--real-data-only")
            if args.dataset_names:
                tune_cmd.extend(["--dataset-names", *[str(x) for x in datasets]])
            if args.require_higher_tf_companions:
                tune_cmd.append("--require-higher-tf-companions")
            else:
                tune_cmd.append("--allow-missing-higher-tf-companions")
            if args.enable_hostility_adaptive_thresholds:
                tune_cmd.append("--enable-hostility-adaptive-thresholds")
            else:
                tune_cmd.append("--disable-hostility-adaptive-thresholds")
            if args.enable_hostility_adaptive_trades_only:
                tune_cmd.append("--enable-hostility-adaptive-trades-only")
            else:
                tune_cmd.append("--disable-hostility-adaptive-trades-only")
            if args.skip_core_vs_legacy_gate:
                tune_cmd.append("--skip-core-vs-legacy-gate")
            tune_proc = subprocess.run(tune_cmd)
            if tune_proc.returncode != 0:
                raise RuntimeError(f"tune_candidate_gate_trade_density.py failed (exit={tune_proc.returncode})")

    lock_path = pathlib.Path(args.verification_lock_path).resolve()
    with verification_lock(
        lock_path,
        timeout_sec=int(args.verification_lock_timeout_sec),
        stale_sec=int(args.verification_lock_stale_sec),
    ):
        run_verification_pipeline()

    print("[RealDataLoop] Completed")
    print(f"gate_report={output_report_json}")
    if not args.skip_entry_rejection_analysis:
        print(f"entry_rejection_report={entry_rejection_output_json}")
    if not args.skip_strategy_rejection_taxonomy:
        print(f"strategy_rejection_taxonomy_report={strategy_rejection_taxonomy_output_json}")
    if not args.skip_loss_contributor_analysis:
        print(f"loss_contributor_market_csv={loss_contributor_market_csv}")
        print(f"loss_contributor_strategy_csv={loss_contributor_strategy_csv}")
    if not args.skip_parity_invariant:
        print(f"parity_invariant_report={parity_invariant_output_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
