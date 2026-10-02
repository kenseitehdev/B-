#include "../include/regalloc.h"
#include <stdlib.h>
#include <string.h>

const unsigned char bpp_payload_regs[BPP_PAYLOAD_REG_COUNT] = {23, 25, 27, 28};

static void *array(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return calloc(n ? n : 1, size);
}
static int has(const uint64_t *bits, size_t r) { return (int)((bits[r / 64] >> (r % 64)) & 1); }
static void set(uint64_t *bits, size_t r) { bits[r / 64] |= UINT64_C(1) << (r % 64); }
static void clear(uint64_t *bits, size_t r) { bits[r / 64] &= ~(UINT64_C(1) << (r % 64)); }
static size_t definition(const BLIRInst *i) {
    return i->op == BL_IR_RELEASE ? i->a : i->dst;
}
static int native_payload_use(const BLIRInst *i, size_t r) {
    switch (i->op) {
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_BRANCH: return r == i->a;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        return r == i->a || r == i->b;
    case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL:
        return r == i->b;
    default: return 0;
    }
}
static void uses(const BLIRInst *i, uint64_t *bits) {
    switch (i->op) {
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER:
    case BL_IR_RELEASE: case BL_IR_BRANCH: case BL_IR_RETURN:
        if (i->a != BL_IR_NONE) set(bits, i->a);
        break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
    case BL_IR_INDEX_GET: case BL_IR_INDEX_SET:
        set(bits, i->a); set(bits, i->b);
        if (i->op == BL_IR_INDEX_SET) set(bits, i->c);
        break;
    case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL:
        set(bits, i->b); break;
    case BL_IR_CALL:
        for (size_t j = 0; j < i->arg_count; ++j) set(bits, i->args[j]);
        break;
    default: break;
    }
}
static void edge(uint64_t *graph, size_t words, size_t a, size_t b) {
    if (a == b) return;
    set(graph + a * words, b); set(graph + b * words, a);
}
void bpp_reg_plan_destroy(BPPRegPlan *p) {
    free(p->physical); free(p->live_in); free(p->live_out); memset(p, 0, sizeof *p);
}
int bpp_reg_live_in(const BPPRegPlan *p, size_t block, size_t reg) {
    return has(p->live_in + block * p->words, reg);
}
int bpp_reg_plan_create(const BLIRFunction *fn, BPPRegPlan *p) {
    memset(p, 0, sizeof *p);
    size_t n = fn->register_count, blocks = fn->block_count;
    if (n > SIZE_MAX - 63) return 0;
    size_t words = (n + 63) / 64;
    if (words && (blocks > SIZE_MAX / words || n > SIZE_MAX / words)) return 0;
    p->register_count = n; p->block_count = blocks; p->words = words;
    p->physical = array(n, 1);
    p->live_in = array(blocks * words, sizeof(uint64_t));
    p->live_out = array(blocks * words, sizeof(uint64_t));
    uint64_t *use = array(blocks * words, sizeof(uint64_t));
    uint64_t *def = array(blocks * words, sizeof(uint64_t));
    uint64_t *temp = array(words, sizeof(uint64_t));
    uint64_t *live = array(words, sizeof(uint64_t));
    uint64_t *graph = array(n * words, sizeof(uint64_t));
    size_t *weight = array(n, sizeof(size_t));
    unsigned char *visited = array(n, 1);
    int ok = p->physical && p->live_in && p->live_out && use && def && temp && live && graph && weight && visited;
    if (!ok) goto done;
    /* Block use-before-def sets, with release modeled as a null definition. */
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t k = 0; k < fn->blocks[b].count; ++k) {
            const BLIRInst *i = &fn->blocks[b].insts[k];
            memset(temp, 0, words * sizeof *temp); uses(i, temp);
            for (size_t w = 0; w < words; ++w) use[b * words + w] |= temp[w] & ~def[b * words + w];
            for (size_t r = 0; r < n; ++r) if (has(temp, r) && native_payload_use(i, r)) ++weight[r];
            size_t d = definition(i); if (d != BL_IR_NONE) set(def + b * words, d);
        }
    }
    int changed;
    do {
        changed = 0;
        for (size_t b = blocks; b-- > 0;) {
            memset(temp, 0, words * sizeof *temp);
            const BLIRInst *end = &fn->blocks[b].insts[fn->blocks[b].count - 1];
            size_t successors[2] = {BL_IR_NONE, BL_IR_NONE};
            if (end->op == BL_IR_JUMP) successors[0] = end->a;
            if (end->op == BL_IR_BRANCH) { successors[0] = end->b; successors[1] = end->c; }
            for (size_t j = 0; j < 2; ++j) if (successors[j] != BL_IR_NONE)
                for (size_t w = 0; w < words; ++w) temp[w] |= p->live_in[successors[j] * words + w];
            for (size_t w = 0; w < words; ++w) {
                uint64_t in = use[b * words + w] | (temp[w] & ~def[b * words + w]);
                if (p->live_in[b * words + w] != in || p->live_out[b * words + w] != temp[w]) changed = 1;
                p->live_in[b * words + w] = in; p->live_out[b * words + w] = temp[w];
            }
        }
    } while (changed);
    for (size_t b = 0; b < blocks; ++b) {
        /* Entry-live values need distinct payload registers even for non-SSA IR. */
        for (size_t a = 0; a < n; ++a) if (bpp_reg_live_in(p, b, a))
            for (size_t c = a + 1; c < n; ++c) if (bpp_reg_live_in(p, b, c)) edge(graph, words, a, c);
        memcpy(live, p->live_out + b * words, words * sizeof *live);
        for (size_t k = fn->blocks[b].count; k-- > 0;) {
            const BLIRInst *i = &fn->blocks[b].insts[k];
            size_t d = definition(i);
            if (d != BL_IR_NONE) {
                /* Even a dead definition must not clobber a live payload. */
                for (size_t r = 0; r < n; ++r) if (has(live, r)) edge(graph, words, d, r);
                clear(live, d);
            }
            uses(i, live);
        }
    }
    /* Greedy weighted allocation; uncolored values retain stack loads. */
    for (size_t j = 0; j < n; ++j) {
        size_t best = BL_IR_NONE;
        for (size_t r = 0; r < n; ++r) if (!visited[r] && (best == BL_IR_NONE || weight[r] > weight[best])) best = r;
        if (best == BL_IR_NONE || !weight[best]) break;
        visited[best] = 1;
        unsigned mask = 0;
        for (size_t r = 0; r < n; ++r) if (has(graph + best * words, r) && p->physical[r])
            for (unsigned color = 0; color < BPP_PAYLOAD_REG_COUNT; ++color)
                if (p->physical[r] == bpp_payload_regs[color]) mask |= 1u << color;
        for (unsigned color = 0; color < BPP_PAYLOAD_REG_COUNT; ++color)
            if (!(mask & (1u << color))) { p->physical[best] = bpp_payload_regs[color]; break; }
        if (p->physical[best]) ++p->cached_count;
    }
done:
    free(use); free(def); free(temp); free(live); free(graph); free(weight); free(visited);
    if (!ok) bpp_reg_plan_destroy(p);
    return ok;
}
