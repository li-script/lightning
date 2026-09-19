<p align="center">
  <a href="https://li-script.github.io/lightning/">
    <img width="128" height="128" alt="Lightning Script" src="web/logo.svg">
  </a>

  <h1 align="center">Lightning Script</h1>

  <p align="center">
    <a href="https://github.com/li-script/lightning/actions/workflows/cmake.yml">
      <img alt="CI" src="https://github.com/li-script/lightning/actions/workflows/cmake.yml/badge.svg"/>
    </a>
    <a href="https://li-script.github.io/lightning/docs/">
      <img alt="Documentation" src="https://img.shields.io/badge/docs-manual-111113?style=flat-square"/>
    </a>
    <a href="https://li-script.github.io/lightning/">
      <img alt="Playground" src="https://img.shields.io/badge/try-playground-111113?style=flat-square"/>
    </a>
  </p>
</p>

Lightning Script is a work-in-progress scripting language with a modern syntax, uncompromising performance, and a public [C and C++ embedding API](docs/12-embedding-api.md). The [manual](docs/INDEX.md) is published at [li-script.github.io/lightning/docs](https://li-script.github.io/lightning/docs/), and the in-browser [playground](https://li-script.github.io/lightning/) runs the WebAssembly interpreter.

## Building

Lightning requires CMake 3.15 or newer, Ninja, a C++20 compiler, and [`just`](https://github.com/casey/just); every command below is a `just` recipe (`just --list` shows them all), and each recipe prints the underlying `cmake`, `ctest`, or `li` invocation it runs. Build options are set at configure time and can be passed to `just build` or `just configure` as extra `-D` flags:

| Option | Default | Purpose |
|---|---:|---|
| `LI_JIT` | `ON` on supported native x86-64 and ARM64 targets | Build the native JIT backend selected for the configure target. |
| `LIGHTNING_TESTS` | `ON` | Build and register the script and native utility test suites. |
| `LI_FAST_MATH` | `OFF` | Permit relaxed floating-point optimizations. |
| `LI_SAFE_STACK` | `ON` | Enable VM stack bounds checks. |

Build the default configuration (Release, JIT on where a backend exists, tests on) with `just build`. Configure an interpreter-only build explicitly, then build it:

```sh
just build Release -DLI_JIT=OFF
```

### Native JIT builds

The x86-64 JIT depends on the repository's pinned Zydis submodule (`73d7dbb3c3979aa2ecabd1df01a2d5454e423fe7`) and its pinned Zycore dependency (`8c30dbe5708638cf389d4fbc7aff808cf59e1a07`). Initialize submodules recursively before configuring a fresh checkout; do not replace the pinned revisions with newer upstream heads:

```sh
just submodules
just build Release -DLI_JIT=ON
```

`LI_JIT=OFF` excludes the JIT sources and Zydis entirely. ARM64 JIT builds do not use Zydis. Setting `LI_JIT=ON` on an architecture without a backend is a configure error; it never manufactures a successful JIT run.

### macOS / Apple Silicon

ARM64 macOS has an A64 JIT backend. Use `LI_JIT=OFF` when an interpreter-only build is required; `--jit` remains strict and does not silently mean interpreted execution.

`just configure` works around the system Command Line Tools compiler/linker combination automatically: when Homebrew LLVM and the macOS 26 Command Line Tools SDK are both present and no `CXX`/`CMAKE_CXX_COMPILER` is set, it configures with `/opt/homebrew/opt/llvm/bin/clang++`, `-DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk`, and `-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld`. This requires CMake, Ninja, and LLVM from Homebrew (including `lld`). If your Command Line Tools provide a different SDK version, pass the SDK explicitly:

```sh
just configure Release -DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
just build
```

### Install and consume the C package

For an interpreter-only package without a Zydis dependency:

```sh
just build Release -DLI_JIT=OFF -DLIGHTNING_TESTS=OFF
just install /path/to/lightning-prefix
```

This installs the `li` executable, the platform's static `lightning` library, `<lightning.h>`, `<lightning.hpp>`, and the CMake package. A minimal external project whose `main.c` contains the [C example](docs/12-embedding-api.md#minimal-c-program) can consume it with:

```cmake
cmake_minimum_required(VERSION 3.15)
project(embed-example LANGUAGES C CXX)

find_package(lightning CONFIG REQUIRED)

add_executable(embed-example main.c)
target_link_libraries(embed-example PRIVATE lightning::lightning)
```

The consuming project is built with its own CMake invocation, pointing `CMAKE_PREFIX_PATH` at the installed package:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/lightning-prefix
cmake --build build
./build/embed-example
```

An x86-64 package built with `LI_JIT=ON` also requires its matching installed `Zydis::Zydis` package to be discoverable. An `LI_JIT=OFF` package has no Zydis dependency.

## Running

With no JIT flag, a JIT-enabled build starts in the interpreter and compiles hot functions automatically. Use `--jit=off` for a non-tiering interpreter reference:

```sh
just run --jit=off tests/function.li
```

On a supported JIT build, `--jit` requires eager native compilation and reports a compilation failure instead of silently interpreting an unsupported function. `--jit-verbose` has the same requirement and prints compiler diagnostics. `--jit=auto` explicitly selects hot-call/backedge counting with interpreter fallback; compiling at a backedge does not restart the current invocation or replay its side effects. Interpreter-only builds stay interpreted.

```sh
just run --jit tests/function.li
just run --jit-verbose tests/function.li
just run --jit=auto tests/function.li
```

`just run` builds first when `build/li` is missing; `just repl` starts the interactive REPL. Request explicit runtime measurements with `--metrics`. It emits one `Metrics: <JSON>` line on stderr. `--benchmark-runs=N` calls the loaded entry `N` times in the same VM; the first call and later warm calls are reported separately.

```sh
just metrics tests/function.li            # --jit=auto, 20 same-VM calls
just metrics tests/function.li 20 on      # --jit
just metrics tests/function.li 20 off     # --jit=off
```

The strict JIT command is available only when the configured target has a native backend. Backend availability is not itself evidence that a target has passed the benchmark or correctness corpus.

Parser and strict-mode diagnostics print `[file:line:col] message`, the offending source line, and a caret underline. Generic instantiation errors report the innermost failed requirement plus one call-site frame; `--verbose-errors` prints the whole instantiation chain.

## Strict tier

`[[strict]]` is an attribute: as the leading statement it applies to the file, on a `fn`, `class`, or `struct` to that declaration. Strict code compiles to the same bytecode and runs on the same VM and JIT; the parser owns a static type environment, so every binding, parameter, and return has a resolved type before code generation and incompatible operations are parse errors. Dynamic code calling a strict function passes through the normal call path and gets a runtime type error at the boundary; strict code calling dynamic code checks the returned value once and treats it as typed afterwards.

```
[[strict]] struct Vec3<T> {
   x: T = {}; y: T = {}; z: T = {}
   new!(x: T, y: T, z: T) { self.x = x; self.y = y; self.z = z }
   add!(other: Self) -> Self { Vec3<T>::new(self.x + other.x, self.y + other.y, self.z + other.z) }
   reduce_add() -> T { self.x + self.y + self.z }
}

[[strict]] fn sum_lengths(points: Vec3<f32>[], count: i32) -> f32 {
   let total: f32 = 0f32
   let index: i32 = 0i32
   while index < count {
      let value = points[index]
      total += value.x * value.x + value.y * value.y + value.z * value.z
      index += 1i32
   }
   total
}
```

- Types: `i8..i64`, `u8..u64`, `f32`, `f64`, `bool`, `string`, class and struct names, `Self`, `T?`, `view T`, `weak T`, `T[]`, `T[N]`, `type Alias = T`. Typed literals: `10i32`, `3f32`, `10u`, digit separators `100'000`.
- `struct` is a class with value semantics: copied on assignment and argument passing in strict code, fieldwise `==`, inline storage in `typed.struct_array(Class, n)` (reads materialize copies, writes copy fields in). A struct that escapes to dynamic code is a box.
- Generics: `fn f<T, U>(...)`, `struct S<T>`, constraints `<T: f32>` (most specific candidate wins, ambiguity is an error), unannotated strict parameters are auto-templated, variadics `Tx...` with `#Tx...`. Instantiation binds types forward from the call site, reparses the declaration span, and caches per type tuple; `reflect.instantiations(f)` lists `Inst-OK` / `InstCache-OK` / `Inst-Failed` records.
- Compile-time evaluation: `static if/else`, `static assert`, `static warn`, `static panic`, `static require` (cancels a candidate SFINAE-style), `typeid(T)`, `typeof(expr)`, `T is U`.
- `union` fields inside strict structs share storage; reading a field other than the most recently assigned one in the same function is an error.
- Attributes are general: `[[name, name(literal, ...)]]` on any declaration, exposed as a frozen table through `reflect.attributes(value)`.

Fixtures: `tests/strict-basics.li`, `tests/structs.li`, `tests/strict-generics.li`, `tests/strict-static.li`, `tests/v2-positive.li`, `tests/v2-negative.li`, `tests/strict-vec3.li`, `tests/diagnostics.li`.

## Embedding

The installed library exposes the public C ABI in `<lightning.h>` and the optional C++20 RAII wrapper in `<lightning.hpp>`. See the [embedding chapter](docs/12-embedding-api.md) for lifecycle, ownership, callbacks, errors, threading, and the trusted-script security boundary. Public API calls serialize VM entry; callers must still synchronize close against new calls.

## Benchmarks and target status

The deterministic corpus contains `binarytrees`, `fib`, `nbody`, `table-heavy`, `string-heavy`, `closure-heavy`, and `alloc-heavy`, plus focused `escape`, `inline`, `integer-ranges`, and `inline-cache` workloads. Run the corpus in a selected mode and write the JSON result explicitly:

```sh
# Five measured child processes. Each loads the workload and calls it once.
just bench interpreter 5 > results.json

# Discard two whole child processes, then measure five fresh child processes.
just bench interpreter 5 --warmups 2 > results-process-warmup.json

# Each measured child loads once and calls the same entry 20 times in one VM.
just bench interpreter 5 --in-process-runs 20 > results-in-process.json

# An enabled native JIT build, with the same in-process call model:
just bench jit 5 --in-process-runs 20 > results-jit.json
```

`just bench` runs `benchmarks/run.py` against `build/li` and supplies `--execution-kind native --build-label release` unless the arguments already name them. The runner always passes the real `--metrics` and `--benchmark-runs=N` executable flags. Each run records the process-inclusive wall clock separately from `parse_compile_ns`, actual JIT compilation time, emitted code bytes, compiled function count, runtime allocation/retain/release deltas, spill slots, first-call latency, and later same-VM call latencies. Warm throughput is derived only when `--in-process-runs` is greater than one; `--warmups` discards entire fresh processes and does not create warm same-VM samples. A missing, malformed, negative, or inconsistent metrics record makes the run unsuccessful, as does any script failure.

Checked-in baselines cover native macOS ARM64 in all three modes (`baseline-macos-arm64.json` interpreter, `-jit.json`, `-auto.json`) and Linux x86-64 translated on Apple Silicon (interpreter). They are machine-specific observations, not cross-runtime performance claims; the historical 2022 numbers embedded in the JSON are explicitly non-comparable. On the recorded ARM64 machine the required JIT beats the interpreter on every workload (fib 260 vs 327 ms warm, binarytrees 367 vs 426, nbody 6.9 vs 11.0, integer-ranges 3.6 vs 92.6) and its retain counts are at or below the interpreter's. There is not yet a physical x86-64 baseline.

Target qualification is correspondingly narrow: only recorded configurations have benchmark evidence. The build supports native x86-64 JIT targets on Windows, macOS, and Linux and native ARM64 JIT targets on macOS and Linux, and the corpus passes on native ARM64 and translated x86-64, but backend availability is not benchmark qualification. WebAssembly, Windows ARM64, and Apple `arm64e` remain interpreter-only. Do not infer qualification for any unrecorded target from a generic architecture name.

## Testing

`just test` runs the default CTest selection (interpreter, util, backend, ownership, hardening, embed, and IR labels); `just test-all` runs every registered test. The interpreter suite is available on every native build:

```sh
just test-interpreter
```

The native `util` suite checks recursive exclusion, cross-translation-unit ownership, publication, encoders, and contended wakeups:

```sh
just test-util
```

On builds configured with `LI_JIT=ON`, the `jit` label runs every script fixture again under strict `--jit`, and the differential suite compares interpreter and JIT output for the whole corpus:

```sh
just test-jit
just test-differential
```

A strict JIT test is not considered compiled merely because its script completed: at least one successful native compilation is required. Extra CTest arguments pass through, for example `just test-jit -R strict`.

`just format` applies `clang-format` to the C and C++ sources, `just lint` checks formatting without modifying files, and `just docs` renders the manual under `docs/` into `build/site/docs`.
