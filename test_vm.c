#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if(!(c)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#c); return 1; } } while(0)
#define NEAR(a,b) (fabs((a)-(b)) < 1e-9)

static MvmValue native_sum(Mvm *vm,int argc,const MvmValue *argv,void *userdata) {
    (void)vm;(void)userdata;double sum=0;
    for(int i=0;i<argc;i++){if(argv[i].type!=MVM_VALUE_NUMBER)return mvm_null();sum+=argv[i].as.number;}
    return mvm_number(sum);
}
static int test_arithmetic(void) {
    Mvm *v=mvm_new();CHECK(v);
    int a=mvm_add_constant(v,mvm_number(20)),b=mvm_add_constant(v,mvm_number(22));
    CHECK(a>=0&&b>=0);
    CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_constant(v,b));CHECK(mvm_emit_add(v));CHECK(mvm_emit_halt(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_NUMBER);CHECK(NEAR(mvm_result(v).as.number,42));
    mvm_free(v);return 0;
}
static int test_string_and_types(void) {
    Mvm *v=mvm_new();CHECK(v);
    int a=mvm_add_constant(v,mvm_string("MacoraOne ")),b=mvm_add_constant(v,mvm_string("VM"));
    CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_constant(v,b));CHECK(mvm_emit_add(v));CHECK(mvm_emit_halt(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_STRING);CHECK(strcmp(mvm_result(v).as.string,"MacoraOne VM")==0);
    CHECK(mvm_value_truthy(mvm_bool(true)));CHECK(!mvm_value_truthy(mvm_null()));
    CHECK(mvm_value_equal(mvm_number(4),mvm_number(4)));
    mvm_free(v);return 0;
}
static int test_locals_and_comparisons(void) {
    Mvm *v=mvm_new();CHECK(v);int c=mvm_add_constant(v,mvm_number(7));CHECK(c>=0);
    CHECK(mvm_emit_constant(v,c));CHECK(mvm_emit_store_local(v,3));CHECK(mvm_emit_load_local(v,3));
    CHECK(mvm_emit_constant(v,c));CHECK(mvm_emit_greater_equal(v));CHECK(mvm_emit_halt(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_BOOL&&mvm_result(v).as.boolean);
    mvm_free(v);return 0;
}
static int test_branch(void) {
    Mvm *v=mvm_new();CHECK(v);int c=mvm_add_constant(v,mvm_number(99));CHECK(c>=0);
    CHECK(mvm_emit_false(v));
    size_t branch=mvm_instruction_count(v);CHECK(mvm_emit_jump_if_false(v,0));
    CHECK(mvm_emit_constant(v,c));CHECK(mvm_emit_halt(v));
    size_t target=mvm_instruction_count(v);CHECK(mvm_emit_constant(v,mvm_add_constant(v,mvm_number(42))));CHECK(mvm_emit_halt(v));
    CHECK(mvm_patch_operand(v,branch,(int32_t)target));CHECK(mvm_run(v));
    CHECK(mvm_result(v).type==MVM_VALUE_NUMBER&&NEAR(mvm_result(v).as.number,42));
    mvm_free(v);return 0;
}
static int test_native_api(void) {
    Mvm *v=mvm_new();CHECK(v);int n=mvm_register_native(v,"sum",native_sum,NULL);CHECK(n==0);
    CHECK(mvm_find_native(v,"sum")==n);CHECK(strcmp(mvm_native_name(v,(unsigned)n),"sum")==0);
    int a=mvm_add_constant(v,mvm_number(10)),b=mvm_add_constant(v,mvm_number(32));CHECK(a>=0&&b>=0);
    CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_constant(v,b));CHECK(mvm_emit_call_native(v,(unsigned)n,2));CHECK(mvm_emit_return(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_NUMBER&&NEAR(mvm_result(v).as.number,42));
    mvm_free(v);return 0;
}
static int test_profile_and_extended_opcodes(void) {
    Mvm *v=mvm_new();CHECK(v);mvm_profile_enable(v,true);
    int a=mvm_add_constant(v,mvm_number(9)),b=mvm_add_constant(v,mvm_number(2));CHECK(a>=0&&b>=0);
    CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_constant(v,b));CHECK(mvm_emit_power(v));
    CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_abs(v));CHECK(mvm_emit_pop(v));
    int d=mvm_add_constant(v,mvm_number(-2.4));CHECK(d>=0);CHECK(mvm_emit_constant(v,d));
    CHECK(mvm_emit_round(v));CHECK(mvm_emit_halt(v));CHECK(mvm_run(v));
    CHECK(mvm_result(v).type==MVM_VALUE_NUMBER&&NEAR(mvm_result(v).as.number,-2));
    const MvmProfile *p=mvm_profile_data(v);CHECK(p&&p->instructions_executed==9);CHECK(p->max_stack_depth>=2);
    CHECK(mvm_profile_instruction_hits(v,2)==1);CHECK(p->opcode_counts[MVM_OP_POWER]==1);
    FILE *f=tmpfile();CHECK(f);CHECK(mvm_profile_write_json(v,f));rewind(f);char buf[512]={0};CHECK(fread(buf,1,sizeof(buf)-1,f)>0);CHECK(strstr(buf,"instructions_executed")!=NULL);fclose(f);
    mvm_free(v);
    v=mvm_new();CHECK(v);CHECK(mvm_emit_constant(v,mvm_add_constant(v,mvm_number(6))));CHECK(mvm_emit_constant(v,mvm_add_constant(v,mvm_number(3))));CHECK(mvm_emit_bit_xor(v));CHECK(mvm_emit_halt(v));CHECK(mvm_run(v));CHECK(mvm_result(v).as.number==5);mvm_free(v);return 0;
}
static int test_runtime_errors(void) {
    Mvm *v=mvm_new();CHECK(v);CHECK(mvm_emit_null(v));CHECK(mvm_emit_negate(v));CHECK(!mvm_run(v));
    CHECK(strstr(mvm_last_error(v),"number")!=NULL);mvm_free(v);
    v=mvm_new();CHECK(v);int a=mvm_add_constant(v,mvm_number(1)),b=mvm_add_constant(v,mvm_number(0));
    CHECK(a>=0&&b>=0);CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_constant(v,b));CHECK(mvm_emit_divide(v));CHECK(!mvm_run(v));
    CHECK(strstr(mvm_last_error(v),"zero")!=NULL);mvm_free(v);return 0;
}

