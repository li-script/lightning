# 3. Language Syntax

Lightning source is UTF-8 text with line-oriented statements, C-like expressions, and brace-delimited blocks. This chapter describes the lexical and declaration rules used throughout the manual.

## Lexical conventions

Whitespace separates tokens and is otherwise insignificant except that a newline normally terminates a statement. Spaces and tabs may be used for indentation; indentation has no syntactic meaning.

Lightning identifiers begin with a letter or underscore and continue with letters, decimal digits, or underscores. Identifiers are case-sensitive:

```li
let item = 1
let item_count = 2
let Item = 3 // distinct from item
```

Choose identifiers that describe their role. By convention, ordinary variables and functions use lower-case names, often with underscores, while classes and structs use capitalized names.

### Keywords

The following words are reserved by the language and cannot be used as ordinary identifiers:

```text
true false nil
let const fn
if else while for loop break continue return leave
try catch throw defer
yield
in is as
class struct object function table array string number bool type
export import dyn match delete
```

Some declaration forms also use contextual words, such as `get`, `set`, and lifecycle or operator hook names ending in `!`. A contextual word is only special in the grammar position that gives it that meaning.

## Comments

Lightning has two line-comment forms. Both continue to the end of the physical line:

```li
// C-style line comment
# shell-style line comment
const answer = 42 // trailing comment
```

C-style block comments use `/*` and `*/`, may span lines, and may nest. They preserve their physical line boundaries for statement separation and diagnostics:

```li
/* This comment can cover
   several lines, including /* a nested comment */ safely. */
const ready = true
```

Long block comments use a `#` followed by matching long-bracket delimiters:

```li
#[=[
Everything here is ignored, including "quotes" and /* markers */.
]=]
```

Additional `=` characters let a comment contain a shorter closing delimiter:

```li
#[==[
The text ]=] does not close this comment.
]==]
```

## Literals

### `nil` and booleans

```li
const missing = nil
const yes = true
const no = false
```

### Numeric literals

Decimal integer and floating-point spellings produce numbers:

```li
const count = 42
const ratio = 3.14159
const small = 6.25e-2
const large = 1e6
```

Binary and hexadecimal prefixes are available. Non-decimal literals may also have a fractional part:

```li
const mask = 0xff
const flags = 0b1010
const binary_fraction = 0b101.01 // 5.25
const hex_fraction = 0xA.F      // 10.9375
```

Digit separators improve readability:

```li
const population = 1'000'000
```

Type suffixes communicate a literal's intended typed or strict representation:

```li
const narrow_float = 1f32
const signed_count = 1i32
const wide_unsigned = 1u64
```

Available typed suffixes include `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`, `f32`, and `f64`; shorthand suffixes may also be accepted by typed contexts. In ordinary dynamic code, language arithmetic still follows binary64 semantics. A suffix becomes significant to strict checking, typed storage, and specialization rather than creating a second dynamic numeric runtime type.

### Quoted strings and character literals

Double-quoted literals represent immutable byte strings and do not interpolate. They interpret `\\`, `\"`, `\'`, `\a`, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, two-digit hexadecimal `\xNN`, and four-digit Unicode `\uNNNN` escapes. A Unicode escape is encoded as UTF-8 bytes; it does not change the byte-oriented indexing and length rules. An unknown or truncated escape is a syntax error.

Single-quoted literals represent one numeric byte rather than a string:

```li
const text = "double quoted"
const byte_a = 'A'

assert(byte_a == 65)
```

```li
const two_lines = "first\nsecond"
const letter_a = "\x41"
const han = "\u6F22"
```

### Raw quoted strings

Prefix a quoted string with `r` to preserve its bytes without processing escapes or interpolation. Add matching `#` delimiters when the content must contain a quote; a quote closes the string only when it is followed by the opener's number of hashes.

```li
const path = r"C:\users\name\file.txt"
const quoted = r#"The word "Lightning" needs no escapes."#
```

Raw quoted strings may span lines. Use additional `#` characters when the content contains a would-be closing delimiter.

### Long strings

A long string begins with `[` followed by one or more `=` characters and another `[`. It ends with `]`, the same number of `=` characters, and `]`. At least one `=` is required because `[[` opens an attribute list, as described in [Chapter 7](7-strict-tier.md).

```li
const path = [=[C:\users\name\file.txt]=]
const multiline = [==[
Long strings preserve backslashes, "quotes", and newlines.
The shorter token ]=] is ordinary content here.
]==]
```

