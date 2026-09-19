# 7. The Strict Tier

Lightning is a dynamic language, and every program in the preceding chapters runs without a single type annotation. The strict tier is an optional layer on top of the same values, bytecode, VM, and JIT: inside strict code every binding has a type that is known when the code is compiled, incompatible operations are reported before the program runs, and generic declarations are instantiated per type tuple. Strict code and dynamic code call each other freely; the boundary between them is checked exactly once at runtime.

Nothing about the strict tier changes how dynamic code behaves. A program that never writes `[[strict]]` never sees it.

## 7.1 Attributes and `[[strict]]`

An attribute list is written `[[name, name(literal, ...)]]`. Arguments must be literals. Placed before a `fn`, `class`, or `struct`, the list attaches to that declaration and is kept as reflection metadata; placed alone as the first statement of a file, it applies to the whole file.

```li
import reflect
[[Entity("record"), cached]]
class Row { id: number = 0 }

[[pure, Cost(3)]] fn triple(value: number) -> number { value * 3 }

const attributes = reflect.attributes(triple)
assert(attributes.Cost[0] == 3 && attributes.pure::len() == 0)
assert(reflect.attributes(Row).Entity[0] == "record")
```

`reflect.attributes(value)` accepts a function, class, struct, or instance and returns a frozen, sealed table mapping each attribute name to its argument array; a declaration without attributes yields an empty frozen table. Duplicate names and non-literal arguments are parse errors, and a file attribute that is not the leading statement is rejected.

`strict` is an attribute like any other. `[[strict]] fn f(...)` makes one function strict; a file that begins with `[[strict]]` is strict throughout, including its imports and nested functions. Because `[[` opens an attribute list, a long string needs at least one separator: `[=[ ... ]=]`.

Put the file attribute first. `import reflect` followed by a lone `[[strict]]` attaches the attribute to the next declaration only, because the attribute is no longer the leading statement.

## 7.2 Types and typed literals

A strict type is written after a colon on parameters, fields, and `let`/`const` bindings, and after `->` for return values.

| Syntax | Meaning |
|---|---|
| `number`, `bool`, `string`, `table`, `array`, `function` | The dynamic value kinds of [Chapter 2](2-basic-concepts.md). |
| `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` | Sized integer storage types. |
| `f32`, `f64` | Single- and double-precision floating storage types. |
| `Name`, `Name<T>`, `Self` | A class or struct, an instantiated generic, or the enclosing declaration. |
| `T?` | `T` or `nil`. |
| `view T` | A borrowed reference to `T` that may not be stored in a field or captured by a closure. |
| `weak T` | A weak reference (see [Chapter 11](11-stdlib-system-and-concurrency.md)). |
| `T[]`, `T[N]` | A typed array of `T`, dynamically or fixed sized. |
| `type Alias = T` | A statement introducing a type alias. |

Typed numeric literals carry their type: `10i32`, `3f32`, `10u` (an `u32`), `0.5f64`. Digit groups may be separated with `'`, as in `100'000i32`.

```li
[[strict]]
type Count = i32

fn add(left: Count, right: Count) -> Count { left + right }

let total: Count = add(10i32, 20i32)
let ratio = 3f32 / 4f32
let separated = 100'000i32
let label = `total {total}`
assert(total == 30i32 && ratio == 0.75f32 && separated == 100000i32)
assert(label == "total 30")
```

Sized integers and `f32` are storage types. Arithmetic is performed in binary64, which is correctly rounded for `+`, `-`, `*`, and `/`; a value is rounded once to its width when it is stored into a typed field or typed array element, and it is boxed to an ordinary `number` when it crosses into dynamic code. An `i64` or `u64` value outside the 53-bit range cannot cross that boundary and raises a runtime error instead of losing precision.

Every strict binding needs a type. Most are inferred forward from the initializer: literals, arithmetic on typed operands, comparisons, string interpolation, constructors, and calls to strict functions all have known types. A value that comes from dynamic code is `any` and must be annotated or converted with `as` before it is used in a typed position; the conversion is the one place a runtime check is emitted.

```li
[[strict]]
let checked: i32 = eval("40") as i32
let maybe: i32? = nil
assert(checked == 40i32 && maybe == nil)
```

Inference is forward only: a binding's type is fixed by the statement that introduces it and is never revised by a later use.

