/*
 * simd.c — SIMD acceleration for the xf interpreter
 *
 * Build notes:
 *   The scalar fallbacks compile on any C11 target.
 *   SIMD paths are guarded by __has_include / compiler-defined macros and
 *   are only compiled when the host toolchain supports them.
 *   Runtime dispatch means a binary built on a modern machine still runs
 *   correctly on older hardware.
 *
 *   Recommended flags:
 *     x86-64:  -O3 -msse4.2   (AVX2 paths need -mavx2 but are checked at runtime)
 *     ARM:     -O3             (NEON is baseline on AArch64)
 */

#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif

#include "../include/simd.h"

#include <string.h>
#include <stdlib.h>
#include <float.h>
#include <math.h>

/* ============================================================
 * Architecture detection
 * ============================================================ */

#if defined(__x86_64__) || defined(__i386__)
#  define XF_ARCH_X86 1
#endif

#if defined(__aarch64__) || defined(__arm__)
#  define XF_ARCH_ARM 1
#endif

/* Pull in intrinsics only when available and not disabled. */
#ifndef XF_SIMD_DISABLE
#  ifdef XF_ARCH_X86
#    include <cpuid.h>
#    ifdef __SSE2__
#      include <emmintrin.h>
#      define XF_HAS_SSE2 1
#    endif
#    ifdef __SSE4_2__
#      include <nmmintrin.h>
#      define XF_HAS_SSE4_2 1
#    endif
#    ifdef __AVX2__
#      include <immintrin.h>
#      define XF_HAS_AVX2 1
#    endif
#  endif
#  ifdef XF_ARCH_ARM
#    if defined(__ARM_NEON) || defined(__ARM_NEON__)
#      include <arm_neon.h>
#      define XF_HAS_NEON 1
#    endif
#  endif
#endif /* XF_SIMD_DISABLE */

/* ============================================================
 * 1.  CPU feature detection
 * ============================================================ */

static xf_cpu_info_t g_cpu = {0};
static bool          g_init = false;

#ifdef XF_ARCH_X86
static uint32_t x86_detect(void) {
    uint32_t flags = 0;
    uint32_t eax, ebx, ecx, edx;

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (edx & (1u << 26)) flags |= XF_CPU_SSE2;
        if (ecx & (1u << 23)) flags |= XF_CPU_POPCNT;
        if (ecx & (1u << 20)) flags |= XF_CPU_SSE4_2;
    }

    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & (1u << 5)) flags |= XF_CPU_AVX2;
    }

    /* Brand string */
    uint32_t brand[12];
    if (__get_cpuid(0x80000002u,
                    &brand[0],  &brand[1],  &brand[2],  &brand[3])  &&
        __get_cpuid(0x80000003u,
                    &brand[4],  &brand[5],  &brand[6],  &brand[7])  &&
        __get_cpuid(0x80000004u,
                    &brand[8],  &brand[9],  &brand[10], &brand[11])) {
        memcpy(g_cpu.brand, brand, 48);
        g_cpu.brand[48] = '\0';
    } else {
        memcpy(g_cpu.brand, "x86_64", 7);
    }

    return flags;
}
#endif

#ifdef XF_ARCH_ARM
static uint32_t arm_detect(void) {
    uint32_t flags = 0;
#ifdef XF_HAS_NEON
    flags |= XF_CPU_NEON;
#endif
    memcpy(g_cpu.brand, "AArch64/ARM", 12);
    return flags;
}
#endif

void xf_simd_init(void) {
    if (g_init) return;
    g_init = true;

#ifdef XF_ARCH_X86
    g_cpu.flags = x86_detect();
#elif defined(XF_ARCH_ARM)
    g_cpu.flags = arm_detect();
#else
    g_cpu.flags = 0;
    memcpy(g_cpu.brand, "unknown", 8);
#endif
}

const xf_cpu_info_t *xf_cpu_info(void) { return &g_cpu; }
bool xf_cpu_has(uint32_t f)            { return (g_cpu.flags & f) == f; }

