#include "../include/target.h"
#include "../include/ir_effects.h"
#include "../include/value_facts.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Return whether this instruction made another virtual register live.  DCE
 * deliberately asks this only of retained instructions, so an unused pure
 * computation cannot keep its own input chain alive. */
static int operands(const BLIRInst *i,unsigned char *used) {
    int changed=0;
#define USE(value) do { BLIRId r_=(value); if(r_!=BL_IR_NONE&&!used[r_]) { used[r_]=1; changed=1; } } while(0)
    switch(i->op) {
    case BL_IR_NEG:case BL_IR_NOT:case BL_IR_BIT_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_BRANCH:case BL_IR_RETURN:case BL_IR_RELEASE:case BL_IR_MALLOC:case BL_IR_PRINT_BYTE:USE(i->a);
    break;
    case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
    case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:
    case BL_IR_INDEX_GET:case BL_IR_INDEX_SET:case BL_IR_BYTE_LOAD:case BL_IR_BYTE_STORE:case BL_IR_REMALLOC:case BL_IR_PRINT_RANGE:USE(i->a);USE(i->b);
    if(i->op==BL_IR_INDEX_SET||i->op==BL_IR_BYTE_STORE)USE(i->c);
    break;
    case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:USE(i->b);
    break;
    case BL_IR_CALL:for(size_t j=0;j<i->arg_count;++j)USE(i->args[j]);
    break;
    default:break;
    }
    return changed;
#undef USE
}

/* A successful instruction in this set never produces an owned object.  It
 * can still fail (for example DIV); that operation is retained as an effect
 * root, but a later release of its primitive result is a no-op. */
static int dce_produces_primitive(BLIROp op) {
    switch(op) {
    case BL_IR_NULL: case BL_IR_INT: case BL_IR_NEG: case BL_IR_SUB:
    case BL_IR_BIT_AND: case BL_IR_BIT_OR: case BL_IR_BIT_XOR: case BL_IR_BIT_NOT:
    case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD: case BL_IR_NOT:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE:
    case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET: case BL_IR_BYTE_LOAD:
        return 1;
    default:
        return 0;
    }
}

static int dce_is_root(const BLIRInst *i, const unsigned char *primitive, size_t n) {
    if (i->op == BL_IR_RELEASE)
        return i->a == BL_IR_NONE || i->a >= n || !primitive[i->a];
    return bpp_ir_effects(i) != BPP_EFFECT_NONE;
}

typedef struct {
    unsigned char valid;
    unsigned generation;
    BLIROp op;
    BLIRId a, b, result;
    long long integer;
} BPPValueNumber;

static BLIRId representative(const BLIRId *map, size_t n, BLIRId r) {
    while (r != BL_IR_NONE && r < n && map[r] != r) r = map[r];
    return r;
}

static void rewrite_uses(BLIRInst *i, const BLIRId *map, size_t n) {
#define REP(field) do { if (i->field != BL_IR_NONE) i->field = representative(map, n, i->field); } while (0)
    switch (i->op) {
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: REP(b); break;
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_BIT_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_BRANCH: case BL_IR_RETURN: case BL_IR_RELEASE: case BL_IR_MALLOC: case BL_IR_PRINT_BYTE: REP(a); break;
    case BL_IR_BIT_AND: case BL_IR_BIT_OR: case BL_IR_BIT_XOR:
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET: case BL_IR_BYTE_LOAD: case BL_IR_REMALLOC: case BL_IR_PRINT_RANGE: REP(a); REP(b); break;
    case BL_IR_INDEX_SET: case BL_IR_BYTE_STORE: REP(a); REP(b); REP(c); break;
    case BL_IR_CALL: for (size_t j = 0; j < i->arg_count; ++j) i->args[j] = representative(map, n, i->args[j]); break;
    default: break;
    }
#undef REP
}

static uint64_t hash_number(BLIROp op, BLIRId a, BLIRId b, long long integer) {
    uint64_t h = (uint64_t)(unsigned)op * UINT64_C(0x9e3779b185ebca87);
    h ^= (uint64_t)a + UINT64_C(0x517cc1b727220a95) + (h << 6) + (h >> 2);
    h ^= (uint64_t)b + UINT64_C(0x6eed0e9da4d94a4f) + (h << 6) + (h >> 2);
    h ^= (uint64_t)integer + UINT64_C(0x94d049bb133111eb) + (h << 6) + (h >> 2);
    return h;
}

