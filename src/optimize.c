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
    case BL_IR_NEG:case BL_IR_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_BRANCH:case BL_IR_RETURN:case BL_IR_RELEASE:USE(i->a);
    break;
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
    case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:
    case BL_IR_INDEX_GET:case BL_IR_INDEX_SET:USE(i->a);USE(i->b);
    if(i->op==BL_IR_INDEX_SET)USE(i->c);
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
    case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD: case BL_IR_NOT:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE:
    case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET:
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
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_BRANCH: case BL_IR_RETURN: case BL_IR_RELEASE: REP(a); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET: REP(a); REP(b); break;
    case BL_IR_INDEX_SET: REP(a); REP(b); REP(c); break;
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
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_BRANCH: case BL_IR_RETURN: case BL_IR_RELEASE: MARK(a); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET: MARK(a); MARK(b); break;
    case BL_IR_INDEX_SET: MARK(a); MARK(b); MARK(c); break;
    case BL_IR_CALL: for (size_t j = 0; j < i->arg_count; ++j) { BLIRId r_ = i->args[j]; if (r_ != BL_IR_NONE && r_ < n && definition_block[r_] != block) cross_block[r_] = 1; } break;
    default: break;
    }
#undef MARK
}

/* A deliberately local forwarding pass.  It only forwards exact primitive
 * integers after their required source check has executed.  Object loads keep
 * clone/release semantics, and debug or sanitizer builds retain all source
 * accesses so hooks and stepping remain observable. */
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
    const int access_elision = !m->instrument_native && !m->preserve_access_observability;
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
                else if(i->op==BL_IR_NOT&&i->a<n&&registers[i->a].truth!=BPP_TRUTH_UNKNOWN){v=registers[i->a].truth==BPP_TRUTH_TRUE?0:1;constant=1;}
                else if(i->op>=BL_IR_ADD&&i->op<=BL_IR_GE&&i->a<n&&i->b<n&&bpp_value_fact_integer(&registers[i->a],&a)&&bpp_value_fact_integer(&registers[i->b],&c))constant=bpp_integer_fold(i->op,a,c,&v);
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
    }
    if(!bl_ir_validate_pass(m,"optimizer before unreachable-procedure elimination"))return 0;
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
            for(size_t j=0;j<fn.local_count;++j)free(fn.locals[j]);
            free(fn.name);free(fn.locals);free(fn.blocks);continue;
        }
        for(size_t b=0;b<fn.block_count;++b)for(size_t k=0;k<fn.blocks[b].count;++k){BLIRInst *i=&fn.blocks[b].insts[k];if(i->op==BL_IR_CALL&&i->a!=BL_IR_NONE)i->a=map[i->a];}
        m->functions[dest++]=fn;
    }
    m->function_count=count;free(reachable);free(map);
    return bl_ir_validate_pass(m,"optimizer unreachable-procedure elimination");
}