/* ============================================================
 * 2.  String operations — scalar fallbacks
 * ============================================================ */

static const char *scalar_find_char(const char *buf, size_t len, char c) {
    return (const char *)memchr(buf, (unsigned char)c, len);
}

static const char *scalar_find_sep_ws(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == ' ' || buf[i] == '\t') return buf + i;
    }
    return NULL;
}

static const char *scalar_skip_ws(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != ' ' && buf[i] != '\t') return buf + i;
    }
    return buf + len;
}

static uint32_t scalar_hash(const char *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

/* ============================================================
 * 2a. String operations — SSE4.2 paths
 * ============================================================ */

#ifdef XF_HAS_SSE4_2

/* _mm_cmpistri with RANGES mode: match any byte in the range set. */
static const char *sse42_find_char(const char *buf, size_t len, char c) {
    /* For single-char search, fall through to optimised memchr which the
     * libc already vectorises well. Use SSE4.2 for the whitespace case. */
    return (const char *)memchr(buf, (unsigned char)c, len);
}

static const char *sse42_find_sep_ws(const char *buf, size_t len) {
    /* Search for ' ' or '\t' using _SIDD_CMP_RANGES. */
    const __m128i ws_range = _mm_set_epi8(
        0,0,0,0, 0,0,0,0, 0,0,0,0, '\t','\t',' ',' ');

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int idx = _mm_cmpistri(ws_range, chunk,
                               _SIDD_UBYTE_OPS |
                               _SIDD_CMP_RANGES |
                               _SIDD_POSITIVE_POLARITY |
                               _SIDD_LEAST_SIGNIFICANT);
        if (idx < 16) return buf + i + idx;
    }
    /* tail */
    return scalar_find_sep_ws(buf + i, len - i);
}

static const char *sse42_skip_ws(const char *buf, size_t len) {
    /* Find first non-whitespace: negate polarity. */
    const __m128i ws_range = _mm_set_epi8(
        0,0,0,0, 0,0,0,0, 0,0,0,0, '\t','\t',' ',' ');

    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(buf + i));
        int idx = _mm_cmpistri(ws_range, chunk,
                               _SIDD_UBYTE_OPS |
                               _SIDD_CMP_RANGES |
                               _SIDD_NEGATIVE_POLARITY |
                               _SIDD_LEAST_SIGNIFICANT);
        if (idx < 16) return buf + i + idx;
    }
    return scalar_skip_ws(buf + i, len - i);
}

#endif /* XF_HAS_SSE4_2 */

/* ============================================================
 * 2b. String hash — SSE2 unrolled FNV-1a
 * ============================================================ */

#ifdef XF_HAS_SSE2
static uint32_t sse2_hash(const char *data, size_t len) {
    uint32_t h = 2166136261u;
    const uint32_t prime = 16777619u;

    size_t i = 0;
    /* Process 8 bytes per iteration unrolled — no gather needed. */
    for (; i + 8 <= len; i += 8) {
        h ^= (unsigned char)data[i+0]; h *= prime;
        h ^= (unsigned char)data[i+1]; h *= prime;
        h ^= (unsigned char)data[i+2]; h *= prime;
        h ^= (unsigned char)data[i+3]; h *= prime;
        h ^= (unsigned char)data[i+4]; h *= prime;
        h ^= (unsigned char)data[i+5]; h *= prime;
        h ^= (unsigned char)data[i+6]; h *= prime;
        h ^= (unsigned char)data[i+7]; h *= prime;
    }
    for (; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= prime;
    }
    return h ? h : 1u;
}
#endif

/* ============================================================
 * 2c. NEON string ops
 * ============================================================ */

#ifdef XF_HAS_NEON

static const char *neon_find_char(const char *buf, size_t len, char c) {
    uint8x16_t vc = vdupq_n_u8((uint8_t)c);
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)(buf + i));
        uint8x16_t eq    = vceqq_u8(chunk, vc);
        /* Reduce: if any lane is 0xFF, there's a match. */
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);
        if (lo | hi) {
            for (int j = 0; j < 16; j++) {
                if (buf[i + j] == c) return buf + i + j;
            }
        }
    }
    return scalar_find_char(buf + i, len - i, c);
}

