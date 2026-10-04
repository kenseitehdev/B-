#include "../include/codegen_abi.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the assembly/runtime boundary without executing target instructions. */
int main(void) {
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
    puts("Native stack-frame ownership tests passed");
    return 0;
}
