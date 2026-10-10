#include "../include/compiler.h"
#include "../include/parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} BLPathList;

typedef struct {
    BLPathList active_paths;
    BLPathList loaded_paths;
    BLPathList active_library_manifests;
    BLPathList loaded_library_manifests;
    BLIRModule *ir;
    const BLCompiler *compiler;
    char *error;
} BLImportState;

typedef struct {
    char *data;
    size_t length;
    size_t cap;
} BLStringBuilder;

static char *bl_strdup_local(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)calloc(length + 1, 1);
    if (!copy) {
        perror("calloc");
        exit(1);
    }
    memcpy(copy, text, length + 1);
    return copy;
}

static char *bl_strndup_local(const char *text, size_t length) {
    char *copy = (char *)calloc(length + 1, 1);
    if (!copy) {
        perror("calloc");
        exit(1);
    }
    memcpy(copy, text, length);
    return copy;
}

static char *bl_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    char *buffer = (char *)calloc((size_t)size + 1, 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }

    if (size > 0 && fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return NULL;
    }

    fclose(file);
    return buffer;
}

static void bl_set_error(BLCompiler *interp, const char *message) {
    if (!interp) return;
    free(interp->last_error);
    interp->last_error = NULL;
    if (!message) return;
    interp->last_error = bl_strdup_local(message);
}

static void bl_path_list_push_owned(BLPathList *list, char *item) {
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        char **items = (char **)realloc(list->items, new_cap * sizeof(char *));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        list->items = items;
        list->cap = new_cap;
    }
    list->items[list->count++] = item;
}

static int bl_path_list_contains(const BLPathList *list, const char *item) {
    for (size_t i = 0; i < list->count; ++i) {
#ifdef _WIN32
        if (_stricmp(list->items[i], item) == 0) return 1;
#else
        if (strcmp(list->items[i], item) == 0) return 1;
#endif
    }
    return 0;
}

static void bl_path_list_pop(BLPathList *list) {
    if (list->count == 0) return;
    free(list->items[list->count - 1]);
    list->items[list->count - 1] = NULL;
    list->count -= 1;
}