static int number_key(const BLIRInst *i, BLIROp *op, BLIRId *a, BLIRId *b, long long *integer) {
    *op = i->op; *a = i->a; *b = i->b; *integer = i->integer;
    if (i->op == BL_IR_NULL || i->op == BL_IR_INT || i->op == BL_IR_NOT) return 1;
    if (i->op == BL_IR_EQ || i->op == BL_IR_NE) {
        if (*a > *b) { BLIRId t = *a; *a = *b; *b = t; }
        return 1;
    }
    return 0;
}

static BLIRId number_lookup_or_add(BPPValueNumber *table, size_t cap, unsigned generation,
                                   const BLIRInst *i) {
    BLIROp op; BLIRId a, b; long long integer;
    if (!number_key(i, &op, &a, &b, &integer)) return BL_IR_NONE;
    size_t slot = (size_t)hash_number(op, a, b, integer) & (cap - 1);
    for (;;) {
        BPPValueNumber *entry = &table[slot];
        if (!entry->valid || entry->generation != generation) {
            entry->valid = 1; entry->generation = generation; entry->op = op;
            entry->a = a; entry->b = b; entry->integer = integer; entry->result = i->dst;
            return BL_IR_NONE;
        }
        if (entry->op == op && entry->a == a && entry->b == b && entry->integer == integer) return entry->result;
        slot = (slot + 1) & (cap - 1);
    }
}

static int integer_value(const unsigned char *known, const long long *values, size_t n, BLIRId r, long long *out) {
    if (r == BL_IR_NONE || r >= n || !known[r]) return 0;
    *out = values[r]; return 1;
}

/* CSE is block-local, so removing a definition is only sound when all of its
 * uses are in that same block and can be rewritten immediately. */
static void mark_cross_block_uses(const BLIRInst *i, size_t block,
                                  const size_t *definition_block,
                                  unsigned char *cross_block, size_t n) {
#define MARK(field) do { BLIRId r_ = i->field; if (r_ != BL_IR_NONE && r_ < n && definition_block[r_] != block) cross_block[r_] = 1; } while (0)
    switch (i->op) {
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: MARK(b); break;
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_BIT_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_BRANCH: case BL_IR_RETURN: case BL_IR_RELEASE: case BL_IR_MALLOC: case BL_IR_PRINT_BYTE: MARK(a); break;
    case BL_IR_BIT_AND: case BL_IR_BIT_OR: case BL_IR_BIT_XOR:
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET: case BL_IR_BYTE_LOAD: case BL_IR_REMALLOC: case BL_IR_PRINT_RANGE: MARK(a); MARK(b); break;
    case BL_IR_INDEX_SET: case BL_IR_BYTE_STORE: MARK(a); MARK(b); MARK(c); break;
    case BL_IR_CALL: for (size_t j = 0; j < i->arg_count; ++j) { BLIRId r_ = i->args[j]; if (r_ != BL_IR_NONE && r_ < n && definition_block[r_] != block) cross_block[r_] = 1; } break;
    default: break;
    }
#undef MARK
}

/* A deliberately local forwarding pass. It only forwards exact primitive
 * integers after their required source check has executed. Object loads keep
 * clone/release semantics. Debug output retains all source accesses; native
 * sanitizer hooks make globals observable, but local scalar accesses do not
 * have a hook and may still be safely folded. */
