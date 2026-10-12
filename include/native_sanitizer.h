#ifndef BPP_NATIVE_SANITIZER_H
#define BPP_NATIVE_SANITIZER_H
#include <stddef.h>
#include <stdint.h>
/* Compiler-private portable boundary. Host sanitizer ABIs stay in its C
 * implementation; no shadow address constants are baked into assembly. */
void bpp_s_site(const char *, size_t, size_t);
void bpp_s_fail(size_t);
uint64_t bpp_s_read(const void *, size_t);
void bpp_s_write(void *, uint64_t, size_t);
uint64_t bpp_s_byte_load(const void *, int64_t);
void bpp_s_byte_store(void *, int64_t, uint64_t);
void *bpp_s_malloc(int64_t);
void *bpp_s_realloc(void *, int64_t);
void bpp_s_free(void *);
void *bpp_s_frame_enter(size_t);
void bpp_s_frame_leave(void *);
void bpp_s_globals(void *, size_t);
void bpp_s_print_range(const void *, int64_t);
void bpp_s_finish(void);
#endif
