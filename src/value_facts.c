#include "../include/value_facts.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { BPP_VALUE_FACTS_MAX_BYTES = 128 * 1024 * 1024 };

static BPPValueFact unknown_fact(void) {
    BPPValueFact f = { BPP_VALUE_UNKNOWN, BPP_TRUTH_UNKNOWN, 0, 0 };
    return f;
}

static BPPValueFact kind_fact(BPPValueKind kind) {
    BPPValueFact f = unknown_fact();
    f.kind = (unsigned char)kind;
    if (kind == BPP_VALUE_NULL) f.truth = BPP_TRUTH_FALSE;
    else if (kind == BPP_VALUE_STRING || kind == BPP_VALUE_BUFFER ||
             kind == BPP_VALUE_ARRAY || kind == BPP_VALUE_OBJECT) f.truth = BPP_TRUTH_TRUE;
    return f;
}

static BPPValueFact integer_fact(long long value, int known) {
    BPPValueFact f = kind_fact(BPP_VALUE_INT);
    f.integer_known = (unsigned char)known;
    f.integer = value;
    if (known) f.truth = value ? BPP_TRUTH_TRUE : BPP_TRUTH_FALSE;
    return f;
}

int bpp_integer_fold(BLIROp op, long long a, long long b, long long *out) {
    long long value = 0;
    switch (op) {
    case BL_IR_ADD:
        if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) return 0;
        value = a + b; break;
    case BL_IR_SUB:
        if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) return 0;
        value = a - b; break;
    case BL_IR_MUL:
        if (a > 0 ? ((b > 0 && a > LLONG_MAX / b) || (b < 0 && b < LLONG_MIN / a))
                  : (a < 0 && ((b > 0 && a < LLONG_MIN / b) || (b < 0 && a < LLONG_MAX / b)))) return 0;
        value = a * b; break;
    case BL_IR_DIV: case BL_IR_MOD:
        if (!b || (a == LLONG_MIN && b == -1)) return 0;
        value = op == BL_IR_DIV ? a / b : a % b; break;
    case BL_IR_EQ: value = a == b; break;
    case BL_IR_NE: value = a != b; break;
    case BL_IR_LT: value = a < b; break;
    case BL_IR_LE: value = a <= b; break;
    case BL_IR_GT: value = a > b; break;
    case BL_IR_GE: value = a >= b; break;
    default: return 0;
    }
    if (out) *out = value;
    return 1;
}

int bpp_value_fact_integer(const BPPValueFact *fact, long long *out) {
    if (!fact || fact->kind != BPP_VALUE_INT || !fact->integer_known) return 0;
    if (out) *out = fact->integer;
    return 1;
}

static BPPValueFact merge_fact(BPPValueFact a, BPPValueFact b) {
    BPPValueFact r = unknown_fact();
    if (a.kind == b.kind) r.kind = a.kind;
    else if (a.kind >= BPP_VALUE_STRING && a.kind <= BPP_VALUE_OBJECT &&
             b.kind >= BPP_VALUE_STRING && b.kind <= BPP_VALUE_OBJECT) r.kind = BPP_VALUE_OBJECT;
    if (a.truth == b.truth) r.truth = a.truth;
    if (r.kind == BPP_VALUE_INT && a.integer_known && b.integer_known && a.integer == b.integer) {
        r.integer_known = 1;
        r.integer = a.integer;
    }
    return r;
}

static int same_fact(BPPValueFact a, BPPValueFact b) {
    return a.kind == b.kind && a.truth == b.truth &&
           a.integer_known == b.integer_known &&
           (!a.integer_known || a.integer == b.integer);
}

static int id_ok(BLIRId id, size_t count) { return id != BL_IR_NONE && id < count; }

static BPPValueFact source(const BPPValueFact *registers, size_t count, BLIRId id) {
    return id_ok(id, count) ? registers[id] : unknown_fact();
}

static void set_dst(const BPPValueFacts *facts, const BLIRInst *i,
                    BPPValueFact *registers, BPPValueFact value) {
    if (id_ok(i->dst, facts->register_count)) registers[i->dst] = value;
}

