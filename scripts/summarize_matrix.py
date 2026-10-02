"""外部比較JSONLを集計し、失敗を残したまま公開用JSONにまとめる。"""
import argparse
import json
from pathlib import Path
import statistics


def summarize(path):
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    metadata = next(r for r in rows if r.get("kind") == "metadata")
    runs = [r for r in rows if r.get("kind") == "run"]
    complete = bool(rows and rows[-1].get("kind") == "summary")
    cases = metadata.get("cases_requested", sorted({r["case"] for r in runs}))
    modes = metadata.get("modes_requested", sorted({r["mode"] for r in runs}))
    variants = metadata.get("variants_requested", sorted({r["variant"] for r in runs}))
    expected_keys = {(case, mode, variant) for case in cases for mode in modes for variant in variants}
    actual_keys = {(r["case"], r["mode"], r["variant"]) for r in runs}
    missing_groups = sorted(expected_keys - actual_keys)
    unexpected_groups = sorted(actual_keys - expected_keys)
    groups = []
    for key in sorted({(r["case"], r["mode"], r["variant"]) for r in runs}):
        matching = [r for r in runs if (r["case"], r["mode"], r["variant"]) == key]
        good = [r for r in matching if r.get("verified")]
        item = {"case": key[0], "mode": key[1], "variant": key[2], "runs": len(matching),
                "failures": len(matching) - len(good),
                "wall_seconds": [r["wall_seconds"] for r in matching],
                "sha256_verified_files": [r.get("sha256_verified_files", 0) for r in matching],
                "peak_rss_kib": [r.get("peak_rss_kib") for r in matching],
                "selected_backends": [r.get("stats", {}).get("selected_backend") for r in matching],
                "selection_reasons": [r.get("stats", {}).get("selection_reason") for r in matching]}
        if "repeats_requested" in metadata:
            item["missing_runs"] = max(0, metadata["repeats_requested"] - len(matching))
            item["repeat_sequence_valid"] = sorted(r.get("repeat") for r in matching) == list(
                range(1, metadata["repeats_requested"] + 1))
        if not item["failures"]:
            item.update(median_seconds=statistics.median(item["wall_seconds"]),
                        min_seconds=min(item["wall_seconds"]), max_seconds=max(item["wall_seconds"]))
        groups.append(item)
    comparisons = []
    for case, mode in sorted({(r["case"], r["mode"]) for r in runs}):
        matching = [g for g in groups if (g["case"], g["mode"]) == (case, mode)]
        failed = any(g["failures"] or g.get("missing_runs", 0)
                     or not g.get("repeat_sequence_valid", True) for g in matching)
        eligible = (complete and not missing_groups and not unexpected_groups and not failed
                    and len({g["runs"] for g in matching}) == 1)
        value = {"case": case, "mode": mode, "complete_success": eligible,
                 "tools": {g["variant"]: g.get("median_seconds") for g in matching}}
        if eligible and "auto" in value["tools"]:
            base = value["tools"]["auto"]
            value["ratio_to_auto"] = {variant: seconds / base for variant, seconds in value["tools"].items()}
            value["fastest_measured"] = min(value["tools"], key=value["tools"].get)
        comparisons.append(value)
    return {"metadata": metadata, "runs": len(runs), "failures": sum(not r.get("verified") for r in runs),
            "complete": complete,
            "missing_groups": missing_groups, "unexpected_groups": unexpected_groups,
            "groups": groups, "comparisons": comparisons}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, nargs="+", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("既存集計を上書きしません")
    reports = [summarize(path) for path in args.input]
    result = {"note": "倍率は同じ報告・case・mode内の中央値同士だけ。環境／cache／永続化条件を跨がない。",
              "total_runs": sum(r["runs"] for r in reports), "total_failures": sum(r["failures"] for r in reports),
              "all_reports_complete": all(r["complete"] for r in reports), "reports": reports}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps({k: v for k, v in result.items() if k != "reports"}, ensure_ascii=False))


if __name__ == "__main__":
    main()
