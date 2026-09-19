# 2. Basic Concepts

Lightning Script is dynamically typed: variables hold values, and each value carries its runtime type. A binding may therefore hold values of different types over its lifetime unless the binding is immutable or constrained by an annotation.

## Values and types

### `nil`

`nil` represents the absence of a value. Missing table fields and out-of-range reads commonly produce `nil`. Only `nil` and `false` are false in a condition.

```li
let result = nil
if !result {
    print("no result")
}
```

### Booleans

The boolean values are `true` and `false`. Conditions do not implicitly convert strings or numbers to booleans; all values other than `false` and `nil` are truthy.

```li
const enabled = true
const disabled = false
```

### Numbers

Dynamic Lightning code has one numeric type: an IEEE 754 binary64 number. Integers written in source are still ordinary binary64 values at runtime unless strict or typed code gives the literal a narrower compile-time type.

```li
const integer = 42
const fraction = 3.5
const hexadecimal = 0xff
const binary = 0b1010
```

The usual binary64 rules apply:

- arithmetic is floating-point arithmetic, and consecutive integers are exactly representable only through $2^{53}$;
- division by zero produces infinities or NaN according to IEEE 754;
- positive and negative infinity compare as ordinary numbers;
- every equality or ordered comparison involving NaN is false except `!=`, which is true;
- `0.0` and `-0.0` compare equal but retain different signs in operations such as reciprocal division.

`x % y` follows ISO C `fmod` semantics. For finite `x` and finite nonzero `y`, the result is `x - trunc(x / y) * y`; its magnitude is less than `abs(y)`, and a nonzero result has the sign of `x`. A zero result preserves the sign of `x`. A zero divisor, infinite dividend, or NaN operand produces NaN, while a finite dividend modulo infinity is the dividend.

Arithmetic operators are numeric-only. Lightning does not silently coerce strings, booleans, objects, or `nil` for `+`, `-`, `*`, `/`, `%`, or `^`; a mismatch raises a catchable type error. Use explicit conversion when conversion is intended:

- `str(value)` returns the language display form.
- `num(value)` leaves a number unchanged, maps `false` and `true` to `0` and `1`, maps `nil` to `0`, and parses a string as one complete Lightning numeric literal, including an optional sign and supported base prefix. Other values and strings with unconsumed characters raise a catchable type or value error rather than producing a default number.
- `int(value)` first applies `num`, then truncates toward zero. Its result is still a dynamic binary64 number; it does not select a 32-bit or 64-bit integer representation. Use the sized types described in [Chapter 7](7-strict-tier.md) or typed arrays when storage width matters.

```li
const parsed = "0x20"::num()
const truncated = (-12.75)::int()
const display = 12.5::str()

assert(parsed == 32)
assert(truncated == -12)
```

Tables deliberately use a different numeric equivalence for keys: SameValueZero. Positive and negative zero identify one key and have one hash. Every boxed NaN identifies one canonical key and hash, so a value stored under NaN can be retrieved, replaced, or deleted with any NaN even though the expression `nan == nan` is false. Repeated NaN stores replace one entry rather than growing the table.

### Strings

Strings are immutable byte sequences. Quoted literals may contain escapes, raw strings preserve their contents, and template strings interpolate expressions.

```li
const ordinary = "line one\nline two"
const raw = [=[C:\temp\file.txt]=]
const count = 3
const message = `There are {count} items`
```

String indexing is zero-based and requires a finite, nonnegative integral number. An in-range read returns a numeric byte from `0` through `255`, while an out-of-range read returns `nil`. A fractional, negative, infinite, NaN, or nonnumeric index raises a catchable index or type error. Length and iteration count bytes; indexing, slicing, equality, and length make no Unicode code-point or normalization promise.

### Tables

A table is a dynamic key/value collection and is also the basis of object-like records.

```li
let user = {
    name: "Ada",
    active: true
}

user.score = 100
print(user.name)
```

A table may contain a key whose value is `nil`; use `key in table` to distinguish a present `nil` entry from a missing entry. Use `delete table[key]` or `delete table.name` to remove an entry.

### Arrays

An array is an ordered, zero-indexed sequence.

```li
let primes = [2, 3, 5]
primes::push(7)
print(primes[0], primes::len())
```

Array indexes must be finite nonnegative integers within the existing array. Library operations such as `push`, `pop`, and `resize` change its length.

### Functions

Functions are first-class values. They can be stored, passed to other functions, returned, and captured by closures.

```li
fn twice(value) {
    value * 2
}

const operation = twice
print(operation(21))
```

### Objects, classes, and structs

Classes define reference objects with fields, methods, properties, and lifecycle or operator hooks. Structs define structured values and are especially useful in typed or strict code.

```li
class Counter {
    value: number

    new!(initial) {
        self.value = initial
    }

    increment() {
        self.value += 1
        self.value
    }
}

const counter = Counter(10)
print(counter.increment())
```

Instances of classes have identity and reference semantics. Struct values have value semantics under the language's typed model.

### Coroutines

A coroutine is a suspended stackful computation. `coroutine.create` creates it, `coroutine.resume` continues it, and `yield` suspends it while preserving its frames and local state.

```li
import coroutine

const sequence = coroutine.create(|| {
    yield 10
    yield 20
})

print(coroutine.resume(sequence))
print(coroutine.resume(sequence))
print(coroutine.status(sequence))
```

Coroutines are stackful: suspension preserves ordinary interpreted or JIT frames rather than translating the function into a separate state machine.

## Interpreted and native execution

