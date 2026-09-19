# 6. Structs and Classes

Lightning has two nominal aggregate types. A `struct` is a value type intended for compact data, while a `class` is a reference type with shared identity. Both can declare typed fields, constructors, methods, properties, and operator hooks.

## 6.1 Structs: value semantics

A `struct` has an inline field layout and no independent object identity in strict code. Assigning a struct to another strict binding, passing it to a strict function, or returning it copies its fields. Mutating the copy does not mutate the original. Struct equality is nominal and fieldwise: values must have the same struct type and equal corresponding fields.

```li
[[strict]] struct Point {
   x: f64 = 0
   y: f64 = 0
}

[[strict]] fn moved(point: Point, dx: f64, dy: f64) -> Point {
   point.x += dx
   point.y += dy
   point
}

let origin = Point{x: 2, y: 3}
let result = moved(origin, 5, -1)
assert(origin.x == 2 && origin.y == 3)
assert(result.x == 7 && result.y == 2)
```

When a struct crosses into dynamic code, the runtime boxes it so it can occupy a dynamic value slot. Ordinary dynamic assignment aliases that box rather than creating another fieldwise copy. [Chapter 7](7-strict-tier.md) describes strict copy boundaries, struct typed arrays, and the rules for mixing boxed and inline values.

## 6.2 Classes: reference semantics

A `class` instance is a reference-counted object with identity. Assignment and argument passing copy the reference, not the fields, so all aliases observe the same instance. Without an `eq!` hook, two references compare equal only when they identify the same instance.

```li
class Counter {
   value: number = 0

   increment() -> number {
      self.value += 1
      self.value
   }
}

let first = Counter{}
let alias = first
alias.increment()
assert(first.value == 1)
```

Private objects use immediate deterministic reference counting. Copying a reference retains it; overwriting or leaving the last owning slot releases it. If the last release reaches zero, `del!()` runs immediately on the VM thread. Reference counting does not make cyclic graphs collectible; use weak references where a back-reference must not own its target.

Classes may inherit from classes:

```li
class Shape {
   dyn area() -> number { 0 }
}

class Rectangle : Shape {
   width: number = 0
   height: number = 0

   area() -> number { self.width * self.height }
}
```

Class types are nominal. A derived instance is also an instance of each base class in its inheritance chain; unrelated classes with the same fields remain different types. Only members declared `dyn` may be overridden. Structs cannot inherit, and classes cannot inherit from structs.

## 6.3 Fields and initialization

A field declaration consists of a name, a type, and optionally an initializer. Newlines or semicolons separate declarations, and fields may be interleaved with methods, properties, and hooks:

```li
class Session {
   user: string = "guest"
   retries: number = 0
   tags: array
}
```

The initializer is evaluated once for each new instance. When no initializer is written, the type supplies its normal default value where that type supports one.

Instances without an explicit constructor support zero-argument construction and field-table construction:

```li
class Options {
   verbose: bool = false
   limit: number = 10
}

let defaults = Options()
let custom = Options{verbose: true, limit: 50}
```

## 6.4 Constructors

`new!(...)` is the construction hook. Lightning allocates the instance, evaluates its field initializers, then calls `new!` with the supplied arguments. The hook's own result is discarded; construction returns the initialized instance.

```li
class Account {
   owner: string = ""
   balance: number = 0

   new!(owner: string, opening_balance: number) {
      self.owner = owner
      self.balance = opening_balance
   }
}

let account = Account("Ada", 125)
let other = Account::new("Grace", 200)
```

`Type(args...)` and `Type::new(args...)` both invoke construction. `Type::new(...)` forwards the actual arguments and returns the constructed instance; it does not perform member lookup. Constructor arity and annotated parameter types are checked.

## 6.5 Methods and `self`

An instance method is declared directly in the type body. Calling `object.method(args...)` looks up the method and supplies `object` as `self`.

```li
class Wallet {
   balance: number = 0

   deposit(amount: number) -> number {
      self.balance += amount
      self.balance
   }
}

let wallet = Wallet{}
assert(wallet.deposit(20) == 20)
```

Extracting `const deposit = wallet.deposit` does not permanently bind the receiver. The UFCS form `wallet::function(args...)` resolves a free or builtin function and supplies `wallet` as its `self` value.

## 6.6 Properties

A getter declared as `get name()` exposes a computed field-like read. A setter declared as `set name(value)` handles assignment:

```li
class Temperature {
   celsius: number = 0

   get fahrenheit() -> number {
      self.celsius * 9 / 5 + 32
   }

   set fahrenheit(value: number) {
      self.celsius = (value - 32) * 5 / 9
   }
}

let reading = Temperature{}
reading.fahrenheit = 68
assert(reading.celsius == 20)
assert(reading.fahrenheit == 68)
```

