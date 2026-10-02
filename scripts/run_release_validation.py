"""媒体・CPU数・データバッファ予算・50GB実データを逐次比較する。"""
import argparse
import json
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--ssd-output", type=Path, required=True)
    parser.add_argument("--optane-output", type=Path, required=True)
    parser.add_argument("--tmpfs-output", type=Path, required=True)
    parser.add_argument("--report-prefix", type=Path, required=True)
    parser.add_argument("--predecessor-report", type=Path, help="別の所有する逐次試験の完了を待ってから開始する")
    parser.add_argument("--parallel-reference-only", action="store_true",
                        help="追加の並列7-Zip参照とauto／GPUを同じ条件で比較する")
    args = parser.parse_args()
    matrix = str(Path(__file__).with_name("benchmark_matrix.py").resolve())
    phases = []
    common = ["--corpus", str(args.corpus), "--repeats", "3", "--variants", "auto", "gpu", "7zip", "python-parallel"]
    for count, cases in ((1, ["documents-level1", "incompressible", "stored-medium"]),
                         (4, ["documents-level1", "zeros-medium", "incompressible", "stored-medium", "skewed"])):
        phases.append((f"cpu{count}", [*common, "--cpu-count", str(count), "--threads", str(count),
                                      "--cases", *cases, "--output-root", str(args.ssd_output)]))
    phases.append(("limited-buffers", [*common, "--cases", "documents-level1", "zeros-medium", "stored-medium", "skewed",
                                       "--host-limit", "32M", "--vram-limit", "256M",
                                       "--output-root", str(args.ssd_output)]))
    for medium, output in (("optane", args.optane_output), ("tmpfs", args.tmpfs_output)):
        phases.append((medium, [*common, "--cases", "stored-medium", "zeros-medium", "documents-level1", "unicode-paths",
                                "--modes", "extract", "--output-root", str(output)]))
    phases.append(("kaggle50", ["--archive", str(args.archive), "--source", str(args.source), "--samples", "128",
                                "--repeats", "3", "--variants", "auto", "gpu-pipeline", "7zip", "python-parallel",
                                "--modes", "extract", "--output-root", str(args.ssd_output)]))
    if args.parallel_reference_only:
        cases = ["stored-small", "stored-medium", "stored-large", "deflate-level0", "incompressible",
                 "zeros-medium", "documents-level1", "documents-level9", "unicode-paths",
                 "empty-directories", "many-empty", "skewed"]
        phases = [
            ("parallel-reference-shapes", ["--corpus", str(args.corpus), "--cases", *cases,
                "--variants", "auto", "gpu", "7zip-parallel", "python-parallel", "--repeats", "3",
                "--output-root", str(args.ssd_output)]),
            ("parallel-reference-kaggle50", ["--archive", str(args.archive), "--source", str(args.source),
                "--samples", "128", "--variants", "auto", "gpu-pipeline", "7zip-parallel", "--repeats", "3",
                "--modes", "extract", "--output-root", str(args.ssd_output)])]
    status_path = Path(str(args.report_prefix) + "-status.jsonl")
    if status_path.exists() or any(Path(str(args.report_prefix) + f"-{name}.jsonl").exists() for name, _ in phases):
        parser.error("既存の逐次試験結果を上書きしません")
    status_path.parent.mkdir(parents=True, exist_ok=True)
    with status_path.open("x", encoding="utf-8", buffering=1) as status:
        def event(**values):
            values["time_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            line = json.dumps(values, ensure_ascii=False)
            status.write(line + "\n")
            print(line, flush=True)
        if args.predecessor_report:
            event(event="waiting_for_predecessor")
            deadline = time.monotonic() + 7200
            while True:
                if time.monotonic() >= deadline:
                    raise SystemExit("前段の試験完了待ちが期限を超えました")
                if args.predecessor_report.exists():
                    lines = args.predecessor_report.read_text().splitlines()
                    if lines and json.loads(lines[-1]).get("kind") == "summary":
                        rows = [json.loads(line) for line in lines]
                        if any(r.get("kind") == "run" and not r.get("verified") for r in rows):
                            raise SystemExit("前段が失敗したため、後続の速度比較を開始しません")
                        break
                time.sleep(2)
        for name, options in phases:
            report = Path(str(args.report_prefix) + f"-{name}.jsonl")
            log = Path(str(args.report_prefix) + f"-{name}.log")
            event(event="start", phase=name)
            with log.open("x", encoding="utf-8") as output:
                process = subprocess.Popen([sys.executable, matrix, *options, "--report", str(report)],
                                           stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    returncode = process.wait()
                except BaseException:
                    # 自分で起動したwrapperだけへSIGINT。matrixは自分のCLI群を回収する。
                    import os
                    os.killpg(process.pid, signal.SIGINT)
                    process.wait(timeout=30)
                    raise
            event(event="complete", phase=name, returncode=returncode)
            if returncode:
                raise SystemExit("失敗を記録したため、残りの速度比較を停止しました")
        event(event="all_phases_complete")


if __name__ == "__main__":
    main()
