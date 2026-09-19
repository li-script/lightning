# 12. Embedding API

Lightning can be embedded through a stable C ABI in `<lightning.h>` or a C++20 RAII layer in `<lightning.hpp>`. Both interfaces drive the same VM used by the `li` executable.

The C ABI is the portability boundary. It never permits a C++ exception to unwind through Lightning frames or across an `extern "C"` call. The C++ wrapper owns C handles, converts non-success statuses into `lightning::error`, and otherwise preserves the C API's execution and ownership rules.

## 12.1 Architecture

An embedded application normally performs these steps:

1. Create one or more `li_vm` instances.
2. Configure imports, allocation, and error observation if the defaults are insufficient.
3. Register host functions.
4. Load a script into a callable function or execute it immediately.
5. Convert arguments to Lightning values and call functions.
6. Release every owning value handle.
7. Close the VM.

A private VM admits one entrant at a time. Public operations serialize through its recursive lock, so a native callback may reenter the same VM on the current thread. Prefer one private VM per execution thread when parallel work is required. Explicit shared values are safe to retain across threads, but their mutable contents still require Lightning shared locks or atomic operations.

## 12.2 C ABI: `<lightning.h>`

Include the public header and link the installed Lightning library:

```c
#include <lightning.h>
```

All fallible operations return `li_status`. `LI_STATUS_OK` means success. Other statuses distinguish invalid arguments, closed VMs, type errors, script exceptions, callback failures, allocation failures, contained C++ exceptions, and lifetime errors. `li_status_name(status)` returns a static name; `li_vm_last_error` returns the VM's detailed diagnostic.

### Minimal C program

```c
#include <lightning.h>

#include <stdio.h>

static li_status twice(
    li_vm* vm,
    const li_value* const* args,
    size_t argument_count,
    void* userdata,
    li_value** result) {
    double input;
    static const char message[] = "twice expects one number";
    (void) userdata;

    if (argument_count != 1 ||
        li_value_get_number(args[0], &input) != LI_STATUS_OK)
        return li_vm_set_error(vm, message, sizeof(message) - 1);
    return li_value_create_number(vm, input * 2, result);
}

static int report_error(li_vm* vm, li_status status) {
    const char* message = NULL;
    size_t size = 0;

    if (vm != NULL &&
        li_vm_last_error(vm, &message, &size) == LI_STATUS_OK && size != 0)
        fprintf(stderr, "%.*s\n", (int) size, message);
    else
        fprintf(stderr, "%s\n", li_status_name(status));
    return 1;
}

int main(void) {
    static const char source[] = "twice(21)";
    li_vm* vm = NULL;
    li_value* value = NULL;
    li_status status;
    double answer;

    status = li_vm_create(NULL, &vm);
    if (status != LI_STATUS_OK)
        return report_error(NULL, status);

    status = li_vm_register_native(vm, "twice", 5, twice, NULL);
    if (status != LI_STATUS_OK) {
        report_error(vm, status);
        li_vm_close(vm);
        return 1;
    }

    status = li_vm_execute(
        vm, source, sizeof(source) - 1, "example", 7, &value);
    if (status != LI_STATUS_OK) {
        report_error(vm, status);
        li_vm_close(vm);
        return 1;
    }

    status = li_value_get_number(value, &answer);
    if (status != LI_STATUS_OK) {
        report_error(vm, status);
        li_value_release(value);
        li_vm_close(vm);
        return 1;
    }

    printf("%.0f\n", answer);
    li_value_release(value);
    li_vm_close(vm);
    return 0;
}
```

This program prints `42`.

### API naming

The exported ABI uses explicit `li_vm_*` and `li_value_*` names. Embedding discussions sometimes use the shorter conceptual names shown in the first column:

| Conceptual name | Exported C symbol | Operation |
|---|---|---|
| `li_create` | `li_vm_create` | Create a VM. |
| `li_close` | `li_vm_close` | Logically close a VM and consume the host pointer. |
| `li_eval` | `li_vm_execute` | Compile and execute source. |
| `li_load_script` | `li_vm_load` | Compile source and return a function without calling it. |
| `li_call` | `li_vm_call` | Invoke a Lightning function. |
| `li_register` | `li_vm_register_native` | Register a native function. |

Use the exported names when compiling against `<lightning.h>`.

### Lifecycle and ownership

```c
li_vm* vm = NULL;
li_status status = li_vm_create(NULL, &vm);
if (status != LI_STATUS_OK) {
    fprintf(stderr, "%s\n", li_status_name(status));
    return 1;
}

/* Register functions and run scripts. */

status = li_vm_close(vm);
vm = NULL; /* li_vm_close consumed the pointer, even if close reported an error. */
```

