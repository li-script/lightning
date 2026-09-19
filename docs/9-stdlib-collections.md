# 9. Collections

Lightning provides two general-purpose mutable containers—tables and arrays—plus eager functional operators in the `collections` module. Packed homogeneous numeric arrays are covered in [Chapter 10](10-stdlib-math-and-typed.md).

## 9.1 Tables

A table literal uses braces and stores hashed key/value entries:

```li
let user = {
   name: "Ada",
   active: true,
   score: 41
}

assert(user.name == "Ada")
user.score += 1
assert(user["score"] == 42)
```

For an identifier-like string key, `table.name` is shorthand for `table["name"]`. Bracket indexing accepts a computed key:

```li
let user = {active: true}
const field = "active"
assert(user[field] == true)
```

A missing key reads as `nil`. Assigning `nil` stores a present entry whose value is `nil`; it does not delete the entry. Table length and iteration include such entries. Use `delete table[key]` or `delete table.name` to remove an entry, and use `key in table` to distinguish a missing key from a present `nil` value. Deleting an absent entry is harmless; deleting an array index or fixed class field is an error.

```li
let options = {cache: nil}
assert("cache" in options)
delete options.cache
assert(!("cache" in options))
```

`nil` is not a valid table key. Numeric keys use SameValueZero hashing: `0` and `-0` identify the same entry, and all boxed NaN values identify one retrievable NaN entry even though `nan == nan` is false. String keys hash by content. Reference-like keys retain their normal identity semantics.

Table iteration yields the original key and value in a stable order for that table state:

```li
let user = {name: "Ada", score: 42}
for key, value in user {
   print(key, value)
}
```

Code should not depend on the specific order of unrelated table keys.

### Reserving table capacity

`table.reserve(table, capacity)` reserves hash capacity without changing entries or length. Capacity must be a finite, non-negative integer. Reserving a frozen table is an error.

Because `table` is also a type keyword, scripts commonly alias the module:

```li
import "table" as tables

let index = {}
tables.reserve(index, 128)
index["lightning"] = 1
```

Reserve when the expected number of entries is known and avoiding intermediate rehashes matters.

## 9.2 Arrays

An array literal uses brackets and stores a contiguous sequence of dynamic values:

```li
let colors = ["red", "green", "blue"]
assert(colors::len() == 3)
assert(colors[0] == "red")
colors[1] = "emerald"
```

Indices are zero-based. An index store requires an existing finite, non-negative integral index; assignment does not grow the array. A missing or out-of-range read returns `nil`. Use `push`, `pop`, or `array.resize` to change length.

```li
let colors = ["red", "green", "blue"]
colors::push("violet")
assert(colors::pop() == "violet")
```

Array iteration yields `(index, value)`:

```li
let colors = ["red", "green", "blue"]
for index, color in colors {
   print(index, color)
}
```

### Capacity, length, and filling

The mutable operations live in the `array` module:

```li
import "array" as arrays

let samples = [1, 2, 3]
arrays.reserve(samples, 32)       # length remains 3
arrays.resize(samples, 5)         # new elements are nil
arrays.fill(samples, 0, 3, 5)     # half-open range [3, 5)
```

- `array.reserve(array, capacity)` increases capacity when necessary without changing length.
- `array.resize(array, length)` truncates or extends the array; extended slots contain `nil`.
- `array.fill(array, value)` fills the whole array.
- `array.fill(array, value, start)` fills `[start, length)`.
- `array.fill(array, value, start, end)` fills the half-open range `[start, end)`.

All counts and bounds must be finite, non-negative integers. `fill` validates the complete range before changing the array. These mutating operations reject frozen arrays.

## 9.3 Iterator protocol

Ranges, generators, iterator adapters, and values with a `next` trait share one logical operation: `next -> (value, done)`. A script-defined `next!()` method or `next` trait returns an exact two-element array `[value, done]`, and `done` must be a boolean. Completion is independent of the value, so `[nil, false]` yields a legitimate `nil`, while `[nil, true]` ends traversal.

```li
import iterator
import traits

let source = {position: 0}
traits.set(source, "next", || {
   self.position += 1
   if self.position == 1 { return [nil, false] }
   if self.position == 2 { return [17, false] }
   [nil, true]
})

let pair = iterator.next(source)
assert(pair[0] == nil && pair[1] == false)
pair = iterator.next(source)
assert(pair[0] == 17 && pair[1] == false)
assert(iterator.next(source)[1] == true)
```

Generic iterators receive increasing numeric enumeration keys. Direct table and array iteration preserves the collection key for `for key, value`.

An iterator over a mutable array, table, or typed array captures that collection's mutation version. Any successful store, insertion, deletion, push, pop, resize, join, or trait-mediated mutation invalidates it. Its next operation then raises a catchable concurrent-mutation error. Mutation is unrestricted after exhaustion, and mutating a different collection or a nested value is allowed.

## 9.4 Shallow duplication

