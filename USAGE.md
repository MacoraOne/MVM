# MVM Usage and Language Integration

This guide explains how to clone MVM, compile it, use its C API, and connect a language implementation to the bytecode runtime. The API declarations in `vm.h` and implementation in `vm.c` are authoritative; `BYTECODE_API.md` documents the opcode families and stack contracts.

## 1. Clone the repository

```sh
git clone https://github.com/MacoraOne/MVM.git
cd MVM
```

To update a local clone later:

```sh
git pull --ff-only
```

## 2. Build and run the regression tests

```sh
make clean test
```

Build the test executable without running it:

```sh
make clean all
./vm-test
```

The Makefile uses C11 and strict compiler diagnostics. A successful test run prints `MACORAONE_VM_ALL_TESTS_PASS`.

## 3. Minimal embedding example

The following program creates a VM, emits bytecode for 40 + 2, runs it, prints the result, and frees the VM. Save it as `example.c` in the repository root:

```c
#include "vm.h"
#include <stdio.h>

int main(void) {
    Mvm *vm = mvm_new();
    if (!vm) {
        fputs("Could not create VM\n", stderr);
        return 1;
    }

    int forty = mvm_add_constant(vm, mvm_number(40));
    int two = mvm_add_constant(vm, mvm_number(2));
    int ok = forty >= 0 && two >= 0
        && mvm_emit_constant(vm, forty)
        && mvm_emit_constant(vm, two)
        && mvm_emit_add(vm)
        && mvm_emit_halt(vm)
        && mvm_run(vm);

    if (!ok) {
        fprintf(stderr, "MVM error: %s\n", mvm_last_error(vm));
        mvm_free(vm);
        return 1;
    }

    mvm_value_print(stdout, mvm_result(vm));
    putchar('\n');
    mvm_free(vm);
    return 0;
}
```

Compile and run it:

```sh
cc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation example.c vm.c -o example -lm
./example
```

Expected output:

```text
42
```

## 4. Recommended architecture for a complete language

Treat MVM as the execution backend, not as a complete source language. Build these frontend/runtime layers in your own project:

1. **Lexer:** convert source text into tokens and preserve file/line/column locations.
2. **Parser:** build an AST for expressions, statements, declarations, functions, arrays, maps, and control flow.
3. **Semantic analysis:** resolve variable names, validate argument counts and types if your language is typed, and report errors before execution.
4. **Compiler/lowering:** translate AST nodes to MVM constants and bytecode. Keep a mapping from bytecode instruction index to source location.
5. **Runtime built-ins:** register host functions such as input/output, file access, time, or project-specific APIs with `mvm_register_native`. Validate arguments in every callback.
6. **Execution:** check every API return value. On failure, report `mvm_last_error(vm)` along with the frontend's source-location mapping.
7. **Diagnostics and profiling:** enable the profiler when requested and map hot instruction indexes back to source lines.
8. **Lifecycle:** create one VM per execution context as appropriate and call `mvm_free` when finished.

## 5. Emitting literals and arithmetic

Add constants to the VM's constant pool, then emit the instructions that consume them. Check all return values because the VM reports invalid indexes, allocation failures, and code limits through failed API calls and `mvm_last_error`.

```c
int a = mvm_add_constant(vm, mvm_number(10));
int b = mvm_add_constant(vm, mvm_number(32));
if (a < 0 || b < 0 ||
    !mvm_emit_constant(vm, a) ||
    !mvm_emit_constant(vm, b) ||
    !mvm_emit_add(vm) ||
    !mvm_emit_halt(vm) ||
    !mvm_run(vm)) {
    fprintf(stderr, "Runtime error: %s\n", mvm_last_error(vm));
}
```

`mvm_run` starts execution at instruction zero. Build a complete instruction stream before running it; use `mvm_instruction_count` to inspect the current code length and `mvm_patch_operand` to patch branch targets.

## 6. Branches and loops

Branch operands are absolute instruction indexes, not byte offsets. Emit a placeholder target and patch it after emitting the target block:

```c
size_t branch_ip = mvm_instruction_count(vm);
if (!mvm_emit_jump_if_false(vm, 0)) { /* handle error */ }
/* Emit the true branch here. */
size_t target_ip = mvm_instruction_count(vm);
if (!mvm_patch_operand(vm, branch_ip, (int32_t)target_ip)) { /* handle error */ }
```