static int eliminate_redundant_accesses(BLIRModule *m, BLIRFunction *fn) {
    const size_t n = fn->register_count, slots = fn->local_count + m->global_count;
    if (slots < fn->local_count) return 0;
    size_t cap = 1;
    while (cap < (n ? n * 2 : 2)) { if (cap > SIZE_MAX / 2) return 0; cap <<= 1; }
    BLIRId *map = calloc(n ? n : 1, sizeof *map);
    unsigned char *known = calloc(n ? n : 1, 1), *slot_known = calloc(slots ? slots : 1, 1),
                  *slot_initialized = calloc(slots ? slots : 1, 1), *cross_block = calloc(n ? n : 1, 1), *drop = NULL;
    size_t *definition_block = malloc((n ? n : 1) * sizeof *definition_block);
    long long *values = calloc(n ? n : 1, sizeof *values), *slot_values = calloc(slots ? slots : 1, sizeof *slot_values);
    BPPValueNumber *numbers = calloc(cap, sizeof *numbers);
    if (!map || !known || !slot_known || !slot_initialized || !cross_block || !definition_block || !values || !slot_values || !numbers) {
        free(map); free(known); free(slot_known); free(slot_initialized); free(cross_block); free(definition_block); free(values); free(slot_values); free(numbers); return 0;
    }
    for (size_t r = 0; r < n; ++r) definition_block[r] = BL_IR_NONE;
    for (size_t b = 0; b < fn->block_count; ++b)
        for (size_t k = 0; k < fn->blocks[b].count; ++k) {
            BLIRInst *i = &fn->blocks[b].insts[k];
            if (i->dst != BL_IR_NONE && i->dst < n) definition_block[i->dst] = b;
        }
    for (size_t b = 0; b < fn->block_count; ++b)
        for (size_t k = 0; k < fn->blocks[b].count; ++k)
            mark_cross_block_uses(&fn->blocks[b].insts[k], b, definition_block, cross_block, n);
    const int access_elision = !m->preserve_access_observability;
    for (size_t block_index = 0; block_index < fn->block_count; ++block_index) {
        BLIRBlock *block = &fn->blocks[block_index];
        drop = calloc(block->count ? block->count : 1, 1);
        if (!drop) { free(map); free(known); free(slot_known); free(slot_initialized); free(cross_block); free(definition_block); free(values); free(slot_values); free(numbers); return 0; }
        for (size_t r = 0; r < n; ++r) map[r] = r;
        memset(known, 0, n); memset(slot_known, 0, slots); memset(slot_initialized, 0, slots);
        memset(numbers, 0, cap * sizeof *numbers);
        unsigned generation = 1;
        for (size_t k = 0; k < block->count; ++k) {
            BLIRInst *i = &block->insts[k];
            if (i->op == BL_IR_BRANCH || i->op == BL_IR_JUMP) ++m->redundant_access_metrics.branches;
            if (i->op == BL_IR_CALL) ++m->redundant_access_metrics.helper_calls;
            /* A value-numbered primitive has no ownership to release.  Do
             * not redirect its original release to the representative: that
             * could end the representative before a later use. */
            if (i->op == BL_IR_RELEASE && i->a != BL_IR_NONE &&
                representative(map, n, i->a) != i->a) {
                drop[k] = 1;
                continue;
            }
            rewrite_uses(i, map, n);
            if (i->op == BL_IR_LOAD_LOCAL || i->op == BL_IR_LOAD_GLOBAL) {
                ++m->redundant_access_metrics.slot_loads;
                const int global = i->op == BL_IR_LOAD_GLOBAL;
                size_t slot = i->a + (global ? fn->local_count : 0);
                if (access_elision && (!global || !m->instrument_native) && slot_initialized[slot] && slot_known[slot]) {
                    i->op = BL_IR_INT; i->integer = slot_values[slot]; i->a = i->b = i->c = BL_IR_NONE;
                    ++m->redundant_access_metrics.eliminated_slot_loads;
                }
            }
            if (i->op == BL_IR_INIT_LOCAL || i->op == BL_IR_STORE_LOCAL || i->op == BL_IR_INIT_GLOBAL || i->op == BL_IR_STORE_GLOBAL) {
                ++m->redundant_access_metrics.slot_stores;
                const int global = i->op == BL_IR_INIT_GLOBAL || i->op == BL_IR_STORE_GLOBAL;
                const int init = i->op == BL_IR_INIT_GLOBAL || i->op == BL_IR_INIT_LOCAL;
                size_t slot = i->a + (global ? fn->local_count : 0); long long value = 0;
                const int source_known = integer_value(known, values, n, i->b, &value);
                if (access_elision && !init && (!global || !m->instrument_native) &&
                    slot_initialized[slot] && slot_known[slot] && source_known && slot_values[slot] == value) {
                    drop[k] = 1;
                    ++m->redundant_access_metrics.eliminated_slot_stores;
                }
                slot_initialized[slot] = 1; slot_known[slot] = (unsigned char)source_known;
                if (source_known) slot_values[slot] = value;
            }
            if (i->op == BL_IR_DELETE_LOCAL || i->op == BL_IR_DELETE_GLOBAL ||
                i->op == BL_IR_FREE_LOCAL || i->op == BL_IR_FREE_GLOBAL) {
                const int global = i->op == BL_IR_DELETE_GLOBAL || i->op == BL_IR_FREE_GLOBAL;
                size_t slot = i->a + (global ? fn->local_count : 0);
                slot_initialized[slot] = 1; slot_known[slot] = 0;
            }
            if (i->dst != BL_IR_NONE) {
                BLIRId previous = number_lookup_or_add(numbers, cap, generation, i);
                if (previous != BL_IR_NONE && bpp_ir_is_pure_primitive(i) && !cross_block[i->dst]) {
                    map[i->dst] = previous; drop[k] = 1;
                    ++m->redundant_access_metrics.value_numbered_expressions;
                } else {
                    map[i->dst] = i->dst;
                    if (i->op == BL_IR_INT) { known[i->dst] = 1; values[i->dst] = i->integer; }
                    else known[i->dst] = 0;
                }
            }
            if (i->op == BL_IR_RELEASE && i->a < n) known[i->a] = 0;
            /* Any observable memory/ownership operation ends the pure-value
             * equivalence window.  Constants can still be reconstructed. */
            if (bpp_ir_effects(i) != BPP_EFFECT_NONE && !bpp_ir_is_pure_primitive(i)) {
                ++m->redundant_access_metrics.effect_barriers;
                if (++generation == 0) { memset(numbers, 0, cap * sizeof *numbers); generation = 1; }
                if (i->op == BL_IR_CALL) {
                    memset(slot_known + fn->local_count, 0, m->global_count);
                    memset(slot_initialized + fn->local_count, 0, m->global_count);
                }
            }
        }
        size_t keep = 0;
        for (size_t k = 0; k < block->count; ++k) {
            if (drop[k]) { free(block->insts[k].text); free(block->insts[k].args); continue; }
            block->insts[keep++] = block->insts[k];
        }
        block->count = keep; free(drop); drop = NULL;
    }
    free(map); free(known); free(slot_known); free(slot_initialized); free(cross_block); free(definition_block); free(values); free(slot_values); free(numbers);
    return 1;
}

