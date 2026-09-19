# Justfile for lightning
# Run `just` or `just --list` to view available recipes.

build_dir := "build"
build_type := "Release"

# Show available recipes
default:
    @just --list

# Initialize the pinned Zydis/Zycore submodules needed by x86-64 JIT builds
submodules:
    git submodule update --init --recursive

# Configure the CMake build
configure type=build_type *flags:
    #!/usr/bin/env bash
    set -euo pipefail
    extra_flags=()
    if [ -z "${CMAKE_CXX_COMPILER:-}" ] && [ -z "${CXX:-}" ]; then
        if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
            if [ -x "/opt/homebrew/opt/llvm/bin/clang++" ] && [ -d "/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk" ]; then
                extra_flags+=(
                    "-DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang"
                    "-DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++"
                    "-DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk"
                    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
                )
            fi
        fi
    fi
    cmake -S . -B {{build_dir}} -G Ninja \
        -DCMAKE_BUILD_TYPE="{{type}}" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DLIGHTNING_TESTS=ON \
        "${extra_flags[@]}" \
        {{flags}}

# Build the project (auto-configures if not yet configured)
build type=build_type *flags:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -f "{{build_dir}}/build.ninja" ] && [ ! -f "{{build_dir}}/Makefile" ] && [ ! -f "{{build_dir}}/CMakeCache.txt" ]; then
        just configure "{{type}}" {{flags}}
    fi
    cmake --build {{build_dir}} --config "{{type}}" --parallel

# Clean build directory
clean:
    rm -rf {{build_dir}}

# Run the lightning binary (builds if missing)
run *args:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -x "{{build_dir}}/li" ]; then
        just build
    fi
    "{{build_dir}}/li" {{args}}

# Run the interactive REPL
repl *args:
    just run {{args}}

# Run a script with --metrics and N same-VM calls (mode: off, auto, or on)
metrics script runs="20" mode="auto":
    #!/usr/bin/env bash
    set -euo pipefail
    case "{{mode}}" in
        on) flag="--jit" ;;
        off) flag="--jit=off" ;;
        auto) flag="--jit=auto" ;;
        *) echo "mode must be on, off, or auto" >&2; exit 2 ;;
    esac
    just run "$flag" --metrics "--benchmark-runs={{runs}}" "{{script}}"

# Run test suite via ctest (default: interpreter, util, backend, ownership, hardening, embed, ir)
test *args:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -f "{{build_dir}}/build.ninja" ] && [ ! -f "{{build_dir}}/Makefile" ]; then
        just build
    fi
    if [ -z "{{args}}" ]; then
        ctest --test-dir {{build_dir}} --output-on-failure -L "interpreter|util|backend|ownership|hardening|embed|ir" --timeout 60
    else
        ctest --test-dir {{build_dir}} --output-on-failure {{args}}
    fi

# Run interpreter test suite
test-interpreter *args:
    ctest --test-dir {{build_dir}} --output-on-failure -L interpreter {{args}}

# Run strict JIT test suite
test-jit *args:
    ctest --test-dir {{build_dir}} --output-on-failure -L jit {{args}}

# Run native utility probes (locks, ownership, encoders, embedding)
test-util *args:
    ctest --test-dir {{build_dir}} --output-on-failure -L util {{args}}

# Run differential test suite
test-differential *args:
    ctest --test-dir {{build_dir}} --output-on-failure -L differential {{args}}

# Run all tests without label filters
test-all *args:
    ctest --test-dir {{build_dir}} --output-on-failure {{args}}

# Format C and C++ source code using clang-format
format:
    find src include tests repl.cpp -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) -exec clang-format -i {} +

# Check C and C++ source code formatting without modifying files
format-check:
    find src include tests repl.cpp -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' \) -exec clang-format --dry-run --Werror {} +

# Lint codebase (checks formatting)
lint: format-check

# Run clang-tidy on specified files (default: repl.cpp)
tidy file="repl.cpp":
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -f "{{build_dir}}/compile_commands.json" ]; then
        just configure
    fi
    TIDY_BIN="clang-tidy"
    if ! command -v "$TIDY_BIN" &>/dev/null && [ -x "/opt/homebrew/opt/llvm/bin/clang-tidy" ]; then
        TIDY_BIN="/opt/homebrew/opt/llvm/bin/clang-tidy"
    fi
    "$TIDY_BIN" -p {{build_dir}} --checks='bugprone-*,performance-*,-clang-analyzer-cplusplus.NewDeleteLeaks' {{file}}

# Install lightning to prefix (default: build/install)
install prefix="build/install" type=build_type:
    cmake --install {{build_dir}} --prefix "{{prefix}}" --config "{{type}}"

# Run benchmarks using benchmarks/run.py (mode: interpreter, jit, auto)
bench mode="interpreter" runs="5" *args:
    #!/usr/bin/env bash
    set -euo pipefail
    if [ ! -x "{{build_dir}}/li" ]; then
        just build
    fi
    extra_args=()
    if [[ ! " {{args}} " =~ " --execution-kind" ]]; then
        extra_args+=("--execution-kind" "native")
    fi
    if [[ ! " {{args}} " =~ " --build-label" ]]; then
        extra_args+=("--build-label" "release")
    fi
    python3 benchmarks/run.py "{{build_dir}}/li" "{{mode}}" "{{runs}}" "${extra_args[@]}" {{args}}

# Build documentation using tools/build_docs.py
docs out="build/site/docs":
    python3 tools/build_docs.py --out "{{out}}"
