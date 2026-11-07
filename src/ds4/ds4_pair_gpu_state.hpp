#pragma once
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace strata::ds4 {
// One persistent prefix bank. A second saved prefix lives only transiently in
// scratch that is dead between pair_state calls; restoring the old bank promotes
// the pending prefix before the next attention/shared-expert work can reuse it.
class PairGpuState {
public:
    struct Span {void* data;size_t bytes;};
private:
    std::vector<Span> scratch_,aliased_bank_;
    size_t reused_available_=0,reused_used_=0,allocated_=0;
    static std::vector<Span> unique_spans(std::vector<Span> spans) {
        std::sort(spans.begin(),spans.end(),[](const Span& a,const Span& b){return (uintptr_t)a.data<(uintptr_t)b.data;});
        std::vector<Span> out;
        for(const auto& s:spans){
            if(!s.data || !s.bytes)continue;
            const uintptr_t begin=(uintptr_t)s.data,end=begin+s.bytes;
            if(end<begin)return {};
            if(!out.empty() && begin<=(uintptr_t)out.back().data+out.back().bytes){
                auto& tail=out.back();tail.bytes=std::max((uintptr_t)tail.data+tail.bytes,end)-(uintptr_t)tail.data;
            }else out.push_back(s);
        }
        return out;
    }
    static bool disjoint(const std::vector<Span>& a,const std::vector<Span>& b) {
        for(auto x:a)for(auto y:b)if((uintptr_t)x.data<(uintptr_t)y.data+y.bytes && (uintptr_t)y.data<(uintptr_t)x.data+x.bytes)return false;
        return true;
    }
    uint8_t* bank_=nullptr;
    size_t cap_=0,borrowed_=0,sizes_[2]={};
    int device_=-1,owner_=-1,pending_=-1;
    uint64_t d2d_=0,avoided_=0,puts_=0,gets_=0,promotions_=0;
    bool copy(void* dst,const void* src,size_t n,cudaStream_t stream,std::string& err) {
        if(!n)return true;
        const auto rc=cudaMemcpyAsync(dst,src,n,cudaMemcpyDeviceToDevice,stream);
        if(rc!=cudaSuccess){err=std::string("pair GPU state copy: ")+cudaGetErrorString(rc);return false;}
        d2d_+=n;return true;
    }
    bool scratch_copy(void* contiguous,size_t n,bool to_scratch,cudaStream_t stream,std::string& err) {
        size_t off=0;
        for(const auto& span:scratch_) {
            const size_t take=std::min(span.bytes,n-off);
            if(!copy(to_scratch?span.data:(uint8_t*)contiguous+off,
                     to_scratch?(uint8_t*)contiguous+off:span.data,take,stream,err))return false;
            off+=take;if(off==n)return true;
        }
        err="pair GPU state scratch too small";return false;
    }
    bool bank_copy(void* h,size_t n,bool put,cudaStream_t stream,std::string& err) {
        if(aliased_bank_.empty())return copy(put?(void*)bank_:h,put?h:(void*)bank_,n,stream,err);
        size_t off=0;
        for(const auto& span:aliased_bank_){
            const size_t take=std::min(span.bytes,n-off);
            if(!copy(put?span.data:(uint8_t*)h+off,put?(uint8_t*)h+off:span.data,take,stream,err))return false;
            off+=take;if(off==n)return true;
        }
        err="aliased bank too small";return false;
    }
    bool promote(size_t n,cudaStream_t stream,std::string& err) {
        if(aliased_bank_.empty())return scratch_copy(bank_,n,false,stream,err);
        size_t si=0,so=0,di=0,dof=0,left=n;
        while(left){
            if(si>=scratch_.size() || di>=aliased_bank_.size()){err="aliased promotion overflow";return false;}
            const auto& src=scratch_[si];const auto& dst=aliased_bank_[di];
            const size_t take=std::min({left,src.bytes-so,dst.bytes-dof});
            if(!copy((uint8_t*)dst.data+dof,(uint8_t*)src.data+so,take,stream,err))return false;
            left-=take;so+=take;dof+=take;
            if(so==src.bytes){++si;so=0;}if(dof==dst.bytes){++di;dof=0;}
        }
        return true;
    }
public:
    PairGpuState()=default;
    PairGpuState(const PairGpuState&)=delete;
    PairGpuState& operator=(const PairGpuState&)=delete;
    ~PairGpuState(){if(bank_){int old=0;cudaGetDevice(&old);cudaSetDevice(device_);cudaFree(bank_);cudaSetDevice(old);}}
    bool init(int device,size_t maximum,std::vector<Span> scratch,size_t reserve=128ull<<20) {
        device_=device;scratch_=std::move(scratch);
        for(const auto& s:scratch_)borrowed_+=s.bytes;
        size_t available=0,total=0;
        if(cudaMemGetInfo(&available,&total)!=cudaSuccess){cudaGetLastError();return false;}
        constexpr size_t quantum=16ull<<20;
        cap_=std::min({maximum,borrowed_,available>reserve?available-reserve:0})/quantum*quantum;
        if(!cap_)return false;
        if(cudaMalloc((void**)&bank_,cap_)!=cudaSuccess){cudaGetLastError();bank_=nullptr;cap_=0;return false;}
        std::fprintf(stderr,"ds4 gpu-state setup dev=%d prefix_bytes=%zu borrowed_bytes=%zu free_before=%zu reserve_bytes=%zu\n",
                     device_,cap_,borrowed_,available,reserve);
        return true;
    }
    // The original allocation/init path stays unchanged. In alias mode only,
    // coalesce overlapping scratch views and keep reclaimed holes disjoint from it.
    bool init_alias(int device,size_t maximum,std::vector<Span> scratch,std::vector<Span> holes,size_t reserve=128ull<<20) {
        device_=device;scratch_=unique_spans(std::move(scratch));holes=unique_spans(std::move(holes));
        if(scratch_.empty() || holes.empty() || !disjoint(scratch_,holes))return false;
        for(const auto& span:scratch_)borrowed_+=span.bytes;
        for(const auto& span:holes)reused_available_+=span.bytes;
        maximum=std::min(maximum,borrowed_);
        size_t left=maximum;
        for(auto span:holes){
            if(!left)break;span.bytes=std::min(span.bytes,left);
            aliased_bank_.push_back(span);left-=span.bytes;reused_used_+=span.bytes;
        }
        if(!reused_used_)return false;
        size_t available=0,total=0;
        if(cudaMemGetInfo(&available,&total)!=cudaSuccess){cudaGetLastError();available=0;}
        const size_t room=available>reserve?available-reserve:0;
        constexpr size_t quantum=16ull<<20;
        allocated_=left<=room?left:room/quantum*quantum;
        if(allocated_ && cudaMalloc((void**)&bank_,allocated_)!=cudaSuccess){cudaGetLastError();bank_=nullptr;allocated_=0;}
        if(allocated_)aliased_bank_.push_back({bank_,allocated_});
        cap_=reused_used_+allocated_;
        std::fprintf(stderr,"ds4 gpu-state setup dev=%d prefix_bytes=%zu borrowed_bytes=%zu free_before=%zu reserve_bytes=%zu\n",device_,cap_,borrowed_,available,reserve);
        std::fprintf(stderr,"ds4 alias-state setup dev=%d reclaimed_bytes=%zu reused_bytes=%zu allocated_bytes=%zu unique_scratch_bytes=%zu\n",device_,reused_available_,reused_used_,allocated_,borrowed_);
        return true;
    }
    size_t capacity()const{return cap_;}
    bool empty()const{return owner_<0 && pending_<0;}
    bool transfer(int slot,void* h,size_t bytes,bool put,cudaStream_t stream,size_t& prefix,std::string& err) {
        prefix=std::min(cap_,bytes);
        if(slot<0 || slot>1 || !prefix){err="invalid GPU state transfer";return false;}
        if(put) {
            if(owner_==slot || pending_>=0){err="GPU state slot overwritten while live";return false;}
            sizes_[slot]=prefix;
            if(owner_<0) {
                if(!bank_copy(h,prefix,true,stream,err))return false;
                owner_=slot;
            } else {
                if(!scratch_copy(h,prefix,true,stream,err))return false;
                pending_=slot;
            }
            ++puts_;
        } else {
            if(owner_!=slot || sizes_[slot]!=prefix){err="GPU state restore out of order or wrong size";return false;}
            if(!bank_copy(h,prefix,false,stream,err))return false;
            owner_=-1;++gets_;
            if(pending_>=0) {
                if(!promote(sizes_[pending_],stream,err))return false;
                owner_=pending_;pending_=-1;++promotions_;
            }
        }
        avoided_+=prefix;
        return true;
    }
    void report()const {
        std::fprintf(stderr,"ds4 gpu-state result dev=%d prefix_bytes=%zu borrowed_bytes=%zu avoided_host_bytes=%llu d2d_bytes=%llu puts=%llu gets=%llu promotions=%llu empty=%d\n",
            device_,cap_,borrowed_,(unsigned long long)avoided_,(unsigned long long)d2d_,
            (unsigned long long)puts_,(unsigned long long)gets_,(unsigned long long)promotions_,int(empty()));
    }
};
} // namespace strata::ds4
