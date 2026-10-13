#include "../include/codegen_abi.h"
#include "../include/native_runtime.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the assembly/runtime boundary without executing target instructions. */
int main(void) {
    assert(bpp_native_runtime_helper_count()==21);
    int lowered=0;
    for(size_t helper=0;helper<bpp_native_runtime_helper_count();++helper){
        const BPPNativeRuntimeHelper *entry=bpp_native_runtime_helper_at(helper);
        assert(entry&&entry->symbol&&entry->source&&entry->effects&&entry->ownership&&entry->error_behavior&&entry->sanitizer_behavior);
        lowered+=entry->native_lowered;
    }
    assert(lowered==8&&!bpp_native_runtime_helper_at(bpp_native_runtime_helper_count()));
    BPPNativeFrameLayout exact;
    assert(bpp_native_frame_layout(0,0,0,&exact));
    assert(exact.value_offset==128&&exact.value_bytes==0&&exact.locals_offset==128);
    assert(exact.flags_offset==128&&exact.outgoing_offset==128&&exact.frame_bytes==128);
    assert(bpp_native_frame_layout(2,3,4,&exact));
    assert(exact.value_bytes==32&&exact.locals_offset==160&&exact.locals_bytes==48);
    assert(exact.flags_offset==208&&exact.flags_bytes==3&&exact.outgoing_offset==224);
    assert(exact.outgoing_bytes==64&&exact.frame_bytes==288);
    /* Sequential values, nested-call argument capacity, many locals, and a
       large frame all share the same exact layout contract. */
    assert(bpp_native_frame_layout(5,2,0,&exact));
    assert(exact.locals_offset==208&&exact.flags_offset==240&&exact.outgoing_offset==256&&exact.frame_bytes==256);
    assert(bpp_native_frame_layout(3,1,5,&exact));
    assert(exact.locals_offset==176&&exact.flags_offset==192&&exact.outgoing_offset==208&&exact.frame_bytes==288);
    assert(bpp_native_frame_layout(4,100,0,&exact));
    assert(exact.locals_offset==192&&exact.flags_offset==1792&&exact.outgoing_offset==1904&&exact.frame_bytes==1904);
    assert(bpp_native_frame_layout(2000,0,0,&exact));
    assert(exact.value_bytes==32000&&exact.frame_bytes==32128);
    BPPNativeStackUsage stack;
    assert(bpp_native_stack_usage(288,2,0,&stack));
    assert(stack.saved_bytes==16&&stack.allocation_bytes==360&&stack.total_bytes==376);
    assert(bpp_native_stack_usage(288,3,0,&stack));
    assert(stack.saved_bytes==24&&stack.allocation_bytes==368&&stack.total_bytes==392);
    assert(bpp_native_stack_usage(288,6,1,&stack));
    assert(stack.saved_bytes==48&&stack.allocation_bytes==288&&stack.total_bytes==352);
    assert(!bpp_native_stack_usage(SIZE_MAX,6,1,&stack));
    assert(!bpp_native_frame_layout(SIZE_MAX,0,0,&exact));
    assert(!bpp_native_frame_layout(0,SIZE_MAX,0,&exact));
    assert(!bpp_native_frame_layout(0,0,SIZE_MAX,&exact));
    unsigned char *process_memory = bpp_a_allocate_zero(4, sizeof(BLValue));
    assert(process_memory && (uintptr_t)process_memory % _Alignof(BLValue) == 0);
    for(size_t byte = 0; byte < 4 * sizeof(BLValue); ++byte) assert(process_memory[byte] == 0);
    bpp_a_free_memory(process_memory);
    assert(!bpp_a_allocate_zero(SIZE_MAX, 2));

    int converted=0;
    BLValue numeric_text=bl_value_string_copy("-9223372036854775808");
    assert(bl_value_to_int(numeric_text,&converted)==LLONG_MIN&&converted);
    bl_value_release(numeric_text);
    numeric_text=bl_value_string_copy("9223372036854775808");
    (void)bl_value_to_int(numeric_text,&converted);assert(!converted);bl_value_release(numeric_text);

    _Alignas(16) unsigned char outer_storage[512], inner_storage[512];
    BPPNativeLayout layout = {1, 4, 2, 2, 4, 2, 4, 0};
    BLRuntime vm = {"frame-test", NULL};
    BLValue globals[1] = {bl_value_null()}, result = bl_value_null();
    unsigned char initialized[1] = {0};
    BLValue input = bl_value_string_copy("owned");
    memset(outer_storage, 0xa5, sizeof outer_storage);
    BPPNativeFrame *outer = bpp_n_enter(&vm, globals, initialized, &input, &layout, outer_storage);
    assert((void *)outer == (void *)outer_storage);
    assert(input.as.obj->ref_count == 2);
    bpp_a_op(outer, BL_IR_LOAD_LOCAL, 0, 0, BL_IR_NONE, BL_IR_NONE, 1, 1);
    assert(input.as.obj->ref_count == 3);
    bpp_a_arg(outer, 0, 0);
    assert((uintptr_t)bpp_a_args(outer) % 16 == 0);
    /* Arguments borrow values; a nested frame retains its own parameter. */
    BPPNativeFrame *inner = bpp_n_enter(&vm, globals, initialized, bpp_a_args(outer), &layout, inner_storage);
    assert(input.as.obj->ref_count == 4);
    bpp_a_op(inner, BL_IR_LOAD_LOCAL, 1, 0, BL_IR_NONE, BL_IR_NONE, 2, 1);
    bpp_a_leave(inner, 1, bpp_a_destination(outer, 1));
    assert(input.as.obj->ref_count == 4);
    bpp_a_leave(outer, 1, &result);
    assert(result.as.obj == input.as.obj && input.as.obj->ref_count == 2);
    bl_value_release(result);
    assert(input.as.obj->ref_count == 1);

    /* The same stack storage can be reused; errors still release every owner. */
    outer = bpp_n_enter(&vm, globals, initialized, &input, &layout, outer_storage);
    bpp_a_op(outer, BL_IR_LOAD_LOCAL, 0, 0, BL_IR_NONE, BL_IR_NONE, 3, 1);
    bpp_a_integer(outer, 1, LLONG_MAX);
    bpp_a_integer(outer, 2, 1);
    bpp_a_op(outer, BL_IR_ADD, 3, 1, 2, BL_IR_NONE, 3, 7);
    assert(bpp_a_failed(outer));
    assert(strstr(vm.error, "frame-test:3:7:") && strstr(vm.error, "integer overflow"));
    bpp_a_leave(outer, BL_IR_NONE, &result);
    assert(result.tag == BLV_NULL && input.as.obj->ref_count == 1);
    free(vm.error);
    bl_value_release(input);

    /* Disjoint virtual values share one physical cell. Cleanup releases the cell once. */
    struct { BPPNativeLayout layout; size_t map[4]; } compact = {{0,4,0,0,1,0,1,BPP_LAYOUT_REGISTER_MAP},{0,0,0,0}};
    vm.error=NULL;
    outer=bpp_n_enter(&vm,globals,initialized,NULL,&compact.layout,outer_storage);
    bpp_a_string(outer,0,"first");
    bpp_a_op(outer,BL_IR_RELEASE,BL_IR_NONE,0,BL_IR_NONE,BL_IR_NONE,1,1);
    bpp_a_string(outer,3,"last");
    bpp_a_leave(outer,3,&result);
    assert(bl_value_is_string(result));
    assert(result.as.obj->ref_count==1);
    assert(!strcmp(bl_value_as_string(result)->data,"last"));
    bl_value_release(result);

    /* Shared local slots release the prior owned occupant on INIT_LOCAL. */
    struct { BPPNativeLayout layout; size_t map[2]; } shared_local =
        {{0,2,2,0,2,1,1,BPP_LAYOUT_LOCAL_MAP},{0,0}};
    outer=bpp_n_enter(&vm,globals,initialized,NULL,&shared_local.layout,outer_storage);
    bpp_a_string(outer,0,"replaced");
    bpp_a_op(outer,BL_IR_INIT_LOCAL,BL_IR_NONE,0,0,BL_IR_NONE,4,1);
    bpp_a_integer(outer,1,7);
    bpp_a_op(outer,BL_IR_INIT_LOCAL,BL_IR_NONE,1,1,BL_IR_NONE,5,1);
    bpp_a_leave(outer,BL_IR_NONE,&result);
    assert(result.tag==BLV_NULL);
    /* A failed rate calculation retains the explicit transition inputs. */
    BLRuntime truth_vm={"transition-error",NULL};BLSite truth_site={17,3,2};
    bl_rt_truth_enable(&truth_vm,1);
    BLValue rate=bl_value_state(bpp_truth_velocity((BPPStateValue){-1,{0}},(BPPStateValue){1,{0}},2),NULL);
    BLValue uncertain=bl_rt_binary(&truth_vm,&truth_site,BL_IR_DIV,rate,bl_value_int(0));
    BLState *error_state=bl_value_as_state(uncertain);
    assert(error_state&&error_state->state.value==0&&error_state->state.context.faults==4);
    assert(error_state->state.context.start==-1&&error_state->state.context.end==1&&error_state->state.context.dt==2);
    assert(error_state->state.context.origin_start==-1&&error_state->state.context.origin_end==1&&error_state->state.context.origin_dt==2);
    assert(error_state->state.context.source_file==2&&error_state->state.context.line==17);
    bl_value_release(rate);bl_value_release(uncertain);
    puts("Native stack-frame ownership tests passed");
    return 0;
}