static const char *neon_find_sep_ws(const char *buf, size_t len) {
    uint8x16_t vsp  = vdupq_n_u8(' ');
    uint8x16_t vtab = vdupq_n_u8('\t');
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)(buf + i));
        uint8x16_t eq = vorrq_u8(vceqq_u8(chunk, vsp), vceqq_u8(chunk, vtab));
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(eq), 1);
        if (lo | hi) {
            for (int j = 0; j < 16; j++) {
                if (buf[i+j] == ' ' || buf[i+j] == '\t') return buf + i + j;
            }
        }
    }
    return scalar_find_sep_ws(buf + i, len - i);
}

static const char *neon_skip_ws(const char *buf, size_t len) {
    uint8x16_t vsp  = vdupq_n_u8(' ');
    uint8x16_t vtab = vdupq_n_u8('\t');
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t chunk = vld1q_u8((const uint8_t *)(buf + i));
        /* non-whitespace = NOT (space OR tab) */
        uint8x16_t ws  = vorrq_u8(vceqq_u8(chunk, vsp), vceqq_u8(chunk, vtab));
        uint8x16_t nws = vmvnq_u8(ws);
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(nws), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(nws), 1);
        if (lo | hi) {
            for (int j = 0; j < 16; j++) {
                if (buf[i+j] != ' ' && buf[i+j] != '\t') return buf + i + j;
            }
        }
    }
    return scalar_skip_ws(buf + i, len - i);
}

static uint32_t neon_hash(const char *data, size_t len) {
    /* Same unrolled FNV — NEON gather would need shuffle tables; just unroll. */
    uint32_t h = 2166136261u;
    const uint32_t prime = 16777619u;
    size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        h ^= (unsigned char)data[i+ 0]; h *= prime;
        h ^= (unsigned char)data[i+ 1]; h *= prime;
        h ^= (unsigned char)data[i+ 2]; h *= prime;
        h ^= (unsigned char)data[i+ 3]; h *= prime;
        h ^= (unsigned char)data[i+ 4]; h *= prime;
        h ^= (unsigned char)data[i+ 5]; h *= prime;
        h ^= (unsigned char)data[i+ 6]; h *= prime;
        h ^= (unsigned char)data[i+ 7]; h *= prime;
        h ^= (unsigned char)data[i+ 8]; h *= prime;
        h ^= (unsigned char)data[i+ 9]; h *= prime;
        h ^= (unsigned char)data[i+10]; h *= prime;
        h ^= (unsigned char)data[i+11]; h *= prime;
        h ^= (unsigned char)data[i+12]; h *= prime;
        h ^= (unsigned char)data[i+13]; h *= prime;
        h ^= (unsigned char)data[i+14]; h *= prime;
        h ^= (unsigned char)data[i+15]; h *= prime;
    }
    for (; i < len; i++) { h ^= (unsigned char)data[i]; h *= prime; }
    return h ? h : 1u;
}

#endif /* XF_HAS_NEON */

/* ============================================================
 * 2d. Runtime dispatch table — string
 * ============================================================ */

typedef const char *(*fn_find_char_t)(const char *, size_t, char);
typedef const char *(*fn_find_ws_t)(const char *, size_t);
typedef const char *(*fn_skip_ws_t)(const char *, size_t);
typedef uint32_t    (*fn_hash_t)(const char *, size_t);

static fn_find_char_t g_find_char = scalar_find_char;
static fn_find_ws_t   g_find_ws   = scalar_find_sep_ws;
static fn_skip_ws_t   g_skip_ws   = scalar_skip_ws;
static fn_hash_t      g_hash      = scalar_hash;

