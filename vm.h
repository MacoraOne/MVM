#ifndef MACORAONE_VM_H
#define MACORAONE_VM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MVM_VERSION "1.2.0"
#define MVM_MAX_STACK 4096
#define MVM_MAX_LOCALS 256
#define MVM_PROFILE_OPCODE_SLOTS 256

typedef struct Mvm Mvm;
typedef struct MvmObject MvmObject;

typedef enum {
    MVM_VALUE_NULL = 0,
    MVM_VALUE_BOOL,
    MVM_VALUE_NUMBER,
    MVM_VALUE_STRING,
    MVM_VALUE_ARRAY,
    MVM_VALUE_MAP,
    MVM_VALUE_FUNCTION,
    MVM_VALUE_CLOSURE
} MvmValueType;

typedef struct {
    MvmValueType type;
    union {
        bool boolean;
        double number;
        const char *string;
        MvmObject *object;
    } as;
} MvmValue;

typedef enum {
    MVM_OP_CONSTANT = 1, MVM_OP_NULL, MVM_OP_TRUE, MVM_OP_FALSE,
    MVM_OP_POP, MVM_OP_DUP, MVM_OP_SWAP, MVM_OP_OVER,
    MVM_OP_ADD, MVM_OP_SUBTRACT, MVM_OP_MULTIPLY, MVM_OP_DIVIDE,
    MVM_OP_MODULO, MVM_OP_NEGATE, MVM_OP_POWER, MVM_OP_ABS,
    MVM_OP_SQRT, MVM_OP_FLOOR, MVM_OP_CEIL, MVM_OP_ROUND,
    MVM_OP_EQUAL, MVM_OP_NOT_EQUAL, MVM_OP_GREATER, MVM_OP_GREATER_EQUAL,
    MVM_OP_LESS, MVM_OP_LESS_EQUAL, MVM_OP_NOT, MVM_OP_AND, MVM_OP_OR,
    MVM_OP_LOAD_LOCAL, MVM_OP_STORE_LOCAL,
    MVM_OP_JUMP, MVM_OP_JUMP_IF_FALSE, MVM_OP_JUMP_IF_TRUE, MVM_OP_LOOP,
    MVM_OP_PRINT, MVM_OP_CALL_NATIVE, MVM_OP_RETURN, MVM_OP_HALT,
    MVM_OP_TYPEOF, MVM_OP_TO_BOOL, MVM_OP_STRING_LENGTH,
    MVM_OP_BIT_AND, MVM_OP_BIT_OR, MVM_OP_BIT_XOR,
    MVM_OP_SHIFT_LEFT, MVM_OP_SHIFT_RIGHT,
    MVM_OP_ARRAY_NEW, MVM_OP_ARRAY_GET, MVM_OP_ARRAY_SET,
    MVM_OP_MAP_NEW, MVM_OP_MAP_GET, MVM_OP_MAP_SET, MVM_OP_LENGTH,
    MVM_OP_CLOSURE, MVM_OP_CALL_FUNCTION, MVM_OP_GET_UPVALUE, MVM_OP_SET_UPVALUE
} MvmOpcode;

typedef struct {
    MvmOpcode opcode;
    int32_t operand;
} MvmInstruction;

typedef struct {
    bool enabled;
    uint64_t instructions_executed;
    uint64_t opcode_counts[MVM_PROFILE_OPCODE_SLOTS];
    uint64_t *instruction_hits;
    size_t instruction_slots;
    size_t max_stack_depth;
    uint64_t elapsed_ns;
} MvmProfile;

typedef MvmValue (*MvmNativeFn)(Mvm *vm, int argc, const MvmValue *argv, void *userdata);

MvmValue mvm_null(void);
MvmValue mvm_bool(bool value);
MvmValue mvm_number(double value);
MvmValue mvm_string(const char *value);

MvmValue mvm_array_new(Mvm *vm);
MvmValue mvm_map_new(Mvm *vm);
MvmValue mvm_function_new(Mvm *vm, const char *name, unsigned arity, unsigned local_count, unsigned upvalue_count, size_t entry);
bool mvm_function_set_entry(Mvm *vm, MvmValue function, size_t entry);
bool mvm_array_push(Mvm *vm, MvmValue array, MvmValue value);
bool mvm_array_get(Mvm *vm, MvmValue array, size_t index, MvmValue *out);
bool mvm_array_set(Mvm *vm, MvmValue array, size_t index, MvmValue value);
size_t mvm_array_length(MvmValue array);
bool mvm_map_set(Mvm *vm, MvmValue map, const char *key, MvmValue value);
bool mvm_map_get(Mvm *vm, MvmValue map, const char *key, MvmValue *out);
size_t mvm_map_length(MvmValue map);
MvmValue mvm_closure_new(Mvm *vm, MvmValue function, const MvmValue *captures, size_t capture_count);
bool mvm_emit_array_new(Mvm *vm, unsigned initial_count);
bool mvm_emit_array_get(Mvm *vm);
bool mvm_emit_array_set(Mvm *vm);
bool mvm_emit_map_new(Mvm *vm, unsigned pair_count);
bool mvm_emit_map_get(Mvm *vm);
bool mvm_emit_map_set(Mvm *vm);
bool mvm_emit_length(Mvm *vm);
bool mvm_emit_closure(Mvm *vm, int function_constant_index);
bool mvm_emit_call_function(Mvm *vm, unsigned argc);
bool mvm_emit_get_upvalue(Mvm *vm, unsigned index);
bool mvm_emit_set_upvalue(Mvm *vm, unsigned index);