Long strings do not process escapes or interpolation. Increase the number of `=` characters when the content contains a would-be closing delimiter.

### Template format strings

Backtick strings interpolate an expression enclosed in `{` and `}`. Each expression is evaluated once, from left to right, and converted to its display string:

```li
const name = "Mira"
const completed = 4
const total = 7
const message = `Hello {name}: {completed + 1} of {total} complete`
```

Double braces produce literal braces:

```li
const example = `Use {{name}} to show a literal field`
```

Within literal portions, `` \` `` emits a backtick, `{{` and `}}` emit braces, and the ordinary string escapes have the same meanings as in a double-quoted string. An unknown or truncated escape, unmatched brace, or incomplete interpolation is a syntax error.

An interpolation expression is evaluated exactly once before the next expression, then converted with `str`. Quoted strings, raw strings, long strings, and comments inside the expression are recognized as complete tokens, so braces inside them do not close the interpolation.

## Statement termination

A source line contains one statement unless semicolons explicitly separate several. A newline terminates a complete statement:

```li
let width = 8
let height = 6
print(width * height)
```

A semicolon separates multiple statements on one line:

```li
let width = 8; let height = 6; print(width * height)
```

Two complete expressions on one line, such as `3 4`, are a syntax error. A semicolon is optional before a newline or a closing brace.

A newline does not terminate a statement inside parentheses or brackets, inside a brace-delimited literal or initializer, or immediately after a binary operator:

```li
const total = 10 +
    20
assert(total == 30)
```

A newline before a binary operator does not continue the preceding statement. Line comments end at that newline, and block comments preserve their contained line boundaries; comments do not otherwise change statement termination.

Prefer one statement per line. Use semicolons only where keeping closely related short statements together genuinely improves readability.

## Bindings and declarations

### Mutable bindings with `let`

`let` creates a mutable lexical binding:

```li
let attempts = 0
attempts += 1
attempts = attempts + 1
```

The binding may be reassigned. Mutation of the object held by a binding is separate from reassignment of the binding itself.

### Immutable bindings with `const`

`const` creates a binding that cannot be reassigned:

```li
const timeout = 30
// timeout = 60 // error
```

`const` does not recursively freeze a referenced collection. The binding remains fixed, but a mutable object it refers to can still be changed:

```li
const settings = {theme: "dark"}
settings.theme = "light" // allowed: settings still names the same table
```

Use `const` by default and `let` when reassignment is part of the algorithm.

### Lexical scope

A block delimited by braces introduces a lexical scope. Inner code can read outer bindings, but bindings declared in the block disappear when the block ends:

```li
const outer = 10
if outer > 0 {
    const inner = outer * 2
    print(inner)
}
// inner is not visible here
```

Function parameters and loop variables are also lexical bindings. A declaration cannot redeclare a parameter or another binding in the same scope. Duplicate module bindings, duplicate class fields, and methods that are not valid dynamic overrides are errors rather than implicit replacement.

Name lookup selects the nearest declaration visible at the use site. Closures likewise capture the nearest declaration visible where the closure is created.

### Shadowing

An inner scope may declare a new binding with the same name, including the name of a module binding or builtin. The inner declaration shadows, rather than changes, the outer binding:

```li
const label = "outer"
if true {
    const label = "inner"
    print(label) // inner
}
print(label)     // outer
```

Shadow deliberately and sparingly. A different name is usually clearer when both values matter to the reader.

### Annotations in dynamic code

A type annotation in dynamic code is a runtime check and an optimizer hint. It does not coerce the value or change its storage layout, ownership, mutability, runtime representation, or overload resolution.

The check runs whenever a value enters annotated storage: initialization, later assignment, argument binding, an annotated field store, and an annotated return. Failure raises a catchable type error at that boundary. The JIT may omit a check only after proving it redundant.

```li
fn double(value: number) -> number {
    value * 2
}

let result: number = double(6)
result = 14
assert(result == 14)
```

Compile-time guarantees belong to the optional strict tier described in [Chapter 7](7-strict-tier.md). An annotation in ordinary dynamic code remains a runtime boundary.

## Complete example

```li
# Compute a display label from immutable inputs.
const product = "Lightning"
const major = 1i32
const notes = [=[fast, embeddable, deterministic]=]

if major > 0 {
    const label = `{product} {major}: {notes}`
    print(label)
}
```

Continue with [Control Flow](4-control-flow.md).
