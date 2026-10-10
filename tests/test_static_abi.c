#include "bpp_abi.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void BPP_HOSTCALL increment(BLRuntime *vm,BLValue *globals,unsigned char *initialized,const BLValue *args,BLValue *out) {
    (void)vm;(void)globals;(void)initialized;
    *out=bl_value_int(args[0].as.i+1);
}

int main(void) {
    BPPStaticProcedure procedures[]={{"increment",1,BPP_STATIC_PROC_EXPORTED,increment}};
    BPPStaticLibrary library={BPP_STATIC_ABI_MAGIC,BPP_STATIC_ABI_MAJOR,BPP_STATIC_ABI_MINOR,
        BPP_STATIC_LIBRARY_ONLY|BPP_STATIC_NO_FFI|BPP_STATIC_VALUE_OWNERSHIP,
        sizeof(BLValue),offsetof(BLValue,tag),offsetof(BLValue,as),0,1,procedures};
    char error[160];assert(bpp_static_library_validate(&library,error,sizeof error));
    BLRuntime runtime={"abi-test",NULL};BLValue args[]={bl_value_int(41)},out=bl_value_null();
    assert(bpp_static_invoke(&procedures[0],&runtime,NULL,NULL,args,1,&out)==BPP_STATIC_OK);
    assert(out.tag==BLV_INT&&out.as.i==42);bl_value_release(out);
    assert(bpp_static_invoke(&procedures[0],&runtime,NULL,NULL,args,0,&out)==BPP_STATIC_ARITY);
    library.major++;
    assert(!bpp_static_library_validate(&library,error,sizeof error));
    assert(strstr(error,"major mismatch"));
    puts("Static ABI descriptor, ownership, and arity tests passed");
    return 0;
}