static void bl_path_list_dispose(BLPathList *list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static void bl_import_state_set_errorf(BLImportState *state, const char *path, const char *detail) {
    if (state->error) return;
    size_t size = strlen(path ? path : "<input>") + strlen(detail ? detail : "import error") + 4;
    state->error = (char *)calloc(size, 1);
    if (!state->error) {
        perror("calloc");
        exit(1);
    }
    snprintf(state->error, size, "%s: %s", path ? path : "<input>", detail ? detail : "import error");
}

static void bl_import_state_dispose(BLImportState *state) {
    bl_path_list_dispose(&state->active_paths);
    bl_path_list_dispose(&state->loaded_paths);
    bl_path_list_dispose(&state->active_library_manifests);
    bl_path_list_dispose(&state->loaded_library_manifests);
    free(state->error);
    memset(state, 0, sizeof(*state));
}

static int bl_identifier(const char *name) {
    if (!name || !(*name == '_' || isalpha((unsigned char)*name))) return 0;
    for (++name; *name; ++name) if (!(*name == '_' || isalnum((unsigned char)*name))) return 0;
    return 1;
}

static void bl_static_push_library(BLIRModule *ir, const char *name, const char *manifest, const char *archive,
                                   size_t archive_size, unsigned long long archive_fingerprint) {
    if (ir->static_library_count == ir->static_library_capacity) {
        size_t cap = ir->static_library_capacity ? ir->static_library_capacity * 2 : 4;
        BPPStaticLibraryImport *items = realloc(ir->static_libraries, cap * sizeof *items);
        if (!items) { perror("realloc"); exit(1); }
        ir->static_libraries = items; ir->static_library_capacity = cap;
    }
    BPPStaticLibraryImport *item = &ir->static_libraries[ir->static_library_count++];
    item->name = bl_strdup_local(name); item->manifest_path = bl_strdup_local(manifest); item->archive_path = bl_strdup_local(archive);
    item->archive_size = archive_size; item->archive_fingerprint = archive_fingerprint;
}

static int bl_static_has_library(const BLIRModule *ir, const char *manifest) {
    for (size_t i = 0; i < ir->static_library_count; ++i)
        if (!strcmp(ir->static_libraries[i].manifest_path, manifest)) return 1;
    return 0;
}
static int bl_static_has_library_name(const BLIRModule *ir, const char *name) {
    for (size_t i = 0; i < ir->static_library_count; ++i)
        if (!strcmp(ir->static_libraries[i].name, name)) return 1;
    return 0;
}

static int bl_static_has_procedure(const BLIRModule *ir, const char *name) {
    return bl_ir_static_procedure_id(ir, name) != BL_IR_NONE;
}

static void bl_static_push_procedure(BLIRModule *ir, const char *name, size_t arity, size_t library_index) {
    if (ir->static_procedure_count == ir->static_procedure_capacity) {
        size_t cap = ir->static_procedure_capacity ? ir->static_procedure_capacity * 2 : 8;
        BPPStaticProcedureImport *items = realloc(ir->static_procedures, cap * sizeof *items);
        if (!items) { perror("realloc"); exit(1); }
        ir->static_procedures = items; ir->static_procedure_capacity = cap;
    }
    size_t symbol_size = strlen("bpp_v1_") + strlen(name) + 1;
    BPPStaticProcedureImport *item = &ir->static_procedures[ir->static_procedure_count++];
    item->name = bl_strdup_local(name); item->symbol = calloc(symbol_size, 1);
    if (!item->symbol) { perror("calloc"); exit(1); }
    snprintf(item->symbol, symbol_size, "bpp_v1_%s", name);
    item->arity = arity; item->library_index = library_index;
}

/* Manifests name a closed set of system requirements rather than accepting
 * arbitrary host-driver flags.  That keeps static library linking explicit
 * without turning the B++ library mechanism into general FFI. */
static const char *bl_static_system_link_flag(const char *name) {
    if (!strcmp(name, "m")) return "-lm";
    if (!strcmp(name, "pthread")) return "-pthread";
    if (!strcmp(name, "dl")) return "-ldl";
    return NULL;
}
static void bl_static_push_link_flag(BLIRModule *ir, const char *name) {
    const char *flag = bl_static_system_link_flag(name);
    if (!flag) return;
    for (size_t i = 0; i < ir->static_link_flag_count; ++i)
        if (!strcmp(ir->static_link_flags[i], flag)) return;
    if (ir->static_link_flag_count == ir->static_link_flag_capacity) {
        size_t cap = ir->static_link_flag_capacity ? ir->static_link_flag_capacity * 2 : 4;
        char **items = realloc(ir->static_link_flags, cap * sizeof *items);
        if (!items) { perror("realloc"); exit(1); }
        ir->static_link_flags = items; ir->static_link_flag_capacity = cap;
    }
    ir->static_link_flags[ir->static_link_flag_count++] = bl_strdup_local(flag);
}

static void bl_sb_reserve(BLStringBuilder *sb, size_t extra) {
    size_t needed = sb->length + extra + 1;
    if (needed <= sb->cap) return;
    size_t new_cap = sb->cap ? sb->cap * 2 : 256;
    while (new_cap < needed) new_cap *= 2;
    char *data = (char *)realloc(sb->data, new_cap);
    if (!data) {
        perror("realloc");
        exit(1);
    }
    sb->data = data;
    sb->cap = new_cap;
}

static void bl_sb_append_n(BLStringBuilder *sb, const char *text, size_t length) {
    if (!text || length == 0) return;
    bl_sb_reserve(sb, length);
    memcpy(sb->data + sb->length, text, length);
    sb->length += length;
    sb->data[sb->length] = '\0';
}

static void bl_sb_append(BLStringBuilder *sb, const char *text) {
    bl_sb_append_n(sb, text, text ? strlen(text) : 0);
}

static char *bl_sb_take(BLStringBuilder *sb) {
    if (!sb->data) return bl_strdup_local("");
    char *data = sb->data;
    sb->data = NULL;
    sb->length = 0;
    sb->cap = 0;
    return data;
}

static void bl_sb_dispose(BLStringBuilder *sb) {
    free(sb->data);
    memset(sb, 0, sizeof(*sb));
}

static int bl_path_is_absolute(const char *path) {
    if (!path || !path[0]) return 0;
    return path[0]=='/'||path[0]=='\\'||(isalpha((unsigned char)path[0])&&path[1]==':');
}

static char *bl_path_dirname(const char *path) {
    if (!path || !path[0]) return bl_strdup_local(".");
    const char *slash = strrchr(path, '/');
    if (!slash) return bl_strdup_local(".");
    if (slash == path) return bl_strdup_local("/");
    return bl_strndup_local(path, (size_t)(slash - path));
}

static char *bl_path_join2(const char *left, const char *right) {
    if (!left || !left[0]) return bl_strdup_local(right ? right : "");
    if (!right || !right[0]) return bl_strdup_local(left);
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    int need_sep = left[left_len - 1] != '/';
    char *out = (char *)calloc(left_len + right_len + (need_sep ? 2 : 1), 1);
    if (!out) {
        perror("calloc");
        exit(1);
    }
    memcpy(out, left, left_len);
    if (need_sep) out[left_len++] = '/';
    memcpy(out + left_len, right, right_len);
    return out;
}

static char *bl_path_canonicalize(const char *path) {
#ifdef _WIN32
    char resolved[4096];if(!_fullpath(resolved,path,sizeof resolved))return NULL;
    for(char *p=resolved;*p;++p)if(*p=='\\')*p='/';
    return bl_strdup_local(resolved);
#endif

    if (!path || !path[0]) return NULL;

    char *absolute = NULL;
    if (bl_path_is_absolute(path)) {
        absolute = bl_strdup_local(path);
    } else {
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) {
            return bl_strdup_local(path);
        }
        absolute = bl_path_join2(cwd, path);
    }

    char **parts = NULL;
    size_t part_count = 0;
    size_t part_cap = 0;
    size_t i = 0;

    while (absolute[i]) {
        while (absolute[i] == '/') i += 1;
        size_t start = i;
        while (absolute[i] && absolute[i] != '/') i += 1;
        size_t length = i - start;
        if (length == 0) break;

        if (length == 1 && absolute[start] == '.') {
            continue;
        }

        if (length == 2 && absolute[start] == '.' && absolute[start + 1] == '.') {
            if (part_count > 0) {
                free(parts[part_count - 1]);
                part_count -= 1;
            }
            continue;
        }

        if (part_count == part_cap) {
            size_t new_cap = part_cap ? part_cap * 2 : 8;
            char **items = (char **)realloc(parts, new_cap * sizeof(char *));
            if (!items) {
                perror("realloc");
                exit(1);
            }
            parts = items;
            part_cap = new_cap;
        }
        parts[part_count++] = bl_strndup_local(absolute + start, length);
    }

    BLStringBuilder out = {0};
    bl_sb_append(&out, "/");
    for (size_t idx = 0; idx < part_count; ++idx) {
        if (idx > 0) bl_sb_append(&out, "/");
        bl_sb_append(&out, parts[idx]);
        free(parts[idx]);
    }

    free(parts);
    free(absolute);

    return bl_sb_take(&out);
}

