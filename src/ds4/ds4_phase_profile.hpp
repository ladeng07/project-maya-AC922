#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cuda_runtime.h>
namespace strata::ds4::phase {
inline bool enabled() { static const bool v = std::getenv("DS4_PHASE_CONTROL") != nullptr; return v; }
inline bool requested() {
    const char* p = std::getenv("DS4_PHASE_CONTROL");
    if (!p) return false;
    FILE* f = std::fopen(p, "rb"); if (!f) return false;
    const int v = std::fgetc(f); std::fclose(f); return v == '1';
}
inline int64_t ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
enum Key { EG_LOCK, EG_LOOKUP, EG_IO_WAIT, EG_PUT_LOCK, EG_PUT, EG_FINISH, EG_TOTAL,
           EG_REUSE, EG_READY, FLOW_PUMP, FLOW_IO_WAIT, FLOW_STAGE_WAIT, COPY_ENQUEUE, LAYER_ENQUEUE, COUNT };
inline const char* names[COUNT] = {"eg_lock", "eg_lookup", "eg_io_wait", "eg_put_lock", "eg_put", "eg_finish", "eg_total",
    "eg_reuse", "eg_ready", "flow_pump", "flow_io_wait", "flow_stage_wait", "copy_enqueue", "layer_enqueue"};
struct Host {
    std::atomic<int64_t> values[COUNT]{};
    void clear() { for (auto& v : values) v.store(0, std::memory_order_relaxed); }
    void add(Key k, int64_t n) { values[k].fetch_add(n, std::memory_order_relaxed); }
    void print(int dev, int tokens) const {
        std::fprintf(stderr, "ds4 phase host dev=%d tokens=%d", dev, tokens);
        for (int i=0;i<COUNT;++i) std::fprintf(stderr," %s_ms=%.3f",names[i],values[i].load(std::memory_order_relaxed)/1e6);
        std::fprintf(stderr,"\n");
    }
};
struct Timer {
    Host* dst; Key key; int64_t start;
    Timer(Host& h, Key k, bool on) : dst(on ? &h : nullptr), key(k), start(on ? ns() : 0) {}
    ~Timer() { if (dst) dst->add(key,ns()-start); }
};
enum GKey { COPY_CHAIN, RING_READY, HANDOFF, GCOUNT };
inline const char* gnames[GCOUNT] = {"copy_chain_sample64", "ring_wait_sample16", "handoff"};
struct Mark { cudaEvent_t a=nullptr,b=nullptr; int key=0; };
struct Gpu {
    std::vector<Mark> marks;
    unsigned copy_seq=0,ring_seq=0;
    void clear() { copy_seq=ring_seq=0; }
    void collect(int dev,int tokens) {
        double sum[GCOUNT]={},max[GCOUNT]={}; unsigned n[GCOUNT]={}; int failures=0;
        for (auto& m:marks) {
            float ms=0;
            if (m.a && m.b && cudaEventSynchronize(m.b)==cudaSuccess && cudaEventElapsedTime(&ms,m.a,m.b)==cudaSuccess) {
                sum[m.key]+=ms;if(ms>max[m.key])max[m.key]=ms;++n[m.key];
            } else ++failures;
            if(m.a)cudaEventDestroy(m.a);if(m.b)cudaEventDestroy(m.b);
        }
        marks.clear();
        for(int i=0;i<GCOUNT;++i)std::fprintf(stderr,"ds4 phase gpu dev=%d tokens=%d kind=%s samples=%u sum_ms=%.3f max_ms=%.3f failures=%d\n",dev,tokens,gnames[i],n[i],sum[i],max[i],failures);
    }
};
struct GpuTimer {
    Gpu* dst; cudaStream_t stream; Mark mark;
    GpuTimer(Gpu& g,GKey k,cudaStream_t s,bool on):dst(on?&g:nullptr),stream(s) {
        if(!dst)return;mark.key=k;
        if(cudaEventCreate(&mark.a)!=cudaSuccess || cudaEventCreate(&mark.b)!=cudaSuccess || cudaEventRecord(mark.a,s)!=cudaSuccess) {
            if(mark.a)cudaEventDestroy(mark.a);if(mark.b)cudaEventDestroy(mark.b);dst=nullptr;
        }
    }
    ~GpuTimer() { if(dst){cudaEventRecord(mark.b,stream);dst->marks.push_back(mark);} }
};
}