static void dispatch_init_strings(void) {
#ifdef XF_HAS_NEON
    if (g_cpu.flags & XF_CPU_NEON) {
        g_find_char = neon_find_char;
        g_find_ws   = neon_find_sep_ws;
        g_skip_ws   = neon_skip_ws;
        g_hash      = neon_hash;
        return;
    }
#endif
#ifdef XF_HAS_SSE4_2
    if (g_cpu.flags & XF_CPU_SSE4_2) {
        g_find_char = sse42_find_char;
        g_find_ws   = sse42_find_sep_ws;
        g_skip_ws   = sse42_skip_ws;
#ifdef XF_HAS_SSE2
        g_hash      = sse2_hash;
#endif
        return;
    }
#endif
#ifdef XF_HAS_SSE2
    if (g_cpu.flags & XF_CPU_SSE2) {
        g_hash = sse2_hash;
    }
#endif
}

/* Public string API — thin wrappers through dispatch table. */

const char *xf_simd_str_find_char(const char *buf, size_t len, char c) {
    return g_find_char(buf, len, c);
}
const char *xf_simd_str_find_sep_ws(const char *buf, size_t len) {
    return g_find_ws(buf, len);
}
const char *xf_simd_str_skip_ws(const char *buf, size_t len) {
    return g_skip_ws(buf, len);
}
int xf_simd_memcmp(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n);   /* libc already vectorises this */
}
uint32_t xf_simd_str_hash(const char *data, size_t len) {
    return g_hash(data, len);
}

/* ============================================================
 * 3.  Record / field splitting
 * ============================================================ */

size_t xf_simd_trim_newline(const char *buf, size_t len) {
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        len--;
    return len;
}

size_t xf_simd_split_ws(char *buf, size_t len,
                         char **fields, size_t max_fields) {
    size_t fc = 0;
    char *p   = buf;
    char *end = buf + len;

    while (p < end && fc < max_fields) {
        /* skip whitespace */
        size_t rem = (size_t)(end - p);
        const char *ns = g_skip_ws(p, rem);
        if (!ns || ns >= end) break;
        p = (char *)ns;

        fields[fc++] = p;

        /* find end of token */
        rem = (size_t)(end - p);
        const char *ws = g_find_ws(p, rem);
        if (!ws) {
            /* last field — no trailing separator */
            break;
        }
        /* NUL-terminate and advance */
        *(char *)ws = '\0';
        p = (char *)ws + 1;
    }

    return fc;
}

size_t xf_simd_split_sep(char *buf, size_t len, char sep,
                          char **fields, size_t max_fields) {
    if (max_fields == 0) return 0;

    size_t fc  = 0;
    char  *p   = buf;
    char  *end = buf + len;

    fields[fc++] = p;

    while (p < end && fc < max_fields) {
        size_t rem = (size_t)(end - p);
        const char *found = g_find_char(p, rem, sep);
        if (!found) break;
        *(char *)found = '\0';
        p = (char *)found + 1;
        fields[fc++] = p;
    }

    return fc;
}

/* ============================================================
 * 4a. Bulk numeric — scalar fallbacks
 * ============================================================ */

static double scalar_sum(const double *xs, size_t n) {
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) acc += xs[i];
    return acc;
}
static double scalar_min(const double *xs, size_t n) {
    if (n == 0) return 0.0;
    double m = xs[0];
    for (size_t i = 1; i < n; i++) if (xs[i] < m) m = xs[i];
    return m;
}
static double scalar_max(const double *xs, size_t n) {
    if (n == 0) return 0.0;
    double m = xs[0];
    for (size_t i = 1; i < n; i++) if (xs[i] > m) m = xs[i];
    return m;
}
static void scalar_scale(double *xs, size_t n, double f) {
    for (size_t i = 0; i < n; i++) xs[i] *= f;
}
static void scalar_add_scalar(double *xs, size_t n, double v) {
    for (size_t i = 0; i < n; i++) xs[i] += v;
}

/* ============================================================
 * 4b. Bulk numeric — AVX2 paths
 * ============================================================ */

#ifdef XF_HAS_AVX2