static BLIRInst *cfg_end(BLIRFunction *fn, size_t block) {
    BLIRBlock *value = &fn->blocks[block];
    return value->count ? &value->insts[value->count - 1] : NULL;
}

static size_t cfg_resolve_jump(const BLIRFunction *fn, size_t block) {
    size_t current = block;
    for (size_t hops = 0; hops < fn->block_count; ++hops) {
        const BLIRBlock *candidate = &fn->blocks[current];
        if (candidate->count != 1 || candidate->insts[0].op != BL_IR_JUMP ||
            candidate->insts[0].a == current) break;
        current = candidate->insts[0].a;
    }
    return current;
}

static int cfg_thread_jumps(BLIRFunction *fn, BPPIROptMetrics *metrics) {
    int changed = 0;
    for (size_t block = 0; block < fn->block_count; ++block) {
        BLIRInst *end = cfg_end(fn, block);
        if (!end) continue;
        if (end->op == BL_IR_JUMP) {
            size_t target = cfg_resolve_jump(fn, end->a);
            if (target != end->a) { end->a = target; ++metrics->cfg_threaded_jumps; changed = 1; }
        } else if (end->op == BL_IR_BRANCH) {
            size_t yes = cfg_resolve_jump(fn, end->b), no = cfg_resolve_jump(fn, end->c);
            if (yes != end->b) { end->b = yes; ++metrics->cfg_threaded_jumps; changed = 1; }
            if (no != end->c) { end->c = no; ++metrics->cfg_threaded_jumps; changed = 1; }
            if (end->b == end->c) {
                /* Keep the condition's later release in the common successor;
                 * only the transfer itself becomes unconditional. */
                end->op = BL_IR_JUMP;
                end->a = end->b;
                end->b = end->c = BL_IR_NONE;
                changed = 1;
            }
        }
    }
    return changed;
}