static char *bl_global_lib_dir(void) {
    const char *home = getenv("HOME");
#ifdef _WIN32
    if(!home)home=getenv("USERPROFILE");
#endif
    if (home && home[0]) {
        char *base = bl_path_join2(home, ".bytelang");
        char *lib = bl_path_join2(base, "lib");
        free(base);
        return lib;
    }
    return bl_strdup_local("./.bytelang/lib");
}

static char *bl_resolve_local_import_path(const char *current_path, const char *import_path) {
    if (bl_path_is_absolute(import_path)) {
        return bl_path_canonicalize(import_path);
    }

    char *dir = bl_path_dirname(current_path);
    char *joined = bl_path_join2(dir, import_path);
    char *canonical = bl_path_canonicalize(joined);
    free(dir);
    free(joined);
    return canonical;
}

static int bl_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

/* FNV-1a identifies the exact archive selected by a manifest. It is not a
 * cryptographic signature; package provenance belongs to buildFile work. */
static int bl_archive_identity(const char *path, size_t *size_out, unsigned long long *fingerprint_out) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    unsigned long long hash = UINT64_C(1469598103934665603);
    unsigned char bytes[4096]; size_t count;
    while ((count = fread(bytes, 1, sizeof bytes, file)) != 0)
        for (size_t i = 0; i < count; ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
    int ok = !ferror(file) && fclose(file) == 0;
    if (!ok) return 0;
    if (size_out) *size_out = (size_t)size;
    if (fingerprint_out) *fingerprint_out = hash;
    return 1;
}

