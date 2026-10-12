#include "../include/native_sanitizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define POISON(p,n) __asan_poison_memory_region(p,n)
#define UNPOISON(p,n) __asan_unpoison_memory_region(p,n)
#else
#define POISON(p,n) ((void)0)
#define UNPOISON(p,n) ((void)0)
#endif
#if defined(__SANITIZE_THREAD__) || __has_feature(thread_sanitizer)
extern void __tsan_func_entry(void *);
extern void __tsan_func_exit(void);
#ifdef __clang__
#define NATIVE_TRACE __attribute__((disable_sanitizer_instrumentation))
#else
#define NATIVE_TRACE __attribute__((no_sanitize_thread))
#endif
#define TRACE_ENTER() __tsan_func_entry(__builtin_return_address(0))
#define TRACE_EXIT() __tsan_func_exit()
#else
#define NATIVE_TRACE
#define TRACE_ENTER() ((void)0)
#define TRACE_EXIT() ((void)0)
#endif
/* The language currently has no threads or public FFI. This registry is
 * intentionally single-threaded. C loads/stores are host-instrumented, so
 * TSAN/MSAN see the actual access once rather than a duplicate callback. */
typedef struct Region {
    unsigned char *base, *allocation, *defined;
    size_t size;
    int live, frame, root, reachable;
    const char *file;
    size_t line, column;
    struct Region *next;
} Region;
static Region *regions;
static const char *source="<native>";
static size_t source_line, source_column;
void bpp_s_site(const char *file,size_t line,size_t column){source=file;source_line=line;source_column=column;}
static void fault(const char *category){
    fprintf(stderr,"%s:%zu:%zu: B++ sanitizer: %s\n",source,source_line,source_column,category);
    exit(86);
}
void bpp_s_fail(size_t category){fault(category==1?"integer-overflow":category==2?"invalid-division":"uninitialized-variable");}
static Region *find(const void *p){
    uintptr_t address=(uintptr_t)p;
    for(Region *r=regions;r;r=r->next)
        if(address>=(uintptr_t)r->base&&address-(uintptr_t)r->base<=r->size)return r;
    return NULL;
}
static Region *range(const void *p,size_t n,int write){
    Region *r=find(p);
    if(!r)fault("invalid-pointer");
    if(!r->live)fault(r->frame?"stack-use-after-return":"heap-use-after-free");
    size_t off=(uintptr_t)p-(uintptr_t)r->base;
    if(n>r->size-off)fault(r->frame?"stack-buffer-overflow":"heap-buffer-overflow");
    if(!write)for(size_t k=0;k<n;++k)if(!r->defined[off+k])fault("uninitialized-read");
    return r;
}
static Region *record(unsigned char *p,unsigned char *allocation,size_t n,int frame,int root,int initialized){
    Region *r=calloc(1,sizeof *r);
    if(!r)fault("instrumentation-out-of-memory");
    r->defined=malloc(n?n:1);
    if(!r->defined)fault("instrumentation-out-of-memory");
    memset(r->defined,initialized?1:0,n);
    r->base=p;r->allocation=allocation;r->size=n;r->frame=frame;r->root=root;r->live=1;
    r->file=source;r->line=source_line;r->column=source_column;r->next=regions;regions=r;return r;
}
static void *allocate(size_t n,int frame){
    if(n>SIZE_MAX-64)fault("allocation-size-overflow");
    unsigned char *p=frame?calloc(1,n+64):malloc(n+64);
    if(!p)fault("allocation-failure");
    record(p+32,p,n,frame,0,frame);
    POISON(p,32);POISON(p+32+n,32);
    return p+32;
}
void *bpp_s_malloc(int64_t n){if(n<0)fault("negative-allocation-size");return allocate((size_t)n,0);}
NATIVE_TRACE void *bpp_s_frame_enter(size_t n){TRACE_ENTER();return allocate(n,1);}
static void retire(Region *r){
    r->live=0;
    UNPOISON(r->allocation,r->size+64);
    free(r->allocation);r->allocation=NULL;
    free(r->defined);r->defined=NULL;
}
void bpp_s_free(void *p){
    if(!p)return;
    Region *r=find(p);
    if(!r||r->base!=p||r->frame||r->root)fault("invalid-free");
    if(!r->live)fault("double-free");
    retire(r);
}
NATIVE_TRACE void bpp_s_frame_leave(void *p){Region *r=find(p);if(!r||!r->live||!r->frame||r->base!=p)fault("invalid-frame");retire(r);TRACE_EXIT();}
void *bpp_s_realloc(void *p,int64_t n){
    if(!p)return bpp_s_malloc(n);
    if(n<0)fault("negative-allocation-size");
    Region *old=find(p);
    if(!old||old->base!=p||old->frame||old->root)fault("invalid-realloc");
    if(!old->live)fault("heap-use-after-free");
    unsigned char *next=allocate((size_t)n,0);Region *r=regions;
    size_t count=old->size<r->size?old->size:r->size;
    /* Copy only initialized bytes. Copying undefined native bytes through C
     * would make MSAN report in this helper instead of at the language read. */
    for(size_t k=0;k<count;++k)if(old->defined[k]){next[k]=old->base[k];r->defined[k]=1;}
    retire(old);return next;
}
void bpp_s_globals(void *p,size_t n){
    for(Region *r=regions;r;r=r->next)if(r->root&&r->base==p)return;
    record(p,NULL,n,0,1,1);
}
uint64_t bpp_s_read(const void *p,size_t n){
    if(n!=1&&n!=8)fault("invalid-access-width");
    range(p,n,0);
    uint64_t value=0;memcpy(&value,p,n);return value;
}
void bpp_s_write(void *p,uint64_t value,size_t n){
    if(n!=1&&n!=8)fault("invalid-access-width");
    Region *r=range(p,n,1);memcpy(p,&value,n);
    memset(r->defined+((uintptr_t)p-(uintptr_t)r->base),1,n);
}
static void *offset(const void *p,int64_t index){
    Region *r=range(p,0,1);size_t off=(uintptr_t)p-(uintptr_t)r->base;
    if(index<0){uint64_t magnitude=0-(uint64_t)index;if(magnitude>off)fault("pointer-underflow");off-=(size_t)magnitude;}
    else {if((uint64_t)index>r->size-off)fault("pointer-overflow");off+=(size_t)index;}
    return r->base+off;
}
uint64_t bpp_s_byte_load(const void *p,int64_t index){return bpp_s_read(offset(p,index),1);}
void bpp_s_byte_store(void *p,int64_t index,uint64_t v){bpp_s_write(offset(p,index),v,1);}
void bpp_s_print_range(const void *p,int64_t n){
    if(n<0)fault("negative-output-size");range(p,(size_t)n,0);
    /* Write once: the C/host boundary sees exactly the bytes checked above. */
#ifdef _WIN32
    if((uint64_t)n>UINT_MAX)fault("output-size-overflow");
    (void)_write(1,p,(unsigned)n);
#else
    (void)write(1,p,(size_t)n);
#endif
}
void bpp_s_finish(void){
    /* Conservative reachability from globals, then initialized aligned words
     * in reachable heap blocks. Cycles with no global root remain leaks. */
    for(Region *r=regions;r;r=r->next)r->reachable=r->live&&r->root;
    int changed=1;
    while(changed){changed=0;for(Region *r=regions;r;r=r->next)if(r->reachable){
        for(size_t off=0;r->size-off>=8;off+=8){int defined=1;for(size_t k=0;k<8;++k)defined&=r->defined[off+k]!=0;
            if(!defined)continue;
            uintptr_t p;memcpy(&p,r->base+off,8);Region *child=find((void *)p);
            if(child&&child->live&&!child->reachable){child->reachable=1;changed=1;}
        }
    }}
    size_t leaks=0;
    for(Region *r=regions;r;r=r->next)if(r->live&&!r->frame&&!r->root&&!r->reachable){
        fprintf(stderr,"%s:%zu:%zu: B++ sanitizer: memory-leak (%zu bytes)\n",r->file,r->line,r->column,r->size);++leaks;
    }
    while(regions){Region *r=regions;regions=r->next;
        if(r->live&&!r->root)retire(r);else free(r->defined);
        free(r);
    }
    if(leaks)exit(86);
}