static int cfg_compact_reachable(BLIRModule *m, BLIRFunction *fn) {
    unsigned char *reachable = calloc(fn->block_count ? fn->block_count : 1, 1);
    size_t *map = calloc(fn->block_count ? fn->block_count : 1, sizeof *map);
    size_t *queue = calloc(fn->block_count ? fn->block_count : 1, sizeof *queue);
    if (!reachable || !map || !queue) { free(reachable); free(map); free(queue); return -1; }
    size_t head = 0, tail = 0;
    reachable[0] = 1; queue[tail++] = 0;
    while (head < tail) {
        BLIRInst *end = cfg_end(fn, queue[head++]);
        size_t successors[2] = {BL_IR_NONE, BL_IR_NONE};
        if (end->op == BL_IR_JUMP) successors[0] = end->a;
        else if (end->op == BL_IR_BRANCH) { successors[0] = end->b; successors[1] = end->c; }
        for (size_t edge = 0; edge < 2; ++edge)
            if (successors[edge] != BL_IR_NONE && !reachable[successors[edge]]) {
                reachable[successors[edge]] = 1; queue[tail++] = successors[edge];
            }
    }
    size_t kept = 0;
    for (size_t block = 0; block < fn->block_count; ++block) if (reachable[block]) map[block] = kept++;
    if (kept == fn->block_count) { free(reachable); free(map); free(queue); return 0; }
    size_t output = 0;
    for (size_t block = 0; block < fn->block_count; ++block) {
        BLIRBlock value = fn->blocks[block];
        if (!reachable[block]) {
            for (size_t k = 0; k < value.count; ++k) { free(value.insts[k].text); free(value.insts[k].args); }
            free(value.insts);
            ++m->redundant_access_metrics.cfg_removed_blocks;
            continue;
        }
        fn->blocks[output++] = value;
    }
    fn->block_count = kept;
    for (size_t block = 0; block < kept; ++block) {
        BLIRInst *end = cfg_end(fn, block);
        if (end->op == BL_IR_JUMP) end->a = map[end->a];
        else if (end->op == BL_IR_BRANCH) { end->b = map[end->b]; end->c = map[end->c]; }
    }
    free(reachable); free(map); free(queue);
    return 1;
}

static int cfg_merge_one(BLIRModule *m, BLIRFunction *fn) {
    size_t *predecessors = calloc(fn->block_count ? fn->block_count : 1, sizeof *predecessors);
    if (!predecessors) return -1;
    for (size_t block = 0; block < fn->block_count; ++block) {
        BLIRInst *end = cfg_end(fn, block);
        if (end->op == BL_IR_JUMP) ++predecessors[end->a];
        else if (end->op == BL_IR_BRANCH) { ++predecessors[end->b]; ++predecessors[end->c]; }
    }
    for (size_t block = 0; block < fn->block_count; ++block) {
        BLIRBlock *from = &fn->blocks[block];
        BLIRInst *end = cfg_end(fn, block);
        if (end->op != BL_IR_JUMP || end->a == block || predecessors[end->a] != 1) continue;
        BLIRBlock *to = &fn->blocks[end->a];
        size_t retained = from->count - 1;
        if (to->count > SIZE_MAX - retained || to->count + retained > SIZE_MAX / sizeof *from->insts) { free(predecessors); return -1; }
        BLIRInst *joined = realloc(from->insts, (retained + to->count) * sizeof *joined);
        if (!joined && retained + to->count) { free(predecessors); return -1; }
        from->insts = joined;
        memcpy(from->insts + retained, to->insts, to->count * sizeof *to->insts);
        from->count = retained + to->count;
        from->capacity = from->count;
        free(to->insts); to->insts = NULL; to->count = to->capacity = 0;
        ++m->redundant_access_metrics.cfg_merged_blocks;
        free(predecessors);
        return 1;
    }
    free(predecessors);
    return 0;
}

static size_t cfg_preferred_successor(const BLIRFunction *fn, size_t block) {
    const BLIRInst *end = cfg_end((BLIRFunction *)fn, block);
    if (end->op == BL_IR_JUMP) return end->a;
    if (end->op != BL_IR_BRANCH) return BL_IR_NONE;
    /* A backwards target is the static loop-hot edge. Otherwise retain the
     * source-order true edge, which is the language's normal then/body path. */
    if (end->b <= block && end->c > block) return end->b;
    if (end->c <= block && end->b > block) return end->c;
    return end->b;
}