static char *bl_resolve_global_import_path(const char *import_name) {
    char *lib_dir = bl_global_lib_dir();
    char *exact = bl_path_join2(lib_dir, import_name);
    if (bl_file_exists(exact)) {
        free(lib_dir);
        return exact;
    }

    size_t name_len = strlen(import_name);
    char *with_ext = (char *)calloc(name_len + 4, 1);
    if (!with_ext) {
        perror("calloc");
        exit(1);
    }
    memcpy(with_ext, import_name, name_len);
    memcpy(with_ext + name_len, ".bl", 4);

    char *candidate = bl_path_join2(lib_dir, with_ext);
    free(with_ext);
    free(exact);
    free(lib_dir);

    if (bl_file_exists(candidate)) {
        return candidate;
    }

    free(candidate);
    return NULL;
}

static char *bl_static_manifest_candidate(const char *directory, const char *name, int prefixed) {
    size_t n = strlen(name);
    size_t bytes = n + (prefixed ? 4 : 1) + strlen(".bppabi") + 1;
    char *file = calloc(bytes, 1);
    if (!file) { perror("calloc"); exit(1); }
    snprintf(file, bytes, prefixed ? "lib%s.bppabi" : "%s.bppabi", name);
    char *candidate = bl_path_join2(directory, file);
    free(file);
    if (bl_file_exists(candidate)) return candidate;
    free(candidate); return NULL;
}

static char *bl_resolve_static_manifest(const BLImportState *state, const char *current_path, const char *name) {
    char *directory = bl_path_dirname(current_path);
    char *found = bl_static_manifest_candidate(directory, name, 1);
    if (!found) found = bl_static_manifest_candidate(directory, name, 0);
    free(directory);
    for (size_t i = 0; !found && state->compiler && i < state->compiler->library_path_count; ++i) {
        const char *path = state->compiler->library_paths[i];
        if (!path || !path[0]) continue;
        found = bl_static_manifest_candidate(path, name, 1);
        if (!found) found = bl_static_manifest_candidate(path, name, 0);
    }
    if (!found) {
        char *global = bl_global_lib_dir();
        found = bl_static_manifest_candidate(global, name, 1);
        if (!found) found = bl_static_manifest_candidate(global, name, 0);
        free(global);
    }
    if (!found) return NULL;
    char *canonical = bl_path_canonicalize(found); free(found); return canonical;
}

typedef struct { char *name; size_t arity; } BLStaticExport;
typedef struct { char **items; size_t count, cap; } BLNameList;

static void bl_name_list_push(BLNameList *list, const char *name) {
    if (list->count == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        char **items = realloc(list->items, cap * sizeof *items);
        if (!items) { perror("realloc"); exit(1); }
        list->items = items; list->cap = cap;
    }
    list->items[list->count++] = bl_strdup_local(name);
}
static void bl_name_list_dispose(BLNameList *list) { for (size_t i = 0; i < list->count; ++i) free(list->items[i]); free(list->items); memset(list, 0, sizeof *list); }

static int bl_static_manifest_error(BLImportState *state, const char *manifest, int line, const char *detail) {
    char message[1024];
    snprintf(message, sizeof message, "static library manifest %.300s:%d: %.600s", manifest, line, detail);
    bl_import_state_set_errorf(state, manifest, message); return 0;
}

static int bl_import_static_manifest(BLImportState *state, const char *manifest, const char *requested_name);

