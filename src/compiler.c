#include "../include/compiler.h"
#include "../include/parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} BLPathList;

typedef struct {
    BLPathList active_paths;
    BLPathList loaded_paths;
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
        if (strcmp(list->items[i], item) == 0) return 1;
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
    free(state->error);
    memset(state, 0, sizeof(*state));
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
    return path[0] == '/';
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

static char *bl_preprocess_file(BLImportState *state, const char *path);

static char *bl_preprocess_source(BLImportState *state, const char *path, const char *source) {
    BLStringBuilder out = {0};
    const char *cursor = source;
    int line_number = 1;

    while (*cursor) {
        const char *line_start = cursor;
        while (*cursor && *cursor != '\n') cursor += 1;
        size_t line_length = (size_t)(cursor - line_start);
        int has_newline = (*cursor == '\n');

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

            char *expanded = bl_preprocess_file(state, resolved);
            free(resolved);
            free(target);
            if (!expanded) {
                bl_sb_dispose(&out);
                return NULL;
            }

            bl_sb_append(&out, expanded);
            if (out.length > 0 && out.data[out.length - 1] != '\n') {
                bl_sb_append(&out, "\n");
            }
            free(expanded);
        } else {
            bl_sb_append_n(&out, line_start, line_length);
            if (has_newline) bl_sb_append(&out, "\n");
        }

        if (has_newline) {
            cursor += 1;
        }
        line_number += 1;
    }

    return bl_sb_take(&out);
}

static char *bl_preprocess_file(BLImportState *state, const char *path) {
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

    bl_path_list_push_owned(&state->active_paths, bl_strdup_local(canonical));
    char *expanded = bl_preprocess_source(state, canonical, source);
    bl_path_list_pop(&state->active_paths);

    free(source);

    if (!expanded) {
        free(canonical);
        return NULL;
    }

    bl_path_list_push_owned(&state->loaded_paths, canonical);
    return expanded;
}

static char *bl_preprocess_root_source(const char *virtual_path, const char *source, char **error_out) {
    BLImportState state;
    memset(&state, 0, sizeof(state));

    char *expanded = bl_preprocess_source(&state, virtual_path ? virtual_path : "<memory>", source ? source : "");
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
    char *import_error = NULL;
    char *expanded = bl_preprocess_root_source(path, source, &import_error);
    if (!expanded) {
        bl_set_error(compiler, import_error ? import_error : "import error");
        free(import_error); return 0;
    }
    BLParser parser; bl_parser_init(&parser, path, expanded);
    BLNode *program = bl_parse_program(&parser);
    int ok = program && bl_ir_compile(ir, path, program);
    bl_set_error(compiler, ok ? NULL : (program ? bl_ir_last_error(ir) : parser.error));
    bl_parser_dispose(&parser); free(expanded); return ok;
}
int bl_compile_file(BLCompiler *compiler, const char *path, BLIRModule *ir) {
    char *source = bl_read_file(path);
    if (!source) { bl_set_error(compiler, "failed to read source file"); return 0; }
    int ok = bl_compile_source(compiler, path, source, ir); free(source); return ok;
}
void bl_compiler_dispose(BLCompiler *compiler) { free(compiler->last_error); compiler->last_error = NULL; }
