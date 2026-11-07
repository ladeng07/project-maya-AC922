#pragma once
#include <array>
#include <cstdio>
#include <map>
#include <tuple>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace strata::ds4::ioprof {
inline uint64_t wall() { timespec t{}; clock_gettime(CLOCK_MONOTONIC,&t); return uint64_t(t.tv_sec)*1000000000+t.tv_nsec; }
inline uint64_t cpu() { timespec t{}; clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t); return uint64_t(t.tv_sec)*1000000000+t.tv_nsec; }
enum Kind { READ, DIRECT, COPY, DEVICE, KEEP, COMPACT, SHARE, HOT_CHECK, HOT_COPY, NK };
inline const char* names[NK]={"pread","direct_check_read","memcpy","device_copy","keep_copy","compact","share","hot_map_check","hot_map_copy"};
struct Time { uint64_t n=0,bytes=0,wall=0,cpu=0,max=0; std::array<uint64_t,32> hist{}; };
inline void add(Time& t,uint64_t w,uint64_t c,uint64_t bytes=0) {
    ++t.n;t.bytes+=bytes;t.wall+=w;t.cpu+=c;if(w>t.max)t.max=w;
    unsigned b=0;uint64_t us=w/1000;while(us>1 && b<31){us>>=1;++b;}++t.hist[b];
}
struct alignas(128) Worker { Time queue,service,part[NK]; uint64_t failures=0,tid=0; };
template<class F> inline bool timed(Worker* s,Kind kind,uint64_t bytes,F fn) {
    if(!s)return fn();const auto w=wall(),c=cpu();const bool ok=fn();
    const auto c1=cpu(),w1=wall();add(s->part[kind],w1-w,c1-c,bytes);return ok;
}
inline void print_time(int dev,int worker,const char* kind,const Time& t) {
    std::fprintf(stderr,"ds4 iopool time dev=%d worker=%d kind=%s n=%llu bytes=%llu wall_ms=%.3f cpu_ms=%.3f max_ms=%.3f hist=",dev,worker,kind,(unsigned long long)t.n,(unsigned long long)t.bytes,t.wall/1e6,t.cpu/1e6,t.max/1e6);
    for(size_t i=0;i<t.hist.size();++i)std::fprintf(stderr,"%s%llu",i?",":"",(unsigned long long)t.hist[i]);std::fprintf(stderr,"\n");
}
struct Reuse {uint64_t count=0,bytes=0,cross=0,mask=0;int last=-1,layer=-1,expert=-1;};
struct Producer {
    Time lock,publish;
    std::map<std::tuple<uint64_t,uint64_t,uint64_t>,Reuse> keys;
    std::map<int,std::pair<uint64_t,uint64_t>> chunks;
    uint64_t peak=0;
    void record(uint64_t fd,uint64_t off,uint64_t len,int layer,int expert,int chunk) {
        auto& r=keys[std::make_tuple(fd,off,len)];
        if(r.count && r.last!=chunk)++r.cross;
        ++r.count;r.bytes+=len;r.last=chunk;r.layer=layer;r.expert=expert;
        if(chunk>=0 && chunk<64)r.mask|=uint64_t(1)<<chunk;
        auto& c=chunks[chunk];++c.first;c.second+=len;
    }
    void print(int dev)const {
        print_time(dev,-1,"push_lock",lock);print_time(dev,-1,"push_publish",publish);
        uint64_t jobs=0,bytes=0,unique=0,cross=0;
        std::map<int,std::array<uint64_t,6>> layers;
        for(const auto& kv:keys){const auto& r=kv.second;jobs+=r.count;bytes+=r.bytes;unique+=std::get<2>(kv.first);cross+=r.cross;
            auto& l=layers[r.layer];l[0]+=r.count;l[1]+=r.bytes;l[2]+=std::get<2>(kv.first);l[3]+=r.cross;++l[4];l[5]|=r.mask;
        }
        for(const auto& kv:layers){const auto& l=kv.second;std::fprintf(stderr,"ds4 iopool reuse dev=%d layer=%d jobs=%llu bytes=%llu unique_bytes=%llu cross=%llu ranges=%llu mask=%llu\n",dev,kv.first,(unsigned long long)l[0],(unsigned long long)l[1],(unsigned long long)l[2],(unsigned long long)l[3],(unsigned long long)l[4],(unsigned long long)l[5]);}
        std::fprintf(stderr,"ds4 iopool total dev=%d jobs=%llu bytes=%llu unique_ranges=%zu unique_bytes=%llu cross=%llu peak_queue=%llu\n",dev,(unsigned long long)jobs,(unsigned long long)bytes,keys.size(),(unsigned long long)unique,(unsigned long long)cross,(unsigned long long)peak);
        for(const auto& kv:chunks)std::fprintf(stderr,"ds4 iopool chunk dev=%d chunk=%d jobs=%llu bytes=%llu\n",dev,kv.first,(unsigned long long)kv.second.first,(unsigned long long)kv.second.second);
    }
};
}
