# 10. Mathematics and Typed Values

The `math` module provides IEEE 754 binary64 constants and numerical functions. The `typed` module provides packed, homogeneous, unboxed arrays for fixed-width numeric storage.

```li
import math
import typed
```

## 10.1 Math constants

| Constant | Meaning |
| --- | --- |
| `math.pi` | π, rounded to binary64. |
| `math.e` | Euler's number, rounded to binary64. |
| `math.inf` | Positive infinity. Use `-math.inf` for negative infinity. |
| `math.nan` | A quiet NaN. It is unordered and is not equal to itself. |
| `math.epsilon` | Difference between `1` and the next representable binary64 number. |
| `math.huge` | Largest finite positive binary64 number. |
| `math.small` | Smallest positive normal binary64 number. |

```li
import math

assert(math.pi > 3.14 && math.pi < 3.15)
assert(math.inf > math.huge)
assert(math.nan != math.nan)
```

## 10.2 Trigonometric functions

Angles are in radians.

| Function | Result |
| --- | --- |
| `math.sin(x)` | Sine of `x`. |
| `math.cos(x)` | Cosine of `x`. |
| `math.tan(x)` | Tangent of `x`. |
| `math.asin(x)` | Inverse sine. |
| `math.acos(x)` | Inverse cosine. |
| `math.atan(x)` | Inverse tangent. |
| `math.atan2(y, x)` | Quadrant-aware angle for the coordinates `y`, `x`. |
| `math.rad(degrees)` | Converts degrees to radians. |
| `math.deg(radians)` | Converts radians to degrees. |

```li
import math

assert(math.sin(math.pi / 2) == 1)
assert(math.cos(0) == 1)
assert(math.atan2(1, 0) == math.pi / 2)
assert(math.rad(180) == math.pi)
assert(math.deg(math.pi) == 180)
```

Inputs outside a function's mathematical domain follow the platform IEEE math result, normally NaN.

## 10.3 Scalar helpers

| Function | Behavior |
| --- | --- |
| `math.abs(x)` | Absolute value. |
| `math.sqrt(x)` | Square root. |
| `math.ceil(x)` | Least integer-valued number not below `x`. |
| `math.floor(x)` | Greatest integer-valued number not above `x`. |
| `math.trunc(x)` | Removes the fractional part toward zero. |
| `math.min(a, b)` | Lesser operand using the numeric math operation. |
| `math.max(a, b)` | Greater operand using the numeric math operation. |
| `math.clamp(value, lower, upper)` | Restricts `value` to ordered, non-NaN bounds. |
| `math.lerp(a, b, t)` | Linear interpolation between `a` and `b`. |
| `math.sign(x)` | `-1`, `0`, or `1`; NaN remains NaN. |

```li
import math

assert(math.abs(-4) == 4)
assert(math.sqrt(81) == 9)
assert(math.ceil(2.1) == 3)
assert(math.floor(2.9) == 2)
assert(math.trunc(-2.9) == -2)
assert(math.min(4, 7) == 4)
assert(math.max(4, 7) == 7)
assert(math.clamp(15, 0, 10) == 10)
assert(math.lerp(10, 20, 0.25) == 12.5)
assert(math.sign(-8) == -1)
```

Every function validates its arity and requires numeric arguments; numeric strings are not converted implicitly.

## 10.4 Integer helpers

`math.gcd(a, b)` and `math.lcm(a, b)` require finite integral operands in Lightning's safe integer range, `[-(2^53-1), 2^53-1]`.

```li
import math

assert(math.gcd(54, 24) == 6)
assert(math.lcm(6, 15) == 30)
```

The result of `lcm` must also fit the safe integer range. Signs are ignored for the magnitude of the result, and `lcm(0, n)` is `0`.

## 10.5 Numeric predicates

| Function | Result |
| --- | --- |
| `math.isnan(x)` | `true` only for NaN. |
| `math.isfinite(x)` | `true` when `x` is neither infinity nor NaN. |
| `math.isinteger(x)` | `true` for finite binary64 values with no fractional part. |

```li
import math

assert(math.isnan(math.nan))
assert(!math.isfinite(math.inf))
assert(math.isinteger(42))
assert(!math.isinteger(4.25))
```

## 10.6 Random values

`math.random` uses the VM's pseudorandom state. `math.srandom` draws from the platform random source. Despite its name, `srandom` is a random-value function, not a seed-setting operation. Both support the same forms:

```li
import math

const unit = math.random()          # scaled to the unit interval
const below_ten = math.random(10)   # scaled between 0 and 10
const offset = math.srandom(5, 8)   # scaled between 5 and 8

assert(unit >= 0 && unit <= 1)
assert(below_ten >= 0 && below_ten <= 10)
assert(offset >= 5 && offset <= 8)
```

The optional bounds must be finite numbers. Use these functions for simulation and ordinary sampling; do not infer a cryptographic guarantee unless the host platform contract explicitly provides one.

## 10.7 Packed typed arrays

A typed array stores elements in one contiguous unboxed buffer. It avoids the per-element boxing and heterogeneous storage of an ordinary array.