## 7.3 Diagnostics and the dynamic boundary

Incompatible operations inside strict code are parse errors. A diagnostic names the location as `[file:line:col]`, shows the source line, and underlines the offending span:

```
[main.li:4:19] incompatible operand types: _i32_ and _string_
let bad = count + label
                  ^~~~~
```

Other strict-only errors include an unannotated binding whose initializer is dynamic, an unresolved type name, assigning a value of the wrong type, storing a `view` in a field or capturing it in a closure, and a strict function whose return type cannot be resolved.

Dynamic code may call a strict function through the ordinary call path. The declared parameter types become runtime checks at function entry, so a wrong argument is a catchable exception rather than undefined behavior:

```li
[[strict]] fn scale(value: i32, factor: i32) -> i32 { value * factor }

assert(scale(6i32, 7i32) == 42i32)
let error = nil
try { scale({}, 2) } catch e { error = e }
assert("expected variable 'value' to be of type 'i32'" in error)
```

The same checks protect the return value of a strict function that a dynamic caller receives, and the `as T` conversion at the other side of the boundary.

## 7.4 Structs in strict code

[Chapter 6](6-structs-and-classes.md) introduces `struct` as a class with value semantics. Strict code is where those semantics are enforced by the compiler: assigning a struct from an lvalue, passing it as an argument, and returning it each copy the fields. A value that is already fresh, such as a constructor result or an element read from a struct typed array, is not copied again.

```li
[[strict]]
struct Pair { left: i32 = 0i32; right: i32 = 0i32 }

let first = Pair{left: 1i32, right: 2i32}
let second = first
second.left = 10i32
assert(first.left == 1i32 && second.left == 10i32)
assert(first == Pair{left: 1i32, right: 2i32})
```