A getter without a setter is read-only. A setter without a getter is write-only. As with methods, accessors must be declared `dyn` in a base class before a derived class may override them.

## 6.7 Operator and lifecycle hooks

Special methods end in `!` and connect a type to language operations:

| Hook | Operation | Contract |
| --- | --- | --- |
| `at!(key)` | unresolved indexing or dynamic member lookup | Returns the value for `key`. |
| `set!(key, value)` | unresolved dynamic assignment | Handles the attempted write. |
| `len!()` | `self::len()` | Returns the logical length. |
| `neg!()` | `-self` | Returns the negated value. |
| `add!(other)`, `sub!(other)`, `mul!(other)` | `+`, `-`, `*` | Receives the left operand as `self`. |
| `div!(other)`, `mod!(other)`, `pow!(other)` | `/`, `%`, `^` | Receives the left operand as `self`. |
| `lt!(other)`, `eq!(other)` | ordering and equality | Must return a boolean. |
| `call!(...)` | `self(...)` | Makes the value callable. |
| `str!()` | `self::str()` | Returns its display string. |
| `next!()` | generic iteration | Returns exactly `[value, done]`, where `done` is boolean. |
| `del!()` | last-reference finalization | Performs deterministic cleanup and returns no useful value. |

```li
class Distance {
   meters: number = 0

   new!(meters: number) { self.meters = meters }

   add!(other: Distance) -> Distance {
      let result = self::dup()
      result.meters += other.meters
      result
   }

   mul!(scale: number) -> Distance {
      let result = self::dup()
      result.meters *= scale
      result
   }
}

let route = Distance(120) + Distance(30)
assert(route.meters == 150)
assert((route * 2).meters == 300)
```

`!=` derives from `eq!`; `>`, `<=`, and `>=` derive from `lt!`. There are no separate `ne!`, `gt!`, `le!`, or `ge!` hooks. Binary arithmetic hooks receive exactly one other operand, and the runtime does not reverse the operands to try a right-hand arithmetic hook. Primitive operand combinations retain their builtin behavior.

Short-circuit `&&`, `||`, and `??`, the conditional `?:`, and assignment are not overloadable. Their evaluation and control-flow rules cannot be replaced by a trait.

`del!()` runs at most once, immediately when the last private strong reference is released. Its borrowed `self` cannot be stored, returned, captured, or otherwise resurrected. It is a deterministic resource finalizer, not a tracing-GC callback.

## 6.8 Type inspection

The `is` operator tests a value against a nominal class or struct type and returns a boolean:

```li
class Message {}
let value = Message{}
assert(value is Message)
assert(!(value is string))
```

Compile-time `typeof` and `typeid`, generic type inspection, and attributes belong to the strict tier; see [Chapter 7](7-strict-tier.md).

## 6.9 Traits and interfaces

Lightning's runtime traits are the hook entries used for operators, indexing, iteration, calls, string conversion, and finalization. Declaring `add!`, `at!`, or `next!` installs the corresponding behavior on a type. The `traits` module can inspect or attach the same hooks dynamically to tables and classes:

```li
import traits

let scalable = {value: 6}
traits.set(scalable, "mul", |factor| self.value * factor)
assert(scalable * 7 == 42)
assert(traits.get(scalable, "mul") is function)

traits.set(scalable, "mul", nil)
assert(traits.get(scalable, "mul") == nil)
```

A hook installed with `traits.set` receives the same `self` and argument contract as a declared special method. Assigning `nil` removes a dynamic hook. Declared properties take precedence over the generic `at` hook, while raw field access bypasses properties and hooks.

The trait flags control different operations:

| Flag | Effect when true |
| --- | --- |
| `seal` | Prevents adding, replacing, or removing hooks. Ordinary data may still change. |
| `freeze` | Prevents data mutation, deletion, and property setter calls. Hook dispatch and hook changes remain available. |
| `hide` | Makes `traits.get` return `nil` for installed hooks while leaving dispatch active. |

Set a flag with its canonical trait name, such as `traits.set(target, "freeze", true)`.

There is no separate `interface` declaration in the current syntax. APIs express interfaces as behavioral protocols:

- an **iterable** provides the builtin collection traversal behavior or a `next!()` hook;
- an **indexable** value provides builtin indexing or `at!(key)`;
- an **addable** value supports builtin numeric addition or `add!(other)`;
- a class hierarchy expresses a nominal method interface, with `dyn` marking members that derived classes may implement differently.

This protocol model lets tables, classes, ranges, generators, and native objects participate in the same APIs without inheriting from a common base class.
