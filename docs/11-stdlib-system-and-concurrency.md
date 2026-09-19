# 11. System, Concurrency, and Runtime Libraries

This chapter covers the standard modules that connect Lightning Script to the host system and runtime. Import only the modules a program uses:

```li
import fs
import chrono
import coroutine
import shared
import weak
import vec3
import reflect
import debug
import jit
```

System modules are not a sandbox. In particular, `fs` uses the permissions of the host process, and native or JIT facilities do not impose resource limits.

## 11.1 File system: `fs`

The `fs` module provides synchronous file and directory operations when the host registers the standard filesystem implementation. An embedder may replace it with a restricted module or omit it entirely.

Paths are UTF-8 byte strings interpreted by the host platform. Embedded NUL bytes are rejected. These functions throw a catchable error for invalid arguments and operating-system failures unless their contract below handles a missing path explicitly.

| Function | Result |
|---|---|
| `fs.read(path)` | Reads the complete file into a binary-safe string. Open, seek, and partial-read failures throw. |
| `fs.write(path, bytes)` | Creates or replaces a file, writes the complete string, and returns the byte count. A partial write or close failure throws. |
| `fs.exists(path)` | Returns `false` for a missing path. Other inspection failures throw. |
| `fs.remove(path)` | Removes one file or empty directory without recursion. Returns whether an entry was removed. |
| `fs.create_directory(path)` | Creates one directory, without creating missing parents. Returns whether a directory was created. |
| `fs.list(path)` | Returns the direct child basenames in bytewise-sorted order. Missing paths and nondirectories throw. |
| `fs.metadata(path)` | Returns `{type, size, modified}` for an existing path. Missing and inaccessible paths throw. |

`fs.write` requires a string. A successful zero-length write still creates or truncates the file and returns `0`.

```li
import fs

const path = ".lightning-manual-fs.bin"
defer { fs.remove(path) }

const payload = "A\x00B\n"
assert(fs.write(path, payload) == 4)
assert(fs.read(path) == payload)
assert(fs.metadata(path).size == 4)
```

The `type` metadata field is `"file"`, `"directory"`, `"symlink"`, or `"other"`, based on the directory entry itself. `size` is the byte size for a regular file and `nil` otherwise. `modified` is a Unix-seconds number; portable last-write-time lookup follows a symlink target.

The standard module provides no path or capability sandbox. Restricted applications expose a host-defined replacement or enforce process-level filesystem boundaries.

## 11.2 High-resolution timing: `chrono`

### `chrono.now()`

Returns the current high-resolution clock reading in nanoseconds. The value is intended for elapsed-time measurement rather than civil-calendar formatting. Subtract two readings obtained in the same process:

```li
import chrono

const started = chrono.now()
// measured work
const elapsed_ns = chrono.now() - started
print(`elapsed: {elapsed_ns} ns`)
```

The result is a binary64 number. Very large absolute timestamps cannot represent every individual nanosecond; elapsed intervals remain the recommended use. The module may also expose target-specific cycle-counter facilities, but portable programs should use `chrono.now()`.

## 11.3 Stackful coroutines: `coroutine`

Lightning coroutines are stackful. Each coroutine owns one VM stack and one guarded native stack segment, so suspension preserves interpreted and JIT frames directly; functions are not rewritten into stackless state machines. A coroutine remains confined to its VM thread, even when its arguments include shared values.

```li
import coroutine

const co = coroutine.create(|| {
   const reply = yield "ready"
   `received {reply}`
})

assert(co.status() == "created")
assert(co.resume() == "ready")
assert(co.status() == "suspended")
assert(co.resume(7) == "received 7")
assert(co.status() == "dead")
```

| Function | Description |
|---|---|
| `coroutine.create(fn)` | Creates a coroutine in the `"created"` state without calling `fn`. |
| `coroutine.resume(co, ...)` | Starts or resumes `co`. Initial arguments go to `fn`; later arguments become the results of the suspended `yield`. |
| `coroutine.yield(value)` or `yield value` | Suspends the running coroutine and returns the value to its resumer. |
| `coroutine.status(co)` | Returns `"created"`, `"running"`, `"suspended"`, or `"dead"`. |
| `coroutine.close(co)` | Unwinds an unstarted or suspended coroutine and marks it dead. |

The method forms, such as `co.resume()` and `co.status()`, have the same behavior. Normal completion returns the function result to the resumer. Resuming a running or dead coroutine is an error.

A suspended coroutine owns every value in its frames, including spills, captures, pending exceptions, handlers, and deferred cleanup. `close` is an unwind operation: it runs `defer` blocks in last-in, first-out order and releases frame values exactly once. Capturing a coroutine from an object in its own frames creates an ordinary strong cycle; use a weak edge or call `close` explicitly.

Yielding inside `try` is allowed because the handler state is part of the coroutine stack. An uncaught exception kills the coroutine and is thrown into the resumer with the coroutine frames in its traceback. Native-stack overflow inside a coroutine is likewise a catchable language error rather than a process crash.

