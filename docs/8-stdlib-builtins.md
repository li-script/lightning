# 8. Built-in Functions

The `builtin` module is installed in every VM. Its public names are also available unqualified, so ordinary scripts call `print`, `assert`, `str`, and the other functions below without an import.

## 8.1 `print(...)`

`print` writes every argument in display form, separated by tab characters, and finishes the record with a newline.

```li
print("position", 12, 4.5, true)
```

`print` accepts zero or more arguments. It is intended for console output and diagnostics; use the `fs` module when output must be written to a file.

## 8.2 `assert(condition, [message])`

`assert` verifies a condition. It returns `nil` when the condition is truthy. If the condition is false or `nil`, it throws a catchable error. A string second argument becomes the error message; without one, Lightning reports the source location.

```li
const width = 8
assert(width > 0)
assert(width % 2 == 0, "width must be even")
```

Only `false` and `nil` are false in a condition.

## 8.3 Explicit conversion

Arithmetic does not implicitly convert strings, booleans, or `nil`. Use the conversion builtins when conversion is intentional.

### `str(value)`

Returns the language display string for `value`. Strings are returned unchanged. A table or class with a `str!()` trait may define its own representation.

```li
const label = str(12.5)
const count = 10::str()
```

The UFCS spelling `value::str()` is equivalent to `str(value)`.

### `num(value)`

Converts a value to the dynamic IEEE 754 binary64 number type:

- numbers are returned unchanged;
- `false` becomes `0` and `true` becomes `1`;
- `nil` becomes `0`;
- strings are parsed as complete Lightning numeric literals, including supported base prefixes and an optional sign.

Other values and strings with trailing nonnumeric text raise a catchable type or value error; conversion never substitutes a default value.

```li
const hexadecimal = num("0xff")
const signed = "+5.5"::num()
const enabled = num(true)
```

### `int(value)`

First applies `num`, then truncates toward zero. The result remains a dynamic binary64 number; `int` does not create a separate boxed integer type.

```li
const positive = int(4.9)
const negative = int(-4.9)
const parsed = "+5.5"::int()
```

Use packed typed arrays or strict integer types when fixed-width integer storage is required.

## 8.4 Type reflection: `typeof` and `typeid`

In strict code, `typeof(value)` is a compile-time type expression and `typeid(Type)` returns the canonical type identifier string. They are contextual language forms, not ordinary runtime functions in dynamic code.

```li
[[strict]] fn describe<T>(value: T) -> string {
   static assert(typeid(T) == typeid(typeof(value)), "type mismatch")
   typeid(T)
}

assert(describe(7i32) == "i32")
```

`typeof(value)` may appear where a strict type is expected:

```li
[[strict]] fn preserve<T>(value: T) -> T {
   let copy: typeof(value) = value
   copy
}
```

For runtime nominal testing, use `value is Type`. Strict types, sized numeric values, and static reflection are described in [Chapter 7](7-strict-tier.md).

## 8.5 Dynamic compilation

### `loadstring(source)`

Compiles a source string as a script chunk and returns a function. Compilation errors are thrown immediately as catchable diagnostics. A diagnostic identifies the line and column and includes the offending source line with a caret. The returned function executes the chunk when called and returns the chunk's last expression or explicit return value.

```li
const chunk = loadstring("const scale = 6\nscale * 7")
assert(chunk() == 42)
```

Use `loadstring` when a compiled chunk will be retained, called later, or called more than once.

### `eval(source)`

Compiles and immediately executes one source string, returning its result.

```li
assert(eval("20 + 22") == 42)
assert(eval("[1, 2, 3]::len()") == 3)
```

Parsing, compilation, and runtime failures propagate as ordinary catchable errors. `eval` and `loadstring` execute trusted Lightning code with the VM's available modules and native bindings; they are not a sandbox.

## 8.6 `struct_copy(value)`

`struct_copy` clones a boxed struct value and returns a new top-level struct box. The field copy is shallow: nested arrays, tables, classes, strings, and other referenced values remain shared according to their normal ownership rules.

```li
struct Pair {
   left: number = 0
   right: number = 0
}

let original = Pair{left: 3, right: 5}
let copy = struct_copy(original)
copy.left = 30
assert(original.left == 3)
assert(copy.left == 30)
```

Passing a non-struct value is an error. Strict assignment and parameter passing already apply the struct value semantics described in [Chapter 7](7-strict-tier.md); `struct_copy` is mainly useful at a dynamic boundary where a struct is represented by a box.

## 8.7 Related ubiquitous operations

Several other builtin operations are commonly reached through UFCS:

```li
let values = [1, 2]
values::push(3)
assert(values::len() == 3)
assert(values::pop() == 3)

let copy = values::dup()
assert(copy::len() == 2)
```

`len` reports the length of strings, arrays, typed arrays, tables, or values with a length trait. `dup` performs a shallow top-level duplication; its precise collection behavior is described in [Chapter 9](9-stdlib-collections.md#94-shallow-duplication). `push`, `pop`, and `join` mutate ordinary arrays as described there.
