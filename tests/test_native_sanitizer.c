#include "codegen_abi.h"
#include <stdlib.h>
int main(void){
    BLRuntime vm={"sanitizer-hook",NULL};
    BLValue *globals=calloc(1,sizeof *globals);unsigned char flags[1]={0};
    _Alignas(16) unsigned char storage[160];BPPNativeLayout layout={0,1,0,0,0};
    if(!globals)return 2;
    BPPNativeFrame *frame=bpp_n_enter(&vm,globals,flags,NULL,&layout,storage);
    /* This deliberately violates a compiled global index to test the ASAN hook. */
    bpp_a_access(frame,BL_IR_LOAD_GLOBAL,1);
    free(globals);return 0;
}
