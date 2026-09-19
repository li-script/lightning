# Lightning Script Documentation

This manual follows the broad organization of the Lua 5.4 Reference Manual: language and execution model first, standard libraries second, and the host embedding interface last. The chapters are intended to be read in order, while each library chapter also serves as a reference.

## Part I — The Language

### [1. Getting Started](1-getting-started.md)

Installing or building Lightning, running the `li` command, the source-file model, the REPL, the first program, imports, and the conventions used throughout this manual.

### [2. Basic Concepts](2-basic-concepts.md)

Values and types, truthiness, variables, lexical scope, deterministic reference counting, equality, indexing, iteration, errors, and the execution model shared by the interpreter and JIT.

### [3. Language Syntax](3-language-syntax.md)

Lexical rules, comments, numeric and string literals, template and raw strings, operators, precedence, table and array construction, declarations, annotations, and statement termination.

### [4. Control Flow](4-control-flow.md)

Conditional execution, `while` and `for`, ranges and collection iteration, `break`, `leave`, `return`, exceptions, `try`/`catch`, `throw`, and deterministic `defer` cleanup.

### [5. Functions and Closures](5-functions-and-closures.md)

Named and anonymous functions, parameters and defaults, optional and variadic calls, return values, `self`, UFCS calls, capture-by-value closures, recursion, and callable values.

### [6. Structs and Classes](6-structs-and-classes.md)

Struct and class declarations, fields, construction and destruction hooks, methods, properties, inheritance, dynamic hooks, operators, object lifetime, and native-layout fields.

### [7. The Strict Tier](7-strict-tier.md)

Attributes and `[[strict]]`, sized and typed values, forward type inference, compile-time diagnostics, the checked boundary with dynamic code, struct copy rules and unions, generics with per-type instantiation, `static` evaluation, and guard-free compiled numeric loops.

## Part II — Standard Libraries

### [8. Built-in Functions](8-stdlib-builtins.md)

Core operations available through `builtin`, including printing, assertions, type inspection, explicit conversions, dynamic loading and evaluation, and other universally available services.

### [9. Collections](9-stdlib-collections.md)

Tables, arrays, ranges, map/filter/reduce operations, iterators, mutation rules, presence and deletion, copying, sorting, and collection-oriented programming patterns.

### [10. Mathematics and Typed Values](10-stdlib-math-and-typed.md)

The `math` module, IEEE 754 behavior, random numbers, numeric utilities, fixed-width integer and floating types, typed arrays, and native numeric storage.

### [11. System, Concurrency, and Runtime Libraries](11-stdlib-system-and-concurrency.md)

File-system access, high-resolution time, stackful coroutines, process-wide shared values, locks and atomic updates, weak references, native vectors, reflection, debugging, and JIT controls.

## Part III — The Host Interface

### [12. Embedding API](12-embedding-api.md)

Embedding architecture, the `<lightning.h>` C ABI, the `<lightning.hpp>` C++20 RAII wrapper, value ownership and NaN-boxing, native registration, callback signatures, cross-VM transfer, hooks, exception conversion, userdata, threading, stack safety, and the trusted-script security boundary.

## Runnable examples

Every example below is a complete Lightning source file. From the repository root, run one with:

```sh
./build/li docs/examples/01_hello.li
```

| Example | Demonstrates |
|---|---|
| [`01_hello.li`](examples/01_hello.li) | A minimal program, bindings, output, and the command-line workflow. |
| [`02_fibonacci.li`](examples/02_fibonacci.li) | Functions, recursion, numeric expressions, and assertions. |
| [`03_strings.li`](examples/03_strings.li) | Quoted, template, escaped, and raw strings. |
| [`04_control_flow.li`](examples/04_control_flow.li) | Conditions, loops, ranges, iteration, and structured exits. |
| [`05_closures.li`](examples/05_closures.li) | Anonymous functions and capture-by-value closure state. |
| [`06_structs_classes.li`](examples/06_structs_classes.li) | User-defined data, constructors, methods, properties, and hooks. |
| [`07_collections.li`](examples/07_collections.li) | Arrays, tables, iteration, mapping, filtering, and reduction. |
| [`08_typed_arrays.li`](examples/08_typed_arrays.li) | Fixed-width values and compact typed-array storage. |
| [`09_coroutines.li`](examples/09_coroutines.li) | Coroutine creation, bidirectional resume/yield, status, close, and cleanup. |
| [`10_math_vectors.li`](examples/10_math_vectors.li) | Mathematics and native `f32` vector operations. |
| [`11_defer_errors.li`](examples/11_defer_errors.li) | Exceptions and LIFO deferred cleanup on return and unwind. |
| [`12_shared_concurrency.li`](examples/12_shared_concurrency.li) | Shared values, lock blocks, atomic updates, and cooperative workers. |