The interpreter and every enabled native backend implement one set of language semantics. The native backend is a method JIT: it compiles and caches whole functions, not traces. Type-specialized instructions guard their assumptions. A failed guard performs the generic operation or returns to the interpreter at the same bytecode position; it never reaches a trap or changes the program's result, exception, or finalizer order.

In automatic mode, call counts and taken loop backedges make eligible functions hot. A hot call compiles a later invocation. A hot backedge can use on-stack replacement, moving the running invocation's live frame values into native code without repeating a side effect. If native speculation later fails, those values move back exactly once and interpretation resumes with the same handlers and locals.

A function unsupported by a backend is rejected as a whole. It remains interpreted in automatic mode, and required `--jit` mode reports the rejection instead of entering a partly compiled function. Execution mode is observable only through JIT diagnostics such as `jit.where` and compilation counters; ordinary results stay identical.

## Compact dynamic representation

Every public dynamic value occupies one trivially copied eight-byte NaN-boxed word. An ordinary binary64 number is stored directly; reserved quiet-NaN patterns encode `nil`, booleans, and managed references. Heap references preserve all 48 low pointer bits, including bit 47 on platforms where it can be part of a valid user-space address. Boxing never truncates those pointers.

Before a numeric NaN enters boxed storage, Lightning normalizes its bit pattern to one canonical quiet NaN. This keeps numeric NaN disjoint from the immediate and reference tags while preserving infinities and every non-NaN binary64 value. The representation does not change the language comparison rules: canonical NaN is still unequal to itself.

Programs should rely on language types rather than tag bits. Copying the eight bytes of a value is not, by itself, an ownership operation; managed references must still cross the retain, release, move, and borrow boundaries below.

## Deterministic reference counting

Heap values use immediate deterministic reference counting. Private strong counts are non-atomic and change on the VM's owning thread. There is no tracing or cycle collector on the execution path; the historical `gc` machinery handles allocation and destruction rather than adding a second lifetime model. Storing a strong reference retains the target, and overwriting or leaving an owning slot releases it.

The main ownership boundaries are:

| Boundary | Ownership |
|---|---|
| Heap fields, collection entries, closure captures, constant pools, module or REPL bindings, and pending exceptions | Hold a strong reference. |
| Interpreter registers and materialized JIT frame slots | Hold a strong reference unless the compiled ownership data explicitly marks a borrow. |
| A returned heap value | Transfers one owned reference to its caller. |
| `weak T` or a weak observer | Does not retain the target; locking it returns a new strong reference or `nil`. |
| `view T` in strict code | Is borrowed and non-owning; it cannot outlive its proven lifetime or be stored in a heap field. |

Replacing an owning location retains and publishes the new value before releasing the old one. This order matters because releasing the old value may run a finalizer that re-enters the program and observes the location. Returns, exceptions, register overwrites, failed initialization, failed module loads, coroutine closure, and VM shutdown all release the roots they own. Deep destruction uses a bounded worklist rather than recursively consuming the native stack.

This gives script code RAII-like behavior:

- local values are released when their scope exits;
- releases also occur during `return` and exception unwinding;
- `defer` bodies run before their enclosing scope is left;
- `del!` hooks and installed `del` traits run deterministically when finalization begins.

```li
class Resource {
    name: string

    new!(name) {
        self.name = name
        print(`open {name}`)
    }

    del!() {
        print(`close {self.name}`)
    }
}

fn use_resource() {
    const resource = Resource("report.txt")
    print(`using {resource.name}`)
} // resource destruction occurs as this scope exits
```

The last private strong release starts finalization immediately on the VM's owning thread. Once finalization begins, weak locks return `nil`. The finalizer receives `self` as a borrowed finalizing reference: storing, returning, or capturing it as a new owner is an error. An object cannot be resurrected, and its finalizer runs at most once. A finalizer may re-enter Lightning subject to those rules; storage is reclaimed after it and queued child releases complete.

### Cycles and weak references

A strong cycle keeps every member's reference count above zero even after the program loses all outside references. Cycles are the program's responsibility: break them explicitly or make a non-owning edge weak. Cycle diagnostics may report suspected cycles, but they never free, retain, finalize, or otherwise alter them.

The `weak` module creates observers that do not retain their target:

```li
import weak

let owner = {name: "service"}
const observer = weak.create(owner)

assert(!weak.expired(observer))
assert(weak.lock(observer).name == "service")

owner = nil
assert(weak.expired(observer))
assert(weak.lock(observer) == nil)
```

`weak.lock` returns a new strong reference when the target is still alive and `nil` after it expires. Use weak edges for parent links, caches, observers, and other relationships that must not extend lifetime.

## Errors and exceptions

`throw` raises any script value. `try` and `catch` intercept the value:

```li
fn require_positive(value) {
    if value <= 0 {
        throw {kind: "range", value: value}
    }
    value
}

try {
    print(require_positive(-1))
} catch error {
    print(`invalid value: {error.value}`)
}
```

Runtime failures such as bad operand types, invalid indexes, missing required arguments, and failed annotations are also catchable script errors. A caught value is the original thrown payload; rethrow it with `throw error` when the current layer cannot handle it.

If an exception escapes the script entry, the CLI prints the exception and its stack trace. Inside a handler, the `debug` module exposes structured traceback information:

```li
import debug

try {
    throw "operation failed"
} catch error {
    const frames = debug.traceback(error)
    for index, frame in frames {
        print(index, frame["function"], frame.line, frame.native)
    }
}
```

Stack traces preserve the original throw location. Frames identify script and native calls, and rethrowing a caught error preserves its origin frames.

## See also

- [Language Syntax](3-language-syntax.md)
- [Control Flow](4-control-flow.md)
- [Functions and Closures](5-functions-and-closures.md)
