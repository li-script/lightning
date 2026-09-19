#!/usr/bin/env python3
"""Run the Lightning benchmark corpus and emit machine-readable evidence."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Final


WORKLOADS: Final = (
    "binarytrees.li",
    "fib.li",
    "nbody.li",
    "table-heavy.li",
    "string-heavy.li",
    "closure-heavy.li",
    "alloc-heavy.li",
    "escape.li",
    "inline.li",
    "integer-ranges.li",
    "inline-cache.li",
    "vectors.li",
)
WORKLOAD_NAMES: Final = tuple(Path(filename).stem for filename in WORKLOADS)
MODE_ARGUMENTS: Final = {
    "interpreter": ("--jit=off",),
    "jit": ("--jit",),
    "auto": ("--jit=auto",),
}
EXECUTION_METRIC_FIELDS: Final = (
    "parse_compile_ns",
    "jit_compile_ns",
    "first_call_ns",
    "warm_call_ns",
    "generated_code_bytes",
    "compiled_functions",
    "allocations",
    "retains",
    "releases",
    "spill_slots",
)
UNSIGNED_EXECUTION_METRIC_FIELDS: Final = (
    "parse_compile_ns",
    "jit_compile_ns",
    "generated_code_bytes",
    "compiled_functions",
    "allocations",
    "retains",
    "releases",
    "spill_slots",
)
SUMMARY_UNITS: Final = {
    "process_wall_time_ns": "ns",
    "parse_compile_ns": "ns",
    "jit_compile_ns": "ns",
    "first_call_ns": "ns",
    "warm_call_ns": "ns",
    "warm_throughput_per_second": "calls/second",
    "generated_code_bytes": "bytes",
    "compiled_functions": "functions",
    "allocations": "allocations",
    "retains": "operations",
    "releases": "operations",
    "spill_slots": "slots",
}

HISTORICAL_REFERENCES: Final = (
    {
        "benchmark": "binarytrees",
        "runtime": "Li",
        "wall_time_ms": 328,
        "measured_at": "2022-06",
        "kind": "historical_reference",
        "comparable_to_current_suite": False,
        "source": "ROADMAP.md historical Discord benchmark report",
    },
    {
        "benchmark": "binarytrees",
        "runtime": "LuaJIT",
        "wall_time_ms": 493,
        "measured_at": "2022-06",
        "kind": "historical_reference",
        "comparable_to_current_suite": False,
        "source": "ROADMAP.md historical Discord benchmark report",
    },
    {
        "benchmark": "binarytrees",
        "runtime": "Node",
        "wall_time_ms": 228,
        "measured_at": "2022-06",
        "kind": "historical_reference",
        "comparable_to_current_suite": False,
        "source": "ROADMAP.md historical Discord benchmark report",
    },
)


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def benchmark_run_count(value: str) -> int:
    parsed = int(value)
    if parsed < 1 or parsed > 1_000_000:
        raise argparse.ArgumentTypeError("must be between 1 and 1000000")
    return parsed


def workload_name(value: str) -> str:
    candidate = value.removesuffix(".li")
    if candidate not in WORKLOAD_NAMES:
        choices = ", ".join(WORKLOAD_NAMES)
        raise argparse.ArgumentTypeError(f"unknown workload {value!r}; choose one of: {choices}")
    return candidate


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run Lightning workloads in fresh processes and write JSON evidence to stdout."
    )
    parser.add_argument("executable", type=Path, help="path to the li executable")
    parser.add_argument("mode", choices=tuple(MODE_ARGUMENTS))
    parser.add_argument("repetitions", type=int, help="measured fresh-process runs per workload")
    parser.add_argument(
        "--execution-kind",
        choices=("native", "translated"),
        required=True,
        help="whether the executable runs natively or through an ISA translation layer",
    )
    parser.add_argument(
        "--build-label",
        required=True,
        help="non-empty label identifying the exact build/configuration under measurement",
    )
    parser.add_argument(
        "--workload",
        action="append",
        type=workload_name,
        dest="workloads",
        help="workload stem or filename to run; repeat to select a subset (default: all)",
    )
    parser.add_argument(
        "--warmups",
        type=int,
        default=0,
        help="discarded fresh-process runs per workload (default: 0)",
    )
    parser.add_argument(
        "--in-process-runs",
        type=benchmark_run_count,
        default=1,
        metavar="N",
        help="calls of the loaded entry in each child process (default: 1)",
    )
    parser.add_argument(
        "--timeout",
        type=positive_float,
        default=60.0,
        help="per-process timeout in seconds (default: 60)",
    )
    parser.add_argument(
        "--collect-verbose-evidence",
        action="store_true",
        help="also use the strict --jit-verbose CLI mode; runtime costs still come from --metrics",
    )
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("repetitions must be at least 1")
    if args.warmups < 0:
        parser.error("warmups must be non-negative")
    if not args.build_label.strip():
        parser.error("build label must not be empty")
    if args.collect_verbose_evidence and args.mode != "jit":
        parser.error("--collect-verbose-evidence requires jit mode")
    if args.workloads and len(set(args.workloads)) != len(args.workloads):
        parser.error("each workload may be selected only once")
    return args


def platform_metadata() -> dict[str, object]:
    return {
        "system": platform.system(),
        "release": platform.release(),
        "version": platform.version(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python_implementation": platform.python_implementation(),
        "python_version": platform.python_version(),
        "logical_cpu_count": os.cpu_count(),
    }


def decode_output(value: bytes | str | None) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    return value.decode("utf-8", errors="replace")


def is_unsigned_integer(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def unavailable_metrics(process_wall_time_ns: int) -> dict[str, object]:
    return {
        "process_wall_time_ns": process_wall_time_ns,
        "parse_compile_ns": None,
        "jit_compile_ns": None,
        "first_call_ns": None,
        "warm_call_ns": None,
        "warm_throughput_per_second": None,
        "generated_code_bytes": None,
        "compiled_functions": None,
        "allocations": None,
        "retains": None,
        "releases": None,
        "spill_slots": None,
    }


def parse_execution_metrics(
    stderr: str,
    process_wall_time_ns: int,
    mode: str,
    in_process_runs: int,
    process_completed: bool,
) -> tuple[dict[str, object], list[str]]:
    metrics = unavailable_metrics(process_wall_time_ns)
    metric_lines = [
        line.removeprefix("Metrics: ")
        for line in stderr.splitlines()
        if line.startswith("Metrics: ")
    ]
    if len(metric_lines) != 1:
        return metrics, [f"expected exactly one Metrics JSON line on stderr, found {len(metric_lines)}"]

    try:
        payload = json.loads(metric_lines[0])
    except json.JSONDecodeError as error:
        return metrics, [f"malformed Metrics JSON: {error.msg}"]
    if not isinstance(payload, dict):
        return metrics, ["Metrics JSON must be an object"]

    expected_fields = set(EXECUTION_METRIC_FIELDS)
    actual_fields = set(payload)
    errors: list[str] = []
    missing = sorted(expected_fields - actual_fields)
    unexpected = sorted(actual_fields - expected_fields)
    if missing:
        errors.append(f"Metrics JSON is missing fields: {', '.join(missing)}")
    if unexpected:
        errors.append(f"Metrics JSON has unexpected fields: {', '.join(unexpected)}")

    for field in UNSIGNED_EXECUTION_METRIC_FIELDS:
        if field in payload and not is_unsigned_integer(payload[field]):
            errors.append(f"Metrics field {field} must be a non-negative integer")

    first_call = payload.get("first_call_ns")
    if first_call is not None and not is_unsigned_integer(first_call):
        errors.append("Metrics field first_call_ns must be null or a non-negative integer")

    warm_calls = payload.get("warm_call_ns")
    if not isinstance(warm_calls, list):
        errors.append("Metrics field warm_call_ns must be an array")
    elif any(not is_unsigned_integer(value) for value in warm_calls):
        errors.append("Metrics field warm_call_ns must contain only non-negative integers")

    if errors:
        return metrics, errors

    typed_warm_calls = warm_calls
    if len(typed_warm_calls) > in_process_runs - 1:
        errors.append(
            f"Metrics warm_call_ns contains {len(typed_warm_calls)} calls, more than the requested {in_process_runs - 1}"
        )
    if first_call is None and typed_warm_calls:
        errors.append("Metrics reports warm calls although the first call did not complete")
    if process_completed:
        if first_call is None:
            errors.append("successful process did not report a completed first call")
        if len(typed_warm_calls) != in_process_runs - 1:
            errors.append(
                f"successful process reported {len(typed_warm_calls)} warm calls; expected {in_process_runs - 1}"
            )

    generated_code_bytes = payload["generated_code_bytes"]
    compiled_functions = payload["compiled_functions"]
    if (compiled_functions == 0) != (generated_code_bytes == 0):
        errors.append("compiled_functions and generated_code_bytes disagree about native code emission")
    if mode == "interpreter" and (compiled_functions != 0 or generated_code_bytes != 0):
        errors.append("interpreter mode reported native code emission")
    if mode == "jit" and process_completed and compiled_functions == 0:
        errors.append("strict JIT mode reported no compiled functions")

    metrics.update(payload)
    if process_completed and in_process_runs > 1 and len(typed_warm_calls) == in_process_runs - 1:
        total_warm_ns = sum(typed_warm_calls)
        if total_warm_ns > 0:
            metrics["warm_throughput_per_second"] = (
                len(typed_warm_calls) * 1_000_000_000 / total_warm_ns
            )
    return metrics, errors


def run_once(
    command: list[str],
    cwd: Path,
    timeout: float,
    kind: str,
    iteration: int,
    mode: str,
    in_process_runs: int,
) -> dict[str, object]:
    started_ns = time.perf_counter_ns()
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            capture_output=True,
            check=False,
            timeout=timeout,
        )
        process_wall_time_ns = time.perf_counter_ns() - started_ns
        stdout = decode_output(completed.stdout)
        stderr = decode_output(completed.stderr)
        exit_code: int | None = completed.returncode
        timed_out = False
    except subprocess.TimeoutExpired as timeout_error:
        process_wall_time_ns = time.perf_counter_ns() - started_ns
        stdout = decode_output(timeout_error.stdout)
        stderr = decode_output(timeout_error.stderr)
        exit_code = None
        timed_out = True

    process_completed = not timed_out and exit_code == 0
    metrics, errors = parse_execution_metrics(
        stderr,
        process_wall_time_ns,
        mode,
        in_process_runs,
        process_completed,
    )
    if timed_out:
        errors.append(f"process exceeded {timeout:g} second timeout")
    elif exit_code != 0:
        errors.append(f"process exited with status {exit_code}")

    return {
        "kind": kind,
        "iteration": iteration,
        "command": command,
        "metrics": metrics,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "stdout": stdout,
        "stderr": stderr,
        "errors": errors,
        "successful": not errors,
    }


def summarize(
    runs: list[dict[str, object]], metric_name: str, unit: str
) -> dict[str, object]:
    values: list[int | float] = []
    for run in runs:
        if not run["successful"]:
            continue
        value = run["metrics"][metric_name]
        if isinstance(value, list):
            values.extend(value)
        elif value is not None:
            values.append(value)
    if not values:
        return {
            "sample_count": 0,
            "minimum": None,
            "median": None,
            "mean": None,
            "maximum": None,
            "unit": unit,
            "reason": "no successful measured run exposed this metric",
        }
    return {
        "sample_count": len(values),
        "minimum": min(values),
        "median": statistics.median(values),
        "mean": statistics.fmean(values),
        "maximum": max(values),
        "unit": unit,
        "reason": None,
    }


def main() -> int:
    args = parse_args()
    executable = args.executable.expanduser().resolve(strict=True)
    if not executable.is_file():
        raise SystemExit(f"not a file: {executable}")

    suite_directory = Path(__file__).resolve().parent
    selected_workloads = args.workloads or list(WORKLOAD_NAMES)
    mode_arguments = (
        ("--jit-verbose",) if args.collect_verbose_evidence else MODE_ARGUMENTS[args.mode]
    )
    benchmark_results: list[dict[str, object]] = []

    for selected_name in selected_workloads:
        workload = suite_directory / f"{selected_name}.li"
        command = [
            str(executable),
            *mode_arguments,
            "--metrics",
            f"--benchmark-runs={args.in_process_runs}",
            str(workload),
        ]
        warmup_runs = [
            run_once(
                command,
                suite_directory,
                args.timeout,
                "warmup_fresh_process",
                iteration,
                args.mode,
                args.in_process_runs,
            )
            for iteration in range(1, args.warmups + 1)
        ]
        runs = [
            run_once(
                command,
                suite_directory,
                args.timeout,
                "measurement_fresh_process",
                repetition,
                args.mode,
                args.in_process_runs,
            )
            for repetition in range(1, args.repetitions + 1)
        ]
        all_runs = [*warmup_runs, *runs]
        benchmark_results.append(
            {
                "name": workload.stem,
                "path": str(workload),
                "command": command,
                "warmup_runs": warmup_runs,
                "runs": runs,
                "summary": {
                    name: summarize(runs, name, unit) for name, unit in SUMMARY_UNITS.items()
                },
                "successful": all(run["successful"] for run in all_runs),
            }
        )

    successful = all(result["successful"] for result in benchmark_results)
    result = {
        "schema_version": 3,
        "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "suite": "lightning-p0",
        "executable": str(executable),
        "build_label": args.build_label,
        "execution_kind": args.execution_kind,
        "mode": args.mode,
        "repetitions": args.repetitions,
        "warmups": args.warmups,
        "in_process_runs": args.in_process_runs,
        "timeout_seconds": args.timeout,
        "selected_workloads": selected_workloads,
        "metrics_requested": True,
        "verbose_evidence_requested": args.collect_verbose_evidence,
        "process_model": "fresh_process_per_invocation",
        "timing_semantics": {
            "process_wall_time_ns": "process-inclusive runner wall clock: startup, file read, parsing, eager JIT compilation, all requested VM calls, and shutdown",
            "parse_compile_ns": "in-process load_script wall clock, including eager JIT compilation",
            "jit_compile_ns": "in-process time spent in actual JIT compilation attempts during load and calls",
            "first_call_ns": "in-call wall clock for the first entry call in a newly loaded VM",
            "warm_call_ns": "per-call in-call wall clocks after the first call, reusing the same loaded VM",
            "warm_throughput_per_second": "derived only from the completed warm_call_ns values when more than one in-process call was requested",
            "runtime_counts": "per-thread runtime counter deltas across the requested calls in one child process",
            "warmups": "discarded fresh child processes; separate from warm calls that reuse a VM inside each child",
        },
        "platform": platform_metadata(),
        "historical_references": list(HISTORICAL_REFERENCES),
        "benchmarks": benchmark_results,
        "successful": successful,
    }
    json.dump(result, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0 if successful else 1


if __name__ == "__main__":
    raise SystemExit(main())