| Constructor | Element storage |
| --- | --- |
| `typed.i8` / `i8[]` | Signed 8-bit integer. |
| `typed.u8` / `u8[]` | Unsigned 8-bit integer. |
| `typed.i16` / `i16[]` | Signed 16-bit integer. |
| `typed.u16` / `u16[]` | Unsigned 16-bit integer. |
| `typed.i32` / `i32[]` | Signed 32-bit integer. |
| `typed.u32` / `u32[]` | Unsigned 32-bit integer. |
| `typed.i64` / `i64[]` | Signed 64-bit storage at the strict/dynamic boundary. |
| `typed.u64` / `u64[]` | Unsigned 64-bit storage at the strict/dynamic boundary. |
| `typed.f32` / `f32[]` | IEEE 754 binary32 storage. |
| `typed.f64` / `f64[]` | IEEE 754 binary64 storage. |

After `import typed`, both `typed.T` and `T[]` name the constructor for element kind `T`. A constructor accepts no argument, a non-negative length, or an ordinary source array:

```li
import typed

let empty = typed.u8()
let zeroed = i32[](16)
let initialized = typed.f32([1, 2.5, 4])
assert(empty::len() == 0)
assert(zeroed::len() == 16)
assert(initialized[1] == 2.5)
```

Length construction zero-initializes every element. A store validates and rounds once to the array's storage width. Integer stores require a finite integral in-range number and never wrap or truncate. An `f32` store rounds once to binary32, while `f64` preserves binary64. Every numeric read widens and boxes the stored value as a dynamic binary64 number.

Dynamic `i64` and `u64` construction, storage, and reads are limited to the exactly representable binary64 integer range, through `2^53-1` in magnitude. A strict `i64` or `u64` value outside that range may exist in strict code, but crossing it into dynamic code raises a catchable boundary range error; Lightning has no first-class dynamic i64/u64 number.

## 10.8 Typed-array access and inspection

Bracket access is equivalent to `typed.get` and `typed.set`:

```li
import typed

let samples = i16[]([10, 20, 30])
assert(typed.get(samples, 1) == 20)
typed.set(samples, 1, 25)
assert(samples[1] == 25)
samples[2] = 35
```

A read beyond the current length returns `nil`. A write requires an existing finite, non-negative integral index and a valid element value. Negative, fractional, nonnumeric, or out-of-range write indices raise a catchable index or bounds error; assignment never grows the array.

Inspection operations are:

| Operation | Result |
| --- | --- |
| `typed.len(array)` | Logical element count. |
| `typed.capacity(array)` | Number of elements that fit before reallocation. |
| `typed.element_size(array)` | Bytes per stored element. |
| `typed.kind(array)` | Kind name such as `"u8"`, `"f32"`, or `"struct"`. |

```li
import typed

let bytes = u8[]([1, 2, 3])
assert(typed.len(bytes) == 3)
assert(bytes::len() == 3)
assert(typed.element_size(bytes) == 1)
assert(typed.kind(bytes) == "u8")
```

## 10.9 Typed-array mutation

### `typed.reserve(array, capacity)`

Ensures at least `capacity` elements of storage without changing logical length.

### `typed.resize(array, length)`

Changes logical length. Truncation discards trailing elements; extension zero-initializes new elements.

### `typed.fill(array, value)`

Validates `value` for the element kind, then fills every current element.

### `typed.dup(array)`

Returns an independent typed array with copied storage and capacity.

```li
import typed

let pixels = u8[](4)
typed.fill(pixels, 255)
typed.reserve(pixels, 64)
assert(typed.len(pixels) == 4)
assert(typed.capacity(pixels) >= 64)

typed.resize(pixels, 6)
assert(pixels[4] == 0 && pixels[5] == 0)

let copy = typed.dup(pixels)
copy[0] = 0
assert(pixels[0] == 255)
```

Counts and capacities must be finite, non-negative integers.

## 10.10 Struct typed arrays

`typed.struct_array(StructType, length)` stores struct fields inline. It is useful for dense records in strict numerical code.

```li
import typed

[[strict]] struct Point {
   x: f32 = 0f32
   y: f32 = 0f32
}

let points = typed.struct_array(Point, 2)
points[0] = Point{x: 1f32, y: 2f32}
let copy = points[0]
copy.x = 99f32
assert(points[0].x == 1f32)
```

The array stores each element inline. A read materializes a struct copy; `array[index] = value` and `array[index].field = value` copy fields back into inline storage rather than publishing a boxed alias. `typed.kind(points)` returns `"struct"` and `typed.element_size(points)` reports the inline stride.

`typed.dup` copies the inline buffer. `shared.create` likewise clones a struct typed array, its element-class metadata, and its inline elements into the process-wide shared heap; reads and writes of the shared clone retain the same copy semantics. Struct value rules and struct-array fusion in strict numerical loops are described in [Chapter 7](7-strict-tier.md).

## 10.11 Native vectors

The built-in vector classes use `f32` components and are documented with the system libraries in [Chapter 11 §11.6](11-stdlib-system-and-concurrency.md#116-native-vectors-vec3).