static BPPValueFact builtin_result(const BLIRInst *i, const BPPValueFact *r,
                                   size_t n) {
    const char *name = i->text ? i->text : "";
    if (!strcmp(name, "string_new") || !strcmp(name, "String.new")) return kind_fact(BPP_VALUE_STRING);
    if (!strcmp(name, "String.concat")) return kind_fact(BPP_VALUE_BUFFER);
    if (!strcmp(name, "String.cmp") || !strcmp(name, "String.length") ||
        !strcmp(name, "len") || !strcmp(name, "array_len") ||
        !strcmp(name, "Array.length")) return kind_fact(BPP_VALUE_INT);
    if (!strcmp(name, "array_new") || !strcmp(name, "Array.new")) return kind_fact(BPP_VALUE_ARRAY);
    if (!strcmp(name, "malloc")) return kind_fact(BPP_VALUE_BUFFER);
    if (!strcmp(name, "realloc") || !strcmp(name, "array_grow") ||
        !strcmp(name, "Array.grow")) return n ? r[i->args[0]] : unknown_fact();
    if (!strcmp(name, "array_set") || !strcmp(name, "Array.set"))
        return i->arg_count >= 3 && id_ok(i->args[2], n) ? r[i->args[2]] : unknown_fact();
    if (!strcmp(name, "assert") || !strcmp(name, "print") || !strcmp(name, "println"))
        return kind_fact(BPP_VALUE_NULL);
    return unknown_fact();
}

void bpp_value_facts_transfer(const BPPValueFacts *facts, const BLIRInst *i,
                              BPPValueFact *r, BPPValueFact *l, BPPValueFact *g) {
    BPPValueFact a = source(r, facts->register_count, i->a);
    BPPValueFact b = source(r, facts->register_count, i->b);
    BPPValueFact out = unknown_fact();
    switch (i->op) {
    case BL_IR_NULL: out = kind_fact(BPP_VALUE_NULL); break;
    case BL_IR_INT: out = integer_fact(i->integer, 1); break;
    case BL_IR_STRING: out = kind_fact(BPP_VALUE_STRING); break;
    case BL_IR_LOAD_LOCAL:
        out = id_ok(i->a, facts->local_count) ? l[i->a] : unknown_fact(); break;
    case BL_IR_LOAD_GLOBAL:
        out = id_ok(i->a, facts->global_count) ? g[i->a] : unknown_fact(); break;
    case BL_IR_STORE_LOCAL: case BL_IR_INIT_LOCAL:
        if (id_ok(i->a, facts->local_count)) l[i->a] = b;
        return;
    case BL_IR_STORE_GLOBAL: case BL_IR_INIT_GLOBAL:
        if (id_ok(i->a, facts->global_count)) g[i->a] = b;
        return;
    case BL_IR_DELETE_LOCAL:
        if (id_ok(i->a, facts->local_count)) l[i->a] = kind_fact(BPP_VALUE_NULL);
        return;
    case BL_IR_DELETE_GLOBAL:
        if (id_ok(i->a, facts->global_count)) g[i->a] = kind_fact(BPP_VALUE_NULL);
        return;
    case BL_IR_FREE_LOCAL:
        if (id_ok(i->a, facts->local_count) &&
            (l[i->a].kind == BPP_VALUE_BUFFER || l[i->a].kind == BPP_VALUE_ARRAY)) return;
        if (id_ok(i->a, facts->local_count)) l[i->a] = unknown_fact();
        return;
    case BL_IR_FREE_GLOBAL:
        if (id_ok(i->a, facts->global_count) &&
            (g[i->a].kind == BPP_VALUE_BUFFER || g[i->a].kind == BPP_VALUE_ARRAY)) return;
        if (id_ok(i->a, facts->global_count)) g[i->a] = unknown_fact();
        return;
    case BL_IR_RELEASE:
        if (id_ok(i->a, facts->register_count)) r[i->a] = unknown_fact();
        return;
    case BL_IR_NOT:
        out = integer_fact(a.truth == BPP_TRUTH_TRUE ? 0 : 1,
                           a.truth != BPP_TRUTH_UNKNOWN); break;
    case BL_IR_NEG:
        out = integer_fact(0, 0);
        if (a.integer_known && a.integer != LLONG_MIN) out = integer_fact(-a.integer, 1);
        break;
    case BL_IR_ADD:
        if (a.kind == BPP_VALUE_INT && b.kind == BPP_VALUE_INT) {
            long long folded = 0;
            out = integer_fact(0, 0);
            if (a.integer_known && b.integer_known && bpp_integer_fold(i->op, a.integer, b.integer, &folded)) out = integer_fact(folded, 1);
        }
        else if (a.kind != BPP_VALUE_UNKNOWN || b.kind != BPP_VALUE_UNKNOWN) out = kind_fact(BPP_VALUE_OBJECT);
        break;
    case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: {
        long long folded = 0;
        out = integer_fact(0, 0);
        if (a.integer_known && b.integer_known && bpp_integer_fold(i->op, a.integer, b.integer, &folded)) out = integer_fact(folded, 1);
        break;
    }
    case BL_IR_INDEX_GET: out = integer_fact(0, 0); break;
    case BL_IR_BYTE_LOAD: out = integer_fact(0, 0); break;
    case BL_IR_MALLOC: case BL_IR_REMALLOC: out = integer_fact(0, 0); break;
    case BL_IR_MFREE_LOCAL: case BL_IR_MFREE_GLOBAL: return;
    case BL_IR_BYTE_STORE: return;
    case BL_IR_PRINT_BYTE: case BL_IR_PRINT_RANGE: return;
    case BL_IR_TO_STRING_BUFFER: out = kind_fact(BPP_VALUE_BUFFER); break;
    case BL_IR_CALL:
        if (i->a == BL_IR_NONE) out = builtin_result(i, r, facts->register_count);
        /* User calls may recurse, return dynamically, and write every global.
         * Do not retain a global value fact across an unmodelled call. */
        else if (g) for (size_t x = 0; x < facts->global_count; ++x) g[x] = unknown_fact();
        break;
    default: return;
    }
    set_dst(facts, i, r, out);
}