static double avx2_sum(const double *xs, size_t n) {
    __m256d acc = _mm256_setzero_pd();
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(xs + i);
        acc = _mm256_add_pd(acc, v);
    }
    /* horizontal add */
    __m128d lo  = _mm256_castpd256_pd128(acc);
    __m128d hi  = _mm256_extractf128_pd(acc, 1);
    __m128d sum = _mm_add_pd(lo, hi);
    sum = _mm_hadd_pd(sum, sum);
    double result = _mm_cvtsd_f64(sum);
    for (; i < n; i++) result += xs[i];
    return result;
}

static double avx2_min(const double *xs, size_t n) {
    if (n == 0) return 0.0;
    __m256d vmin = _mm256_set1_pd(DBL_MAX);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(xs + i);
        vmin = _mm256_min_pd(vmin, v);
    }
    double lanes[4];
    _mm256_storeu_pd(lanes, vmin);
    double m = lanes[0];
    for (int j = 1; j < 4; j++) if (lanes[j] < m) m = lanes[j];
    for (; i < n; i++) if (xs[i] < m) m = xs[i];
    return m;
}

static double avx2_max(const double *xs, size_t n) {
    if (n == 0) return 0.0;
    __m256d vmax = _mm256_set1_pd(-DBL_MAX);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(xs + i);
        vmax = _mm256_max_pd(vmax, v);
    }
    double lanes[4];
    _mm256_storeu_pd(lanes, vmax);
    double m = lanes[0];
    for (int j = 1; j < 4; j++) if (lanes[j] > m) m = lanes[j];
    for (; i < n; i++) if (xs[i] > m) m = xs[i];
    return m;
}

static void avx2_scale(double *xs, size_t n, double f) {
    __m256d vf = _mm256_set1_pd(f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d v = _mm256_loadu_pd(xs + i);
        _mm256_storeu_pd(xs + i, _mm256_mul_pd(v, vf));
    }
    for (; i < n; i++) xs[i] *= f;
}

static void avx2_add_scalar(double *xs, size_t n, double v) {
    __m256d vv = _mm256_set1_pd(v);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d chunk = _mm256_loadu_pd(xs + i);
        _mm256_storeu_pd(xs + i, _mm256_add_pd(chunk, vv));
    }
    for (; i < n; i++) xs[i] += v;
}

#endif /* XF_HAS_AVX2 */

/* ============================================================
 * 4c. Bulk numeric — NEON paths
 * ============================================================ */

#ifdef XF_HAS_NEON

static double neon_sum(const double *xs, size_t n) {
    float64x2_t acc = vdupq_n_f64(0.0);
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        float64x2_t v = vld1q_f64(xs + i);
        acc = vaddq_f64(acc, v);
    }
    double result = vgetq_lane_f64(acc, 0) + vgetq_lane_f64(acc, 1);
    for (; i < n; i++) result += xs[i];
    return result;
}

static double neon_min(const double *xs, size_t n) {
    if (n == 0) return 0.0;
    float64x2_t vmin = vdupq_n_f64(DBL_MAX);
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        vmin = vminq_f64(vmin, vld1q_f64(xs + i));
    }
    double m = fmin(vgetq_lane_f64(vmin, 0), vgetq_lane_f64(vmin, 1));
    for (; i < n; i++) if (xs[i] < m) m = xs[i];
    return m;
}

static double neon_max(const double *xs, size_t n) {
    if (n == 0) return 0.0;
    float64x2_t vmax = vdupq_n_f64(-DBL_MAX);
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        vmax = vmaxq_f64(vmax, vld1q_f64(xs + i));
    }
    double m = fmax(vgetq_lane_f64(vmax, 0), vgetq_lane_f64(vmax, 1));
    for (; i < n; i++) if (xs[i] > m) m = xs[i];
    return m;
}

static void neon_scale(double *xs, size_t n, double f) {
    float64x2_t vf = vdupq_n_f64(f);
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        vst1q_f64(xs + i, vmulq_f64(vld1q_f64(xs + i), vf));
    }
    for (; i < n; i++) xs[i] *= f;
}

