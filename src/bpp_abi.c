#include "../include/bpp_abi.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static void message(char *out,size_t size,const char *format,...) {
    if(!out||!size)return;
    va_list args;va_start(args,format);vsnprintf(out,size,format,args);va_end(args);
}

const char *bpp_static_status_name(BPPStaticStatus status) {
    static const char *names[]={"ok","bad-library","bad-procedure","arity","runtime-error"};
    return (unsigned)status<sizeof names/sizeof names[0]?names[status]:"unknown";
}

int bpp_static_library_validate(const BPPStaticLibrary *library,char *error,size_t error_size) {
    if(!library){message(error,error_size,"missing B++ static library descriptor");return 0;}
    if(library->magic!=BPP_STATIC_ABI_MAGIC){message(error,error_size,"B++ static ABI magic mismatch");return 0;}
    if(library->major!=BPP_STATIC_ABI_MAJOR){message(error,error_size,"B++ static ABI major mismatch: library=%u runtime=%u",library->major,BPP_STATIC_ABI_MAJOR);return 0;}
    if(library->minor>BPP_STATIC_ABI_MINOR){message(error,error_size,"B++ static ABI minor is newer: library=%u runtime=%u",library->minor,BPP_STATIC_ABI_MINOR);return 0;}
    if((library->flags&(BPP_STATIC_LIBRARY_ONLY|BPP_STATIC_NO_FFI|BPP_STATIC_VALUE_OWNERSHIP))!=(BPP_STATIC_LIBRARY_ONLY|BPP_STATIC_NO_FFI|BPP_STATIC_VALUE_OWNERSHIP)){message(error,error_size,"B++ static ABI required policy flags are missing");return 0;}
    if(library->value_bytes!=sizeof(BLValue)||library->value_tag_offset!=offsetof(BLValue,tag)||library->value_payload_offset!=offsetof(BLValue,as)){message(error,error_size,"B++ static ABI value layout mismatch");return 0;}
    if(library->procedure_count&&!library->procedures){message(error,error_size,"B++ static ABI procedure table is missing");return 0;}
    for(size_t i=0;i<library->procedure_count;++i){const BPPStaticProcedure *p=&library->procedures[i];if(!p->name||!p->name[0]||!p->entry||(p->flags&BPP_STATIC_PROC_EXPORTED)==0){message(error,error_size,"B++ static ABI invalid procedure at index %zu",i);return 0;}}
    if(error&&error_size)error[0]=0;
    return 1;
}

BPPStaticStatus bpp_static_invoke(const BPPStaticProcedure *procedure,BLRuntime *runtime,BLValue *globals,unsigned char *initialized,const BLValue *args,size_t argc,BLValue *out) {
    if(!procedure||!procedure->entry||!runtime||!out)return BPP_STATIC_BAD_PROCEDURE;
    if(argc!=procedure->arity)return BPP_STATIC_ARITY;
    if(argc&&!args)return BPP_STATIC_BAD_PROCEDURE;
    if(runtime->error)return BPP_STATIC_RUNTIME_ERROR;
    *out=bl_value_null();
    procedure->entry(runtime,globals,initialized,args,out);
    return runtime->error?BPP_STATIC_RUNTIME_ERROR:BPP_STATIC_OK;
}

void bpp_static_runtime_error(BLRuntime *runtime,const char *detail) {
    if (!runtime || runtime->error) return;
    const char *path = runtime->path ? runtime->path : "<input>";
    const char *message = detail ? detail : "static library failure";
    size_t bytes = strlen(path) + strlen(message) + 8;
    runtime->error = calloc(bytes, 1);
    if (!runtime->error) { perror("calloc"); exit(1); }
    snprintf(runtime->error, bytes, "%s: %s", path, message);
}