`dup` creates new top-level storage for an array or table while retaining the same elements, keys, and present `nil` entries. Nested heap values remain shared. A duplicated class instance has distinct top-level identity with shallowly copied fields, and a duplicated closure receives a by-value copy of its captures at duplication time. Immutable values may return themselves. `dup` never recursively clones an object graph.

```li
let child = {value: 1}
let original = [child]
let copy = original::dup()
copy::push({value: 2})
copy[0].value = 7
assert(original::len() == 1)
assert(original[0].value == 7)
```

## 9.5 Functional collection operators

Import the module before using its eager operators:

```li
import collections
```

Unless stated otherwise, callbacks receive `(value, key)`. For arrays, strings, and typed arrays, `key` is the numeric index. For tables, it is the table key. Generic iterables receive increasing numeric enumeration keys.

The operators accept arrays, tables, strings, typed arrays, ranges, generators, and objects that implement the `next` protocol. They are eager: traversal and every callback finish before the operator returns. Returned collection results are ordinary arrays.

A non-iterable source, a non-callable callback, an invalid callback result, or malformed `next` result raises a catchable error. Callback errors propagate unchanged; no partial result is returned. Predicates and sort comparators must return booleans.

### `collections.map(collection, callback)`

Calls `callback(value, key)` for every element and returns an array of callback results.

```li
import collections

const labels = collections.map([10, 20, 30], |value, index| `#{index}: {value}`)
assert(labels[1] == "#1: 20")
```

### `collections.filter(collection, predicate)`

Returns an array containing values for which `predicate(value, key)` returns `true`. The predicate must return a boolean.

```li
import collections

const even = collections.filter([1, 2, 3, 4], |value, index| value % 2 == 0)
assert(even::len() == 2)
```

### `collections.reduce(collection, initial, callback)`

Folds the collection from its traversal order. The callback receives `(accumulator, value, key)` and returns the next accumulator. Reducing an empty collection returns `initial`.

```li
import collections

const total = collections.reduce([3, 5, 7], 0,
   |sum, value, index| sum + value)
assert(total == 15)
```

### `collections.each(collection, callback)`

Calls `callback(value, key)` for side effects and returns the original collection.

```li
import collections

let by_name = {}
collections.each(["Ada", "Grace"], |name, index| {
   by_name[name] = index
})
assert(by_name.Grace == 1)
```

### `collections.any` and `collections.all`

`any` returns `true` as soon as one predicate call returns `true`. `all` returns `false` as soon as one call returns `false`. Predicates must return booleans.

```li
import collections

assert(collections.any([2, 5, 8], |value, index| value % 2 == 1))
assert(collections.all([2, 4, 8], |value, index| value % 2 == 0))
assert(!collections.any([], |value, index| true))
assert(collections.all([], |value, index| false))
```

### `collections.find(collection, predicate)`

Returns the first value whose predicate returns `true`, or `nil` if there is no match.

```li
import collections

const first_large = collections.find([4, 12, 7], |value, index| value > 10)
assert(first_large == 12)
```

Because `nil` is a legitimate collection value, a returned `nil` can mean either “found a nil value” or “not found.” Track the predicate separately when that distinction matters.

### `collections.sort(collection, comparator)`

Returns a new, stable sorted array. The source is not modified. `comparator(left, right)` must return `true` exactly when `left` belongs before `right`. A non-boolean result is an error, and an error thrown by the comparator propagates unchanged.

```li
import collections

const records = [
   {score: 8, name: "Ada"},
   {score: 3, name: "Grace"},
   {score: 8, name: "Lin"}
]
const ordered = collections.sort(records, |left, right| left.score < right.score)
assert(ordered[0].name == "Grace")
assert(ordered[1].name == "Ada")  # equal scores retain input order
```

### `collections.keys` and `collections.values`

`keys` and `values` return parallel arrays in the collection's traversal order.

```li
import collections

const source = {alpha: 10, beta: 20}
const keys = collections.keys(source)
const values = collections.values(source)
assert(keys::len() == 2 && values::len() == 2)
```

For an array, `keys` returns `[0, 1, ...]`. Present table entries whose value is `nil` appear in both results.

### `collections.zip(iterable, ...)`

Traverses one or more iterables in parallel and returns an array of row arrays. It stops when the shortest input is exhausted.

```li
import collections

const rows = collections.zip(["Ada", "Grace"], [10, 20, 30])
assert(rows::len() == 2)
assert(rows[0][0] == "Ada" && rows[0][1] == 10)
```

Calling `zip` without an input is an error.

## 9.6 Mutation during traversal

The [mutation-version rule from §9.3](#93-iterator-protocol) also applies to every eager adapter. Mutating a source array, table, or typed array from its callback invalidates the traversal and raises a catchable concurrent-mutation error—even when the store writes the same value. This check occurs before a short-circuiting operator returns. Mutating a different collection is allowed.

```li
import collections

let source = [2, 4, 6]
let output = []
collections.each(source, |value, index| output::push(value * 2))
assert(output[2] == 12)
```

This rule prevents skipped elements, duplicate visits, and table-rehash hazards.