static int successors(const BLIRFunction *fn, size_t block, const BPPValueFact *r, size_t *a, size_t *b) {
    const BLIRBlock *bb = &fn->blocks[block];
    if (!bb->count) return 0;
    const BLIRInst *i = &bb->insts[bb->count - 1];
    if (i->op == BL_IR_JUMP) { *a = i->a; return 1; }
    if (i->op == BL_IR_BRANCH) {
        BPPValueFact condition = source(r, fn->register_count, i->a);
        if (condition.truth == BPP_TRUTH_TRUE) { *a = i->b; return 1; }
        if (condition.truth == BPP_TRUTH_FALSE) { *a = i->c; return 1; }
        *a = i->b; *b = i->c; return 2;
    }
    return 0;
}

static void unknown_array(BPPValueFact *a, size_t n) { for (size_t i = 0; i < n; ++i) a[i] = unknown_fact(); }

static int checked_bytes(size_t a, size_t b, size_t *out) {
    if (a && b > SIZE_MAX / a) return 0;
    *out = a * b; return 1;
}

int bpp_value_facts_create(const BLIRModule *m, size_t fi, BPPValueFacts *facts) {
    memset(facts, 0, sizeof *facts);
    if (!m || fi >= m->function_count) return 0;
    facts->module = m; facts->function = &m->functions[fi]; facts->function_index = fi;
    facts->register_count = facts->function->register_count;
    facts->local_count = facts->function->local_count;
    facts->global_count = m->global_count; facts->block_count = facts->function->block_count;
    size_t cells = 0, bytes = 0, part = 0;
    if (!checked_bytes(facts->block_count, facts->register_count, &cells) ||
        !checked_bytes(cells, sizeof(BPPValueFact), &bytes) ||
        !checked_bytes(facts->block_count, facts->local_count, &cells) ||
        !checked_bytes(cells, sizeof(BPPValueFact), &part) || bytes > SIZE_MAX - part) return 1;
    bytes += part;
    if (!checked_bytes(facts->block_count, facts->global_count, &cells) ||
        !checked_bytes(cells, sizeof(BPPValueFact), &part) || bytes > SIZE_MAX - part) return 1;
    bytes += part;
    if (bytes > BPP_VALUE_FACTS_MAX_BYTES) return 1;
    facts->register_in = calloc(facts->block_count * (facts->register_count ? facts->register_count : 1), sizeof *facts->register_in);
    facts->local_in = calloc(facts->block_count * (facts->local_count ? facts->local_count : 1), sizeof *facts->local_in);
    facts->global_in = calloc(facts->block_count * (facts->global_count ? facts->global_count : 1), sizeof *facts->global_in);
    if (!facts->register_in || !facts->local_in || !facts->global_in) { bpp_value_facts_destroy(facts); return 1; }
    facts->available = 1;
    /* All-zero representation is the unknown lattice bottom.  Worklist-style
     * fixed point with monotone joins; bounded because facts only lose detail. */
    size_t rn = facts->register_count ? facts->register_count : 1;
    size_t ln = facts->local_count ? facts->local_count : 1;
    size_t gn = facts->global_count ? facts->global_count : 1;
    BPPValueFact *r = calloc(rn, sizeof *r), *l = calloc(ln, sizeof *l), *g = calloc(gn, sizeof *g);
    if (!r || !l || !g) { free(r); free(l); free(g); bpp_value_facts_destroy(facts); return 1; }
    unsigned char *seen = calloc(facts->block_count ? facts->block_count : 1, 1);
    if (!seen) { free(r); free(l); free(g); bpp_value_facts_destroy(facts); return 1; }
    seen[0] = 1;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (size_t block = 0; block < facts->block_count; ++block) {
            /* Do not let an unreachable all-unknown state poison a later
             * merge.  Constant branches intentionally leave one successor
             * unseen, and only executable predecessors contribute facts. */
            if (!seen[block]) continue;
            memcpy(r, facts->register_in + block * facts->register_count, facts->register_count * sizeof *r);
            memcpy(l, facts->local_in + block * facts->local_count, facts->local_count * sizeof *l);
            memcpy(g, facts->global_in + block * facts->global_count, facts->global_count * sizeof *g);
            const BLIRBlock *bb = &facts->function->blocks[block];
            for (size_t k = 0; k < bb->count; ++k) bpp_value_facts_transfer(facts, &bb->insts[k], r, l, g);
            size_t to[2] = {0, 0}; int count = successors(facts->function, block, r, &to[0], &to[1]);
            for (int edge = 0; edge < count; ++edge) {
                if (to[edge] >= facts->block_count) continue;
                BPPValueFact *dr = facts->register_in + to[edge] * facts->register_count;
                BPPValueFact *dl = facts->local_in + to[edge] * facts->local_count;
                BPPValueFact *dg = facts->global_in + to[edge] * facts->global_count;
                if (!seen[to[edge]]) {
                    memcpy(dr, r, facts->register_count * sizeof *r);
                    memcpy(dl, l, facts->local_count * sizeof *l);
                    memcpy(dg, g, facts->global_count * sizeof *g);
                    seen[to[edge]] = 1; changed = 1;
                } else {
                    for (size_t x = 0; x < facts->register_count; ++x) { BPPValueFact v = merge_fact(dr[x], r[x]); if (!same_fact(v, dr[x])) { dr[x] = v; changed = 1; } }
                    for (size_t x = 0; x < facts->local_count; ++x) { BPPValueFact v = merge_fact(dl[x], l[x]); if (!same_fact(v, dl[x])) { dl[x] = v; changed = 1; } }
                    for (size_t x = 0; x < facts->global_count; ++x) { BPPValueFact v = merge_fact(dg[x], g[x]); if (!same_fact(v, dg[x])) { dg[x] = v; changed = 1; } }
                }
            }
        }
    }
    free(seen); free(r); free(l); free(g);
    return 1;
}

void bpp_value_facts_destroy(BPPValueFacts *facts) {
    if (!facts) return;
    free(facts->register_in); free(facts->local_in); free(facts->global_in);
    memset(facts, 0, sizeof *facts);
}

void bpp_value_facts_begin_block(const BPPValueFacts *f, size_t block,
                                 BPPValueFact *r, BPPValueFact *l, BPPValueFact *g) {
    if (!f->available || block >= f->block_count) {
        unknown_array(r, f->register_count); unknown_array(l, f->local_count); unknown_array(g, f->global_count); return;
    }
    memcpy(r, f->register_in + block * f->register_count, f->register_count * sizeof *r);
    memcpy(l, f->local_in + block * f->local_count, f->local_count * sizeof *l);
    memcpy(g, f->global_in + block * f->global_count, f->global_count * sizeof *g);
}

const char *bpp_value_kind_name(BPPValueKind k) {
    static const char *names[] = { "unknown", "null", "integer", "string", "buffer", "array", "object" };
    return k <= BPP_VALUE_OBJECT ? names[k] : "unknown";
}
const char *bpp_truth_name(BPPTruth t) { return t == BPP_TRUTH_FALSE ? "false" : t == BPP_TRUTH_TRUE ? "true" : "unknown"; }