static int bl_import_static_manifest(BLImportState *state, const char *manifest, const char *requested_name) {
    if (state->error) return 0;
    if (bl_path_list_contains(&state->active_library_manifests, manifest)) {
        char message[1024]; snprintf(message, sizeof message, "cyclic static-library dependency: %s", manifest);
        bl_import_state_set_errorf(state, manifest, message); return 0;
    }
    if (bl_path_list_contains(&state->loaded_library_manifests, manifest)) return 1;
    char *source = bl_read_file(manifest);
    if (!source) { bl_import_state_set_errorf(state, manifest, "failed to read static library manifest"); return 0; }
    char *name = NULL, *archive = NULL; unsigned abi_major = 0, abi_minor = 0, runtime_revision = 0;
    size_t expected_archive_size = 0; unsigned long long expected_archive_fingerprint = 0; int archive_selected = 0;
    int header = 0, line_number = 0, ok = 1;
    BLStaticExport *exports = NULL; size_t export_count = 0, export_cap = 0;
    BLNameList dependencies = {0};
    char *cursor = source;
    while (ok && *cursor) {
        char *line = cursor; while (*cursor && *cursor != '\n') ++cursor;
        if (*cursor == '\n') *cursor++ = '\0';
        ++line_number;
        while (isspace((unsigned char)*line)) ++line;
        char *end = line + strlen(line); while (end > line && isspace((unsigned char)end[-1])) *--end = '\0';
        if (!line[0] || line[0] == '#') continue;
        char key[32] = {0}, first[256] = {0}, second[4096] = {0}, third[64] = {0}, fourth[64] = {0};
        int fields = sscanf(line, "%31s %255s %4095s %63s %63s", key, first, second, third, fourth);
        if (!strcmp(key, "bppabi")) {
            unsigned version = 0;
            if (fields != 2 || sscanf(first, "%u", &version) != 1 || version != 1 || header) ok = bl_static_manifest_error(state, manifest, line_number, "expected exactly 'bppabi 1'");
            else header = 1;
        } else if (!strcmp(key, "name")) {
            if (fields != 2 || !bl_identifier(first) || name) ok = bl_static_manifest_error(state, manifest, line_number, "invalid or duplicate library name");
            else name = bl_strdup_local(first);
        } else if (!strcmp(key, "abi")) {
            if (fields != 2 || sscanf(first, "%u.%u", &abi_major, &abi_minor) != 2) ok = bl_static_manifest_error(state, manifest, line_number, "expected ABI version such as 'abi 1.0'");
        } else if (!strcmp(key, "runtime")) {
            if (fields != 2 || sscanf(first, "%u", &runtime_revision) != 1) ok = bl_static_manifest_error(state, manifest, line_number, "expected runtime revision");
        } else if (!strcmp(key, "archive")) {
            BPPTarget target;
            unsigned long long byte_count = 0, fingerprint = 0; char extra = 0;
            if (fields != 5 || !bpp_target_parse(first, &target) || !second[0] ||
                sscanf(third, "%llu%c", &byte_count, &extra) != 1 || byte_count > SIZE_MAX ||
                sscanf(fourth, "%llx%c", &fingerprint, &extra) != 1)
                ok = bl_static_manifest_error(state, manifest, line_number, "expected 'archive TARGET relative-path bytes fnv1a64-hex'");
            else if (state->compiler && target == state->compiler->target) {
                if (archive_selected) ok = bl_static_manifest_error(state, manifest, line_number, "duplicate archive entry for selected target");
                else { archive = bl_strdup_local(second); expected_archive_size = (size_t)byte_count; expected_archive_fingerprint = fingerprint; archive_selected = 1; }
            }
        } else if (!strcmp(key, "export")) {
            unsigned long arity = 0; char extra = 0;
            if (fields != 3 || !bl_identifier(first) || sscanf(second, "%lu%c", &arity, &extra) != 1 || arity > SIZE_MAX) ok = bl_static_manifest_error(state, manifest, line_number, "expected 'export identifier arity'");
            else {
                for (size_t i = 0; i < export_count; ++i) if (!strcmp(exports[i].name, first)) ok = bl_static_manifest_error(state, manifest, line_number, "duplicate exported procedure");
                if (!ok) break;
                if (export_count == export_cap) { size_t cap = export_cap ? export_cap * 2 : 4; BLStaticExport *items = realloc(exports, cap * sizeof *items); if (!items) { perror("realloc"); exit(1); } exports = items; export_cap = cap; }
                exports[export_count].name = bl_strdup_local(first); exports[export_count++].arity = (size_t)arity;
            }
        } else if (!strcmp(key, "dependency")) {
            if (fields != 2 || !bl_identifier(first)) ok = bl_static_manifest_error(state, manifest, line_number, "expected 'dependency library-name'");
            else bl_name_list_push(&dependencies, first);
        } else if (!strcmp(key, "link")) {
            BPPTarget target;
            if (fields != 3 || !bpp_target_parse(first, &target) || !bl_static_system_link_flag(second))
                ok = bl_static_manifest_error(state, manifest, line_number, "expected 'link TARGET m|pthread|dl'");
            else if (state->compiler && target == state->compiler->target)
                bl_static_push_link_flag(state->ir, second);
        } else ok = bl_static_manifest_error(state, manifest, line_number, "unknown manifest directive");
    }
    if (ok && (!header || !name || !archive_selected || abi_major != 1 || abi_minor > 0 || runtime_revision != 1))
        ok = bl_static_manifest_error(state, manifest, line_number ? line_number : 1, "manifest must declare bppabi 1, name, ABI 1.0, runtime 1, and an archive for this target");
    if (ok && requested_name && strcmp(name, requested_name)) ok = bl_static_manifest_error(state, manifest, 1, "manifest name does not match requested library");
    char *archive_path = NULL;
    size_t actual_archive_size = 0; unsigned long long actual_archive_fingerprint = 0;
    if (ok) {
        char *dir = bl_path_dirname(manifest); archive_path = bl_path_join2(dir, archive); free(dir);
        if (!bl_archive_identity(archive_path, &actual_archive_size, &actual_archive_fingerprint)) {
            char detail[1024]; snprintf(detail, sizeof detail, "archive '%s' does not exist or cannot be read", archive_path); ok = bl_static_manifest_error(state, manifest, line_number, detail);
        } else if (actual_archive_size != expected_archive_size || actual_archive_fingerprint != expected_archive_fingerprint) {
            ok = bl_static_manifest_error(state, manifest, line_number, "archive identity mismatch; regenerate the manifest after rebuilding the archive");
        }
    }
    int active_pushed = 0;
    if (ok) { bl_path_list_push_owned(&state->active_library_manifests, bl_strdup_local(manifest)); active_pushed = 1; }
    for (size_t i = 0; ok && i < dependencies.count; ++i) {
        char *dependency_manifest = bl_resolve_static_manifest(state, manifest, dependencies.items[i]);
        if (!dependency_manifest) { char detail[512]; snprintf(detail, sizeof detail, "failed to resolve dependency '%s'", dependencies.items[i]); ok = bl_static_manifest_error(state, manifest, line_number, detail); }
        else { ok = bl_import_static_manifest(state, dependency_manifest, dependencies.items[i]); free(dependency_manifest); }
    }
    if (ok && !bl_static_has_library(state->ir, manifest)) {
        size_t library_index = state->ir->static_library_count;
        for (size_t i = 0; i < export_count; ++i) if (bl_static_has_procedure(state->ir, exports[i].name)) { char detail[512]; snprintf(detail, sizeof detail, "exported procedure '%s' conflicts with another imported static library", exports[i].name); ok = bl_static_manifest_error(state, manifest, line_number, detail); break; }
        if (ok && bl_static_has_library_name(state->ir, name)) ok = bl_static_manifest_error(state, manifest, line_number, "library name conflicts with another imported manifest");
        if (ok) { bl_static_push_library(state->ir, name, manifest, archive_path, actual_archive_size, actual_archive_fingerprint); for (size_t i = 0; i < export_count; ++i) bl_static_push_procedure(state->ir, exports[i].name, exports[i].arity, library_index); }
    }
    if (active_pushed) bl_path_list_pop(&state->active_library_manifests);
    if (ok) bl_path_list_push_owned(&state->loaded_library_manifests, bl_strdup_local(manifest));
    for (size_t i = 0; i < export_count; ++i) free(exports[i].name);
    free(exports); bl_name_list_dispose(&dependencies); free(name); free(archive); free(archive_path); free(source);
    return ok;
}

