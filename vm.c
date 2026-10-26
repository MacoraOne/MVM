#include "vm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MVM_MAX_CODE 65536
#define MVM_MAX_CONSTANTS 16384
#define MVM_MAX_NATIVES 256
#define MVM_MAX_ARGS 256
#define MVM_MAX_NAME 128
#define MVM_MAX_FRAMES 256
#define MVM_MAX_UPVALUES 256

typedef struct { char *name; MvmNativeFn fn; void *userdata; } Native;
typedef enum { OBJ_ARRAY, OBJ_MAP, OBJ_FUNCTION, OBJ_CLOSURE } ObjectKind;
typedef struct { char *key; MvmValue value; } MapEntry;
struct MvmObject { ObjectKind kind; MvmObject *next; union { struct { MvmValue *items; size_t count, capacity; } array; struct { MapEntry *entries; size_t count, capacity; } map; struct { char *name; unsigned arity, local_count, upvalue_count; size_t entry; } function; struct { MvmObject *function; MvmValue *captures; size_t count; } closure; } as; };
typedef struct { MvmObject *closure; size_t return_ip, base_sp; MvmValue locals[MVM_MAX_LOCALS]; bool local_set[MVM_MAX_LOCALS]; } CallFrame;
struct Mvm {
    MvmInstruction *code;
    size_t code_count, code_capacity, ip;
    MvmValue *constants;
    size_t constant_count, constant_capacity;
    char **owned_strings;
    size_t owned_string_count, owned_string_capacity;
    MvmValue stack[MVM_MAX_STACK];
    size_t sp;
    MvmValue locals[MVM_MAX_LOCALS];
    bool local_set[MVM_MAX_LOCALS];
    Native natives[MVM_MAX_NATIVES];
    size_t native_count;
    char error[512];
    MvmValue result;
    MvmProfile profile;
    MvmObject *objects;
    CallFrame frames[MVM_MAX_FRAMES];
    size_t frame_count;
};

