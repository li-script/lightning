# 5. Functions and Closures

Functions are first-class Lightning values. A function may be declared by name, created anonymously, stored in a collection, passed as an argument, or returned from another function.

## Function declarations

Declare a named function with `fn`:

```li
fn add(left, right) {
    return left + right
}
```

A function also returns its final expression implicitly:

```li
fn add(left, right) {
    left + right
}
```

Parameters are lexical bindings local to the call. Calls are ordinary expressions. A plain `f(arguments...)` call supplies `nil` as `self`:

```li
fn add(left, right) {
    assert(self == nil)
    left + right
}

const total = add(20, 22)
assert(total == 42)
```

Type annotations and a return annotation may constrain the call boundary:

```li
fn distance_squared(x: number, y: number) -> number {
    x * x + y * y
}
```

The `->` token introduces the return type; it is not a receiver-call operator. In dynamic code, annotations check values at runtime and can guide optimization. They do not coerce an invalid value. A return annotation checks both explicit `return` values and the final implicit result.

## Anonymous functions

Use `||` for an anonymous function with no parameters:

```li
const answer = || {
    42
}

print(answer())
```

Use `|a, b|` for one or more parameters:

```li
const multiply = |a, b| {
    a * b
}

print(multiply(6, 7))
```

A short body may be written directly after the parameter list:

```li
const square = |value| value * value
```

Anonymous functions are useful as callbacks:

```li
import collections

const doubled = collections.map([1, 2, 3], |value, index| value * 2)
```

## Closures and upvalues

An anonymous or nested function can refer to bindings from its lexical environment. Those captured values are its **upvalues**:

```li
fn make_adder(amount) {
    |value| value + amount
}

const add_ten = make_adder(10)
print(add_ten(5))
```

### Capture by value

Lightning captures each upvalue by value when the closure is created. Reassigning the outer binding later does not change the closure's captured slot:

```li
let label = "before"
const read_label = || label
label = "after"

assert(read_label() == "before")
```

If a captured binding was declared with `let`, the closure can update its own captured slot, and that updated value persists for later calls to that closure:

```li
fn make_counter(start) {
    let current = start
    || {
        current += 1
        current
    }
}

const first = make_counter(0)
const second = make_counter(100)
assert(first() == 1)
assert(first() == 2)
assert(second() == 101)
```

The counters do not share an implicit mutable cell. To share state deliberately, capture a reference to the same table or object:

```li
const shared = {count: 0}
const increment = || { shared.count += 1 }
const read = || shared.count

increment()
assert(read() == 1)
```

Captured heap values hold strong references. A closure therefore keeps its captured objects alive until the closure releases them. Duplicating a closure with `dup` copies the closure's captures at that moment; later mutations of either closure's captured slots remain independent.

### Closures and `self`

A nested closure does not inherit the `self` supplied to the method that created it. Capture the receiver through an ordinary local when the closure needs it:

```li
class Source {
    nested_receiver() {
        || self
    }

    captured_receiver() {
        const owner = self
        || owner
    }
}

let source = Source()
assert(source.nested_receiver()() == nil)
assert(source.captured_receiver()() == source)
```

The nested functions above are called with ordinary `f()` syntax, so their own `self` is `nil`. The second closure reads the explicitly captured `owner` instead.

## Required, optional, and defaulted arguments

Parameters are required by default:

```li
fn greet(name) {
    `Hello, {name}`
}
```

Append `?` to permit omission. An omitted optional parameter receives `nil`:

```li
fn greet(name?) {
    const actual_name = name ?? "world"
    `Hello, {actual_name}`
}

assert(greet() == "Hello, world")
assert(greet("Mira") == "Hello, Mira")
```

The `??` expression is the usual way to give an optional argument a default value. It chooses its right operand only when the left operand is `nil`:

```li
fn connect(host?, port?) {
    const actual_host = host ?? "127.0.0.1"
    const actual_port = port ?? 8080
    `{actual_host}:{actual_port}`
}
```

Optional parameters must follow required positional parameters. They can carry type annotations:

```li
fn scale(value: number, factor?: number) -> number {
    value * (factor ?? 1)
}
```

Omission is distinct from explicitly passing `nil` when a non-nullable annotation is present: omission bypasses that parameter's guard, while an explicit `nil` is checked and rejected. Use a nullable annotation when explicit `nil` is valid.

## Rest and variadic parameters

A bare `...` accepts surplus arguments and exposes them through the function's `$VA` rest view:

```li
fn show_all(...) {
    for index in 0..$VA::len() {
        print(index, $VA[index])
    }
}

show_all("one", 2, true)
```

A named rest parameter is usually clearer:

```li
fn summarize(label, values...) {
    print(label, values::len())
    for index in 0..values::len() {
        print(index, values[index])
    }
}

summarize("readings", 10, 20, 30)
```

The rest parameter must be last. It contains only arguments beyond the fixed parameters and preserves explicit `nil` values.

## Uniform Function Call Syntax

Lightning has two receiver call forms with distinct lookup rules.

### Member calls with `.`

`receiver.func(arguments...)` looks up `func` as a member of `receiver` and calls it with `self` set to the receiver:

```li
class Counter {
    value: number

    new!(value) {
        self.value = value
    }

    add(amount) {
        self.value += amount
        self.value
    }

    receiver() {
        self
    }
}

const counter = Counter(5)
assert(counter.add(3) == 8)

const extracted = counter.receiver
assert(extracted() == nil)
```

Extracting a member does not permanently bind its receiver. In `counter.receiver()`, the member-call expression supplies `counter`; after extraction, the ordinary `extracted()` call supplies `nil`.

### UFCS calls with `::`

`receiver::func(arguments...)` resolves a free or builtin function named `func` and supplies `receiver` as its `self` value. It does not perform member lookup. This lets free functions read naturally from left to right:

```li
const text = 42::str()
const parsed = "0xff"::num()

let values = [1, 2]
values::push(3)
const length = values::len()
```

Conceptually, when a compatible free function is in scope, UFCS supplies the left operand as that function's `self`:

```li
fn scaled_by(factor) {
    self * factor
}

assert(6::scaled_by(7) == 42)
```

UFCS is common throughout the standard library for string conversion and collection operations. Use `.` when invoking a member stored on the receiver; use `::` when invoking a free or builtin operation with a receiver.

## Higher-order example

```li
import collections

fn make_threshold_filter(minimum?) {
    const limit = minimum ?? 0
    |value, index| value >= limit
}

const keep = make_threshold_filter(10)
const selected = collections.filter([4, 10, 15, 3], keep)

print(`kept {selected::len()} values`)
for index, value in selected {
    print(index, value)
}
```

See [`docs/examples/05_closures.li`](examples/05_closures.li) for a runnable example combining optional arguments, closure state, and UFCS.