static char *bl_parse_static_import_target(const char *line_start, size_t line_length) {
    const char *cursor = line_start, *end = line_start + line_length;
    while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    if ((size_t)(end - cursor) < 14 || memcmp(cursor, "import library", 14) != 0 || (cursor + 14 < end && !isspace((unsigned char)cursor[14]))) return NULL;
    cursor += 14; while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    if (cursor == end || *cursor++ != '"') return NULL;
    const char *start = cursor; while (cursor < end && *cursor != '"') ++cursor;
    if (cursor == end) return NULL;
    char *name = bl_strndup_local(start, (size_t)(cursor - start)); ++cursor;
    while (cursor < end && isspace((unsigned char)*cursor)) ++cursor;
    if (cursor < end && *cursor == ';') { ++cursor; while (cursor < end && isspace((unsigned char)*cursor)) ++cursor; }
    if (cursor < end && !(cursor + 1 < end && cursor[0] == '/' && cursor[1] == '/')) { free(name); return NULL; }
    if (!bl_identifier(name)) { free(name); return NULL; }
    return name;
}

static char *bl_parse_import_target(const char *path, int line_number, const char *line_start, size_t line_length, int *is_global) {
    const char *cursor = line_start;
    const char *line_end = line_start + line_length;

    while (cursor < line_end && isspace((unsigned char)*cursor)) cursor += 1;
    if ((size_t)(line_end - cursor) < 6 || memcmp(cursor, "import", 6) != 0) return NULL;
    if ((cursor + 6) < line_end) {
        unsigned char next = (unsigned char)cursor[6];
        if (!isspace(next) && next != '"' && next != '{') return NULL;
    }
    cursor += 6;

    while (cursor < line_end && isspace((unsigned char)*cursor)) cursor += 1;
    if (cursor >= line_end) return NULL;

    if (*cursor == '"') {
        const char *start = ++cursor;
        while (cursor < line_end && *cursor != '"') cursor += 1;
        if (cursor >= line_end) return NULL;
        char *target = bl_strndup_local(start, (size_t)(cursor - start));
        cursor += 1;
        while (cursor < line_end && isspace((unsigned char)*cursor)) cursor += 1;
        if (cursor < line_end && *cursor == ';') {
            cursor += 1;
            while (cursor < line_end && isspace((unsigned char)*cursor)) cursor += 1;
        }
        if (cursor < line_end && !(cursor + 1 < line_end && cursor[0] == '/' && cursor[1] == '/')) {
            free(target);
            return NULL;
        }
        *is_global = 0;
        (void)path;
        (void)line_number;
        return target;
    }

    if (*cursor == '{') {
        const char *start = ++cursor;
        while (cursor < line_end && *cursor != '}') cursor += 1;
        if (cursor >= line_end) return NULL;
        const char *end = cursor;
        while (start < end && isspace((unsigned char)*start)) start += 1;
        while (end > start && isspace((unsigned char)end[-1])) end -= 1;
        if (end == start) return NULL;
        char *target = bl_strndup_local(start, (size_t)(end - start));
        cursor += 1;
        while (cursor < line_end && isspace((unsigned char)*cursor)) cursor += 1;
        if (cursor < line_end && *cursor == ';') {
            cursor += 1;
            while (cursor < line_end && isspace((unsigned char)*cursor)) cursor += 1;
        }
        if (cursor < line_end && !(cursor + 1 < line_end && cursor[0] == '/' && cursor[1] == '/')) {
            free(target);
            return NULL;
        }
        *is_global = 1;
        (void)path;
        (void)line_number;
        return target;
    }

    return NULL;
}

