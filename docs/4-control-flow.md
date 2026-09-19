# 4. Control Flow

Lightning uses brace-delimited blocks for conditionals, loops, cleanup, and exception handling. Only `false` and `nil` are false in a condition; every other value is truthy.

## Conditionals

### `if`

An `if` executes its block when the condition is truthy:

```li
const temperature = -4
if temperature < 0 {
    print("freezing")
}
```

Parentheses around the condition are optional.

### `else if` and `else`

Chain mutually exclusive cases with `else if`, and use `else` as the fallback:

```li
fn classify(score) {
    if score >= 90 {
        "excellent"
    } else if score >= 70 {
        "passing"
    } else {
        "needs work"
    }
}
```

A function returns its last expression when execution reaches the end, so the example does not need an explicit `return` in every branch.

## Loops

### `while`

A `while` loop evaluates its condition before each iteration:

```li
let remaining = 3
while remaining > 0 {
    print(remaining)
    remaining -= 1
}
```

If the condition is false initially, the body does not run.

### Range loops

The half-open range `start..end` includes `start` and excludes `end`:

```li
for index in 0..5 {
    print(index) // 0, 1, 2, 3, 4
}
```

The inclusive form `start..=end` includes the end value:

```li
for index in 1..=3 {
    print(index) // 1, 2, 3
}
```

An omitted end creates an open range. Such a loop must normally exit with `break`, `return`, or `throw`:

```li
for attempt in 0.. {
    if attempt == 3 {
        break
    }
}
```

### Collection loops

The two-variable form receives each key and value:

```li
const items = ["zero", "one", "two"]
for index, value in items {
    print(index, value)
}

const capitals = {france: "Paris", japan: "Tokyo"}
for country, city in capitals {
    print(country, city)
}
```

The one-variable form receives values from a range or generic iterator:

```li
for value in 10..13 {
    print(value)
}
```

Do not structurally mutate the collection being traversed. Collection iterators detect mutation and raise a catchable error rather than silently skipping or repeating entries.

## Loop control

### `continue`

`continue` ends the current iteration and begins the next one:

```li
for value in 0..10 {
    if value % 2 != 0 {
        continue
    }
    print(value)
}
```

Any `defer` registered in the loop-body scope runs before control advances to the next iteration.

### `break`

`break` exits the nearest loop:

```li
let found = nil
for value in [3, 5, 8, 13] {
    if value % 2 == 0 {
        found = value
        break
    }
}
```

Loops are expressions and may be exited with a value:

```li
const first_even = for value in [3, 5, 8, 13] {
    if value % 2 == 0 {
        break value
    }
}
```

When no value is produced, the loop expression evaluates to `nil`.

### `leave`

`leave` exits the nearest brace-delimited block expression, independently of any surrounding loop. Like `break`, it may supply the block's value:

```li
const answer = ({
    leave 42
})

assert(answer == 42)
```

Any `defer` in a scope crossed by `leave` runs before the containing expression continues.

## Function exit with `return`

`return` immediately exits the current function. It may return a value or return `nil` when no value is supplied:

```li
fn find(values, wanted) {
    for index, value in values {
        if value == wanted {
            return index
        }
    }
    return nil
}
```

Without an explicit `return`, the function's final expression is its result:

```li
fn square(value) {
    value * value
}
```

Before control leaves the function, every active scope is unwound and its deferred cleanup runs.

## Scope guards with `defer`

`defer { ... }` registers a cleanup block in the current lexical scope. Deferred blocks run in last-in, first-out order when that scope exits:

```li
fn process() {
    defer { print("release connection") }
    defer { print("close temporary file") }

    print("work")
}
```

The output order is:

```text
work
close temporary file
release connection
```

Cleanup runs on every way out of the scope:

- normal fallthrough;
- `continue`, `break`, or `leave` crossing the scope;
- `return`;
- exception unwinding after `throw` or a runtime error.

Deferred bodies can read captured local bindings at exit time:

```li
fn report() {
    let status = "starting"
    defer { print(`final status: {status}`) }

    status = "complete"
}
```

Use `defer` immediately after acquiring a resource or entering a state that must be undone. This keeps acquisition and cleanup adjacent and provides RAII-style control in script code.

## Exceptions

### `throw`

`throw` raises a script value and begins unwinding:

```li
fn divide(left, right) {
    if right == 0 {
        throw {kind: "division-by-zero", left: left}
    }
    left / right
}
```

Strings, numbers, tables, and other values can all be thrown. Structured table values are useful when handlers need fields such as an error kind, operation, or input.

### `try` and `catch`

A `catch` block handles an exception raised from its corresponding `try` block:

```li
try {
    throw {kind: "division-by-zero", left: 10}
} catch error {
    assert(error.kind == "division-by-zero")
    assert(error.left == 10)
}
```

The identifier after `catch` is a new lexical binding containing the thrown value. When the value is not needed, omit the binding:

```li
fn risky_operation() {
    throw "unavailable"
}

try {
    risky_operation()
} catch {
    print("operation failed")
}
```

To perform local work and propagate the same failure, throw the caught value again. A rethrow preserves the exception's original source and traceback:

```li
fn save_record() {
    throw "disk full"
}

let propagated = nil
try {
    try {
        save_record()
    } catch error {
        print("save failed")
        throw error
    }
} catch error {
    propagated = error
}
assert(propagated == "disk full")
```

Runtime errors participate in the same mechanism:

```li
try {
    const invalid = "text" + 1
} catch error {
    print(`type error: {error}`)
}
```

Catchable runtime failures include:

- an operand or annotated value with the wrong type;
- an invalid or out-of-range index;
- mutation of frozen data;
- mutation of a collection while one of its iterators is active.

These categories use the same `try`/`catch` path as explicitly thrown values. Fatal VM invariant failures are host failures and cannot be caught by a script.

### Tracebacks

An exception records its call chain when it is raised. After importing `debug`, `debug.traceback(error)` returns the recorded frames in innermost-first order. Calling `debug.traceback()` without an argument inside a handler uses the currently caught exception.

Each frame has the shape `{function, line, native}`. `function` identifies the function, `line` is its source line, and `native` reports whether the frame is a native callback. Interpreted and JIT-compiled executions report the same source line.

```li
import debug

fn origin() {
    throw "failed"
}

let failure = nil
try {
    origin()
} catch error {
    failure = error
}

const frames = debug.traceback(failure)
assert(frames::len() >= 1)
assert("function" in frames[0])
assert("line" in frames[0])
assert("native" in frames[0])
assert(":origin" in frames[0].function)
```

## Cleanup and exceptions together

Deferred cleanup runs before control reaches the matching handler:

```li
fn load() {
    defer { print("cleanup") }
    throw "cannot load"
}

try {
    load()
} catch error {
    print(`caught: {error}`)
}
```

This prints `cleanup` before `caught: cannot load`. If a deferred block itself throws while another exception is pending, the deferred failure becomes the exception that continues outward; outer deferred blocks still run.

## Complete example

```li
fn first_multiple(values, divisor) {
    defer { print("search finished") }

    if divisor == 0 {
        throw "divisor must not be zero"
    }

    for index, value in values {
        if value < 0 {
            continue
        }
        if value % divisor == 0 {
            return {index: index, value: value}
        }
    }

    nil
}

try {
    const result = first_multiple([-3, 5, 12, 14], 3)
    if result {
        print(`found {result.value} at index {result.index}`)
    } else {
        print("no result")
    }
} catch error {
    print(`search failed: {error}`)
}
```

Continue with [Functions and Closures](5-functions-and-closures.md).