Use the precise semantics and helper declarations in `vm.h`; add tests for every control-flow pattern your compiler generates.

## 7. Arrays, maps, and lengths

Host code can use the object APIs directly:

```c
MvmValue array = mvm_array_new(vm);
if (array.type != MVM_VALUE_ARRAY ||
    !mvm_array_push(vm, array, mvm_number(7)) ||
    !mvm_array_push(vm, array, mvm_number(9))) {
    fprintf(stderr, "Array error: %s\n", mvm_last_error(vm));
}

MvmValue first = mvm_null();
if (mvm_array_get(vm, array, 0, &first)) {
    mvm_value_print(stdout, first);
}

MvmValue map = mvm_map_new(vm);
if (map.type == MVM_VALUE_MAP)
    (void)mvm_map_set(vm, map, "answer", mvm_number(42));
```

Arrays use zero-based indexes. Map keys are strings. Missing map keys and out-of-range array indexes are runtime errors for the corresponding get operations. The bytecode collection contracts are listed in `BYTECODE_API.md`.

## 8. User functions and closures

Create function descriptors with `mvm_function_new(vm, name, arity, local_count, upvalue_count, entry)`, add descriptors to the constant pool, and patch the entry point once the function body has been emitted. Normally emit a jump around function bodies so top-level execution does not fall into them. At a call site, the stack contains the closure followed by arguments in source order. `CALL_FUNCTION argc` checks the arity and creates a bounded call frame; arguments initialize local slots starting at zero. Return with `RETURN`.

Closures copy capture values when created. `SET_UPVALUE` changes the capture in that closure, but separate closures do not share an open-upvalue cell in this release. Design your language's closure semantics with that limitation in mind.

## 9. Native functions: connect your standard library

Use `mvm_register_native(vm, name, callback, userdata)` to register a C function. It returns a native-function index or a failure value; inspect the declaration in `vm.h`. Your callback receives the VM, its argument values/count, and the userdata pointer. Validate the count and every argument type before using them, then return an `MvmValue`. Emit a native call using the returned index and the number of arguments with `mvm_emit_call_native`.

Use native callbacks as the boundary for operating-system services, host libraries, file APIs, networking, or built-in language functions. Avoid letting callbacks keep pointers to VM-owned objects after `mvm_free`.

## 10. Profiling

Enable profiling before execution:

```c
mvm_profile_enable(vm, true);
if (!mvm_run(vm))
    fprintf(stderr, "%s\n", mvm_last_error(vm));
const MvmProfile *profile = mvm_profile_data(vm);
if (profile) {
    printf("instructions=%llu max_stack=%zu\n",
        (unsigned long long)profile->instructions_executed,
        profile->max_stack_depth);
}
mvm_profile_write_text(vm, stdout);
```

The VM can write text and JSON profile reports and return per-instruction hit counts. To make a source-level profiler, join each instruction index to the source mapping retained by your compiler.

## 11. Memory and error-handling rules

- Always call `mvm_free` for every successfully created VM.
- Check return values from emitters, registration calls, collection operations, and `mvm_run`.
- Read `mvm_last_error(vm)` immediately when an operation fails; clear errors only when you intentionally begin a new operation.
- Strings supplied through `mvm_string` or string constants must remain alive for the VM lifetime unless the API explicitly says the VM owns the allocation.
- Heap arrays, maps, function descriptors, and closures are retained until `mvm_free`; there is no tracing garbage collector in this version.
- Treat object values as invalid after their owning VM is freed.
- The VM has bounded code, constants, operand stack, locals, native functions, upvalues, and call frames; handle limit errors in your frontend.
- The VM is not a sandbox. Native callbacks execute host C code with the permissions of the process. Add explicit restrictions if executing untrusted programs.

## 12. Test workflow

GitHub Actions is manual-only. Open [MVM Actions](https://github.com/MacoraOne/MVM/actions/workflows/test-vm.yml), select **Run workflow**, and start the run. The workflow builds with strict warnings, executes the tests, and runs sanitizer checks. Commits and pushes do not start it automatically.

## API reference

- [Public C header](vm.h)
- [Bytecode and embedding API](BYTECODE_API.md)
- [VM tests](test_vm.c)
- [Build file](Makefile)