static void set_error(Mvm *vm, const char *message) {
    if (vm && vm->error[0] == '\0') {
        (void)snprintf(vm->error, sizeof(vm->error), "%s", message);
    }
}
static bool fail(Mvm *vm, const char *message) { set_error(vm, message); return false; }
static uint64_t profile_now_ns(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static void profile_finish(Mvm *vm, uint64_t start) {
    if (vm && vm->profile.enabled && start) {
        uint64_t end = profile_now_ns();
        if (end >= start) vm->profile.elapsed_ns += end - start;
    }
}
static bool own_string(Mvm *vm, char *s) {
    if (vm->owned_string_count == vm->owned_string_capacity) {
        size_t cap = vm->owned_string_capacity ? vm->owned_string_capacity * 2 : 8;
        char **p = (char**)realloc(vm->owned_strings, cap * sizeof(*p));
        if (!p) return fail(vm, "out of memory tracking owned strings");
        vm->owned_strings = p;
        vm->owned_string_capacity = cap;
    }
    vm->owned_strings[vm->owned_string_count++] = s;
    return true;
}
static bool need_stack(Mvm *vm, size_t count) {
    if (vm->sp < count) return fail(vm, "stack underflow");
    return true;
}
static bool numeric(Mvm *vm, MvmValue v, double *out) {
    if (v.type != MVM_VALUE_NUMBER) return fail(vm, "numeric opcode requires number operands");
    *out = v.as.number; return true;
}
static bool valid_target(Mvm *vm, int32_t target) {
    if (target < 0 || (size_t)target >= vm->code_count) return fail(vm, "jump target out of range");
    return true;
}


static MvmObject *new_object(Mvm *vm, ObjectKind kind) {
    if (!vm) return NULL;
    MvmObject *o=(MvmObject*)calloc(1,sizeof(*o));
    if(!o){set_error(vm,"out of memory allocating VM object");return NULL;}
    o->kind=kind;o->next=vm->objects;vm->objects=o;return o;
}
static void free_object(MvmObject *o) {
    if(!o)return;
    if(o->kind==OBJ_ARRAY)free(o->as.array.items);
    else if(o->kind==OBJ_MAP){for(size_t i=0;i<o->as.map.count;i++)free(o->as.map.entries[i].key);free(o->as.map.entries);}
    else if(o->kind==OBJ_FUNCTION)free(o->as.function.name);
    else if(o->kind==OBJ_CLOSURE)free(o->as.closure.captures);
    free(o);
}
MvmValue mvm_array_new(Mvm *vm){MvmValue v=mvm_null();MvmObject*o=new_object(vm,OBJ_ARRAY);if(o){v.type=MVM_VALUE_ARRAY;v.as.object=o;}return v;}
MvmValue mvm_map_new(Mvm *vm){MvmValue v=mvm_null();MvmObject*o=new_object(vm,OBJ_MAP);if(o){v.type=MVM_VALUE_MAP;v.as.object=o;}return v;}
MvmValue mvm_function_new(Mvm *vm,const char *name,unsigned arity,unsigned locals,unsigned upvalues,size_t entry){
    MvmValue v=mvm_null();
    if(!vm||!name||arity>MVM_MAX_LOCALS||locals>MVM_MAX_LOCALS||arity>locals||upvalues>MVM_MAX_UPVALUES||entry>INT32_MAX){if(vm)set_error(vm,"invalid function descriptor");return v;}
    MvmObject*o=new_object(vm,OBJ_FUNCTION);if(!o)return v;
    size_t n=strlen(name);o->as.function.name=(char*)malloc(n+1);
    if(!o->as.function.name){set_error(vm,"out of memory naming function");return v;}
    memcpy(o->as.function.name,name,n+1);o->as.function.arity=arity;o->as.function.local_count=locals;o->as.function.upvalue_count=upvalues;o->as.function.entry=entry;
    v.type=MVM_VALUE_FUNCTION;v.as.object=o;return v;
}
bool mvm_function_set_entry(Mvm *vm,MvmValue f,size_t entry){if(!vm||f.type!=MVM_VALUE_FUNCTION||!f.as.object||f.as.object->kind!=OBJ_FUNCTION||entry>=vm->code_count)return fail(vm,"invalid function or entry point");f.as.object->as.function.entry=entry;return true;}
bool mvm_array_push(Mvm *vm,MvmValue a,MvmValue x){
    if(!vm||a.type!=MVM_VALUE_ARRAY||!a.as.object||a.as.object->kind!=OBJ_ARRAY)return fail(vm,"array_push requires array");
    MvmObject*o=a.as.object;if(o->as.array.count==o->as.array.capacity){size_t n=o->as.array.capacity?o->as.array.capacity*2:8;MvmValue*p=(MvmValue*)realloc(o->as.array.items,n*sizeof(*p));if(!p)return fail(vm,"out of memory growing array");o->as.array.items=p;o->as.array.capacity=n;}
    o->as.array.items[o->as.array.count++]=x;return true;
}
bool mvm_array_get(Mvm *vm,MvmValue a,size_t i,MvmValue*out){if(!vm||a.type!=MVM_VALUE_ARRAY||!a.as.object||a.as.object->kind!=OBJ_ARRAY)return fail(vm,"array_get requires array");if(i>=a.as.object->as.array.count)return fail(vm,"array index out of range");if(out)*out=a.as.object->as.array.items[i];return true;}
bool mvm_array_set(Mvm *vm,MvmValue a,size_t i,MvmValue x){if(!vm||a.type!=MVM_VALUE_ARRAY||!a.as.object||a.as.object->kind!=OBJ_ARRAY)return fail(vm,"array_set requires array");if(i>=a.as.object->as.array.count)return fail(vm,"array index out of range");a.as.object->as.array.items[i]=x;return true;}
size_t mvm_array_length(MvmValue a){return a.type==MVM_VALUE_ARRAY&&a.as.object&&a.as.object->kind==OBJ_ARRAY?a.as.object->as.array.count:0;}
bool mvm_map_set(Mvm *vm,MvmValue m,const char*k,MvmValue x){
    if(!vm||m.type!=MVM_VALUE_MAP||!m.as.object||m.as.object->kind!=OBJ_MAP||!k)return fail(vm,"map_set requires map and key");
    MvmObject*o=m.as.object;for(size_t i=0;i<o->as.map.count;i++)if(strcmp(o->as.map.entries[i].key,k)==0){o->as.map.entries[i].value=x;return true;}
    if(o->as.map.count==o->as.map.capacity){size_t n=o->as.map.capacity?o->as.map.capacity*2:8;MapEntry*p=(MapEntry*)realloc(o->as.map.entries,n*sizeof(*p));if(!p)return fail(vm,"out of memory growing map");o->as.map.entries=p;o->as.map.capacity=n;}
    size_t n=strlen(k);char*copy=(char*)malloc(n+1);if(!copy)return fail(vm,"out of memory copying map key");memcpy(copy,k,n+1);o->as.map.entries[o->as.map.count++]=(MapEntry){copy,x};return true;
}
bool mvm_map_get(Mvm *vm,MvmValue m,const char*k,MvmValue*out){if(!vm||m.type!=MVM_VALUE_MAP||!m.as.object||m.as.object->kind!=OBJ_MAP||!k)return fail(vm,"map_get requires map and key");MvmObject*o=m.as.object;for(size_t i=0;i<o->as.map.count;i++)if(strcmp(o->as.map.entries[i].key,k)==0){if(out)*out=o->as.map.entries[i].value;return true;}return fail(vm,"map key not found");}
size_t mvm_map_length(MvmValue m){return m.type==MVM_VALUE_MAP&&m.as.object&&m.as.object->kind==OBJ_MAP?m.as.object->as.map.count:0;}
MvmValue mvm_closure_new(Mvm *vm,MvmValue f,const MvmValue*caps,size_t n){
    MvmValue v=mvm_null();if(!vm||f.type!=MVM_VALUE_FUNCTION||!f.as.object||f.as.object->kind!=OBJ_FUNCTION||n!=f.as.object->as.function.upvalue_count||n>MVM_MAX_UPVALUES){if(vm)set_error(vm,"closure capture count does not match function descriptor");return v;}
    MvmObject*o=new_object(vm,OBJ_CLOSURE);if(!o)return v;if(n){o->as.closure.captures=(MvmValue*)malloc(n*sizeof(MvmValue));if(!o->as.closure.captures){set_error(vm,"out of memory allocating closure captures");return v;}memcpy(o->as.closure.captures,caps,n*sizeof(MvmValue));}
    o->as.closure.count=n;o->as.closure.function=f.as.object;v.type=MVM_VALUE_CLOSURE;v.as.object=o;return v;
}

MvmValue mvm_null(void) { MvmValue v; v.type=MVM_VALUE_NULL; v.as.number=0; return v; }
MvmValue mvm_bool(bool x) { MvmValue v; v.type=MVM_VALUE_BOOL; v.as.boolean=x; return v; }
MvmValue mvm_number(double x) { MvmValue v; v.type=MVM_VALUE_NUMBER; v.as.number=x; return v; }
MvmValue mvm_string(const char *x) { MvmValue v; v.type=MVM_VALUE_STRING; v.as.string=x ? x : ""; return v; }
bool mvm_value_truthy(MvmValue v) {
    switch(v.type) {
        case MVM_VALUE_NULL: return false;
        case MVM_VALUE_BOOL: return v.as.boolean;
        case MVM_VALUE_NUMBER: return v.as.number != 0.0 && !isnan(v.as.number);
        case MVM_VALUE_STRING: return v.as.string && v.as.string[0] != '\0';
        case MVM_VALUE_ARRAY: case MVM_VALUE_MAP: case MVM_VALUE_FUNCTION: case MVM_VALUE_CLOSURE: return v.as.object != NULL;
        default: return false;
    }
}
bool mvm_value_equal(MvmValue a, MvmValue b) {
    if (a.type != b.type) return false;
    switch(a.type) {
        case MVM_VALUE_NULL: return true;
        case MVM_VALUE_BOOL: return a.as.boolean == b.as.boolean;
        case MVM_VALUE_NUMBER: return a.as.number == b.as.number;
        case MVM_VALUE_STRING: return strcmp(a.as.string ? a.as.string : "", b.as.string ? b.as.string : "") == 0;
        case MVM_VALUE_ARRAY: case MVM_VALUE_MAP: case MVM_VALUE_FUNCTION: case MVM_VALUE_CLOSURE: return a.as.object==b.as.object;
        default: return false;
    }
}
void mvm_value_print(FILE *out, MvmValue v) {
    if (!out) return;
    switch(v.type) {
        case MVM_VALUE_NULL: fputs("null", out); break;
        case MVM_VALUE_BOOL: fputs(v.as.boolean ? "true" : "false", out); break;
        case MVM_VALUE_NUMBER:
            if (isfinite(v.as.number) && v.as.number >= -9e15 && v.as.number <= 9e15 && floor(v.as.number)==v.as.number)
                fprintf(out, "%.0f", v.as.number);
            else fprintf(out, "%.15g", v.as.number);
            break;
        case MVM_VALUE_STRING: fputs(v.as.string ? v.as.string : "", out); break;
        case MVM_VALUE_ARRAY: fprintf(out,"<array:%zu>",mvm_array_length(v)); break;
        case MVM_VALUE_MAP: fprintf(out,"<map:%zu>",mvm_map_length(v)); break;
        case MVM_VALUE_FUNCTION: fprintf(out,"<function:%s>",v.as.object&&v.as.object->kind==OBJ_FUNCTION?v.as.object->as.function.name:"invalid"); break;
        case MVM_VALUE_CLOSURE: fprintf(out,"<closure:%s>",v.as.object&&v.as.object->kind==OBJ_CLOSURE?v.as.object->as.closure.function->as.function.name:"invalid"); break;
        default: fputs("<invalid>", out); break;
    }
}
Mvm *mvm_new(void) {
    Mvm *vm=(Mvm*)calloc(1,sizeof(*vm));
    if (!vm) return NULL;
    vm->code_capacity=64;
    vm->code=(MvmInstruction*)calloc(vm->code_capacity,sizeof(*vm->code));
    vm->constant_capacity=16;
    vm->constants=(MvmValue*)calloc(vm->constant_capacity,sizeof(*vm->constants));
    if (!vm->code || !vm->constants) { mvm_free(vm); return NULL; }
    vm->result=mvm_null();
    vm->profile.instruction_slots=MVM_MAX_CODE;
    vm->profile.instruction_hits=(uint64_t*)calloc(MVM_MAX_CODE,sizeof(uint64_t));
    if (!vm->profile.instruction_hits) { mvm_free(vm); return NULL; }
    return vm;
}
void mvm_free(Mvm *vm) {
    if (!vm) return;
    for (size_t i=0;i<vm->native_count;i++) free(vm->natives[i].name);
    for (size_t i=0;i<vm->owned_string_count;i++) free(vm->owned_strings[i]);
    MvmObject *obj=vm->objects; while(obj){MvmObject *next=obj->next;free_object(obj);obj=next;}
    free(vm->owned_strings);
    free(vm->profile.instruction_hits);
    free(vm->code); free(vm->constants); free(vm);
}
void mvm_reset(Mvm *vm) {
    if (!vm) return;
    vm->ip=0; vm->sp=0; vm->frame_count=0; vm->error[0]='\0'; vm->result=mvm_null();
    memset(vm->local_set,0,sizeof(vm->local_set));
    for (size_t i=0;i<MVM_MAX_LOCALS;i++) vm->locals[i]=mvm_null();
}
const char *mvm_last_error(const Mvm *vm) { return vm ? vm->error : "VM is null"; }
void mvm_clear_error(Mvm *vm) { if(vm) vm->error[0]='\0'; }
void mvm_profile_enable(Mvm *vm,bool enabled){if(vm)vm->profile.enabled=enabled;}
void mvm_profile_reset(Mvm *vm){if(!vm)return;bool enabled=vm->profile.enabled;uint64_t *hits=vm->profile.instruction_hits;size_t slots=vm->profile.instruction_slots;memset(&vm->profile,0,sizeof(vm->profile));vm->profile.enabled=enabled;vm->profile.instruction_hits=hits;vm->profile.instruction_slots=slots;if(hits)memset(hits,0,slots*sizeof(*hits));}
const MvmProfile *mvm_profile_data(const Mvm *vm){return vm?&vm->profile:NULL;}
uint64_t mvm_profile_instruction_hits(const Mvm *vm,size_t i){return vm&&vm->profile.instruction_hits&&i<vm->profile.instruction_slots?vm->profile.instruction_hits[i]:0;}
bool mvm_profile_write_text(const Mvm *vm,FILE *out){if(!vm||!out)return false;const MvmProfile *p=&vm->profile;fprintf(out,"MacoraOne VM profile\nInstructions: %llu\nElapsed: %llu ns\nMax stack depth: %zu\n",(unsigned long long)p->instructions_executed,(unsigned long long)p->elapsed_ns,p->max_stack_depth);for(size_t i=0;i<MVM_PROFILE_OPCODE_SLOTS;i++)if(p->opcode_counts[i])fprintf(out,"opcode[%zu]: %llu\n",i,(unsigned long long)p->opcode_counts[i]);for(size_t i=0;i<vm->code_count;i++)if(mvm_profile_instruction_hits(vm,i))fprintf(out,"ip[%zu] opcode=%d hits=%llu\n",i,(int)vm->code[i].opcode,(unsigned long long)mvm_profile_instruction_hits(vm,i));return !ferror(out);}
bool mvm_profile_write_json(const Mvm *vm,FILE *out){if(!vm||!out)return false;const MvmProfile *p=&vm->profile;fprintf(out,"{\"instructions_executed\":%llu,\"elapsed_ns\":%llu,\"max_stack_depth\":%zu,\"opcodes\":[",(unsigned long long)p->instructions_executed,(unsigned long long)p->elapsed_ns,p->max_stack_depth);bool comma=false;for(size_t i=0;i<MVM_PROFILE_OPCODE_SLOTS;i++)if(p->opcode_counts[i]){fprintf(out,"%s{\"opcode\":%zu,\"count\":%llu}",comma?",":"",i,(unsigned long long)p->opcode_counts[i]);comma=true;}fprintf(out,"],\"hot_instructions\":[");comma=false;for(size_t i=0;i<vm->code_count;i++){uint64_t n=mvm_profile_instruction_hits(vm,i);if(n){fprintf(out,"%s{\"ip\":%zu,\"opcode\":%d,\"count\":%llu}",comma?",":"",i,(int)vm->code[i].opcode,(unsigned long long)n);comma=true;}}fprintf(out,"]}\n");return !ferror(out);}
size_t mvm_instruction_count(const Mvm *vm) { return vm ? vm->code_count : 0; }
const MvmInstruction *mvm_instructions(const Mvm *vm) { return vm ? vm->code : NULL; }
int mvm_add_constant(Mvm *vm, MvmValue v) {
    if (!vm || vm->constant_count>=MVM_MAX_CONSTANTS) { if(vm) set_error(vm,"constant pool limit exceeded"); return -1; }
    if (vm->constant_count==vm->constant_capacity) {
        size_t cap=vm->constant_capacity*2;
        MvmValue *p=(MvmValue*)realloc(vm->constants,cap*sizeof(*p));
        if(!p){set_error(vm,"out of memory growing constant pool");return -1;}
        vm->constants=p;vm->constant_capacity=cap;
    }
    vm->constants[vm->constant_count]=v;
    return (int)vm->constant_count++;
}
bool mvm_emit(Mvm *vm, MvmOpcode op, int32_t operand) {
    if(!vm) return false;
    if(vm->code_count>=MVM_MAX_CODE) return fail(vm,"bytecode limit exceeded");
    if(vm->code_count==vm->code_capacity) {
        size_t cap=vm->code_capacity*2;
        MvmInstruction *p=(MvmInstruction*)realloc(vm->code,cap*sizeof(*p));
        if(!p) return fail(vm,"out of memory growing bytecode");
        vm->code=p;vm->code_capacity=cap;
    }
    vm->code[vm->code_count].opcode=op;
    vm->code[vm->code_count].operand=operand;
    vm->code_count++;
    return true;
}
bool mvm_patch_operand(Mvm *vm,size_t i,int32_t operand) {
    if(!vm || i>=vm->code_count) return fail(vm,"cannot patch missing instruction");
    vm->code[i].operand=operand; return true;
}
#define EMIT0(name,op) bool name(Mvm *v){return mvm_emit(v,op,0);}
bool mvm_emit_constant(Mvm *v,int i){if(i<0 || !v || (size_t)i>=v->constant_count)return fail(v,"constant index out of range");return mvm_emit(v,MVM_OP_CONSTANT,i);}
EMIT0(mvm_emit_null,MVM_OP_NULL) EMIT0(mvm_emit_true,MVM_OP_TRUE) EMIT0(mvm_emit_false,MVM_OP_FALSE)
EMIT0(mvm_emit_pop,MVM_OP_POP) EMIT0(mvm_emit_dup,MVM_OP_DUP) EMIT0(mvm_emit_swap,MVM_OP_SWAP) EMIT0(mvm_emit_over,MVM_OP_OVER)
EMIT0(mvm_emit_add,MVM_OP_ADD) EMIT0(mvm_emit_subtract,MVM_OP_SUBTRACT) EMIT0(mvm_emit_multiply,MVM_OP_MULTIPLY)
EMIT0(mvm_emit_divide,MVM_OP_DIVIDE) EMIT0(mvm_emit_modulo,MVM_OP_MODULO) EMIT0(mvm_emit_negate,MVM_OP_NEGATE)
EMIT0(mvm_emit_power,MVM_OP_POWER) EMIT0(mvm_emit_abs,MVM_OP_ABS) EMIT0(mvm_emit_sqrt,MVM_OP_SQRT)
EMIT0(mvm_emit_floor,MVM_OP_FLOOR) EMIT0(mvm_emit_ceil,MVM_OP_CEIL) EMIT0(mvm_emit_round,MVM_OP_ROUND)
EMIT0(mvm_emit_equal,MVM_OP_EQUAL) EMIT0(mvm_emit_not_equal,MVM_OP_NOT_EQUAL) EMIT0(mvm_emit_greater,MVM_OP_GREATER)
EMIT0(mvm_emit_greater_equal,MVM_OP_GREATER_EQUAL) EMIT0(mvm_emit_less,MVM_OP_LESS) EMIT0(mvm_emit_less_equal,MVM_OP_LESS_EQUAL)
EMIT0(mvm_emit_not,MVM_OP_NOT) EMIT0(mvm_emit_and,MVM_OP_AND) EMIT0(mvm_emit_or,MVM_OP_OR)
EMIT0(mvm_emit_print,MVM_OP_PRINT) EMIT0(mvm_emit_return,MVM_OP_RETURN) EMIT0(mvm_emit_halt,MVM_OP_HALT)
EMIT0(mvm_emit_typeof,MVM_OP_TYPEOF) EMIT0(mvm_emit_to_bool,MVM_OP_TO_BOOL) EMIT0(mvm_emit_string_length,MVM_OP_STRING_LENGTH)
EMIT0(mvm_emit_bit_and,MVM_OP_BIT_AND) EMIT0(mvm_emit_bit_or,MVM_OP_BIT_OR) EMIT0(mvm_emit_bit_xor,MVM_OP_BIT_XOR)
EMIT0(mvm_emit_shift_left,MVM_OP_SHIFT_LEFT) EMIT0(mvm_emit_shift_right,MVM_OP_SHIFT_RIGHT)
bool mvm_emit_array_new(Mvm*v,unsigned n){if(n>MVM_MAX_STACK)return fail(v,"array initializer too large");return mvm_emit(v,MVM_OP_ARRAY_NEW,(int32_t)n);}
EMIT0(mvm_emit_array_get,MVM_OP_ARRAY_GET) EMIT0(mvm_emit_array_set,MVM_OP_ARRAY_SET)
bool mvm_emit_map_new(Mvm*v,unsigned n){if(n>MVM_MAX_STACK/2)return fail(v,"map initializer too large");return mvm_emit(v,MVM_OP_MAP_NEW,(int32_t)n);}
EMIT0(mvm_emit_map_get,MVM_OP_MAP_GET) EMIT0(mvm_emit_map_set,MVM_OP_MAP_SET) EMIT0(mvm_emit_length,MVM_OP_LENGTH)
bool mvm_emit_closure(Mvm*v,int i){if(!v||i<0||(size_t)i>=v->constant_count||v->constants[i].type!=MVM_VALUE_FUNCTION)return fail(v,"closure requires function descriptor constant");return mvm_emit(v,MVM_OP_CLOSURE,i);}
bool mvm_emit_call_function(Mvm*v,unsigned n){if(n>MVM_MAX_LOCALS)return fail(v,"function argument limit exceeded");return mvm_emit(v,MVM_OP_CALL_FUNCTION,(int32_t)n);}
bool mvm_emit_get_upvalue(Mvm*v,unsigned i){if(i>=MVM_MAX_UPVALUES)return fail(v,"upvalue index out of range");return mvm_emit(v,MVM_OP_GET_UPVALUE,(int32_t)i);}
bool mvm_emit_set_upvalue(Mvm*v,unsigned i){if(i>=MVM_MAX_UPVALUES)return fail(v,"upvalue index out of range");return mvm_emit(v,MVM_OP_SET_UPVALUE,(int32_t)i);}
bool mvm_emit_load_local(Mvm *v,unsigned s){if(s>=MVM_MAX_LOCALS)return fail(v,"local slot out of range");return mvm_emit(v,MVM_OP_LOAD_LOCAL,(int32_t)s);}
bool mvm_emit_store_local(Mvm *v,unsigned s){if(s>=MVM_MAX_LOCALS)return fail(v,"local slot out of range");return mvm_emit(v,MVM_OP_STORE_LOCAL,(int32_t)s);}
static bool emit_target(Mvm *v,MvmOpcode op,size_t t){if(t>INT32_MAX)return fail(v,"jump target too large");return mvm_emit(v,op,(int32_t)t);}
bool mvm_emit_jump(Mvm *v,size_t t){return emit_target(v,MVM_OP_JUMP,t);}
bool mvm_emit_jump_if_false(Mvm *v,size_t t){return emit_target(v,MVM_OP_JUMP_IF_FALSE,t);}
bool mvm_emit_jump_if_true(Mvm *v,size_t t){return emit_target(v,MVM_OP_JUMP_IF_TRUE,t);}
bool mvm_emit_loop(Mvm *v,size_t t){return emit_target(v,MVM_OP_LOOP,t);}
bool mvm_emit_call_native(Mvm *v,unsigned n,unsigned a){if(n>=MVM_MAX_NATIVES||a>MVM_MAX_ARGS)return fail(v,"native call argument limit exceeded");return mvm_emit(v,MVM_OP_CALL_NATIVE,(int32_t)((n<<16)|a));}
int mvm_register_native(Mvm *v,const char *name,MvmNativeFn fn,void *ud) {
    if(!v||!name||!fn||v->native_count>=MVM_MAX_NATIVES){if(v)set_error(v,"invalid native registration or native table full");return -1;}
    if(mvm_find_native(v,name)>=0){set_error(v,"duplicate native function name");return -1;}
    size_t n=strlen(name);char *copy=(char*)malloc(n+1);
    if(!copy){set_error(v,"out of memory registering native");return -1;}memcpy(copy,name,n+1);
    size_t i=v->native_count++;v->natives[i]=(Native){copy,fn,ud};return (int)i;
}
int mvm_find_native(const Mvm *v,const char *name){if(!v||!name)return -1;for(size_t i=0;i<v->native_count;i++)if(strcmp(v->natives[i].name,name)==0)return (int)i;return -1;}
const char *mvm_native_name(const Mvm *v,unsigned i){return v&&i<v->native_count?v->natives[i].name:NULL;}
bool mvm_stack_push(Mvm *v,MvmValue x){if(!v)return false;if(v->sp>=MVM_MAX_STACK)return fail(v,"stack overflow");v->stack[v->sp++]=x;if(v->profile.enabled&&v->sp>v->profile.max_stack_depth)v->profile.max_stack_depth=v->sp;return true;}
bool mvm_stack_pop(Mvm *v,MvmValue *out){if(!v||!need_stack(v,1))return false;if(out)*out=v->stack[v->sp-1];v->sp--;return true;}
size_t mvm_stack_size(const Mvm *v){return v?v->sp:0;}
MvmValue mvm_result(const Mvm *v){return v?v->result:mvm_null();}

bool mvm_run(Mvm *v) {
    if(!v)return false;
    if(v->error[0])return false;
    uint64_t profile_start=v->profile.enabled?profile_now_ns():0;
    v->ip=0;v->frame_count=0;v->result=mvm_null();
    while(v->ip<v->code_count) {
        size_t current_ip=v->ip;
        MvmInstruction in=v->code[v->ip++];
        if(v->profile.enabled){v->profile.instructions_executed++;if((unsigned)in.opcode<MVM_PROFILE_OPCODE_SLOTS)v->profile.opcode_counts[(unsigned)in.opcode]++;if(current_ip<v->profile.instruction_slots)v->profile.instruction_hits[current_ip]++;}
        MvmValue a,b;double x,y;
        switch(in.opcode) {
        case MVM_OP_CONSTANT:
            if(in.operand<0||(size_t)in.operand>=v->constant_count)return fail(v,"invalid constant index");
            if(!mvm_stack_push(v,v->constants[in.operand]))return false;break;
        case MVM_OP_NULL: if(!mvm_stack_push(v,mvm_null()))return false;break;
        case MVM_OP_TRUE: if(!mvm_stack_push(v,mvm_bool(true)))return false;break;
        case MVM_OP_FALSE: if(!mvm_stack_push(v,mvm_bool(false)))return false;break;
        case MVM_OP_POP: if(!mvm_stack_pop(v,NULL))return false;break;
        case MVM_OP_DUP: if(!need_stack(v,1)||!mvm_stack_push(v,v->stack[v->sp-1]))return false;break;
        case MVM_OP_SWAP: if(!need_stack(v,2))return false;a=v->stack[v->sp-1];v->stack[v->sp-1]=v->stack[v->sp-2];v->stack[v->sp-2]=a;break;
        case MVM_OP_OVER: if(!need_stack(v,2)||!mvm_stack_push(v,v->stack[v->sp-2]))return false;break;
        case MVM_OP_ADD:
            if(!need_stack(v,2))return false;b=v->stack[--v->sp];a=v->stack[--v->sp];
            if(a.type==MVM_VALUE_STRING&&b.type==MVM_VALUE_STRING){
                size_t na=strlen(a.as.string?a.as.string:""),nb=strlen(b.as.string?b.as.string:"");
                char *s=(char*)malloc(na+nb+1);if(!s)return fail(v,"out of memory concatenating strings");
                memcpy(s,a.as.string?a.as.string:"",na);memcpy(s+na,b.as.string?b.as.string:"",nb+1);
                /* The VM owns concatenated strings until mvm_free; store them in an internal constant. */
                if(!own_string(v,s)){free(s);return false;}
                int ci=mvm_add_constant(v,mvm_string(s));if(ci<0)return false;
                if(!mvm_stack_push(v,v->constants[ci]))return false;
            } else {if(!numeric(v,a,&x)||!numeric(v,b,&y))return false;if(!mvm_stack_push(v,mvm_number(x+y)))return false;}break;
        case MVM_OP_SUBTRACT: case MVM_OP_MULTIPLY: case MVM_OP_DIVIDE: case MVM_OP_MODULO: case MVM_OP_POWER:
        case MVM_OP_GREATER: case MVM_OP_GREATER_EQUAL: case MVM_OP_LESS: case MVM_OP_LESS_EQUAL:
            if(!need_stack(v,2))return false;b=v->stack[--v->sp];a=v->stack[--v->sp];
            if(!numeric(v,a,&x)||!numeric(v,b,&y))return false;
            if((in.opcode==MVM_OP_DIVIDE||in.opcode==MVM_OP_MODULO)&&y==0.0)return fail(v,"division by zero");
            switch(in.opcode){
                case MVM_OP_SUBTRACT: a=mvm_number(x-y);break;
                case MVM_OP_MULTIPLY: a=mvm_number(x*y);break;
                case MVM_OP_DIVIDE: a=mvm_number(x/y);break;
                case MVM_OP_MODULO: a=mvm_number(fmod(x,y));break;
                case MVM_OP_POWER: a=mvm_number(pow(x,y));break;
                case MVM_OP_GREATER: a=mvm_bool(x>y);break;
                case MVM_OP_GREATER_EQUAL: a=mvm_bool(x>=y);break;
                case MVM_OP_LESS: a=mvm_bool(x<y);break;
                default: a=mvm_bool(x<=y);break;
            } if(!mvm_stack_push(v,a))return false;break;
        case MVM_OP_NEGATE: if(!need_stack(v,1)||!numeric(v,v->stack[v->sp-1],&x))return false;v->stack[v->sp-1]=mvm_number(-x);break;
        case MVM_OP_ABS: if(!need_stack(v,1)||!numeric(v,v->stack[v->sp-1],&x))return false;v->stack[v->sp-1]=mvm_number(fabs(x));break;
        case MVM_OP_SQRT: if(!need_stack(v,1)||!numeric(v,v->stack[v->sp-1],&x))return false;if(x<0)return fail(v,"sqrt requires non-negative number");v->stack[v->sp-1]=mvm_number(sqrt(x));break;
        case MVM_OP_FLOOR: if(!need_stack(v,1)||!numeric(v,v->stack[v->sp-1],&x))return false;v->stack[v->sp-1]=mvm_number(floor(x));break;
        case MVM_OP_CEIL: if(!need_stack(v,1)||!numeric(v,v->stack[v->sp-1],&x))return false;v->stack[v->sp-1]=mvm_number(ceil(x));break;
        case MVM_OP_ROUND: if(!need_stack(v,1)||!numeric(v,v->stack[v->sp-1],&x))return false;v->stack[v->sp-1]=mvm_number(round(x));break;
        case MVM_OP_EQUAL: case MVM_OP_NOT_EQUAL:
            if(!need_stack(v,2))return false;b=v->stack[--v->sp];a=v->stack[--v->sp];
            if(!mvm_stack_push(v,mvm_bool(in.opcode==MVM_OP_EQUAL?mvm_value_equal(a,b):!mvm_value_equal(a,b))))return false;break;
        case MVM_OP_NOT: if(!need_stack(v,1))return false;v->stack[v->sp-1]=mvm_bool(!mvm_value_truthy(v->stack[v->sp-1]));break;
        case MVM_OP_AND: case MVM_OP_OR:
            if(!need_stack(v,2))return false;b=v->stack[--v->sp];a=v->stack[--v->sp];
            if(!mvm_stack_push(v,mvm_bool(in.opcode==MVM_OP_AND?(mvm_value_truthy(a)&&mvm_value_truthy(b)):(mvm_value_truthy(a)||mvm_value_truthy(b)))))return false;break;
        case MVM_OP_LOAD_LOCAL:
            if(in.operand<0||in.operand>=MVM_MAX_LOCALS)return fail(v,"local slot out of range");
            if(v->frame_count&&(unsigned)in.operand>=v->frames[v->frame_count-1].closure->as.closure.function->as.function.local_count)return fail(v,"local slot exceeds function local count");
            if(v->frame_count){CallFrame*frame=&v->frames[v->frame_count-1];if(!frame->local_set[in.operand])return fail(v,"read of uninitialized local");a=frame->locals[in.operand];}
            else {if(!v->local_set[in.operand])return fail(v,"read of uninitialized local");a=v->locals[in.operand];}
            if(!mvm_stack_push(v,a))return false;break;
        case MVM_OP_STORE_LOCAL:
            if(in.operand<0||in.operand>=MVM_MAX_LOCALS)return fail(v,"local slot out of range");
            if(v->frame_count&&(unsigned)in.operand>=v->frames[v->frame_count-1].closure->as.closure.function->as.function.local_count)return fail(v,"local slot exceeds function local count");
            if(!need_stack(v,1))return false;a=v->stack[--v->sp];
            if(v->frame_count){CallFrame*frame=&v->frames[v->frame_count-1];frame->locals[in.operand]=a;frame->local_set[in.operand]=true;}
            else {v->locals[in.operand]=a;v->local_set[in.operand]=true;}break;
        case MVM_OP_JUMP: case MVM_OP_LOOP:
            if(!valid_target(v,in.operand))return false;v->ip=(size_t)in.operand;break;
        case MVM_OP_JUMP_IF_FALSE: case MVM_OP_JUMP_IF_TRUE:
            if(!need_stack(v,1)||!valid_target(v,in.operand))return false;
            if(in.opcode==MVM_OP_JUMP_IF_FALSE?!mvm_value_truthy(v->stack[v->sp-1]):mvm_value_truthy(v->stack[v->sp-1]))v->ip=(size_t)in.operand;
            v->sp--;break;
        case MVM_OP_PRINT: if(!mvm_stack_pop(v,&a))return false;mvm_value_print(stdout,a);fputc('\n',stdout);break;
        case MVM_OP_TYPEOF:
            if(!need_stack(v,1))return false;
            switch(v->stack[v->sp-1].type){case MVM_VALUE_NULL:v->stack[v->sp-1]=mvm_string("null");break;case MVM_VALUE_BOOL:v->stack[v->sp-1]=mvm_string("bool");break;case MVM_VALUE_NUMBER:v->stack[v->sp-1]=mvm_string("number");break;case MVM_VALUE_STRING:v->stack[v->sp-1]=mvm_string("string");break;case MVM_VALUE_ARRAY:v->stack[v->sp-1]=mvm_string("array");break;case MVM_VALUE_MAP:v->stack[v->sp-1]=mvm_string("map");break;case MVM_VALUE_FUNCTION:v->stack[v->sp-1]=mvm_string("function");break;case MVM_VALUE_CLOSURE:v->stack[v->sp-1]=mvm_string("closure");break;default:v->stack[v->sp-1]=mvm_string("invalid");break;}break;
        case MVM_OP_TO_BOOL: if(!need_stack(v,1))return false;v->stack[v->sp-1]=mvm_bool(mvm_value_truthy(v->stack[v->sp-1]));break;
        case MVM_OP_STRING_LENGTH: if(!need_stack(v,1))return false;if(v->stack[v->sp-1].type!=MVM_VALUE_STRING)return fail(v,"string_length requires string");v->stack[v->sp-1]=mvm_number((double)strlen(v->stack[v->sp-1].as.string?v->stack[v->sp-1].as.string:""));break;
        case MVM_OP_BIT_AND: case MVM_OP_BIT_OR: case MVM_OP_BIT_XOR: case MVM_OP_SHIFT_LEFT: case MVM_OP_SHIFT_RIGHT:
            if(!need_stack(v,2))return false;b=v->stack[--v->sp];a=v->stack[--v->sp];if(!numeric(v,a,&x)||!numeric(v,b,&y))return false;
            if(!isfinite(x)||!isfinite(y)||x<(double)INT32_MIN||x>(double)INT32_MAX||y<(double)INT32_MIN||y>(double)INT32_MAX||floor(x)!=x||floor(y)!=y)return fail(v,"bitwise op requires 32-bit integers");
            if((in.opcode==MVM_OP_SHIFT_LEFT||in.opcode==MVM_OP_SHIFT_RIGHT)&&(y<0||y>31))return fail(v,"shift count must be 0..31");
            {int32_t ix=(int32_t)x,iy=(int32_t)y;int32_t z=0;switch(in.opcode){case MVM_OP_BIT_AND:z=ix&iy;break;case MVM_OP_BIT_OR:z=ix|iy;break;case MVM_OP_BIT_XOR:z=ix^iy;break;case MVM_OP_SHIFT_LEFT:z=(int32_t)((uint32_t)ix<<(unsigned)iy);break;default:z=ix>>(unsigned)iy;break;}if(!mvm_stack_push(v,mvm_number((double)z)))return false;}break;

        case MVM_OP_ARRAY_NEW: {
            if(in.operand<0||(size_t)in.operand>v->sp)return fail(v,"invalid array initializer count");
            size_t n=(size_t)in.operand,base=v->sp-n;MvmValue arr=mvm_array_new(v);if(arr.type!=MVM_VALUE_ARRAY)return false;
            for(size_t i=base;i<v->sp;i++)if(!mvm_array_push(v,arr,v->stack[i]))return false;
            v->sp=base;if(!mvm_stack_push(v,arr))return false;break;
        }
        case MVM_OP_ARRAY_GET: {
            if(!need_stack(v,2))return false;MvmValue idx=v->stack[--v->sp],arr=v->stack[--v->sp];
            if(idx.type!=MVM_VALUE_NUMBER||!isfinite(idx.as.number)||idx.as.number<0||floor(idx.as.number)!=idx.as.number)return fail(v,"array index must be non-negative integer");
            if(!mvm_array_get(v,arr,(size_t)idx.as.number,&a)||!mvm_stack_push(v,a))return false;break;
        }
        case MVM_OP_ARRAY_SET: {
            if(!need_stack(v,3))return false;MvmValue val=v->stack[--v->sp],idx=v->stack[--v->sp],arr=v->stack[--v->sp];
            if(idx.type!=MVM_VALUE_NUMBER||!isfinite(idx.as.number)||idx.as.number<0||floor(idx.as.number)!=idx.as.number)return fail(v,"array index must be non-negative integer");
            if(!mvm_array_set(v,arr,(size_t)idx.as.number,val)||!mvm_stack_push(v,val))return false;break;
        }
        case MVM_OP_MAP_NEW: {
            if(in.operand<0||(size_t)in.operand*2>v->sp)return fail(v,"invalid map initializer count");
            size_t n=(size_t)in.operand,base=v->sp-n*2;MvmValue map=mvm_map_new(v);if(map.type!=MVM_VALUE_MAP)return false;
            for(size_t i=base;i<v->sp;i+=2){if(v->stack[i].type!=MVM_VALUE_STRING)return fail(v,"map keys must be strings");if(!mvm_map_set(v,map,v->stack[i].as.string,v->stack[i+1]))return false;}
            v->sp=base;if(!mvm_stack_push(v,map))return false;break;
        }
        case MVM_OP_MAP_GET: {
            if(!need_stack(v,2))return false;b=v->stack[--v->sp];a=v->stack[--v->sp];if(b.type!=MVM_VALUE_STRING)return fail(v,"map key must be string");
            if(!mvm_map_get(v,a,b.as.string,&b)||!mvm_stack_push(v,b))return false;break;
        }
        case MVM_OP_MAP_SET: {
            if(!need_stack(v,3))return false;MvmValue val=v->stack[--v->sp],key=v->stack[--v->sp],map=v->stack[--v->sp];if(key.type!=MVM_VALUE_STRING)return fail(v,"map key must be string");
            if(!mvm_map_set(v,map,key.as.string,val)||!mvm_stack_push(v,val))return false;break;
        }
        case MVM_OP_LENGTH:
            if(!need_stack(v,1))return false;a=v->stack[v->sp-1];
            if(a.type==MVM_VALUE_ARRAY)v->stack[v->sp-1]=mvm_number((double)mvm_array_length(a));
            else if(a.type==MVM_VALUE_MAP)v->stack[v->sp-1]=mvm_number((double)mvm_map_length(a));
            else if(a.type==MVM_VALUE_STRING)v->stack[v->sp-1]=mvm_number((double)strlen(a.as.string?a.as.string:""));
            else return fail(v,"length requires array, map, or string");break;
        case MVM_OP_CLOSURE: {
            if(in.operand<0||(size_t)in.operand>=v->constant_count)return fail(v,"invalid function descriptor constant");
            MvmValue fn=v->constants[in.operand];if(fn.type!=MVM_VALUE_FUNCTION||!fn.as.object||fn.as.object->kind!=OBJ_FUNCTION)return fail(v,"closure constant is not a function descriptor");
            size_t n=fn.as.object->as.function.upvalue_count;if(!need_stack(v,n))return false;
            MvmValue closure=mvm_closure_new(v,fn,v->stack+v->sp-n,n);if(closure.type!=MVM_VALUE_CLOSURE)return false;v->sp-=n;if(!mvm_stack_push(v,closure))return false;break;
        }
        case MVM_OP_CALL_FUNCTION: {
            unsigned argc=(unsigned)in.operand;if(in.operand<0||argc>MVM_MAX_LOCALS||!need_stack(v,(size_t)argc+1))return fail(v,"invalid function call arguments");
            size_t base=v->sp-argc-1;MvmValue callee=v->stack[base];
            if(callee.type!=MVM_VALUE_CLOSURE||!callee.as.object||callee.as.object->kind!=OBJ_CLOSURE)return fail(v,"call target is not closure");
            MvmObject*cl=callee.as.object;MvmObject*fn=cl->as.closure.function;
            if(argc!=fn->as.function.arity)return fail(v,"function arity mismatch");
            if(v->frame_count>=MVM_MAX_FRAMES)return fail(v,"call frame limit exceeded");
            if(fn->as.function.entry>=v->code_count)return fail(v,"function entry point out of range");
            CallFrame*frame=&v->frames[v->frame_count++];memset(frame,0,sizeof(*frame));frame->closure=cl;frame->return_ip=v->ip;frame->base_sp=base;
            for(unsigned i=0;i<argc;i++){frame->locals[i]=v->stack[base+1+i];frame->local_set[i]=true;}
            v->sp=base;v->ip=fn->as.function.entry;break;
        }
        case MVM_OP_GET_UPVALUE: {
            if(!v->frame_count)return fail(v,"upvalue access outside function");CallFrame*frame=&v->frames[v->frame_count-1];MvmObject*cl=frame->closure;
            if(in.operand<0||(size_t)in.operand>=cl->as.closure.count)return fail(v,"upvalue index out of range");
            if(!mvm_stack_push(v,cl->as.closure.captures[in.operand]))return false;break;
        }
        case MVM_OP_SET_UPVALUE: {
            if(!v->frame_count||!need_stack(v,1))return fail(v,"upvalue assignment outside function");CallFrame*frame=&v->frames[v->frame_count-1];MvmObject*cl=frame->closure;
            if(in.operand<0||(size_t)in.operand>=cl->as.closure.count)return fail(v,"upvalue index out of range");
            cl->as.closure.captures[in.operand]=v->stack[--v->sp];break;
        }
        case MVM_OP_CALL_NATIVE: {
            unsigned ni=(unsigned)in.operand>>16,argc=(unsigned)in.operand&0xffffu;
            if(ni>=v->native_count||argc>MVM_MAX_ARGS)return fail(v,"invalid native call");
            if(!need_stack(v,argc))return false;
            MvmValue args[MVM_MAX_ARGS];for(unsigned i=0;i<argc;i++)args[i]=v->stack[v->sp-argc+i];
            v->sp-=argc;MvmValue r=v->natives[ni].fn(v,(int)argc,args,v->natives[ni].userdata);
            if(v->error[0])return false;if(!mvm_stack_push(v,r))return false;break;
        }
        case MVM_OP_RETURN:
            if(v->sp)v->result=v->stack[--v->sp];else v->result=mvm_null();
            if(v->frame_count){CallFrame frame=v->frames[--v->frame_count];v->ip=frame.return_ip;v->sp=frame.base_sp;if(!mvm_stack_push(v,v->result))return false;break;}
            profile_finish(v,profile_start);return true;
        case MVM_OP_HALT:
            if(v->sp)v->result=v->stack[v->sp-1];profile_finish(v,profile_start);return true;
        default: return fail(v,"unknown bytecode opcode");
        }
    }
    if(v->sp)v->result=v->stack[v->sp-1];
    profile_finish(v,profile_start);
    return true;
}