static void neon_add_scalar(double *xs, size_t n, double v) {
    float64x2_t vv = vdupq_n_f64(v);
    size_t i = 0;
    for (; i + 2 <= n; i += 2) {
        vst1q_f64(xs + i, vaddq_f64(vld1q_f64(xs + i), vv));
    }
    for (; i < n; i++) xs[i] += v;
}

#endif /* XF_HAS_NEON */

/* ============================================================
 * 4d. Runtime dispatch table — numerics
 * ============================================================ */

typedef double (*fn_sum_t)  (const double *, size_t);
typedef double (*fn_min_t)  (const double *, size_t);
typedef double (*fn_max_t)  (const double *, size_t);
typedef void   (*fn_scale_t)(double *, size_t, double);
typedef void   (*fn_addv_t) (double *, size_t, double);

static fn_sum_t   g_sum   = scalar_sum;
static fn_min_t   g_min   = scalar_min;
static fn_max_t   g_max   = scalar_max;
static fn_scale_t g_scale = scalar_scale;
static fn_addv_t  g_addv  = scalar_add_scalar;

static void dispatch_init_numerics(void) {
#ifdef XF_HAS_NEON
    if (g_cpu.flags & XF_CPU_NEON) {
        g_sum   = neon_sum;
        g_min   = neon_min;
        g_max   = neon_max;
        g_scale = neon_scale;
        g_addv  = neon_add_scalar;
        return;
    }
#endif
#ifdef XF_HAS_AVX2
    if (g_cpu.flags & XF_CPU_AVX2) {
        g_sum   = avx2_sum;
        g_min   = avx2_min;
        g_max   = avx2_max;
        g_scale = avx2_scale;
        g_addv  = avx2_add_scalar;
        return;
    }
#endif
}

double xf_simd_sum  (const double *xs, size_t n) { return g_sum(xs, n); }
double xf_simd_min  (const double *xs, size_t n) { return g_min(xs, n); }
double xf_simd_max  (const double *xs, size_t n) { return g_max(xs, n); }
double xf_simd_mean (const double *xs, size_t n) {
    return n ? g_sum(xs, n) / (double)n : 0.0;
}
void xf_simd_scale      (double *xs, size_t n, double f) { g_scale(xs, n, f); }
void xf_simd_add_scalar (double *xs, size_t n, double v) { g_addv(xs, n, v); }

/* ============================================================
 * 5.  Worker / memory helpers
 * ============================================================ */

void xf_simd_memcpy_nt(void *dst, const void *src, size_t n) {
    if (n < XF_SIMD_NT_THRESHOLD) {
        memcpy(dst, src, n);
        return;
    }

#ifdef XF_HAS_AVX2
    if (g_cpu.flags & XF_CPU_AVX2) {
        const char *s = (const char *)src;
        char       *d = (char *)dst;
        size_t i = 0;
        for (; i + 32 <= n; i += 32) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
            _mm256_stream_si256((__m256i *)(d + i), v);
        }
        _mm_sfence();
        if (i < n) memcpy(d + i, s + i, n - i);
        return;
    }
#endif

#ifdef XF_HAS_SSE2
    if (g_cpu.flags & XF_CPU_SSE2) {
        const char *s = (const char *)src;
        char       *d = (char *)dst;
        size_t i = 0;
        for (; i + 16 <= n; i += 16) {
            __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
            _mm_stream_si128((__m128i *)(d + i), v);
        }
        _mm_sfence();
        if (i < n) memcpy(d + i, s + i, n - i);
        return;
    }
#endif

    memcpy(dst, src, n);
}

/* ============================================================
 * Final dispatch initialisation — called at end of xf_simd_init
 * ============================================================ */

/* Re-open xf_simd_init to wire dispatch after feature detection. */
/* We use a constructor attribute so init happens before main on GCC/Clang. */

static void __attribute__((constructor)) simd_auto_init(void) {
    xf_simd_init();
    dispatch_init_strings();
    dispatch_init_numerics();
}