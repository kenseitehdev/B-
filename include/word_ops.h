#ifndef BPP_WORD_OPS_H
#define BPP_WORD_OPS_H
#include "ir.h"
#include <stdint.h>
#include <limits.h>
/* Defined conversion of a 64-bit bit pattern to its signed word value. */
static inline long long bpp_word_from_bits(uint64_t bits){
    return bits<=(uint64_t)LLONG_MAX?(long long)bits:-1-(long long)(UINT64_MAX-bits);
}
static inline long long bpp_word_bitop(BLIROp op,long long a,long long b){
    uint64_t x=(uint64_t)a,y=(uint64_t)b;
    switch(op){
    case BL_IR_BIT_AND:return bpp_word_from_bits(x&y);
    case BL_IR_BIT_OR:return bpp_word_from_bits(x|y);
    case BL_IR_BIT_XOR:return bpp_word_from_bits(x^y);
    case BL_IR_BIT_NOT:return bpp_word_from_bits(~x);
    default:return 0;
    }
}
#endif
