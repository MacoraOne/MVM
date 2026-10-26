# MacoraOne MVM

MacoraOne MVM is an embeddable C11 stack-bytecode virtual machine for language runtimes. It provides a typed value system, bytecode emission helpers, checked execution, native C callbacks, heap collections, user-function call frames, closures, and an optional instruction profiler.

## Clone

```sh
git clone https://github.com/MacoraOne/MVM.git
cd MVM
```

## Build and test

Requirements: a C11 compiler and Make. On Ubuntu/Debian, install them with `sudo apt-get update && sudo apt-get install -y build-essential`.

```sh
make clean test
```

The test executable prints `MACORAONE_VM_ALL_TESTS_PASS` when the suite passes. To run the tests with AddressSanitizer and UndefinedBehaviorSanitizer, use:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -fsanitize=address,undefined -fno-omit-frame-pointer vm.c test_vm.c -o vm-test-sanitized -lm
ASAN_OPTIONS=detect_leaks=1 ./vm-test-sanitized
```

## Files

- `vm.h` — public C API, value types, opcode definitions, and profiler structures.
- `vm.c` — VM implementation.
- `test_vm.c` — API and bytecode regression tests.
- `Makefile` — strict-warning build and test targets.
- `BYTECODE_API.md` — bytecode, stack contracts, embedding API, and runtime boundaries.
- `USAGE.md` — clone/build instructions, integration workflow, and examples for language implementers.
- `.github/workflows/test-vm.yml` — manually triggered build, test, and sanitizer workflow.

## Connect a programming language

Your language frontend remains responsible for source syntax, lexing, parsing, diagnostics, and translating language constructs into VM bytecode. Include `vm.h`, compile `vm.c` into your executable, add literal values with `mvm_add_constant`, emit instructions using the `mvm_emit_*` helpers, and execute with `mvm_run`. Use `mvm_register_native` to expose host functions. See [USAGE.md](USAGE.md) and [BYTECODE_API.md](BYTECODE_API.md) for the complete documented public API.

## Runtime capabilities and limits

The VM supports null, boolean, number, string, array, map, function, and closure values; arithmetic and comparison; branches and loops; local slots; native callbacks; user-function call frames; copied closure captures; collection operations; and profiling in text or JSON form. All heap objects are owned by a VM and released by `mvm_free`. This version does not implement tracing garbage collection or shared open-upvalue cells. Strings passed to `mvm_string` or used as string constants must remain valid for the VM lifetime, except strings explicitly allocated and owned internally by the VM.

## Run the GitHub Actions tests

Open the [Actions page](https://github.com/MacoraOne/MVM/actions/workflows/test-vm.yml), select the VM test workflow, and click **Run workflow**. The workflow is intentionally manual-only; commits and pushes do not trigger it.

## License

No license file was copied from the source repository. Until a license is added to this repository, do not assume the code is available under an open-source license.
