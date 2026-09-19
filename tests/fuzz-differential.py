#!/usr/bin/env python3
"""Generate bounded Lightning programs and compare JIT-off with required JIT."""

import argparse
import json
import math
from pathlib import Path
import shlex
import shutil
import sys
import tempfile
from typing import Callable, List, Optional, Sequence, Tuple

from differential import (
    _check_test,
    _positive_timeout,
    _resolve_executable,
    _run,
    _without_ansi,
)


_MASK64 = (1 << 64) - 1
_SCENARIOS = (
    "arithmetic",
    "control-flow",
    "table-mutation",
    "array-mutation",
    "typed-array-mutation",
    "closures",
    "exceptions",
    "traits",
    "coroutine",
)


class _Generator:
    """Small stable PRNG; its output does not depend on Python's random module."""

    def __init__(self, seed: int) -> None:
        self.state = seed & _MASK64

    def next_u64(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & _MASK64
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & _MASK64
        return (value ^ (value >> 31)) & _MASK64

    def integer(self, lower: int, upper: int) -> int:
        assert lower <= upper
        return lower + self.next_u64() % (upper - lower + 1)

    def choose(self, values: Sequence[str]) -> str:
        return values[self.next_u64() % len(values)]


def _seed_value(value: str) -> int:
    try:
        seed = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError("seed must be an integer") from error
    if not 0 <= seed <= _MASK64:
        raise argparse.ArgumentTypeError("seed must be between 0 and 2^64 - 1")
    return seed


def _positive_cases(value: str) -> int:
    try:
        cases = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("cases must be an integer") from error
    if cases <= 0:
        raise argparse.ArgumentTypeError("cases must be positive")
    return cases


def _case_seed(seed: int, case_index: int) -> int:
    generator = _Generator((seed + case_index * 0xD1342543DE82EF95) & _MASK64)
    return generator.next_u64()


def _header(seed: int, case_index: int, case_seed: int, scenario: str) -> str:
    return (
        "# generated differential fuzz case\n"
        "# seed: {}\n"
        "# case: {}\n"
        "# case-seed: 0x{:016x}\n"
        "# scenario: {}\n"
    ).format(seed, case_index, case_seed, scenario)


def _arithmetic(generator: _Generator) -> str:
    edges = (
        "-9007199254740991",
        "-2147483648",
        "-1",
        "0",
        "1",
        "2147483647",
        "9007199254740991",
    )
    left = generator.choose(edges)
    right = generator.choose(edges)
    divisor = generator.integer(1, 97)
    shift = generator.integer(-31, 31)
    return """import math

const left = {left}
const right = {right}
const divisor = {divisor}
const positive_zero = 0.0
const negative_zero = -positive_zero
const not_a_number = math.nan
let result = (left + right) * divisor
result += (left - right) / divisor
result += {shift}
result += math.copysign(1, negative_zero) == -1 ? 101 : -101
result += positive_zero == negative_zero ? 103 : -103
result += not_a_number != not_a_number ? 107 : -107
result += !(not_a_number <= 0) ? 109 : -109
print("arithmetic", result)
result
""".format(left=left, right=right, divisor=divisor, shift=shift)


def _control_flow(generator: _Generator) -> str:
    limit = generator.integer(2, 7)
    while_limit = generator.integer(1, 5)
    bias = generator.integer(-20, 20)
    parity = generator.integer(0, 1)
    return """let trace = {{calls: 0}}
fn touch(trace, value) {{
    trace.calls += 1
    value
}}

let result = {bias}
for index in 0..{limit} {{
    if index % 2 == {parity} {{
        result += index * 3
    }} else {{
        result -= index
    }}
}}
let turn = 0
while turn < {while_limit} {{
    result += turn
    turn += 1
}}
false && touch(trace, true)
true || touch(trace, false)
(nil ?? touch(trace, result))
result += trace.calls * 113
print("control", result)
result
""".format(
        bias=bias,
        limit=limit,
        parity=parity,
        while_limit=while_limit,
    )


def _table_mutation(generator: _Generator) -> str:
    first = generator.integer(-200, 200)
    second = generator.integer(-200, 200)
    delta = generator.integer(-30, 30)
    extra = generator.integer(-200, 200)
    return """let values = {{first: {first}, second: {second}}}
values.first += {delta}
values["extra"] = {extra}
const old_second = values.second
delete values.second
values["nested"] = {{value: values.first - values.extra}}
values.nested.value += old_second
const missing_score = values.second == nil ? 127 : -127
const result = values.first + values.extra + values.nested.value + missing_score
print("table", result)
result
""".format(first=first, second=second, delta=delta, extra=extra)


def _array_mutation(generator: _Generator) -> str:
    values = [generator.integer(-100, 100) for _ in range(4)]
    replacement = generator.integer(-100, 100)
    pushed = generator.integer(-100, 100)
    return """let values = [{v0}, {v1}, {v2}, {v3}]
values[1] = {replacement}
values::push({pushed})
const removed = values::pop()
values[2] += values[0]
let result = removed + values[0] + values[1] + values[2] + values[3]
for index, value in values {{
    result += index * value
}}
print("array", result)
result
""".format(
        v0=values[0],
        v1=values[1],
        v2=values[2],
        v3=values[3],
        replacement=replacement,
        pushed=pushed,
    )


def _typed_array_mutation(generator: _Generator) -> str:
    values = [generator.integer(-100000, 100000) for _ in range(3)]
    replacement = generator.integer(-100000, 100000)
    return """import math
import typed

let integers = i32[]([{v0}, {v1}, {v2}])
integers[1] = {replacement}
let floats = f64[]([0.0, -0.0, math.nan])
floats[0] = integers[0] / 8
const signed_zero = math.copysign(1, floats[1]) == -1 ? 131 : -131
const nan_score = floats[2] != floats[2] ? 137 : -137
const result = integers[0] + integers[1] + integers[2] + floats[0] + signed_zero + nan_score
print("typed", result)
result
""".format(
        v0=values[0],
        v1=values[1],
        v2=values[2],
        replacement=replacement,
    )


def _closures(generator: _Generator) -> str:
    start = generator.integer(-50, 50)
    step = generator.integer(1, 12)
    calls = generator.integer(2, 6)
    return """fn make_counter(start, step) {{
    let current = start
    || {{
        current += step
        current
    }}
}}

const counter = make_counter({start}, {step})
let result = 0
for ignored in 0..{calls} {{
    result += counter()
}}
const sibling = make_counter(result, -{step})
result += sibling()
result += sibling()
print("closure", result)
result
""".format(start=start, step=step, calls=calls)


def _exceptions(generator: _Generator) -> str:
    value = generator.integer(10, 100)
    limit = generator.integer(0, 9)
    tag = generator.integer(1000, 9999)
    return """fn checked(value, limit) {{
    if value > limit {{
        throw {{code: value - limit}}
    }}
    value + limit
}}

let caught = 0
try {{
    caught = checked({value}, {limit})
}} catch error {{
    caught = error.code
}}
print("exception", caught)
throw "fuzz-exception-{tag}"
""".format(value=value, limit=limit, tag=tag)


def _traits(generator: _Generator) -> str:
    left = generator.integer(-100, 100)
    right = generator.integer(-100, 100)
    multiplier = generator.integer(2, 7)
    return """import traits

fn add_values(other) {{
    self.value + other.value
}}

let receiver = {{value: {left}}}
traits.set(receiver, "add", add_values)
traits.set(receiver, "at", |key| {{
    if key == "scaled" {{
        return self.value * {multiplier}
    }}
    nil
}})
const result = (receiver + {{value: {right}}}) + receiver.scaled
print("traits", result)
result
""".format(left=left, right=right, multiplier=multiplier)


def _coroutine(generator: _Generator) -> str:
    first = generator.integer(-100, 100)
    second = generator.integer(-100, 100)
    third = generator.integer(-100, 100)
    delta = generator.integer(1, 20)
    return """import coroutine

const worker = coroutine.create(|| {{
    const resumed = yield {first}
    const resumed_again = yield resumed + {delta}
    resumed_again + {first}
}})
const first = worker.resume()
const second = worker.resume({second})
const third = worker.resume({third})
const status_score = worker.status() == "dead" ? 139 : -139
const result = first + second + third + status_score
print("coroutine", result)
result
""".format(first=first, second=second, third=third, delta=delta)


_BUILDERS: Tuple[Callable[[_Generator], str], ...] = (
    _arithmetic,
    _control_flow,
    _table_mutation,
    _array_mutation,
    _typed_array_mutation,
    _closures,
    _exceptions,
    _traits,
    _coroutine,
)


def generate_source(seed: int, case_index: int) -> Tuple[str, int, str]:
    """Return one independently reproducible source program and its metadata."""
    case_seed = _case_seed(seed, case_index)
    scenario_index = case_index % len(_BUILDERS)
    scenario = _SCENARIOS[scenario_index]
    generator = _Generator(case_seed)
    body = _BUILDERS[scenario_index](generator)
    return _header(seed, case_index, case_seed, scenario) + body, case_seed, scenario


def _failure_signature(failure: str) -> Tuple[str, ...]:
    first_line = failure.splitlines()[0] if failure else ""
    if " timed out after " in first_line:
        return ("timeout", first_line.split(" timed out", 1)[0])
    if "terminated by " in first_line or "crash-like exit status" in first_line:
        return ("crash", first_line.split(" ", 1)[0])
    if first_line.startswith("expected exactly one JIT compilation evidence line"):
        return ("jit-evidence", "missing")
    if first_line.startswith("required JIT compiled no functions"):
        return ("jit-evidence", "zero")

    parts = ["semantic-divergence"]
    if "exit status: interpreter=" in failure:
        parts.append("exit-status")
    if "interpreter stdout" in failure:
        parts.append("stdout")
    if "interpreter stderr" in failure:
        parts.append("stderr")
    if "filesystem mutations:" in failure:
        parts.append("filesystem")
    return tuple(parts)


def _is_parser_error(executable: Path, source_path: Path, timeout: float) -> bool:
    result = _run(executable, source_path, jit=False, timeout=timeout)
    if result.timed_out or result.returncode is None:
        return False
    output = _without_ansi(result.stdout) + _without_ansi(result.stderr)
    return b"Parser error:" in output


def _minimize_source(
    source: str,
    source_path: Path,
    executable: Path,
    timeout: float,
    original_failure: str,
) -> Tuple[str, str]:
    """Deterministically delete line chunks while preserving the failure class."""
    target_signature = _failure_signature(original_failure)
    lines = source.splitlines(keepends=True)
    granularity = 2
    attempts = 0
    last_failure = original_failure
    max_attempts = 48

    while len(lines) >= 2 and attempts < max_attempts:
        chunk_size = max(1, int(math.ceil(len(lines) / float(granularity))))
        reduced = False
        for start in range(0, len(lines), chunk_size):
            if attempts >= max_attempts:
                break
            candidate_lines = lines[:start] + lines[start + chunk_size :]
            candidate = "".join(candidate_lines)
            if not candidate.strip():
                continue
            attempts += 1
            source_path.write_text(candidate, encoding="utf-8")
            failure = _check_test(executable, source_path, timeout)
            if failure is None or _failure_signature(failure) != target_signature:
                continue
            if target_signature[0] == "jit-evidence" and _is_parser_error(
                executable, source_path, timeout
            ):
                continue
            lines = candidate_lines
            last_failure = failure
            granularity = max(2, granularity - 1)
            reduced = True
            break
        if reduced:
            continue
        if granularity >= len(lines):
            break
        granularity = min(len(lines), granularity * 2)

    minimized = "".join(lines)
    source_path.write_text(minimized, encoding="utf-8")
    return minimized, last_failure


def _reproduction_commands(
    executable: Path,
    saved_source: Path,
    seed: int,
    case_index: int,
    timeout: float,
    output_directory: Path,
    minimized: bool,
) -> List[str]:
    harness_command = [
        sys.executable,
        str(Path(__file__).resolve()),
        str(executable),
        "--seed",
        str(seed),
        "--cases",
        str(case_index + 1),
        "--timeout",
        str(timeout),
        "--output-dir",
        str(output_directory),
    ]
    if minimized:
        harness_command.append("--minimize")
    return [
        shlex.join(harness_command),
        shlex.join([str(executable), "--jit=off", str(saved_source)]),
        shlex.join([str(executable), "--jit", str(saved_source)]),
    ]


def _save_failure(
    output_directory: Path,
    executable: Path,
    seed: int,
    case_index: int,
    case_seed: int,
    scenario: str,
    source: str,
    failure: str,
    timeout: float,
    minimized: bool,
) -> Tuple[Path, Path, List[str]]:
    output_directory.mkdir(parents=True, exist_ok=True)
    stem = "fuzz-failure-seed-{:016x}-case-{:06d}".format(seed, case_index)
    source_path = (output_directory / (stem + ".li")).resolve()
    metadata_path = (output_directory / (stem + ".json")).resolve()
    commands = _reproduction_commands(
        executable,
        source_path,
        seed,
        case_index,
        timeout,
        output_directory,
        minimized,
    )
    preamble = [
        "# saved differential fuzz failure",
        "# seed: {}".format(seed),
        "# case: {}".format(case_index),
        "# case-seed: 0x{:016x}".format(case_seed),
        "# scenario: {}".format(scenario),
    ]
    preamble.extend("# reproduce: {}".format(command) for command in commands)
    saved_source = "\n".join(preamble) + "\n" + source
    source_path.write_text(saved_source, encoding="utf-8")
    metadata = {
        "case": case_index,
        "case_seed": "0x{:016x}".format(case_seed),
        "commands": commands,
        "failure": failure,
        "minimized": minimized,
        "scenario": scenario,
        "seed": seed,
        "source": str(source_path),
        "timeout": timeout,
    }
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return source_path, metadata_path, commands


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="path or command name for the Lightning executable")
    parser.add_argument(
        "--seed",
        type=_seed_value,
        default=0,
        help="unsigned 64-bit generator seed (default: 0)",
    )
    parser.add_argument(
        "--cases",
        type=_positive_cases,
        default=100,
        help="number of bounded programs to run (default: 100)",
    )
    parser.add_argument(
        "--timeout",
        type=_positive_timeout,
        default=10.0,
        help="per-process timeout in seconds (default: 10)",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="directory used only for retained failing programs and metadata",
    )
    parser.add_argument(
        "--minimize",
        action="store_true",
        help="minimize the first failure by deterministic line-chunk deletion",
    )
    arguments = parser.parse_args(argv)

    try:
        executable = _resolve_executable(arguments.executable)
    except ValueError as error:
        parser.error(str(error))

    output_directory = Path(arguments.output_dir).expanduser().resolve()
    if output_directory.exists() and not output_directory.is_dir():
        parser.error("output directory is not a directory: {}".format(output_directory))

    with tempfile.TemporaryDirectory(prefix="li-differential-fuzz-") as temporary:
        source_path = Path(temporary) / "case.li"
        for case_index in range(arguments.cases):
            source, case_seed, scenario = generate_source(arguments.seed, case_index)
            source_path.write_text(source, encoding="utf-8")
            try:
                failure = _check_test(executable, source_path, arguments.timeout)
            except (OSError, shutil.Error) as error:
                print("fuzz harness error in case {}: {}".format(case_index, error), file=sys.stderr)
                return 2
            if failure is None:
                continue

            try:
                if arguments.minimize:
                    source, failure = _minimize_source(
                        source,
                        source_path,
                        executable,
                        arguments.timeout,
                        failure,
                    )

                saved_source, metadata, commands = _save_failure(
                    output_directory,
                    executable,
                    arguments.seed,
                    case_index,
                    case_seed,
                    scenario,
                    source,
                    failure,
                    arguments.timeout,
                    arguments.minimize,
                )
            except (OSError, shutil.Error) as error:
                print("fuzz harness error in case {}: {}".format(case_index, error), file=sys.stderr)
                return 2
            print(
                "FAIL seed={} case={} case-seed=0x{:016x} scenario={}\n{}".format(
                    arguments.seed,
                    case_index,
                    case_seed,
                    scenario,
                    failure,
                ),
                file=sys.stderr,
            )
            print("saved source: {}".format(saved_source), file=sys.stderr)
            print("saved metadata: {}".format(metadata), file=sys.stderr)
            for command in commands:
                print("reproduce: {}".format(command), file=sys.stderr)
            return 1

    print(
        "PASS {} differential fuzz cases (seed={})".format(
            arguments.cases,
            arguments.seed,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