static char *bl_preprocess_file(BLImportState *state, const char *path, size_t parent_file, int import_line);

static char *bl_preprocess_source(BLImportState *state, const char *path, const char *source, size_t source_file) {
    BLStringBuilder out = {0};
    const char *cursor = source;
    int line_number = 1;

    while (*cursor) {
        const char *line_start = cursor;
        while (*cursor && *cursor != '\n') cursor += 1;
        size_t line_length = (size_t)(cursor - line_start);
        int has_newline = (*cursor == '\n');

        char *static_target = bl_parse_static_import_target(line_start, line_length);
        if (static_target) {
            char *manifest = bl_resolve_static_manifest(state, path, static_target);
            if (!manifest) {
                char message[1024]; snprintf(message, sizeof message, "line %d: failed to resolve static library \"%s\" (searched source directory, -L paths, and ~/.bytelang/lib)", line_number, static_target);
                bl_import_state_set_errorf(state, path, message); free(static_target); bl_sb_dispose(&out); return NULL;
            }
            int imported = bl_import_static_manifest(state, manifest, static_target);
            free(manifest); free(static_target);
            if (!imported) { bl_sb_dispose(&out); return NULL; }
            /* Preserve source line numbers after removing a compile-time-only
             * directive so diagnostics still point at user source. */
            if (has_newline) {
                bl_sb_append(&out, "\n");
                if (!bpp_ir_add_expanded_line(state->ir, source_file, line_number, 1)) { bl_sb_dispose(&out); return NULL; }
            }
        } else {
        int is_global = 0;
        char *target = bl_parse_import_target(path, line_number, line_start, line_length, &is_global);
        if (target) {
            char *resolved = is_global
                ? bl_resolve_global_import_path(target)
                : bl_resolve_local_import_path(path, target);

            if (!resolved) {
                char message[1024];
                if (is_global) {
                    snprintf(message, sizeof(message),
                             "line %d: failed to resolve global import {%s} in ~/.bytelang/lib",
                             line_number, target);
                } else {
                    snprintf(message, sizeof(message),
                             "line %d: failed to resolve local import \"%s\"",
                             line_number, target);
                }
                bl_import_state_set_errorf(state, path, message);
                free(target);
                bl_sb_dispose(&out);
                return NULL;
            }

            char *expanded = bl_preprocess_file(state, resolved, source_file, line_number);
            free(resolved);
            free(target);
            if (!expanded) {
                bl_sb_dispose(&out);
                return NULL;
            }

            bl_sb_append(&out, expanded);
            if (out.length > 0 && out.data[out.length - 1] != '\n') {
                bl_sb_append(&out, "\n");
                if (!bpp_ir_add_expanded_line(state->ir, source_file, line_number, 1)) { free(expanded); bl_sb_dispose(&out); return NULL; }
            }
            free(expanded);
        } else {
            bl_sb_append_n(&out, line_start, line_length);
            if (has_newline) bl_sb_append(&out, "\n");
            if (!bpp_ir_add_expanded_line(state->ir, source_file, line_number, 1)) { bl_sb_dispose(&out); return NULL; }
        }
        }

        if (has_newline) {
            cursor += 1;
        }
        line_number += 1;
    }

    return bl_sb_take(&out);
}

