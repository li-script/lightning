#!/usr/bin/env python3
"""Compare each Lightning test under explicitly disabled and required JIT modes."""

import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Dict, List, Mapping, Optional, Sequence, Tuple


_ANSI_RE = re.compile(
    rb"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[@-_])"
)
_TIMING_RE = re.compile(
    rb"(?m)^\([0-9]+(?:\.[0-9]+)? ms\) (?=(?:Exception|Result):)"
)
_JIT_EVIDENCE_RE = re.compile(
    rb"(?m)^JIT compiled functions: ([0-9]+)\r?(?:\n|$)"
)


@dataclass(frozen=True)
class _RunResult:
    command: Tuple[str, ...]
    returncode: Optional[int]
    stdout: bytes
    stderr: bytes
    timed_out: bool
    mutations: Mapping[str, str]


@dataclass(frozen=True)
class _Observation:
    returncode: int
    stdout: bytes
    stderr: bytes
    mutations: Mapping[str, str]


def _positive_timeout(value: str) -> float:
    timeout = float(value)
    if timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be positive")
    return timeout


def _resolve_executable(value: str) -> Path:
    candidate = Path(value).expanduser()
    if candidate.exists():
        executable = candidate.resolve()
    else:
        found = shutil.which(value)
        if found is None:
            raise ValueError("executable not found: {}".format(value))
        executable = Path(found).resolve()
    if not executable.is_file() or not os.access(str(executable), os.X_OK):
        raise ValueError("not an executable file: {}".format(executable))
    return executable


def _collect_tests(values: Sequence[str]) -> List[Path]:
    if not values:
        tests = sorted(Path(__file__).resolve().parent.glob("*.li"))
    else:
        tests = []
        for value in values:
            path = Path(value).expanduser()
            if path.is_dir():
                tests.extend(sorted(path.glob("*.li")))
            else:
                tests.append(path)

    resolved = []
    seen = set()
    for test in tests:
        test = test.resolve()
        if not test.is_file():
            raise ValueError("test not found: {}".format(test))
        if test.suffix.lower() != ".li":
            raise ValueError("test is not a .li file: {}".format(test))
        # A fixture that observes the execution mode itself cannot have identical
        # output in both modes; it declares that in its first lines.
        with test.open("r", encoding="utf-8", errors="replace") as source:
            head = source.read(512)
        if "differential: mode-dependent" in head:
            continue
        if test not in seen:
            seen.add(test)
            resolved.append(test)
    if not resolved:
        raise ValueError("no .li tests selected")
    return resolved


def _snapshot(root: Path) -> Dict[str, str]:
    snapshot = {}
    for path in sorted(root.rglob("*"), key=lambda item: item.as_posix()):
        relative = path.relative_to(root).as_posix()
        metadata = path.lstat()
        mode = stat.S_IMODE(metadata.st_mode)
        if stat.S_ISLNK(metadata.st_mode):
            signature = "symlink:{:o}:{}".format(mode, os.readlink(str(path)))
        elif stat.S_ISDIR(metadata.st_mode):
            signature = "directory:{:o}".format(mode)
        elif stat.S_ISREG(metadata.st_mode):
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            signature = "file:{:o}:{}:{}".format(mode, metadata.st_size, digest)
        else:
            signature = "other:{:o}:{}".format(mode, metadata.st_mode)
        snapshot[relative] = signature
    return snapshot


def _mutation_delta(before: Mapping[str, str], after: Mapping[str, str]) -> Dict[str, str]:
    mutations = {}
    for path in sorted(set(before) | set(after)):
        old = before.get(path)
        new = after.get(path)
        if old == new:
            continue
        if old is None:
            mutations[path] = "created {}".format(new)
        elif new is None:
            mutations[path] = "deleted {}".format(old)
        else:
            mutations[path] = "changed {} -> {}".format(old, new)
    return mutations