Field defaults may be written as `= {}` (the default value of the field's type: zero, `false`, `""`, `nil` for an optional, or a default-constructed struct) or `= {value}` for a typed initial value. A struct may contain a `union { ... }` block whose fields share storage; strict code may read only the union field that was most recently assigned in the same function body, and any other read is a parse error.

Struct typed arrays (`typed.struct_array(Type, length)`, [Chapter 10](10-stdlib-math-and-typed.md)) store elements inline. `array[i]` materializes a copy, while both `array[i] = value` and `array[i].field = value` write back into the element.

### Atomic fields

A numeric field of a strict struct or class may be declared `atomic`. Every write to it is one atomic update: `=` becomes an atomic store, and `+=`, `-=`, `*=`, `/=`, `%=`, `++`, and `--` each become a single atomic read-modify-write. Reads are ordinary loads. On a private instance the operations are plain typed stores; on a `shared` instance ([Chapter 11](11-stdlib-system-and-concurrency.md)) they use the same compare-and-exchange path as `shared.atomic_add`, and dynamic code that writes the field is routed through the same path, so a non-atomic write to an atomic field cannot happen.

```li
[[strict]]
import shared

class Counter {
   atomic count: i32 = 0i32
   new!() {}
}

const counter: Counter = shared.create(Counter::new())
counter.count += 5i32
counter.count *= 4i32
assert(counter.count == 20i32)
```

`atomic` requires strict mode and a numeric field type; `^=` is not available on atomic fields.

## 7.5 Generics

A declaration with type parameters is a template. It is instantiated when it is used with a concrete type tuple, either written explicitly as `identity<i32>(5i32)` or deduced forward from the argument types.

```li
[[strict]]
import reflect

fn identity<T>(value: T) -> T { value }
fn describe<T>(value: T) -> string {
   static if T is i32 { "integer" } else { "other" }
}
fn pick<T>(value: T) -> i32 { 1i32 }
fn pick<T: f32>(value: T) -> i32 { 2i32 }
fn count_args<Tx...>(args: Tx...) -> u32 { #Tx... }
fn auto(value) { value }

assert(identity(5i32) == 5i32)
assert(identity("text") == "text")
assert(describe(1i32) == "integer" && describe(1f32) == "other")
assert(pick(1i32) == 1i32 && pick(1f32) == 2i32)
assert(count_args(1i32, 2i32, 3i32) == 3u)
assert(auto(4i32) == 4i32)
```

- Each instantiation reparses the declaration with its type parameters bound, produces a separate function or class, and is cached per type tuple. Instantiated classes are named by their arguments, so `Vec3<f32>` prints and compares as such.
- A constraint such as `<T: f32>` makes a candidate more specific. When several candidates match, the most specific wins; two equally specific candidates are an ambiguity error.
- `Tx...` declares a type pack. `args: Tx...` accepts one argument per pack element and `#Tx...` is the element count, a `u32`.
- In strict code an unannotated parameter makes the function a template over that parameter, so `fn auto(value)` behaves like `fn auto<T>(value: T)`.
- Deduction is forward only. A call decides the type tuple from the arguments it has; nothing is inferred backward from how the result is later used.

`reflect.instantiations(template)` reports what the cache saw, one record per attempt with `types` (the spelled type arguments) and `status` (`"Inst-OK"`, `"InstCache-OK"`, or `"Inst-Failed"`):

```li
identity(6i32)
for record in reflect.instantiations(identity) { print(record.status, record.types[0]) }
```

```
Inst-OK      i32
Inst-OK      string
InstCache-OK i32
```

Generic structs and classes follow the same rules, and a template may refer to itself:

```li
[[strict]]
import typed

struct Vec3<T> {
   x: T = {}; y: T = {}; z: T = {}
   new!(x: T, y: T, z: T) { self.x = x; self.y = y; self.z = z }
   add!(other: Self) -> Self { Vec3<T>::new(self.x + other.x, self.y + other.y, self.z + other.z) }
   reduce_add() -> T { self.x + self.y + self.z }
}

const points: Vec3<f32>[] = typed.struct_array(Vec3<f32>, 4)
let i = 0
while i < 4 { points[i] = Vec3<f32>::new(1f32, 2f32, 3f32); i += 1 }
assert((points[0] + points[1]).reduce_add() == 12f32)
```

Operator methods such as `add!`, comparison methods `eq!`/`lt!`, the conversion method `to!<T>()`, and the `new!`/`del!` lifecycle hooks are resolved statically inside strict code: a call is a direct call to the instantiated method rather than a runtime trait lookup.

## 7.6 Compile-time evaluation

Inside a template body, `static` statements run while the instantiation is being parsed. They see the bound type parameters through the predicates `T is U`, `typeid(T)` (the canonical type identifier string), and `typeof(expr)` (the static type of an expression), together with the constant folder.

| Statement | Effect |
|---|---|
| `static if cond { ... } else { ... }` | Compiles only the selected branch. |
| `static assert(cond, message?)` | Fails the instantiation with `message`. |
| `static warn(message)` | Prints `message` during instantiation and continues. |
| `static panic(message)` | Fails the instantiation unconditionally. |
| `static require(cond, message)` | Cancels only this candidate; the call fails with `message` when no candidate remains. |

`static require` is how a template restricts itself without producing a wall of errors. A failed requirement removes that candidate from consideration, so an overloaded template falls through to the next candidate, and only when every candidate has been cancelled does the call report the innermost message:

```
[main.li:6:10] only_bool converts booleans
only_bool(1i32)
         ^
```

A hard failure inside an instantiation is reported at the failing statement together with one instantiation frame:

```
[main.li:3:11] checked wants i32
   static assert(typeid(T) == typeid(i32), "checked wants i32")
          ^~~~~~
  note: in instantiation of checked<f32>
```

Only the innermost frame is shown by default; `li --verbose-errors` prints the full instantiation chain.

## 7.7 Strict code and the JIT

Strict code compiles to the same bytecode as dynamic code and runs on the same interpreter and JIT. The difference is what the compiler already knows: a strict function's declared parameter and return types seed the JIT's type information, so the compiled function starts from those types instead of speculating and guarding.

In a strict function that loops over a struct typed array, an element read whose only uses are field reads is not materialized at all. The compiled loop performs one element-class check and one bounds check per access and then loads the fields directly from the array's storage, with no allocation, no boxing, and no speculative type guards:

```li
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

`jit.guards(f)` reports the number of speculative type guards left in a compiled function, and `li --metrics` reports the total as `jit_guards`; both exclude the declared-signature checks at the dynamic boundary. For `sum_lengths` both are zero on every native backend, and the loop allocates nothing. See [Chapter 11](11-stdlib-system-and-concurrency.md) for the `jit` module.