static int cfg_layout_traces(BLIRModule *m, BLIRFunction *fn) {
    if (fn->block_count < 2) return 0;
    unsigned char *placed = calloc(fn->block_count, 1);
    size_t *order = calloc(fn->block_count, sizeof *order), *map = calloc(fn->block_count, sizeof *map);
    BLIRBlock *reordered = calloc(fn->block_count, sizeof *reordered);
    if (!placed || !order || !map || !reordered) { free(placed); free(order); free(map); free(reordered); return -1; }
    order[0] = 0; placed[0] = 1;
    for (size_t position = 1; position < fn->block_count; ++position) {
        size_t previous = order[position - 1], next = cfg_preferred_successor(fn, previous);
        if (next == BL_IR_NONE || placed[next]) {
            next = BL_IR_NONE;
            for (size_t candidate = 0; candidate < fn->block_count && next == BL_IR_NONE; ++candidate) if (!placed[candidate]) {
                for (size_t pred = 0; pred < fn->block_count; ++pred) if (placed[pred]) {
                    BLIRInst *end = cfg_end(fn, pred);
                    if ((end->op == BL_IR_JUMP && end->a == candidate) ||
                        (end->op == BL_IR_BRANCH && (end->b == candidate || end->c == candidate))) { next = candidate; break; }
                }
            }
            if (next == BL_IR_NONE) for (size_t candidate = 0; candidate < fn->block_count; ++candidate) if (!placed[candidate]) { next = candidate; break; }
        }
        order[position] = next; placed[next] = 1;
    }
    int changed = 0;
    for (size_t position = 0; position < fn->block_count; ++position) { map[order[position]] = position; changed |= order[position] != position; }
    if (changed) {
        for (size_t old = 0; old < fn->block_count; ++old) reordered[map[old]] = fn->blocks[old];
        for (size_t block = 0; block < fn->block_count; ++block) {
            BLIRInst *end = reordered[block].count ? &reordered[block].insts[reordered[block].count - 1] : NULL;
            if (end->op == BL_IR_JUMP) end->a = map[end->a];
            else if (end->op == BL_IR_BRANCH) { end->b = map[end->b]; end->c = map[end->c]; }
        }
        free(fn->blocks); fn->blocks = reordered; reordered = NULL;
        ++m->redundant_access_metrics.cfg_layout_moves;
    }
    free(placed); free(order); free(map); free(reordered);
    return changed;
}

static int cfg_optimize(BLIRModule *m, BLIRFunction *fn) {
    for (size_t round = 0; round <= fn->block_count; ++round) {
        int changed = cfg_thread_jumps(fn, &m->redundant_access_metrics);
        int compacted = cfg_compact_reachable(m, fn);
        if (compacted < 0) return 0;
        int merged = cfg_merge_one(m, fn);
        if (merged < 0) return 0;
        /* A merged successor has no remaining predecessor, so compact it
         * before validating: every observable intermediate stays valid IR. */
        if (merged) {
            int post_merge_compacted = cfg_compact_reachable(m, fn);
            if (post_merge_compacted < 0) return 0;
            compacted |= post_merge_compacted;
        }
        if (!bl_ir_validate_pass(m, "optimizer CFG simplify")) return 0;
        if (!changed && !compacted && !merged) break;
    }
    int laid_out = cfg_layout_traces(m, fn);
    if (laid_out < 0) return 0;
    return bl_ir_validate_pass(m, "optimizer CFG layout");
}

