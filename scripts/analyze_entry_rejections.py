#!/usr/bin/env python3
import argparse
import csv
import json
import pathlib
from typing import Any, Dict, List, Tuple

from _script_common import dump_json, ensure_parent_directory, resolve_repo_path


def parse_args(argv=None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--report-json",
        "-ReportJson",
        default=r".\build\Release\logs\profitability_gate_report_realdata.json",
    )
    parser.add_argument(
        "--output-json",
        "-OutputJson",
        default=r".\build\Release\logs\entry_rejection_summary_realdata.json",
    )
    parser.add_argument(
        "--output-profile-csv",
        "-OutputProfileCsv",
        default=r".\build\Release\logs\entry_rejection_by_profile_realdata.csv",
    )
    parser.add_argument(
        "--output-dataset-csv",
        "-OutputDatasetCsv",
        default=r".\build\Release\logs\entry_rejection_by_dataset_realdata.csv",
    )
    parser.add_argument("--top-k", "-TopK", type=int, default=8)
    return parser.parse_args(argv)


def parse_reason_counts(raw: Any) -> Dict[str, int]:
    payload: Dict[str, Any]
    if isinstance(raw, dict):
        payload = raw
    else:
        text = str(raw or "").strip()
        if not text:
            return {}
        try:
            payload = json.loads(text)
        except Exception:
            return {}

    out: Dict[str, int] = {}
    for k, v in payload.items():
        reason = str(k).strip()
        if not reason:
            continue
        try:
            count = int(v)
        except Exception:
            continue
        if count <= 0:
            continue
        out[reason] = out.get(reason, 0) + count
    return out


def sorted_items(counts: Dict[str, int]) -> List[Tuple[str, int]]:
    return sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))


def merge_counts(dst: Dict[str, int], src: Dict[str, int]) -> None:
    for reason, count in src.items():
        dst[reason] = dst.get(reason, 0) + int(count)


def write_profile_csv(path_value: pathlib.Path, rows: List[Dict[str, Any]]) -> None:
    ensure_parent_directory(path_value)
    fields = [
        "profile_id",
        "entry_rejection_total",
        "top_reason",
        "top_reason_count",
        "top_reasons_json",
    ]
    with path_value.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def write_dataset_csv(path_value: pathlib.Path, rows: List[Dict[str, Any]]) -> None:
    ensure_parent_directory(path_value)
    fields = [
        "profile_id",
        "dataset",
        "entry_rejection_total",
        "top_reason",
        "top_reason_count",
        "top_reasons_json",
    ]
    with path_value.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main(argv=None) -> int:
    args = parse_args(argv)
    report_json = resolve_repo_path(args.report_json)
    output_json = resolve_repo_path(args.output_json)
    output_profile_csv = resolve_repo_path(args.output_profile_csv)
    output_dataset_csv = resolve_repo_path(args.output_dataset_csv)
    top_k = max(1, int(args.top_k))

    if not report_json.exists():
        raise FileNotFoundError(f"report not found: {report_json}")

    report = json.loads(report_json.read_text(encoding="utf-8-sig"))
    profile_summaries = report.get("profile_summaries") or []
    matrix_rows = report.get("matrix_rows") or []

    profile_rows: List[Dict[str, Any]] = []
    profile_top: Dict[str, List[Dict[str, Any]]] = {}
    for profile in profile_summaries:
        profile_id = str(profile.get("profile_id", "")).strip()
        if not profile_id:
            continue
        counts = parse_reason_counts(profile.get("entry_rejection_reason_counts_json"))
        top = [{"reason": r, "count": c} for r, c in sorted_items(counts)[:top_k]]
        top_reason = top[0]["reason"] if top else ""
        top_reason_count = int(top[0]["count"]) if top else 0
        profile_rows.append(
            {
                "profile_id": profile_id,
                "entry_rejection_total": int(sum(counts.values())),
                "top_reason": top_reason,
                "top_reason_count": top_reason_count,
                "top_reasons_json": json.dumps(top, ensure_ascii=False, separators=(",", ":")),
            }
        )
        profile_top[profile_id] = top

    dataset_rows: List[Dict[str, Any]] = []
    overall_counts: Dict[str, int] = {}
    profile_overall_counts: Dict[str, Dict[str, int]] = {}
    for row in matrix_rows:
        profile_id = str(row.get("profile_id", "")).strip()
        dataset = str(row.get("dataset", "")).strip()
        if not profile_id or not dataset:
            continue
        counts = parse_reason_counts(row.get("entry_rejection_reason_counts_json"))
        merge_counts(overall_counts, counts)
        profile_bucket = profile_overall_counts.setdefault(profile_id, {})
        merge_counts(profile_bucket, counts)
        top = [{"reason": r, "count": c} for r, c in sorted_items(counts)[:top_k]]
        top_reason = top[0]["reason"] if top else ""
        top_reason_count = int(top[0]["count"]) if top else 0
        dataset_rows.append(
            {
                "profile_id": profile_id,
                "dataset": dataset,
                "entry_rejection_total": int(sum(counts.values())),
                "top_reason": top_reason,
                "top_reason_count": top_reason_count,
                "top_reasons_json": json.dumps(top, ensure_ascii=False, separators=(",", ":")),
            }
        )

    dataset_rows.sort(key=lambda x: (x["profile_id"], x["dataset"]))
    profile_rows.sort(key=lambda x: x["profile_id"])

    write_profile_csv(output_profile_csv, profile_rows)
    write_dataset_csv(output_dataset_csv, dataset_rows)

    payload = {
        "generated_at": __import__("datetime").datetime.now().astimezone().isoformat(),
        "report_json": str(report_json),
        "profile_summary_count": len(profile_rows),
        "dataset_row_count": len(dataset_rows),
        "top_k": top_k,
        "overall_top_reasons": [
            {"reason": reason, "count": count}
            for reason, count in sorted_items(overall_counts)[:top_k]
        ],
        "profile_top_reasons": profile_top,
        "profile_overall_reason_counts": {
            profile_id: dict(sorted_items(counts))
            for profile_id, counts in sorted(profile_overall_counts.items(), key=lambda kv: kv[0])
        },
        "outputs": {
            "profile_csv": str(output_profile_csv),
            "dataset_csv": str(output_dataset_csv),
        },
    }
    dump_json(output_json, payload)

    print("[AnalyzeEntryRejections] Completed")
    print(f"report_json={report_json}")
    print(f"profile_csv={output_profile_csv}")
    print(f"dataset_csv={output_dataset_csv}")
    print(f"summary_json={output_json}")
    if payload["overall_top_reasons"]:
        top = payload["overall_top_reasons"][0]
        print(f"overall_top_reason={top['reason']}:{top['count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

