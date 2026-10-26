# MacoraOne VM bytecode, profiler, and embedding API

The public ABI is `vm.h`; the execution engine is `vm.c`. Compile `vm.c` with your language's C runtime and include `vm.h`.

## Bytecode families

| Family | Instructions / API |
|---|---|
| Values | `CONSTANT`, `NULL`, `TRUE`, `FALSE` |
| Stack | `POP`, `DUP`, `SWAP`, `OVER` |
| Arithmetic | `ADD`, `SUBTRACT`, `MULTIPLY`, `DIVIDE`, `MODULO`, `NEGATE`, `POWER`, `ABS`, `SQRT`, `FLOOR`, `CEIL`, `ROUND` |
| Comparison | `EQUAL`, `NOT_EQUAL`, `GREATER`, `GREATER_EQUAL`, `LESS`, `LESS_EQUAL` |
| Logic | `NOT`, `AND`, `OR`, `TO_BOOL` |
| Locals | `LOAD_LOCAL`, `STORE_LOCAL` |
| Control flow | `JUMP`, `JUMP_IF_FALSE`, `JUMP_IF_TRUE`, `LOOP` |
| Strings and types | `TYPEOF`, `STRING_LENGTH`, string concatenation through `ADD` |
| Integer bitwise | `BIT_AND`, `BIT_OR`, `BIT_XOR`, `SHIFT_LEFT`, `SHIFT_RIGHT` |
| Heap collections | `ARRAY_NEW`, `ARRAY_GET`, `ARRAY_SET`, `MAP_NEW`, `MAP_GET`, `MAP_SET`, `LENGTH` |
| User functions | `CLOSURE`, `CALL_FUNCTION`, `GET_UPVALUE`, `SET_UPVALUE` |
| Host I/O and termination | `PRINT`, `CALL_NATIVE`, `RETURN`, `HALT` |

Use typed `mvm_emit_*` functions for compiler output. `mvm_emit(vm, opcode, operand)` is available for generic backends. Branch operands are absolute instruction indexes and can be patched with `mvm_patch_operand`. Constants are added through `mvm_add_constant`.

## Minimal compiler/runtime integration

```c
Mvm *vm = mvm_new();
int c = mvm_add_constant(vm, mvm_number(40));
int d = mvm_add_constant(vm, mvm_number(2));
if (c < 0 || d < 0 ||
    !mvm_emit_constant(vm, c) || !mvm_emit_constant(vm, d) ||
    !mvm_emit_add(vm) || !mvm_emit_halt(vm) || !mvm_run(vm)) {
    fprintf(stderr, "VM error: %s\\n", mvm_last_error(vm));
}
MvmValue result = mvm_result(vm);
mvm_free(vm);
```

A frontend owns lexing, parsing, source locations, and lowering to this stable public API. The host can register built-ins as C callbacks with `mvm_register_native`; bytecode references the returned native index using `mvm_emit_call_native`. Native arguments are passed in source order; a callback returns one `MvmValue`. Keep any strings passed into `mvm_string` or string constants alive for the VM's lifetime; concatenation results are VM-owned.

## Built-in profiler

Profiling is optional and disabled by default. Enable it before `mvm_run`:

```c
mvm_profile_enable(vm, true);
if (!mvm_run(vm)) fprintf(stderr, "%s\\n", mvm_last_error(vm));
const MvmProfile *p = mvm_profile_data(vm);
printf("instructions=%llu max_stack=%zu elapsed_ns=%llu\\n",
       (unsigned long long)p->instructions_executed,
       p->max_stack_depth, (unsigned long long)p->elapsed_ns);
mvm_profile_write_text(vm, stdout);
FILE *json = fopen("profile.json", "w");
if (json) { mvm_profile_write_json(vm, json); fclose(json); }
```

The profiler reports total instruction count, opcode histogram, per-instruction-index hit counts (hot loop/branch locations), maximum operand-stack depth, and accumulated elapsed nanoseconds for successful runs. `mvm_profile_reset` clears counters without changing whether profiling is enabled. `mvm_profile_instruction_hits(vm, ip)` queries a specific bytecode instruction. For source-level profiling, your compiler should keep its own instruction-index-to-file/line/column table and join it with hit counts in the report.

## Safety and current boundaries

The runtime checks stack underflow/overflow, invalid constant indexes, local slots, branch targets, native indexes, numeric types, divide/modulo by zero, bitwise integer ranges, and shift counts. Bitwise operators require integral signed 32-bit operands. `mvm_run` begins at instruction zero. The runtime now includes VM-owned heap objects, collection operations, closure values, copied upvalues, and bounded user-function call frames. Objects are retained until `mvm_free`; tracing garbage collection and shared open-upvalue cells are not implemented in this version.


## Heap objects, collections, closures, and user functions (1.2.0)

The VM now exposes VM-owned array/map/function/closure values. All object allocations are tracked by the VM and released by `mvm_free`; the current policy is VM-lifetime ownership, **not** tracing garbage collection. Do not retain object values after freeing the VM.

### Embedding API

- `mvm_array_new(vm)`, `mvm_array_push(vm,array,value)`, `mvm_array_get`, `mvm_array_set`, `mvm_array_length`
- `mvm_map_new(vm)`, `mvm_map_set(vm,map,"key",value)`, `mvm_map_get`, `mvm_map_length`
- `mvm_function_new(vm,name,arity,local_count,upvalue_count,entry)` creates a function descriptor.
- `mvm_function_set_entry(vm,function,entry)` patches a descriptor once the body address is known.
- `mvm_closure_new(vm,function,captures,count)` creates a closure that copies capture values.

Function descriptors should be added to the constant pool. Emit a jump around function bodies so normal top-level execution does not fall into them. At a call site, stack order is closure followed by arguments in source order. `CALL_FUNCTION argc` checks arity and call-frame depth; argument values initialize local slots starting at 0. A called function returns through `RETURN`. Capture values are copied when a closure is made; `SET_UPVALUE` changes that closure's capture, but this release does not yet share open upvalue cells between separate closures.

### Collection bytecodes

| Opcode | Stack contract |
|---|---|
| `ARRAY_NEW n` | Consumes n values, preserving order, and pushes an array |
| `ARRAY_GET` | Consumes array,index; pushes the indexed value |
| `ARRAY_SET` | Consumes array,index,value; pushes the assigned value |
| `MAP_NEW n` | Consumes n key/value pairs; keys must be strings |
| `MAP_GET` | Consumes map,key; pushes value; missing key is a runtime error |
| `MAP_SET` | Consumes map,key,value; pushes the assigned value |
| `LENGTH` | Replaces array/map/string with its length |

The equivalent C APIs are useful to host interpreters that want to keep their own evaluator and use the VM only for selected compiled functions.

### Recommended interpreter integration

1. Keep lexing, parsing, ASTs, diagnostics and source locations in your language frontend.
2. Use typed `mvm_emit_*` helpers for bytecode and `mvm_add_constant` for literals and function descriptors.
3. Record instruction-to-source mappings in the frontend; join them with profiler instruction-hit counts for source-level reports.
4. Register language built-ins and host services with `mvm_register_native`.
5. Check every API return value and surface `mvm_last_error(vm)` in language diagnostics.
6. Call `mvm_free(vm)` once when the runtime instance is finished.

The workflow remains manual-only (`workflow_dispatch`); push/commit events do not run tests automatically.
