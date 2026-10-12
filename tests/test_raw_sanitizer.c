#include "native_sanitizer.h"
#include <assert.h>
#include <string.h>
int main(int argc,char **argv){
    bpp_s_site("intentional-native-fault.bpp",7,3);
    const char *mode=argc>1?argv[1]:"ok";
    void *p=bpp_s_malloc(8);
    if(!strcmp(mode,"invalid-pointer"))return (int)bpp_s_byte_load(NULL,0);
    if(!strcmp(mode,"allocation-overflow"))bpp_s_frame_enter(SIZE_MAX);
    if(!strcmp(mode,"uninitialized"))return (int)bpp_s_byte_load(p,0);
    bpp_s_write(p,42,8);
    if(!strcmp(mode,"invalid-realloc"))bpp_s_realloc((char *)p+1,8);
    if(!strcmp(mode,"output-bounds"))bpp_s_print_range(p,9);
    if(!strcmp(mode,"negative-output"))bpp_s_print_range(p,-1);
    if(!strcmp(mode,"bounds"))return (int)bpp_s_byte_load(p,8);
    if(!strcmp(mode,"underflow"))return (int)bpp_s_byte_load(p,-1);
    if(!strcmp(mode,"invalid-free"))bpp_s_free((char *)p+1);
    if(!strcmp(mode,"leak")){bpp_s_finish();return 0;}
    if(!strcmp(mode,"cycle")){
        void *q=bpp_s_malloc(8);bpp_s_write(p,(uintptr_t)q,8);bpp_s_write(q,(uintptr_t)p,8);bpp_s_finish();return 0;
    }
    if(!strcmp(mode,"root")){
        uintptr_t root=(uintptr_t)p;bpp_s_globals(&root,8);bpp_s_finish();return 0;
    }
    p=bpp_s_realloc(p,16);assert(bpp_s_read(p,8)==42);
    if(!strcmp(mode,"grown-uninitialized"))return (int)bpp_s_byte_load(p,8);
    bpp_s_byte_store(p,8,43);assert(bpp_s_byte_load(p,8)==43);
    bpp_s_free(p);
    if(!strcmp(mode,"use-after-free"))return (int)bpp_s_byte_load(p,0);
    if(!strcmp(mode,"double-free"))bpp_s_free(p);
    if(!strcmp(mode,"negative-size"))bpp_s_malloc(-1);
    void *frame=bpp_s_frame_enter(64);assert(bpp_s_read(frame,8)==0);
    if(!strcmp(mode,"stack-bounds"))return (int)bpp_s_read((char *)frame+64,8);
    bpp_s_frame_leave(frame);
    if(!strcmp(mode,"stack-return"))return (int)bpp_s_read(frame,8);
    if(!strcmp(mode,"overflow"))bpp_s_fail(1);
    bpp_s_finish();return 0;
}