Passing `NULL` options selects defaults. To install hooks, initialize the versioned options structure first:

```c
li_vm_options options;
li_vm_options_init(&options);
options.import = my_import;
options.import_userdata = app;
options.error = observe_error;
options.error_userdata = app;

li_vm* vm = NULL;
li_status status = li_vm_create(&options, &vm);
```

Do not zero-initialize `li_vm_options` by hand: `li_vm_options_init` sets `struct_size` and future-compatible defaults. Do not change the initialized `struct_size`. Hook functions and their userdata are borrowed and must remain valid until physical VM destruction.

Every successful operation that writes an `li_value**` returns one owning persistent host handle. Release it exactly once with `li_value_release`; releasing `NULL` is harmless. `li_value_copy` creates another independently owned handle. The public C ABI exposes strong handles only. A handle whose value has kind `LI_KIND_WEAK` keeps the weak observer alive, not its target; `weak.lock` returns a new strong script reference or `nil` as described in [Chapter 2](2-basic-concepts.md#cycles-and-weak-references).

`li_vm_close` consumes the host's VM pointer immediately and rejects new script work. Never use that pointer again, even when close reports an error. Outstanding `li_value` handles retain the underlying VM and delay physical heap destruction until the last handle is released. Kind and value getters, `li_value_copy`, and `li_value_release` remain valid on those handles after close. A string or userdata pointer returned by a getter is borrowed from its handle and must not outlive that handle.

Synchronize close against new calls from other threads. The source VM of a live handle may already be logically closed when the value is copied or shared to an open destination VM.

### Loading, evaluating, and calling

`li_vm_load` compiles a byte span and returns an owning function handle. It does not call the function. `li_vm_execute` compiles and immediately calls the resulting top-level function.

```c
static const char source[] = "fn square(x) { x * x }\nsquare";
li_value* square = NULL;
li_status status = li_vm_execute(
    vm,
    source, sizeof(source) - 1,
    "embedded-square.li", sizeof("embedded-square.li") - 1,
    &square);
```

The source and source-name parameters are byte spans and need not be NUL-terminated. The source name appears in diagnostics and stack traces.

Call a returned function by passing borrowed argument handles. The result is a new owning handle:

```c
li_value* argument = NULL;
li_value* result = NULL;
const li_value* arguments[1];
double number = 0;

status = li_value_create_number(vm, 9.0, &argument);
if (status == LI_STATUS_OK) {
    arguments[0] = argument;
    status = li_vm_call(vm, square, arguments, 1, &result);
}
if (status == LI_STATUS_OK)
    status = li_value_get_number(result, &number);

li_value_release(result);
li_value_release(argument);
li_value_release(square);
```

Output handles are set to `NULL` on failure. Always check the status before inspecting them. `li_value_release(NULL)` is harmless.

### Reporting errors

```c
static void print_error(li_vm* vm, li_status status) {
    const char* message = NULL;
    size_t size = 0;

    if (vm != NULL &&
        li_vm_last_error(vm, &message, &size) == LI_STATUS_OK &&
        size != 0) {
        fprintf(stderr, "%.*s\n", (int)size, message);
    } else {
        fprintf(stderr, "%s\n", li_status_name(status));
    }
}
```

The message returned by `li_vm_last_error` is borrowed and remains valid only until the VM reports another error. The error hook observes a report but does not replace status checking or `li_vm_last_error`. A native or import callback uses `li_vm_set_error` to create a catchable script exception and returns the resulting `LI_STATUS_SCRIPT_ERROR`.

### Hooks

`li_vm_options` provides allocator, import, and error hooks. All strings passed to callbacks are byte spans and need not be NUL-terminated. The hooks and their userdata are borrowed; keep them valid until physical VM destruction. Registered native userdata follows the same host-owned lifetime rule.

The allocator works in 4096-byte pages. A call with a `NULL` pointer requests `page_count` pages; returning `NULL` reports allocation failure. A call with a non-`NULL` pointer releases the same page count and must return `NULL`. `executable` is nonzero for code pages. Physical destruction can release pages after `li_vm_close` when value handles remain alive. An allocator exception is contained as an allocation failure; during VM creation it produces `LI_STATUS_CXX_EXCEPTION`.

The import hook receives the importing module name and the requested module name. On success it must transfer one owning handle from the callback's VM whose value is an exports table. Returning another kind produces `LI_STATUS_CALLBACK_ERROR` and the importing script fails. The hook may reenter `li_vm_execute`, for example to execute a loader that returns its `$E` exports table. Once transferred, the hook must not release or reuse the handle. On failure, call `li_vm_set_error` before returning so the importing script receives a useful catchable exception.

The error hook runs synchronously with a borrowed message. An exception thrown by the error hook is swallowed. An exception thrown by a native or import callback becomes a script failure, and an exception thrown by a userdata destructor is reported through the error hook.

Fatal VM invariant failures are process or host failures, not catchable script exceptions. The public C ABI has no panic hook; do not treat the error callback as one.

## 12.3 Values, `any_t`, and NaN-boxing

Inside the runtime, a Lightning value is represented by the eight-byte NaN-boxed word commonly called `any_t`. Ordinary IEEE 754 binary64 numbers are stored directly. Reserved quiet-NaN bit patterns encode `nil`, booleans, and heap references. Numeric NaNs are canonicalized so they remain disjoint from tagged values. Heap pointers retain all required low address bits.

NaN-boxing is an implementation representation, not permission for an embedder to reinterpret value bits. The public C ABI deliberately exposes opaque `li_value*` handles. This keeps ownership, VM association, pointer-width details, and future representation changes behind the ABI.

### Type tests and conversions

Bindings often describe conversion helpers generically as three families:

- `li_is_*` — test whether an `any_t` has a particular kind;
- `li_as_*` — extract an already-validated payload;
- `li_from_*` — construct a boxed value.

At the public handle boundary, the corresponding supported operations are:

| Generic family | Public C operation |
|---|---|
| `li_is_*` | `li_value_get_kind(value)` and comparison with `LI_KIND_*`. |
| `li_as_bool` | `li_value_get_bool(value, &out)`. |
| `li_as_number` | `li_value_get_number(value, &out)`. |
| `li_as_string` | `li_value_get_string(value, &data, &size)`. |
| `li_from_nil` | `li_value_create_nil(vm, &out)`. |
| `li_from_bool` | `li_value_create_bool(vm, value, &out)`. |
| `li_from_number` | `li_value_create_number(vm, value, &out)`. |
| `li_from_string` | `li_value_create_string(vm, data, size, &out)`. |

Getters perform checked conversion and return `LI_STATUS_TYPE_ERROR` on a mismatched kind; they do not coerce. A string view returned by `li_value_get_string` is borrowed from the owning handle and must not outlive that handle.

`li_value_copy` creates another independently owned persistent handle to the same value. The copy must be released exactly once. Callback argument handles are borrowed and must not be released; copy one before retaining it beyond the callback.

### Cross-VM values

Private heap values belong to their creating VM. Creating another handle or passing a value to an API does not silently publish its object graph. Use explicit transfer:

- `li_value_share(destination, source, &result)` copies a supported private string, table, array, class instance, class, or script function into process-wide shared storage. Private strings become immutable shared strings. Immediate roots, unsupported heap graphs, and graphs with private mutable child edges fail with `LI_STATUS_SCRIPT_ERROR`; an already shared value is retained.
- `li_value_copy_to(destination, source, &result)` transfers immediate values and shared values, copies an immutable private string into the destination, and returns `LI_STATUS_TYPE_ERROR` for another private heap value. When source and destination are the same VM, it behaves like an ordinary handle copy.

Both operations borrow `source`, set `result` to `NULL` on failure, and return one independently owned destination handle on success. The destination VM must be open. The source VM may be logically closed because the source handle keeps its heap alive.

To pass a foreign immediate value or private string to `li_vm_call`, first rehome it with `li_value_copy_to`. A foreign shared function or shared argument may be used directly. The target VM owns the call's returned handle.

The C++ spellings are `destination.share(source)` and `destination.copy_to(source)`. Script code publishes values explicitly with `shared.create`. Concurrent compound mutation of a shared container must use `shared.lock` and `shared.unlock`; supported numeric field updates may use `shared.atomic_add`. [Chapter 11](11-stdlib-system-and-concurrency.md) describes the complete locking contract.

## 12.4 Registering native functions

A native callback has this signature:

```c
typedef li_status (*li_native_fn)(
    li_vm* vm,
    const li_value* const* args,
    size_t argument_count,
    void* userdata,
    li_value** result);
```

The callback receives its VM, borrowed arguments, host userdata, and an output slot for an optional owning result.

### Native names and transfers

An unqualified registration such as `"twice"` is placed in `builtin` and is visible as a script global. A dotted name preserves its module path. Canonical duplicate detection treats `"twice"` and `"builtin.twice"` as the same registration; registering either form again returns `LI_STATUS_INVALID_ARGUMENT`. Registration borrows `userdata`; the host controls its lifetime.

The argument array and each argument handle are borrowed only for the callback. Do not release them. Call `li_value_copy` before retaining an argument beyond the callback. A borrowed value that is already finalizing cannot be resurrected: copying it returns `LI_STATUS_LIFETIME_ERROR`, leaves the output `NULL`, and reports the diagnostic through the error hook and `li_vm_last_error`.

`*result` starts as `NULL`. On success, leave it `NULL` to return `nil`, or transfer one owning handle created by the callback's VM. After transfer, do not release or reuse the handle. On failure, use `li_vm_set_error` to set the current script exception and return the resulting status.

### Custom calling signatures

The stable C callback always receives a value array, but a binding layer can impose a custom signature by validating arity and kinds before dispatch:

```c
typedef struct scale_context {
    double factor;
} scale_context;

static li_status scale_native(
    li_vm* vm,
    const li_value* const* args,
    size_t count,
    void* userdata,
    li_value** result) {
    scale_context* context = (scale_context*)userdata;
    double input;

    if (count != 1 ||
        li_value_get_number(args[0], &input) != LI_STATUS_OK) {
        static const char message[] = "scale(number) expected";
        return li_vm_set_error(vm, message, sizeof(message) - 1);
    }
    return li_value_create_number(vm, input * context->factor, result);
}
```

A generated trampoline may perform the same checks for richer signatures, optional parameters, or userdata receivers. Keep the C callback as the exception boundary: catch host-language exceptions, convert them to `li_vm_set_error`, and return a failure status. Although the ABI contains C++ exceptions automatically, explicit conversion produces better domain diagnostics.

## 12.5 Stack, thread, and lifetime safety

Native code participates in Lightning's ownership and call stack. Follow these rules:

1. Treat callback arguments as borrowed. Never release them.
2. Use `li_value_copy` before storing an argument after the callback returns.
3. Return only an owning handle from the callback's VM, and transfer it exactly once.
4. Check every `li_status`; do not interpret an output after failure.
5. Do not keep string or userdata views past the owning handle's lifetime.
6. Do not release the same handle concurrently with an operation using it.
7. Do not let a C++ exception escape a C callback intentionally.
8. Avoid unbounded native recursion. Calling back into Lightning consumes native and Lightning stack space and may encounter coroutine stack-headroom checks.
9. A callback may reenter its VM, but it must preserve its borrowed inputs and must tolerate script exceptions returned by the nested call.
10. Do not close a VM while another thread is beginning an entry. Closing from the currently executing callback is allowed, but the in-flight script invocation then fails as closed.

Public C operations that enter or mutate a VM, together with handle copy and release, serialize through the VM's recursive lock. A native or import callback may therefore reenter the public API on the same VM from its current thread. Cross-VM sharing and private-string copying lock both VMs in a stable order.

Independent host calls and distinct handles may be used from different threads, but each VM still admits one executing entrant at a time. Never race an operation on a particular handle with release of that handle. Shared-value reference counts are cross-thread safe; mutable shared contents still require the shared locks or atomic operations in [Chapter 11](11-stdlib-system-and-concurrency.md). Prefer one private VM per execution thread.

Closing a VM from its current native or import callback is allowed. The close consumes the pointer and the in-flight script operation then fails with `LI_STATUS_CLOSED`. Externally synchronize close against any thread that could begin a new entry.

Script throws unwind interpreted, JIT, and native-call frames in one logical order. When a native calls back into script, an uncaught script exception returns as a non-OK status; the native may handle it or propagate a catchable script error with `li_vm_set_error`. An exception escaping the outermost C invocation returns failure and is never disguised as a successful `nil`. The diagnostic is available through `li_vm_last_error`.

`debug.traceback(error)` returns frames innermost first with `function`, `line`, and `native` fields. Registered native frames have `native == true`; interpreted and JIT frames have `native == false` and report the same source line for the same function. Rethrowing preserves the original frames.

Lightning catches exceptions from a C++ implementation of a native or import callback before they can unwind through VM or JIT frames. Public operations report `LI_STATUS_CXX_EXCEPTION` where a status can be returned. Do not throw from callbacks as a control-flow mechanism.

## 12.6 C++20 RAII interface: `<lightning.hpp>`

The C++ wrapper provides `lightning::vm`, `lightning::value`, and `lightning::error`.

```cpp
#include <lightning.hpp>

#include <array>
#include <iostream>

int main() {
    try {
        lightning::vm vm;
        lightning::value square = vm.execute(
            "fn square(x) { x * x }\nsquare",
            "cpp-example.li");
        lightning::value input = vm.number(12);
        std::array<lightning::value, 1> arguments{std::move(input)};
        lightning::value output = vm.call(square, arguments);
        std::cout << output.as_number() << '\n';
    } catch (const lightning::error& error) {
        std::cerr << li_status_name(error.code())
                  << ": " << error.what() << '\n';
        return 1;
    }
}
```

### `lightning::vm`

`lightning::vm` is move-only and closes its handle in its destructor. Its principal methods are:

| Method | C equivalent |
|---|---|
| `load(source, source_name)` | `li_vm_load` |
| `execute(source, source_name)` | `li_vm_execute` |
| `call(function, arguments)` | `li_vm_call` |
| `register_native(name, callback, userdata)` | `li_vm_register_native` |
| `nil()`, `boolean(v)`, `number(v)`, `string(v)` | `li_value_create_*` |
| `userdata(type, pointer, destroy, userdata)` | `li_value_create_userdata` |
| `share(value)` | `li_value_share` |
| `copy_to(value)` | `li_value_copy_to` |
| `close()` | `li_vm_close` |

`lightning::value` releases its owning C handle in its destructor. It is copyable by creating an independent C handle and movable without allocation. `kind()`, `as_bool()`, `as_number()`, `as_string()`, and userdata accessors provide checked inspection. A `std::string_view` returned by `as_string()` remains borrowed from the `lightning::value`.

### Exception conversion

Every wrapper method checks the returned status. A failure throws `lightning::error`, derived from `std::runtime_error`. `what()` contains the VM diagnostic and `code()` preserves the original `li_status`.

This conversion is one-way at the wrapper boundary:

- script or VM failure becomes `lightning::error` in host C++;
- a C++ exception thrown by a registered callback is contained by the C ABI and becomes a catchable script/native failure;
- no C++ exception is allowed to unwind through interpreter or JIT frames.

Catch `lightning::error` around each host transaction where recovery is meaningful. Destructors for `lightning::value` and `lightning::vm` are non-throwing cleanup boundaries.

## 12.7 Userdata

Register a process-global opaque type ID, then create typed userdata values:

```cpp
struct counter { int value; };

const li_type_id counter_type =
    lightning::register_userdata_type("example.counter");

lightning::value wrapped = vm.userdata(
    counter_type,
    new counter{0},
    [](void* pointer, void*) {
        delete static_cast<counter*>(pointer);
    });
```

`li_userdata_type_register` maps an exact byte name to a process-global opaque type ID. Registering the same name again returns the same ID, including from another VM. Register a type before creating its values; do not assign IDs yourself.

The destroy callback runs exactly once when the last script or host reference is released, possibly after logical VM close. Retrieving a pointer requires the expected type ID; a mismatch returns `LI_STATUS_TYPE_ERROR`. Lightning synchronizes the userdata object's lifetime, not the pointee's internal state—the host remains responsible for that synchronization and for the pointee's invariants.

## 12.8 Security boundary

Lightning executes trusted scripts. It is not a sandbox and provides no memory, CPU, recursion, allocation, module, filesystem, native-call, or denial-of-service isolation from hostile code. Validate untrusted input outside the VM, expose only intended natives and imports, and enforce resource and privilege boundaries at the process level.

The `fs` module uses the host process's filesystem permissions and performs no path or capability confinement. The host controls whether scripts can import it; an embedder that needs confinement must deny it through the import policy, replace it with a restricted module, or isolate the process. Likewise, `loadstring` and `eval` compile and execute source with the VM's available modules and native bindings. The host must treat access to them as permission to run dynamic Lightning code.

A crash, type confusion, or ownership violation caused by well-formed trusted script is still an implementation bug. The trusted-script boundary does not weaken the API contracts in this chapter.

## 12.9 Embedding checklist

- Initialize options with `li_vm_options_init`.
- Check every `li_status` or catch `lightning::error`.
- Give source buffers meaningful source names.
- Release every owning C handle exactly once.
- Copy borrowed callback arguments before retaining them.
- Keep registered userdata and hook contexts alive long enough.
- Convert host failures into `li_vm_set_error` diagnostics.
- Use explicit `share`/`copy_to` operations across VMs.
- Synchronize VM close and same-handle release across threads.
- Enforce trust and resource boundaries outside Lightning.