bool mvm_value_truthy(MvmValue value);
bool mvm_value_equal(MvmValue a, MvmValue b);
void mvm_value_print(FILE *out, MvmValue value);

Mvm *mvm_new(void);
void mvm_free(Mvm *vm);
void mvm_reset(Mvm *vm);
const char *mvm_last_error(const Mvm *vm);
void mvm_clear_error(Mvm *vm);
size_t mvm_instruction_count(const Mvm *vm);
const MvmInstruction *mvm_instructions(const Mvm *vm);

/* Constants and raw instruction access are the common extension point for compilers. */
int mvm_add_constant(Mvm *vm, MvmValue value);
bool mvm_emit(Mvm *vm, MvmOpcode opcode, int32_t operand);
bool mvm_patch_operand(Mvm *vm, size_t instruction_index, int32_t operand);

/* Typed public emitters: language compilers should use these instead of numeric opcodes. */
bool mvm_emit_constant(Mvm *vm, int constant_index);
bool mvm_emit_null(Mvm *vm);
bool mvm_emit_true(Mvm *vm);
bool mvm_emit_false(Mvm *vm);
bool mvm_emit_pop(Mvm *vm);
bool mvm_emit_dup(Mvm *vm);
bool mvm_emit_swap(Mvm *vm);
bool mvm_emit_over(Mvm *vm);
bool mvm_emit_add(Mvm *vm);
bool mvm_emit_subtract(Mvm *vm);
bool mvm_emit_multiply(Mvm *vm);
bool mvm_emit_divide(Mvm *vm);
bool mvm_emit_modulo(Mvm *vm);
bool mvm_emit_negate(Mvm *vm);
bool mvm_emit_power(Mvm *vm);
bool mvm_emit_abs(Mvm *vm);
bool mvm_emit_sqrt(Mvm *vm);
bool mvm_emit_floor(Mvm *vm);
bool mvm_emit_ceil(Mvm *vm);
bool mvm_emit_round(Mvm *vm);
bool mvm_emit_equal(Mvm *vm);
bool mvm_emit_not_equal(Mvm *vm);
bool mvm_emit_greater(Mvm *vm);
bool mvm_emit_greater_equal(Mvm *vm);
bool mvm_emit_less(Mvm *vm);
bool mvm_emit_less_equal(Mvm *vm);
bool mvm_emit_not(Mvm *vm);
bool mvm_emit_and(Mvm *vm);
bool mvm_emit_or(Mvm *vm);
bool mvm_emit_load_local(Mvm *vm, unsigned slot);
bool mvm_emit_store_local(Mvm *vm, unsigned slot);
bool mvm_emit_jump(Mvm *vm, size_t target);
bool mvm_emit_jump_if_false(Mvm *vm, size_t target);
bool mvm_emit_jump_if_true(Mvm *vm, size_t target);
bool mvm_emit_loop(Mvm *vm, size_t target);
bool mvm_emit_print(Mvm *vm);
bool mvm_emit_call_native(Mvm *vm, unsigned native_index, unsigned argc);
bool mvm_emit_return(Mvm *vm);
bool mvm_emit_halt(Mvm *vm);
bool mvm_emit_typeof(Mvm *vm);
bool mvm_emit_to_bool(Mvm *vm);
bool mvm_emit_string_length(Mvm *vm);
bool mvm_emit_bit_and(Mvm *vm);
bool mvm_emit_bit_or(Mvm *vm);
bool mvm_emit_bit_xor(Mvm *vm);
bool mvm_emit_shift_left(Mvm *vm);
bool mvm_emit_shift_right(Mvm *vm);

/* Host/native API: language runtimes can expose C functions to bytecode. */
int mvm_register_native(Mvm *vm, const char *name, MvmNativeFn fn, void *userdata);
int mvm_find_native(const Mvm *vm, const char *name);
const char *mvm_native_name(const Mvm *vm, unsigned native_index);
bool mvm_run(Mvm *vm);
MvmValue mvm_result(const Mvm *vm);

/* Optional built-in profiler: opcode counts, hot instruction indexes, stack high-water mark, elapsed time. */
void mvm_profile_enable(Mvm *vm, bool enabled);
void mvm_profile_reset(Mvm *vm);
const MvmProfile *mvm_profile_data(const Mvm *vm);
uint64_t mvm_profile_instruction_hits(const Mvm *vm, size_t instruction_index);
bool mvm_profile_write_text(const Mvm *vm, FILE *out);
bool mvm_profile_write_json(const Mvm *vm, FILE *out);
bool mvm_stack_push(Mvm *vm, MvmValue value);
bool mvm_stack_pop(Mvm *vm, MvmValue *out);
size_t mvm_stack_size(const Mvm *vm);

#ifdef __cplusplus
}
#endif
#endif
