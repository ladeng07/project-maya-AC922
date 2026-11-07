#include "ds4_pool.hpp"
#include <cassert>
#include <fcntl.h>
#include <cstdlib>
#include <sys/mman.h>
#include <cstdio>
using namespace strata::ds4;
int main(){
 char name[]="/tmp/maya-downslice-test-XXXXXX";int fd=mkstemp(name);assert(fd>=0);unlink(name);
 const size_t size=16*1024*1024,guard=64;
 std::vector<uint8_t> src(size);for(size_t i=0;i<size;++i)src[i]=(i*17+i/31)%251;
 assert(write(fd,src.data(),size)==(ssize_t)size);
 auto* mapped=(uint8_t*)mmap(nullptr,size,PROT_READ,MAP_SHARED,fd,0);assert(mapped!=MAP_FAILED);
 auto* cold=(uint8_t*)mmap(nullptr,size,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(cold!=MAP_FAILED);
 assert(os::resident(mapped,size));assert(!os::resident(cold,size));
 assert(!valid_row_slice(100,0,10,0,5));assert(!valid_row_slice(100,1,0,0,5));
 assert(!valid_row_slice(100,10,10,9,2));assert(!valid_row_slice(100,11,10,0,5));
 assert(!valid_row_slice(100,1,10,SIZE_MAX,2));
 IoQueue q(8);size_t checked=0;
 for(bool diag:{false,true})for(size_t block:{size_t(66),size_t(74),size_t(82),size_t(98),size_t(110),size_t(144),size_t(176),size_t(210)})for(int member=0;member<2;++member){
  q.diag_begin(diag);const size_t rows=5120,pitch=9*block,coff=member?4*block:0,clen=(member?5:4)*block,len=rows*pitch,off=123+member*4096,lead=off%os::kDirectAlign,packed=rows*clen;
  const int n=9;std::vector<std::vector<uint8_t>> dst(n,std::vector<uint8_t>(len+8192,0xA5));
  std::vector<uint8_t> keep(len,0);std::atomic<int> left{n};std::atomic<bool> bad{false};IoQueue::Job jobs[n];
  for(int i=0;i<n;++i){auto& j=jobs[i];j.hot_map=true;j.hot_slice=true;j.fd=fd;j.off=off;j.len=len;j.map=mapped+off;j.dst=dst[i].data()+guard;j.left=&left;j.bad=&bad;j.crows=rows;j.cpitch=pitch;j.coff=coff;j.clen=clen;}
  jobs[1].map=nullptr;jobs[2].map=cold;jobs[3].map=(const void*)1;
  jobs[4].keep=keep.data();jobs[5].mem=src.data()+off;
  jobs[6].fd=os::kBadFd;jobs[7].crows=SIZE_MAX;jobs[8].hot_slice=false;
  const auto a=q.slice_stats();q.push(jobs,n);while(left.load(std::memory_order_acquire))q.wait_any(100);assert(bad.load());
  const auto z=q.slice_stats();assert(z.jobs-a.jobs==1 && z.full_bytes-a.full_bytes==len && z.packed_bytes-a.packed_bytes==packed && z.fallback-a.fallback==7);
  for(int i=0;i<n;++i){
   if(i==6 || i==7){for(auto v:dst[i])assert(v==0xA5);continue;}
   size_t base=guard+(i==5?0:lead);
   for(size_t r=0;r<rows;++r)assert(!memcmp(dst[i].data()+base+r*clen,src.data()+off+r*pitch+coff,clen));
   for(size_t x=0;x<base;++x)assert(dst[i][x]==0xA5);
   const size_t touched=i==0?packed:len;
   for(size_t x=base+touched;x<dst[i].size();++x)assert(dst[i][x]==0xA5);
   ++checked;
  }
  assert(!memcmp(keep.data(),src.data()+off,len));
 }
 q.diag_begin(false);
 // Reuse destinations with different offsets and slices while eight workers are active.
 for(int round=0;round<40;++round){
  const int n=32;std::vector<std::vector<uint8_t>> dst(n,std::vector<uint8_t>(262144,0xA5));
  std::atomic<int> left{n};std::atomic<bool> bad{false};IoQueue::Job jobs[n];
  for(int i=0;i<n;++i){auto& j=jobs[i];j.hot_map=true;j.hot_slice=true;j.fd=fd;j.off=(round*713+i*977)%1048576;j.crows=127+i;j.cpitch=990;j.coff=(i%2)*440;j.clen=i%2?550:440;j.len=j.crows*j.cpitch;j.map=mapped+j.off;j.dst=dst[i].data()+guard;j.left=&left;j.bad=&bad;}
  q.push(jobs,n);while(left.load(std::memory_order_acquire))q.wait_any(100);assert(!bad.load());
  for(int i=0;i<n;++i){const auto& j=jobs[i];size_t base=guard+j.off%os::kDirectAlign;
   for(size_t r=0;r<j.crows;++r)assert(!memcmp(dst[i].data()+base+r*j.clen,src.data()+j.off+r*j.cpitch+j.coff,j.clen));
   for(size_t x=0;x<base;++x)assert(dst[i][x]==0xA5);
   for(size_t x=base+j.crows*j.clen;x<dst[i].size();++x)assert(dst[i][x]==0xA5);++checked;
  }
 }
 munmap(mapped,size);munmap(cold,size);close(fd);
 printf("DOWN_SLICE_QUEUE_PASS checked=%zu unequal4_5_blocks=1 byte_canaries=1 preserved_keep=1 invalid_bounds_rejected=1 fallback_null_cold_bad_mapping=1 bad_fd=1 existing_ram=1 concurrent_jobs=1280\n",checked);
}
