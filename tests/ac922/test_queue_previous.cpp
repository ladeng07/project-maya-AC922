#include "ds4_pool.hpp"
#include <cassert>
#include <fcntl.h>
#include <cstdlib>
#include <sys/mman.h>
using namespace strata::ds4;
int main(){
 char name[]="/tmp/maya-hotmap-test-XXXXXX";int fd=mkstemp(name);assert(fd>=0);unlink(name);
 const size_t size=4*1024*1024,off=123,len=65537,guard=64,lead=off%os::kDirectAlign;
 std::vector<uint8_t> src(size);for(size_t i=0;i<size;++i)src[i]=(i*17+i/32)%251;
 assert(write(fd,src.data(),size)==(ssize_t)size);
 auto* mapped=(uint8_t*)mmap(nullptr,size,PROT_READ,MAP_SHARED,fd,0);assert(mapped!=MAP_FAILED);
 auto* cold=(uint8_t*)mmap(nullptr,size,PROT_READ,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(cold!=MAP_FAILED);
 assert(os::resident(mapped+off,len));assert(!os::resident(cold,len));
 IoQueue q(8);
 for(bool diag:{false,true})for(bool hot:{false,true}){
  q.diag_begin(diag);
  const int n=7;std::vector<std::vector<uint8_t>> dst(n,std::vector<uint8_t>(len+8192,0xA5));
  std::vector<uint8_t> keep(len,0);std::atomic<int> left{n};std::atomic<bool> bad{false};IoQueue::Job jobs[n];
  for(int i=0;i<n;++i){auto& j=jobs[i];j.hot_map=hot;j.fd=fd;j.off=off;j.len=len;j.map=mapped+off;j.dst=dst[i].data()+guard;j.left=&left;j.bad=&bad;}
  jobs[0].keep=keep.data(); // hot/cold copies use the same lead and keep convention.
  jobs[1].map=nullptr; // no mapping falls back.
  jobs[2].map=cold; // not resident falls back without touching the mapping.
  jobs[3].map=(const void*)1; // failed mincore falls back.
  jobs[4].fd=os::kBadFd; // invalid file fails, never masks failure via hot mapping.
  jobs[5].crows=1024;jobs[5].cpitch=64;jobs[5].coff=16;jobs[5].clen=32;
  jobs[6].mem=src.data()+off; // existing RAM-copy branch keeps dst (no lead).
  q.push(jobs,n);while(left.load(std::memory_order_acquire))q.wait_any(100);
  assert(bad.load());
  for(int i=0;i<4;++i){assert(!memcmp(dst[i].data()+guard+lead,src.data()+off,len));
   for(size_t x=0;x<guard+lead;++x)assert(dst[i][x]==0xA5);
   for(size_t x=guard+lead+len;x<dst[i].size();++x)assert(dst[i][x]==0xA5);}
  assert(!memcmp(keep.data(),src.data()+off,len));
  for(auto x:dst[4])assert(x==0xA5);
  for(int r=0;r<1024;++r)assert(!memcmp(dst[5].data()+guard+lead+r*32,src.data()+off+r*64+16,32));
  assert(!memcmp(dst[6].data()+guard,src.data()+off,len));
  if(diag)q.diag_print(hot?1:0);
 }
 // Many differently aligned reads, concurrent workers and repeated destination reuse.
 for(bool hot:{false,true})for(int round=0;round<40;++round){
  std::vector<std::vector<uint8_t>> dst(32,std::vector<uint8_t>(131072+8192,0xA5));
  std::atomic<int> left{32};std::atomic<bool> bad{false};IoQueue::Job jobs[32];
  for(int i=0;i<32;++i){auto& j=jobs[i];j.hot_map=hot;j.fd=fd;j.off=(round*713+i*977)%1048576;j.len=65536+i*127;j.map=mapped+j.off;j.dst=dst[i].data()+guard;j.left=&left;j.bad=&bad;}
  q.push(jobs,32);while(left.load())q.wait_any(100);assert(!bad.load());
  for(int i=0;i<32;++i){auto& j=jobs[i];size_t a=guard+j.off%os::kDirectAlign;
   assert(!memcmp(dst[i].data()+a,src.data()+j.off,j.len));
   for(size_t x=0;x<a;++x)assert(dst[i][x]==0xA5);
   for(size_t x=a+j.len;x<dst[i].size();++x)assert(dst[i][x]==0xA5);}
 }
 munmap(mapped,size);munmap(cold,size);close(fd);
 puts("HOT_MAP_QUEUE_EXACT_PASS hot_and_off=1 nonaligned=1 keep=1 compact=1 fallback_null_cold_failed_check=1 invalid_fd=1 concurrent_jobs=2560 canaries=1");
}