static char *bl_preprocess_file(BLImportState *state, const char *path, size_t parent_file, int import_line) {
    char *canonical = bl_path_canonicalize(path);
    if (!canonical) {
        bl_import_state_set_errorf(state, path, "failed to normalize path");
        return NULL;
    }

    if (bl_path_list_contains(&state->active_paths, canonical)) {
        char message[1024];
        snprintf(message, sizeof(message), "cyclic import detected: %s", canonical);
        bl_import_state_set_errorf(state, canonical, message);
        free(canonical);
        return NULL;
    }

    if (bl_path_list_contains(&state->loaded_paths, canonical)) {
        free(canonical);
        return bl_strdup_local("");
    }

    char *source = bl_read_file(canonical);
    if (!source) {
        char message[1024];
        snprintf(message, sizeof(message), "failed to read file");
        bl_import_state_set_errorf(state, canonical, message);
        free(canonical);
        return NULL;
    }

    size_t source_file = bpp_ir_add_source_file(state->ir, canonical, parent_file, import_line);
    if (source_file == BL_IR_NONE) { free(source); free(canonical); return NULL; }
    bl_path_list_push_owned(&state->active_paths, bl_strdup_local(canonical));
    char *expanded = bl_preprocess_source(state, canonical, source, source_file);
    bl_path_list_pop(&state->active_paths);

    free(source);

    if (!expanded) {
        free(canonical);
        return NULL;
    }

    bl_path_list_push_owned(&state->loaded_paths, canonical);
    return expanded;
}

static char *bl_preprocess_root_source(const BLCompiler *compiler, BLIRModule *ir, const char *virtual_path, const char *source, char **error_out) {
    BLImportState state;
    memset(&state, 0, sizeof(state));
    state.compiler = compiler; state.ir = ir;

    const char *root_path = virtual_path ? virtual_path : "<memory>";
    size_t root_file = bpp_ir_add_source_file(ir, root_path, BL_IR_NONE, 0);
    char *expanded = root_file == BL_IR_NONE ? NULL : bl_preprocess_source(&state, root_path, source ? source : "", root_file);
    if (!expanded && error_out) {
        *error_out = state.error ? bl_strdup_local(state.error) : bl_strdup_local("import error");
    }

    if (expanded && error_out) {
        *error_out = NULL;
    }

    bl_import_state_dispose(&state);
    return expanded;
}

int bl_compile_source(BLCompiler *compiler, const char *path, const char *source, BLIRModule *ir) {
    if (!compiler || !ir) return 0;
    bl_ir_dispose(ir);
    char *import_error = NULL;
    char *expanded = bl_preprocess_root_source(compiler, ir, path, source, &import_error);
    if (!expanded) {
        bl_set_error(compiler, import_error ? import_error : "import error");
        free(import_error); return 0;
    }
    BLParser parser; bl_parser_init(&parser, path, expanded);
    BLNode *program = bl_parse_program(&parser);
    int ok = program && bl_ir_compile(ir, path, program);
    bl_set_error(compiler, ok ? NULL : (program ? bl_ir_last_error(ir) : parser.error));
    if(ok){ir->expanded_source=expanded;expanded=NULL;}
    bl_parser_dispose(&parser); free(expanded); return ok;
}
int bl_compile_file(BLCompiler *compiler, const char *path, BLIRModule *ir) {
    char *source = bl_read_file(path);
    if (!source) { bl_set_error(compiler, "failed to read source file"); return 0; }
    int ok = bl_compile_source(compiler, path, source, ir); free(source); return ok;
}
void bl_compiler_dispose(BLCompiler *compiler) { free(compiler->last_error); compiler->last_error = NULL; }
