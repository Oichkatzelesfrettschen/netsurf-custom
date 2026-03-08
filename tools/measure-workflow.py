#!/usr/bin/env python3
"""Measure repeated benchmark and workflow command timings."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
PROFILE_DIR = ROOT / "build" / "profiles"


def run_command(cmd: list[str], env: dict[str, str] | None = None) -> float:
    start = time.monotonic()
    result = subprocess.run(
        cmd,
        cwd=ROOT,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    elapsed = time.monotonic() - start
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        raise SystemExit(result.returncode)
    return elapsed


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def summary(values: list[float]) -> dict[str, float]:
    return {
        "min_s": round(min(values), 6),
        "max_s": round(max(values), 6),
        "mean_s": round(statistics.fmean(values), 6),
        "median_s": round(statistics.median(values), 6),
    }


def compare_monkey_engines(args: argparse.Namespace) -> int:
    PROFILE_DIR.mkdir(parents=True, exist_ok=True)
    engines = [
        {
            "name": "standard",
            "build_target": ["make", "build-monkey"],
            "benchmark_env": None,
        },
        {
            "name": "enhanced",
            "build_target": ["make", "build-monkey-enhanced"],
            "benchmark_env": {"NETSURF_JS_ENGINE": "enhanced"},
        },
    ]
    results: dict[str, dict] = {}

    for engine in engines:
        run_command(engine["build_target"])
        warmups: list[dict[str, float | int]] = []
        for idx in range(args.warmup_runs):
            with tempfile.NamedTemporaryFile(
                prefix=f"monkey-{engine['name']}-warmup-",
                suffix=".json",
                dir=PROFILE_DIR,
                delete=False,
            ) as tmp:
                tmp_path = Path(tmp.name)
            benchmark_cmd = [
                "python3",
                "test/run-benchmark.py",
                "-m",
                "./nsmonkey",
                "--json",
                str(tmp_path),
            ]
            env = os.environ.copy()
            if engine["benchmark_env"] is not None:
                env.update(engine["benchmark_env"])
            run_command(benchmark_cmd, env=env)
            data = json.loads(tmp_path.read_text(encoding="utf-8"))
            tmp_path.unlink(missing_ok=True)
            warmups.append(
                {
                    "run": idx + 1,
                    "elapsed_s": round(float(data["elapsed_s"]), 6),
                    "score": float(data["score"]),
                    "pass": int(data["totals"]["pass"]),
                    "fail": int(data["totals"]["fail"]),
                    "skip": int(data["totals"]["skip"]),
                }
            )
        runs: list[dict[str, float | int]] = []
        for idx in range(args.runs):
            with tempfile.NamedTemporaryFile(
                prefix=f"monkey-{engine['name']}-",
                suffix=".json",
                dir=PROFILE_DIR,
                delete=False,
            ) as tmp:
                tmp_path = Path(tmp.name)
            benchmark_cmd = [
                "python3",
                "test/run-benchmark.py",
                "-m",
                "./nsmonkey",
                "--json",
                str(tmp_path),
            ]
            env = os.environ.copy()
            if engine["benchmark_env"] is not None:
                env.update(engine["benchmark_env"])
            run_command(benchmark_cmd, env=env)
            data = json.loads(tmp_path.read_text(encoding="utf-8"))
            tmp_path.unlink(missing_ok=True)
            runs.append(
                {
                    "run": idx + 1,
                    "elapsed_s": round(float(data["elapsed_s"]), 6),
                    "score": float(data["score"]),
                    "pass": int(data["totals"]["pass"]),
                    "fail": int(data["totals"]["fail"]),
                    "skip": int(data["totals"]["skip"]),
                }
            )
        elapsed_values = [float(run["elapsed_s"]) for run in runs]
        results[engine["name"]] = {
            "warmups": warmups,
            "runs": runs,
            "summary": summary(elapsed_values),
        }

    payload = {
        "schema": 1,
        "scenario": "monkey-js-engine-compare",
        "warmup_runs_per_engine": args.warmup_runs,
        "runs_per_engine": args.runs,
        "results": results,
        "comparison": {
            "enhanced_vs_standard_mean_ratio": round(
                results["enhanced"]["summary"]["mean_s"]
                / results["standard"]["summary"]["mean_s"],
                6,
            ),
            "enhanced_minus_standard_mean_s": round(
                results["enhanced"]["summary"]["mean_s"]
                - results["standard"]["summary"]["mean_s"],
                6,
            ),
        },
    }
    write_json(Path(args.output), payload)

    if args.restore_standard:
        run_command(["make", "build-monkey"])

    return 0


def measure_bootstrap_costs(args: argparse.Namespace) -> int:
    PROFILE_DIR.mkdir(parents=True, exist_ok=True)
    commands = [
        ("bootstrap-tools", ["make", "bootstrap-tools"]),
        ("bootstrap-libs", ["make", "bootstrap-libs"]),
        ("bootstrap", ["make", "bootstrap"]),
    ]
    measurements: dict[str, dict] = {}
    for name, cmd in commands:
        runs = []
        for idx in range(args.runs):
            elapsed = run_command(cmd)
            runs.append({"run": idx + 1, "elapsed_s": round(elapsed, 6)})
        measurements[name] = {
            "command": " ".join(cmd),
            "runs": runs,
            "summary": summary([float(run["elapsed_s"]) for run in runs]),
        }

    payload = {
        "schema": 1,
        "scenario": "repo-root-bootstrap-costs",
        "measurement_mode": "incremental-existing-workspace",
        "runs_per_command": args.runs,
        "measurements": measurements,
        "notes": (
            "These timings reflect an existing repo-local workspace. "
            "They are intended to show incremental command cost, not cold "
            "clone or first-build time."
        ),
    }
    write_json(Path(args.output), payload)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    compare = sub.add_parser("compare-monkey-engines")
    compare.add_argument("--warmup-runs", type=int, default=1)
    compare.add_argument("--runs", type=int, default=5)
    compare.add_argument(
        "--output",
        default=str(PROFILE_DIR / "monkey-engine-compare.json"),
    )
    compare.add_argument("--restore-standard", action="store_true")
    compare.set_defaults(func=compare_monkey_engines)

    bootstrap = sub.add_parser("measure-bootstrap-costs")
    bootstrap.add_argument("--runs", type=int, default=3)
    bootstrap.add_argument(
        "--output",
        default=str(PROFILE_DIR / "bootstrap-costs.json"),
    )
    bootstrap.set_defaults(func=measure_bootstrap_costs)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
