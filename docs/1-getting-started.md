# 1. Getting Started

Lightning Script (`li`) is a fast, embeddable scripting language designed for predictable native applications. Its dynamic values use a compact NaN-boxed representation, heap objects are managed by immediate deterministic reference counting, and hot functions can be compiled by the native method JIT. The same virtual machine is available through the command-line executable, the C ABI in `<lightning.h>`, and the C++20 RAII API in `<lightning.hpp>`.

Lightning is intentionally small enough to embed while still providing lexical closures, classes and structs, exceptions, deterministic cleanup, stackful coroutines, and a practical standard library.

## Build from source

### Requirements

A native build requires:

- CMake 3.15 or newer;
- a C++20 compiler;
- Ninja (recommended, though another CMake generator may be used);
- the platform thread library, found automatically by CMake on native targets.

The native JIT is available on supported 64-bit x86 and ARM targets. The x86-64 backend uses the repository's pinned Zydis and Zycore submodules, so initialize submodules before configuring a JIT build. The ARM64 backend does not depend on Zydis.

### Native build with the JIT

```sh
git clone --recurse-submodules https://github.com/li-script/lightning.git
cd lightning

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLI_JIT=ON \
  -DLIGHTNING_TESTS=ON
cmake --build build
```

If the repository was cloned without submodules, initialize them separately:

```sh
git submodule update --init --recursive
```

`LI_JIT=ON` selects the native backend for the configure target. Configuration fails rather than pretending JIT support exists when the target has no backend.

### Interpreter-only build

Set `LI_JIT=OFF` to exclude the native JIT and, on x86-64, the Zydis dependency:

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLI_JIT=OFF \
  -DLIGHTNING_TESTS=ON
cmake --build build
```

Useful configure options include:

| Option | Typical default | Purpose |
|---|---:|---|
| `LI_JIT` | `ON` on supported native targets | Build the target's native JIT backend. |
| `LIGHTNING_TESTS` | `ON` | Build and register the test suites. |
| `LI_FAST_MATH` | `OFF` | Permit relaxed floating-point optimizations. |
| `LI_SAFE_STACK` | `ON` | Enable VM stack-bounds checks. |

The default `LI_FAST_MATH=OFF` build preserves the numeric behavior described in [Basic Concepts](2-basic-concepts.md), including IEEE edge cases. `LI_FAST_MATH=ON` is an explicit relaxed mode: a backend may reassociate arithmetic, contract multiply and add, use reciprocal approximations, flush subnormal values, and lower `%` to `x - trunc(x / y) * y`. For finite normal inputs with a finite normal result, the result remains a correctly signed finite approximation within that backend's documented tolerance. The result bits are unspecified for NaN, infinity, subnormal, signed-zero, overflow, and zero-divisor cases.

Fast math does not relax operand checks, truthiness, conversions, equality or ordering, table-key hashing, control flow, ownership, exceptions, or memory safety. The imported `math` module exposes `math.fast`, so a script or embedder can reject a relaxed build when reproducible IEEE edge behavior is required.

The resulting executable is `build/li` for a single-configuration Ninja build. Multi-configuration generators may place it in a configuration-specific subdirectory.

## Run a script

Create `hello.li`:

```li
const name = "Lightning"
print(`Hello from {name}!`)
```

Run it with:

```sh
./build/li hello.li
```

The general command form is:

```text
./build/li [options] [script]
```

If `script` is omitted, the native executable starts an interactive REPL. Use `--` when a script path begins with `-`:

```sh
./build/li -- -example.li
```

### Execution options

| Option | Behavior |
|---|---|
| `--jit` | Require eager native compilation. A compilation failure is reported instead of silently falling back to interpretation. |
| `--jit=auto` | Start in the interpreter, count hot calls and loop backedges, and compile eligible hot functions with interpreter fallback. This is the default policy of a JIT-enabled build. |
| `--jit=off` | Disable tiering and execute only in the interpreter. This is useful as a reference mode. |
| `--metrics` | Emit one `Metrics: <JSON>` record on standard error with parse, JIT, allocation, and timing counters. A script path is required. |
| `--benchmark-runs=N` | Invoke the loaded script entry `N` times in the same VM, separating the first call from later warm calls. `N` must be from 1 through 1,000,000. A script path is required. |
| `--verbose-errors` | Include the full available diagnostic context for errors, including expanded instantiation context where applicable. |

Examples:

```sh
# Interpreter reference run
./build/li --jit=off docs/examples/01_hello.li

# Strict JIT run
./build/li --jit docs/examples/02_fibonacci.li

# Automatic tiering with metrics and repeated in-process calls
./build/li --jit=auto --metrics --benchmark-runs=20 docs/examples/02_fibonacci.li
```

A build made with `LI_JIT=OFF` rejects `--jit`, because required JIT execution cannot be satisfied. `--jit=auto` remains interpreted in an interpreter-only build.

The native backend is a method JIT: it compiles and caches whole functions rather than recording traces. Automatic mode counts calls and taken loop backedges. A hot call can compile a later invocation, while a hot backedge can enter compiled code by on-stack replacement without repeating the loop body's effects. Failed type guards return to the generic operation or deoptimize to the same interpreter position; they never trap. A function the backend cannot compile stays interpreted in automatic mode, while required `--jit` execution reports the compilation failure before running that function.

## Imports and module loading

An import names either an installed module or a source module resolved by the VM's import loader. The ordinary form requires the module; `import?` is the optional form and produces `nil` when loading fails. An `as` clause chooses the local binding name.

Parsing recognizes imports and produces a module unit, but it does not invoke loaders or run module code. Loading begins only when execution reaches the import, with allocation, exception handling, and lifetime management fully active. Modules are cached by canonical identity, so aliases or paths that resolve to the same module share one record and one exports table.

A module moves through three states:

1. The first import publishes a `loading` record and its exports table, then invokes the loader once.
2. A cyclic import receives that same table. An export not published yet reads as `nil`; the cycle never starts a second loader.
3. Normal completion freezes the exports table, marks the record `initialized`, and returns it. Later imports return the same table without rerunning top-level code.
4. An exception clears the partial exports table, releases temporary load roots, stores the exception, and marks the record `failed`. A later required import rethrows the stored failure; `import?` returns `nil`. Neither form reruns the failed loader.

After active calls return, VM shutdown releases initialized modules and cached failures in reverse initialization order.

## Browser playground and REPL

Lightning also builds with Emscripten. The browser distribution consists of `li.js` and `li.wasm` plus the files in `web/`. The included page provides a script editor, curated examples, an output terminal, and a one-line REPL backed by a persistent WebAssembly VM. The **Reset VM** action discards that VM and creates a clean one.

A local WebAssembly build can be produced with an activated Emscripten SDK:

```sh
emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLI_JIT=OFF \
  -DLIGHTNING_TESTS=OFF
cmake --build build-wasm
```

Copy `build-wasm/li.js` and `build-wasm/li.wasm` beside `web/index.html`, `web/app.js`, and `web/style.css`, then serve that directory through a local HTTP server. Browsers normally do not load WebAssembly correctly from a `file://` URL.

The WebAssembly build is interpreter-only. Editor runs and REPL submissions share state until the VM is reset, which makes the page useful both as a playground and as an interactive learning environment.

## Next steps

- Continue with [Basic Concepts](2-basic-concepts.md).
- See [The Strict Tier](7-strict-tier.md) for checked declarations, sized types, generics, static evaluation, attributes, and structs.
- Run the examples under [`docs/examples/`](examples/).
- See [Embedding API](12-embedding-api.md) for the C and C++ host APIs.