def _run(executable: Path, test: Path, jit: bool, timeout: float) -> _RunResult:
    with tempfile.TemporaryDirectory(prefix="li-differential-") as temporary:
        workspace = Path(temporary)
        shutil.copytree(str(test.parent), str(workspace), dirs_exist_ok=True, symlinks=True)
        local_test = workspace / test.name
        before = _snapshot(workspace)
        command = [str(executable), "--jit" if jit else "--jit=off"]
        command.append(local_test.name)

        try:
            completed = subprocess.run(
                command,
                cwd=str(workspace),
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
            returncode = completed.returncode
            stdout = completed.stdout
            stderr = completed.stderr
            timed_out = False
        except subprocess.TimeoutExpired as error:
            returncode = None
            stdout = error.stdout or b""
            stderr = error.stderr or b""
            timed_out = True

        after = _snapshot(workspace)
        return _RunResult(
            command=tuple(command),
            returncode=returncode,
            stdout=stdout,
            stderr=stderr,
            timed_out=timed_out,
            mutations=_mutation_delta(before, after),
        )


def _without_ansi(data: bytes) -> bytes:
    return _ANSI_RE.sub(b"", data)


def _normalize_output(data: bytes, remove_jit_evidence: bool) -> bytes:
    normalized = _without_ansi(data)
    if remove_jit_evidence:
        normalized = _JIT_EVIDENCE_RE.sub(b"", normalized)
    return _TIMING_RE.sub(b"", normalized)


def _jit_count(result: _RunResult) -> Tuple[Optional[int], Optional[str]]:
    counts = _JIT_EVIDENCE_RE.findall(_without_ansi(result.stdout))
    counts += _JIT_EVIDENCE_RE.findall(_without_ansi(result.stderr))
    if len(counts) != 1:
        return None, "expected exactly one JIT compilation evidence line, found {}".format(len(counts))
    count = int(counts[0])
    if count <= 0:
        return None, "required JIT compiled no functions"
    return count, None


def _crash_reason(returncode: Optional[int]) -> Optional[str]:
    if returncode is None:
        return None
    if returncode < 0:
        try:
            name = signal.Signals(-returncode).name
        except ValueError:
            name = "signal {}".format(-returncode)
        return "terminated by {}".format(name)
    if os.name == "nt" and returncode >= 0x80000000:
        return "terminated with Windows status 0x{:08x}".format(returncode)
    if os.name != "nt" and 128 <= returncode <= 255:
        return "terminated with crash-like exit status {}".format(returncode)
    return None


def _observation(result: _RunResult, jit: bool) -> _Observation:
    assert result.returncode is not None
    return _Observation(
        returncode=result.returncode,
        stdout=_normalize_output(result.stdout, remove_jit_evidence=jit),
        stderr=_normalize_output(result.stderr, remove_jit_evidence=jit),
        mutations=result.mutations,
    )


def _decode(data: bytes) -> str:
    return data.decode("utf-8", errors="backslashreplace")


def _output_diff(test: Path, stream: str, interpreted: bytes, jitted: bytes) -> str:
    return "".join(
        difflib.unified_diff(
            _decode(interpreted).splitlines(keepends=True),
            _decode(jitted).splitlines(keepends=True),
            fromfile="{}:interpreter {}".format(test, stream),
            tofile="{}:jit {}".format(test, stream),
        )
    )


def _divergence_report(test: Path, interpreted: _Observation, jitted: _Observation) -> str:
    details = []
    if interpreted.returncode != jitted.returncode:
        details.append(
            "exit status: interpreter={} jit={}".format(
                interpreted.returncode, jitted.returncode
            )
        )
    if interpreted.stdout != jitted.stdout:
        details.append(_output_diff(test, "stdout", interpreted.stdout, jitted.stdout))
    if interpreted.stderr != jitted.stderr:
        details.append(_output_diff(test, "stderr", interpreted.stderr, jitted.stderr))
    if interpreted.mutations != jitted.mutations:
        details.append(
            "filesystem mutations:\ninterpreter={}\njit={}".format(
                json.dumps(interpreted.mutations, sort_keys=True, indent=2),
                json.dumps(jitted.mutations, sort_keys=True, indent=2),
            )
        )
    return "\n".join(details)


def _command_text(result: _RunResult) -> str:
    return shlex.join(result.command)


def _check_test(executable: Path, test: Path, timeout: float) -> Optional[str]:
    interpreted = _run(executable, test, jit=False, timeout=timeout)
    jitted = _run(executable, test, jit=True, timeout=timeout)

    for mode, result in (("interpreter", interpreted), ("jit", jitted)):
        if result.timed_out:
            return "{} timed out after {}s: {}".format(mode, timeout, _command_text(result))
        crash = _crash_reason(result.returncode)
        if crash is not None:
            return "{} {}: {}".format(mode, crash, _command_text(result))

    _, evidence_error = _jit_count(jitted)
    if evidence_error is not None:
        return "{}: {}".format(evidence_error, _command_text(jitted))

    interpreted_observation = _observation(interpreted, jit=False)
    jitted_observation = _observation(jitted, jit=True)
    if interpreted_observation != jitted_observation:
        return _divergence_report(test, interpreted_observation, jitted_observation)
    return None


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Run selected tests in both modes and report semantic divergence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="path or command name for the Lightning executable")
    parser.add_argument(
        "tests",
        nargs="*",
        help=".li test files or directories (default: every tests/*.li)",
    )
    parser.add_argument(
        "--timeout",
        type=_positive_timeout,
        default=60.0,
        help="per-process timeout in seconds (default: 60)",
    )
    arguments = parser.parse_args(argv)

    try:
        executable = _resolve_executable(arguments.executable)
        tests = _collect_tests(arguments.tests)
    except ValueError as error:
        parser.error(str(error))

    failures = 0
    for test in tests:
        try:
            failure = _check_test(executable, test, arguments.timeout)
        except (OSError, shutil.Error) as error:
            failure = "harness error: {}".format(error)
        if failure is None:
            print("PASS {}".format(test))
        else:
            failures += 1
            print("FAIL {}\n{}".format(test, failure), file=sys.stderr)

    if failures:
        print("{} of {} differential tests failed".format(failures, len(tests)), file=sys.stderr)
        return 1
    print("{} differential tests passed".format(len(tests)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