static int test_heap_collections_and_closures(void) {
    Mvm *v=mvm_new();CHECK(v);
    MvmValue a=mvm_array_new(v);CHECK(a.type==MVM_VALUE_ARRAY);
    CHECK(mvm_array_push(v,a,mvm_number(11)));CHECK(mvm_array_push(v,a,mvm_number(22)));
    MvmValue x=mvm_null();CHECK(mvm_array_get(v,a,1,&x));CHECK(x.type==MVM_VALUE_NUMBER&&x.as.number==22);
    CHECK(mvm_array_set(v,a,0,mvm_number(7)));CHECK(mvm_array_length(a)==2);
    MvmValue map=mvm_map_new(v);CHECK(map.type==MVM_VALUE_MAP);CHECK(mvm_map_set(v,map,"answer",mvm_number(42)));
    CHECK(mvm_map_get(v,map,"answer",&x));CHECK(x.as.number==42&&mvm_map_length(map)==1);
    MvmValue fn=mvm_function_new(v,"plusCaptured",1,1,1,0);CHECK(fn.type==MVM_VALUE_FUNCTION);
    int fc=mvm_add_constant(v,fn);CHECK(fc>=0);
    size_t skip=mvm_instruction_count(v);CHECK(mvm_emit_jump(v,0));
    size_t entry=mvm_instruction_count(v);
    CHECK(mvm_emit_get_upvalue(v,0));CHECK(mvm_emit_load_local(v,0));CHECK(mvm_emit_add(v));CHECK(mvm_emit_return(v));
    CHECK(mvm_function_set_entry(v,fn,entry));
    size_t main_ip=mvm_instruction_count(v);CHECK(mvm_patch_operand(v,skip,(int32_t)main_ip));
    int cap=mvm_add_constant(v,mvm_number(32)),arg=mvm_add_constant(v,mvm_number(10));CHECK(cap>=0&&arg>=0);
    CHECK(mvm_emit_constant(v,cap));CHECK(mvm_emit_closure(v,fc));CHECK(mvm_emit_constant(v,arg));
    CHECK(mvm_emit_call_function(v,1));CHECK(mvm_emit_halt(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_NUMBER&&NEAR(mvm_result(v).as.number,42));
    mvm_free(v);return 0;
}


static int test_collection_bytecodes(void) {
    Mvm *v=mvm_new();CHECK(v);
    int a=mvm_add_constant(v,mvm_number(11)),b=mvm_add_constant(v,mvm_number(22));CHECK(a>=0&&b>=0);
    CHECK(mvm_emit_constant(v,a));CHECK(mvm_emit_constant(v,b));CHECK(mvm_emit_array_new(v,2));
    int ix=mvm_add_constant(v,mvm_number(1));CHECK(ix>=0);CHECK(mvm_emit_constant(v,ix));
    CHECK(mvm_emit_array_get(v));CHECK(mvm_emit_halt(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_NUMBER&&mvm_result(v).as.number==22);mvm_free(v);
    v=mvm_new();CHECK(v);
    int key=mvm_add_constant(v,mvm_string("answer")),val=mvm_add_constant(v,mvm_number(42));CHECK(key>=0&&val>=0);
    CHECK(mvm_emit_constant(v,key));CHECK(mvm_emit_constant(v,val));CHECK(mvm_emit_map_new(v,1));
    CHECK(mvm_emit_constant(v,key));CHECK(mvm_emit_map_get(v));CHECK(mvm_emit_halt(v));
    CHECK(mvm_run(v));CHECK(mvm_result(v).type==MVM_VALUE_NUMBER&&mvm_result(v).as.number==42);mvm_free(v);
    return 0;
}

int main(void) {
    CHECK(strcmp(MVM_VERSION,"1.2.0")==0);
    CHECK(test_arithmetic()==0);
    CHECK(test_string_and_types()==0);
    CHECK(test_locals_and_comparisons()==0);
    CHECK(test_branch()==0);
    CHECK(test_native_api()==0);
    CHECK(test_profile_and_extended_opcodes()==0);
    CHECK(test_heap_collections_and_closures()==0);
    CHECK(test_collection_bytecodes()==0);
    CHECK(test_runtime_errors()==0);
    puts("MACORAONE_VM_ALL_TESTS_PASS");
    return 0;
}