Coroutine storage has deterministic lifetime. Dropping or closing a suspended coroutine releases both stacks and all retained frame values. One million create, resume, and drop cycles must return live-object and stack resources to their baseline. A coroutine used as a generator implements the common iterator result `(value, done)`. Coroutines do not provide `async`/`await`, an event loop, or cross-thread resume.

A coroutine cannot yield while a shared lock is held, while a finalizer is running, or while forced cleanup is in progress. The locking rules are detailed in the next section.

## 11.4 Process-wide shared values and concurrency

A VM and its private heap are confined to one thread. Private values use non-atomic reference counts and cannot be sent to another VM. Private allocation, retain and release, and container access do not take a shared-heap mutex, perform atomic reference counting, or acquire object locks.

Cross-VM reachability is explicit. Values published through `shared` live in a process-wide heap with its own allocator and registries, atomic strong counts, and a 64-bit recursive lock word in each object header that records the owning thread id and recursion count. A shared value remains valid after its creating VM closes while another strong reference exists. Releasing the last shared reference destroys the value on the releasing VM, expires synchronized weak observers, and returns its storage to the shared heap.

```li
import shared

const counters = shared {produced: 0, consumed: 0}
assert(shared.is_shared(counters))

lock counters {
   counters.produced += 1
   counters.consumed = counters.produced
}
assert(shared.atomic_add(counters, "produced", 2) == 3)

atomic {
   counters.produced += 4
   counters.consumed *= 2
}
assert(counters.produced == 7)
assert(counters.consumed == 2)
```

The explicitly imported module provides these operations:

| API | Contract |
|---|---|
| `shared.create(value)` | Returns an owned shared copy of a table, array, class instance, immutable string, class, or transferable function. Immediate roots are invalid. Immediate and already-shared children are retained; private immutable strings and code metadata are copied; any private mutable child rejects the entire publication. |
| `shared.is_shared(value)` | Reports whether a heap value belongs to the shared heap. Immediate and private values return `false`. |
| `shared.lock(value)` | Acquires the recursive header lock and a strong pin for this acquisition. It is an error to lock a private value. |
| `shared.unlock(value)` | Releases one recursion level and that acquisition's pin. A non-owner unlock, over-unlock, or private target throws. |
| `shared.atomic_add(value, key, delta)` | Atomically adds numeric `delta` to a numeric table, array, or object field and returns the new number. A missing or nonnumeric field, invalid key, or private target fails before mutation. |

Shared container reads and writes acquire the header lock internally. An explicit lock extends that same recursive lock across several operations; it does not convert a private value to shared storage. Each recursive acquisition holds a separate strong pin, so dropping the caller's original reference cannot reclaim a locked object. Failing to unlock leaks the lock and pin rather than leaving a dangling locked object.

A shared store validates its new edge before changing the destination or its mutation version. Shared destinations accept immediate and shared values, copy private immutable strings, and reject private mutable values. Shared strings compare and hash by content across VM-local intern tables.

Prefer `lock value { ... }` to the low-level functions. The target expression is evaluated once, and normal exit, `return`, `leave`, or exception unwind releases the lock exactly once. Code that calls `shared.lock` directly must pair it with `defer { shared.unlock(value) }`.

A lexical `yield` inside a lock block is a compile error. If a function called while any lock is held attempts to yield indirectly, the yield throws a catchable runtime error before switching contexts; unwind cleanup then releases the lock.

### Atomic blocks

An `atomic { ... }` block is a restricted numeric transaction over shared fields and numeric locals. It supports field `set`, `add`, `sub`, `mul`, `div`, and `mod`, along with arithmetic, comparisons, branches, and bounded language loops. Calls, allocation, callbacks, exception handling, throwing, and yielding are forbidden. A form that cannot be lowered to this numeric plan is a compile error with a diagnostic telling you to use `lock` instead.

Before publishing anything, the runtime validates every target, key, current field, operation, operand, result slot, and captured-local copyback. It then locks unique targets in stable address order, computes the complete batch, and commits it all at once. A failure therefore exposes neither partial shared-field writes nor partial local copyback. `shared.atomic_add` uses the single-field compare/exchange path; larger representable batches use the same all-or-nothing transaction contract.

