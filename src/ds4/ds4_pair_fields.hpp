#pragma once
// Only MoE state that must survive while the adjacent chunk runs attention/routing.
// Scratch GEMM buffers and the original expert/staging rings remain shared.
#define DS4_PAIR_FIELDS(X) \
 X(float,x,Z*D) X(uint16_t,x16,Z*D) X(float,acc,Z*D) \
    X(long long,acc64,chunk_par()?Z*D:0) \
 X(float,rw,Z*kPicks) X(int,row_tok,Z*kPicks) X(int,pos,Z*kPicks) \
 X(int,bounds,2048) X(float,f_pre,Z*8) X(float,f_post,Z*8) X(float,f_comb,Z*64)
struct PairView {
#define DS4_PAIR_DECLARE(type,name,count) type* name=nullptr;
 DS4_PAIR_FIELDS(DS4_PAIR_DECLARE)
#undef DS4_PAIR_DECLARE
};
