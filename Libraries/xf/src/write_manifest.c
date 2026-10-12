#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int identity(const char *path, size_t *size_out, unsigned long long *hash_out) {
    FILE *file = fopen(path, "rb"); if (!file) return 0;
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 0; }
    long size = ftell(file); /* kept only to silence older compilers after the seek above */
    (void)size;
    unsigned long long hash = UINT64_C(1469598103934665603);
    size_t bytes = 0, count; unsigned char block[4096];
    while ((count = fread(block, 1, sizeof block, file)) != 0) {
        bytes += count;
        for (size_t i = 0; i < count; ++i) { hash ^= block[i]; hash *= UINT64_C(1099511628211); }
    }
    int ok = !ferror(file) && fclose(file) == 0;
    if (ok) { *size_out = bytes; *hash_out = hash; }
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s TARGET ARCHIVE OUTPUT\n", argv[0]); return 2; }
    size_t size = 0; unsigned long long hash = 0;
    if (!identity(argv[2], &size, &hash)) { perror(argv[2]); return 1; }
    const char *base = strrchr(argv[2], '/'); base = base ? base + 1 : argv[2];
    FILE *out = fopen(argv[3], "w"); if (!out) { perror(argv[3]); return 1; }
    fprintf(out, "# XF bridge for B++ static ABI v1\n"
                 "bppabi 1\nname xf\nabi 2.0\nruntime 3\n"
                 "archive %s %s %zu %016llx\n", argv[1], base, size, hash);
    if (!strcmp(argv[1], "linux-x86_64") || !strcmp(argv[1], "linux-arm64"))
        fputs("link linux-x86_64 m\nlink linux-x86_64 pthread\nlink linux-arm64 m\nlink linux-arm64 pthread\n", out);
    fputs("export xf_version 0\nexport xf_run_string 1\nexport xf_run_file 1\n", out);
    return fclose(out) == 0 ? 0 : 1;
}