A strict struct or class can mark a numeric field `atomic` so that every assignment and compound assignment to it is a single-field atomic update without writing an `atomic` block; see [Chapter 7](7-strict-tier.md#atomic-fields).

## 11.5 Weak references: `weak`

A weak reference observes an object without keeping it alive. It is the standard tool for parent links, caches, and other graph edges that must not form an owning cycle.

```li
import weak

let target = {name: "temporary"}
const observer = weak.create(target)
assert(weak.lock(observer).name == "temporary")
target = nil
assert(weak.lock(observer) == nil)
```

| Function | Description |
|---|---|
| `weak.create(obj)` | Creates a weak reference to a heap object. |
| `weak.lock(w)` | Returns a new strong reference to the live target, or `nil` after expiration. |
| `weak.expired(w)` | Reports whether the target has expired. |

A successful `weak.lock` owns a strong reference: the returned value remains alive independently of the weak reference.

Weak references do not collect strong cycles. Lightning uses deterministic reference counting; a diagnostic cycle detector can report cycles, but programs must break owning cycles themselves.

## 11.6 Native vectors: `vec3`

`vec3` is a native three-component vector class with IEEE 754 binary32 (`f32`) storage. Constructor arguments, component assignments, and operation results round once when stored; reading `x`, `y`, or `z` widens the stored value to an ordinary binary64 Lightning number.

```li
import vec3

const position = vec3::new(1, 2, 3)
const velocity = vec3::new(0.5, 0, -1)
const next = position + velocity

assert(next == vec3::new(1.5, 2, 2))
assert(position.dot(velocity) == -2.5)
assert(position.cross(velocity) == vec3::new(-2, 2.5, -1))
assert(vec3::new(16777217, 0, 0).x == 16777216)
```

Component-wise `+`, `-`, `*`, and `/` accept either another `vec3` or a number. Unary `-` negates each component, and `==` compares components. The methods are `dot`, `cross`, `length`, `normalize`, and `lerp`.

`vec2` and `vec4` provide the corresponding two- and four-component types; only `vec3` has `cross`. Vector instances are private values and may be published with `shared.create` like other class instances.

## 11.7 Reflection and debugging

### `reflect`

Reflection exposes declaration metadata without making executable declarations mutable. Attributes, generics, and instantiation records are described in [Chapter 7](7-strict-tier.md).

`reflect.attributes(value)` accepts a function, class, struct, or instance and returns its declaration attributes as a frozen, sealed table:

```li
import reflect

[[Route("/health"), Public]]
fn health() { "ok" }

const attributes = reflect.attributes(health)
assert(attributes.Route[0] == "/health")
assert(attributes.Public::len() == 0)
```

`reflect.instantiations(template)` returns the cached successful, reused, and failed instantiation records for a generic template. `reflect.signature(value)` returns immutable callable metadata: parameter names and annotations, optional/default and variadic status, and the return annotation when available. A native function exposes only metadata registered by its host binding.

### `debug`

The debug module is diagnostic. Exact bytecode, address, and formatting details are not a program-logic or serialization interface.

| Function | Result |
|---|---|
| `debug.traceback(error?)` | Returns exception frames innermost first. Each frame has `{function, line, native}`; native callbacks set `native` to `true`. With no argument inside a `catch`, it uses the current exception. Rethrow preserves the original frames. |
| `debug.live_objects()` | Returns the current VM's live heap-object count, useful for lifetime diagnostics. |
| `debug.dump(fn)` | Prints the bytecode of a script function and returns `nil`. |
| `debug.find_cycles()` | Reports suspected strong cycles without retaining, marking, finalizing, or freeing them. It is not a tracing collector. |

`debug.stacktrace()` inspects the currently executing stack, as distinct from the stored trace attached to an exception. Debug and reflection results retain any returned metadata through ordinary ownership rules.

## 11.8 JIT controls: `jit`

Lightning uses a method JIT: it compiles and caches whole functions rather than recording traces. Interpreter and native execution have the same results, errors, finalizer order, ownership, cleanup, coroutine behavior, and shared-memory rules. Only JIT diagnostics and compilation counters reveal which mode executed a function.

The command-line policy selects when compilation is required:

| Option | Policy |
|---|---|
| `--jit=off` | Runs interpreted code without automatic compilation. Explicit `jit.on` may still request a supported function. |
| `--jit=auto` | Counts calls and taken backedges and compiles a function after it becomes hot. This is the default in a JIT build. |
| `--jit` | Requires native compilation. A function that the backend cannot compile produces an error rather than entering an incomplete native body. |

Automatic compilation requested by a hot loop backedge creates an on-stack-replacement entry for that loop header. At the next backedge, the running invocation adopts its live frame slots and continues natively without replaying a side effect. If later numeric speculation fails, the native frame restores its live values and handler state and resumes the interpreter at the same bytecode position.

The `jit` module controls and inspects individual script functions:

| Function | Description |
|---|---|
| `jit.on(fn)` | Compiles and enables native execution for a private function value. Compilation failure throws and records a rejection diagnostic. |
| `jit.off(fn)` | Disables native execution for that private function value. Shared functions cannot be disabled. |
| `jit.where(fn)` | Returns the native entry address as a string, or `"N/A"` while the function is interpreted. |
| `jit.disasm(fn)` | Returns native disassembly for compiled code, or the recorded diagnostic for a rejected compilation. |
| `jit.guards(fn)` | Returns the number of speculative guards in the compiled function. It requires a JIT record. |

```li
import jit

fn add(left, right) { left + right }

jit.on(add)
assert(jit.where(add) != "N/A")
assert(add(20, 22) == 42)
assert(jit.guards(add) >= 0)
assert(jit.disasm(add) is string)

jit.off(add)
assert(jit.where(add) == "N/A")
```

Type-specialized operations guard their assumptions. A failed guard performs the generic operation or a defined side exit; it never executes `ud2`, `unreachable`, or another trap. Unsupported functions are rejected before native execution: automatic mode records the rejection and leaves them interpreted, while required mode fails. `jit.disasm` preserves the rejection reason so tooling can explain why a function stayed interpreted.