int bpp_ir_optimize(BLIRModule *m) {
    if(!bl_ir_validate_pass(m,"optimizer input"))return 0;
    memset(&m->redundant_access_metrics,0,sizeof m->redundant_access_metrics);
    for(size_t f=0;f<m->function_count;++f) {
        BLIRFunction *fn=&m->functions[f];size_t n=fn->register_count;
        unsigned char *used=calloc(n?n:1,1),*primitive=calloc(n?n:1,1);
        unsigned char *reachable=calloc(fn->block_count,1);size_t *map=calloc(fn->block_count,sizeof *map);
        BPPValueFacts facts;
        if(!used||!primitive||!reachable||!map||!bpp_value_facts_create(m,f,&facts)){free(used);free(primitive);free(reachable);free(map);return 0;}
        BPPValueFact *registers=calloc(n?n:1,sizeof *registers),*locals=calloc(fn->local_count?fn->local_count:1,sizeof *locals),*globals=calloc(m->global_count?m->global_count:1,sizeof *globals);
        if(!registers||!locals||!globals){free(used);free(primitive);free(reachable);free(map);free(registers);free(locals);free(globals);bpp_value_facts_destroy(&facts);return 0;}
        for(size_t b=0;b<fn->block_count;++b) {
            bpp_value_facts_begin_block(&facts,b,registers,locals,globals);
            for(size_t k=0;k<fn->blocks[b].count;++k) {
                BLIRInst *i=&fn->blocks[b].insts[k];long long a=0,c=0,v=0;int constant=0;
                if((i->op==BL_IR_LOAD_LOCAL||i->op==BL_IR_LOAD_GLOBAL)&&!m->preserve_access_observability){
                    const BPPValueFact *slot=i->op==BL_IR_LOAD_LOCAL?(i->a<fn->local_count?&locals[i->a]:NULL):(i->a<m->global_count?&globals[i->a]:NULL);
                    if(bpp_value_fact_integer(slot,&v))constant=1;
                } else if(i->op==BL_IR_NEG&&i->a<n&&bpp_value_fact_integer(&registers[i->a],&a)&&a!=LLONG_MIN){v=-a;constant=1;}
                else if(i->op==BL_IR_BIT_NOT&&i->a<n&&bpp_value_fact_integer(&registers[i->a],&a)){constant=bpp_integer_fold(i->op,a,0,&v);}
                else if(i->op==BL_IR_NOT&&i->a<n&&registers[i->a].truth!=BPP_TRUTH_UNKNOWN){v=registers[i->a].truth==BPP_TRUTH_TRUE?0:1;constant=1;}
                else if(((i->op>=BL_IR_ADD&&i->op<=BL_IR_GE)||i->op==BL_IR_BIT_AND||i->op==BL_IR_BIT_OR||i->op==BL_IR_BIT_XOR)&&i->a<n&&i->b<n&&bpp_value_fact_integer(&registers[i->a],&a)&&bpp_value_fact_integer(&registers[i->b],&c))constant=bpp_integer_fold(i->op,a,c,&v);
                if(constant){i->op=BL_IR_INT;i->integer=v;i->a=i->b=i->c=BL_IR_NONE;}
                if(i->op==BL_IR_BRANCH&&i->a<n&&registers[i->a].truth!=BPP_TRUTH_UNKNOWN){i->a=registers[i->a].truth==BPP_TRUTH_TRUE?i->b:i->c;i->op=BL_IR_JUMP;i->b=i->c=BL_IR_NONE;}
                bpp_value_facts_transfer(&facts,i,registers,locals,globals);
            }
        }
        reachable[0]=1;int changed;
        do {changed=0;for(size_t b=0;b<fn->block_count;++b)if(reachable[b]){
            BLIRInst *end=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t next[2]={BL_IR_NONE,BL_IR_NONE};
            if(end->op==BL_IR_JUMP)next[0]=end->a;
    if(end->op==BL_IR_BRANCH){next[0]=end->b;next[1]=end->c;}
            for(unsigned j=0;j<2;++j)if(next[j]!=BL_IR_NONE&&!reachable[next[j]]){reachable[next[j]]=1;changed=1;}
        }}while(changed);
        size_t count=0;
        for(size_t b=0;b<fn->block_count;++b)if(reachable[b])map[b]=count++;
        /* First record which definitions are proven primitive on normal
         * continuation.  This is ownership information, not a claim that an
         * instruction is free of errors. */
        for(size_t b=0;b<fn->block_count;++b) if(reachable[b])
            for(size_t k=0;k<fn->blocks[b].count;++k) {
                BLIRInst *i=&fn->blocks[b].insts[k];
                if(i->dst!=BL_IR_NONE&&i->dst<n) primitive[i->dst]=(unsigned char)dce_produces_primitive(i->op);
            }
        /* Mark from observable roots backwards through their operands.  A
         * pure result is retained only if a retained instruction needs it.
         * This fixed point removes whole chains such as !x == 0 when the
         * result is unused, while retaining loads, arithmetic checks, calls,
         * stores, releases of possibly-owned values, and terminators. */
        do {changed=0;
            for(size_t b=0;b<fn->block_count;++b) if(reachable[b])
                for(size_t k=0;k<fn->blocks[b].count;++k) {
                    BLIRInst *i=&fn->blocks[b].insts[k];
                    if(dce_is_root(i,primitive,n)||(i->dst!=BL_IR_NONE&&i->dst<n&&used[i->dst]))
                        changed|=operands(i,used);
                }
        } while(changed);
        size_t dest=0;
        for(size_t b=0;b<fn->block_count;++b){BLIRBlock block=fn->blocks[b];
            if(!reachable[b]){for(size_t k=0;k<block.count;++k){free(block.insts[k].text);free(block.insts[k].args);}free(block.insts);continue;}
            size_t keep=0;
            for(size_t k=0;k<block.count;++k){BLIRInst i=block.insts[k];
                if(!dce_is_root(&i,primitive,n)&&
                   (i.dst==BL_IR_NONE||i.dst>=n||!used[i.dst])) {
                    free(i.text);free(i.args);continue;
                }
                if(i.op==BL_IR_JUMP)i.a=map[i.a];
    if(i.op==BL_IR_BRANCH){i.b=map[i.b];i.c=map[i.c];}
                block.insts[keep++]=i;
            }
            block.count=keep;
    fn->blocks[dest++]=block;
        }fn->block_count=count;
        free(used);free(primitive);free(reachable);free(map);free(registers);free(locals);free(globals);bpp_value_facts_destroy(&facts);
        if(!eliminate_redundant_accesses(m,fn)) {
            snprintf(m->error,sizeof m->error,"%s: optimizer redundant-access pass allocation failed in function '%s'",m->path?m->path:"<input>",fn->name?fn->name:"<module>");
            return 0;
        }
        if(!bl_ir_validate_pass(m,"optimizer function folding and CFG cleanup"))return 0;
        if(!cfg_optimize(m,fn)) {
            if (!m->error[0]) snprintf(m->error,sizeof m->error,"%s: optimizer CFG pass allocation failed in function '%s'",m->path?m->path:"<input>",fn->name?fn->name:"<module>");
            return 0;
        }
    }
    if(!bl_ir_validate_pass(m,"optimizer before unreachable-procedure elimination"))return 0;
    /* A static-library assembly unit exports every named procedure.  Its
     * roots are external archive consumers rather than calls from module 0. */
    if(m->emit_library)return bl_ir_validate_pass(m,"optimizer library export retention");
    /* Native procedures are private; retain only those reachable from module entry. */
    unsigned char *reachable=calloc(m->function_count,1);
    size_t *map=calloc(m->function_count,sizeof *map);
    if(!reachable||!map){free(reachable);free(map);return 0;}
    reachable[0]=1;int changed;
    do{changed=0;for(size_t f=0;f<m->function_count;++f)if(reachable[f])for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){
        BLIRInst *i=&m->functions[f].blocks[b].insts[k];
        if(i->op==BL_IR_CALL&&i->a!=BL_IR_NONE&&!reachable[i->a]){reachable[i->a]=1;changed=1;}
    }}while(changed);
    size_t count=0;for(size_t f=0;f<m->function_count;++f)if(reachable[f])map[f]=count++;
    size_t dest=0;
    for(size_t f=0;f<m->function_count;++f){BLIRFunction fn=m->functions[f];
        if(!reachable[f]){
            for(size_t b=0;b<fn.block_count;++b){for(size_t k=0;k<fn.blocks[b].count;++k){free(fn.blocks[b].insts[k].text);free(fn.blocks[b].insts[k].args);}free(fn.blocks[b].insts);}
            for(size_t j=0;j<fn.local_count;++j){free(fn.locals[j]);if(fn.local_types)free(fn.local_types[j]);}
            free(fn.local_types);free(fn.name);free(fn.locals);free(fn.blocks);continue;
        }
        for(size_t b=0;b<fn.block_count;++b)for(size_t k=0;k<fn.blocks[b].count;++k){BLIRInst *i=&fn.blocks[b].insts[k];if(i->op==BL_IR_CALL&&i->a!=BL_IR_NONE)i->a=map[i->a];}
        m->functions[dest++]=fn;
    }
    m->function_count=count;free(reachable);free(map);
    return bl_ir_validate_pass(m,"optimizer unreachable-procedure elimination");
}
