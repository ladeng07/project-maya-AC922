#pragma once

#include "ds4_host.hpp"
#include <cuda_runtime.h>
#include <functional>
#include <map>
#include <system_error>

namespace strata::ds4 {

// Adapted in concept from Strata-AC922's cuda_host_alloc_near_gpu:
// https://github.com/eelgaev/Strata-AC922/blob/8eef05e18c48f5d509928079f7f75c46310109af/src/core/pinned.cu
// Keep Maya's existing staging capacities and cudaFreeHost ownership. Never register
// existing pages. A helper thread owns allocation/first touch; the caller keeps its pin.
class NumaStaging {
public:
    struct Place { int node = -1; std::vector<int> cpus; };

    NumaStaging() {
        const char* value = std::getenv("DS4_NUMA_STAGE");
        if (value && (std::strcmp(value, "1") == 0 || std::strcmp(value, "2") == 0)) mode_ = value[0] - '0';
#if defined(__linux__) && !defined(STRATA_USE_HIP)
        CPU_ZERO(&allowed_);
        allowed_ok_ = sched_getaffinity(0, sizeof allowed_, &allowed_) == 0;
#else
        mode_ = 0;
#endif
    }
    int mode() const { return mode_; }

    Place place(int device) const {
        Place result;
#if defined(__linux__) && !defined(STRATA_USE_HIP)
        if (!mode_ || !allowed_ok_) return result;
        char bus[64] = {};
        unsigned domain = 0, b = 0, d = 0, function = 0;
        if (cudaDeviceGetPCIBusId(bus, sizeof bus, device) != cudaSuccess ||
            std::sscanf(bus, "%x:%x:%x.%x", &domain, &b, &d, &function) != 4) return result;
        char path[128];
        std::snprintf(path, sizeof path, "/sys/bus/pci/devices/%04x:%02x:%02x.%x/numa_node", domain, b, d, function);
        std::istringstream node_file(host::read_file(path));
        if (!(node_file >> result.node) || result.node < 0) return Place{};
        for (int cpu : host::cpu_list(host::read_file("/sys/devices/system/node/node" + std::to_string(result.node) + "/cpulist")))
            if (cpu >= 0 && cpu < CPU_SETSIZE && CPU_ISSET(cpu, &allowed_)) result.cpus.push_back(cpu);
        // CPU-less GPU memory nodes are never valid staging destinations. Respect an
        // explicit caller memory bind as well as its CPU affinity; only narrow either.
        if (result.cpus.empty()) return Place{};
        const auto online = host::cpu_list(host::read_file("/sys/devices/system/node/online"));
        constexpr size_t bits = sizeof(unsigned long) * 8;
        const size_t maxnode = online.empty() ? 1024 : (size_t)*std::max_element(online.begin(), online.end()) + 1;
        std::vector<unsigned long> mask((maxnode + bits - 1) / bits, 0);
        int policy = 0;
        if (syscall(SYS_get_mempolicy, &policy, mask.data(), mask.size() * bits, nullptr, 0) == 0 &&
            (policy & 0x3fff) == 2 /* MPOL_BIND */ &&
            ((size_t)result.node / bits >= mask.size() || !(mask[(size_t)result.node / bits] & (1ul << ((size_t)result.node % bits)))))
            return Place{};
#endif
        return result;
    }

    static bool bind(const Place& place) {
#if defined(__linux__) && !defined(STRATA_USE_HIP)
        if (place.cpus.empty()) return false;
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (int cpu : place.cpus) CPU_SET(cpu, &mask);
        return pthread_setaffinity_np(pthread_self(), sizeof mask, &mask) == 0;
#else
        (void)place;
        return false;
#endif
    }

    static bool bind_memory(const Place& place) {
#if defined(__linux__) && !defined(STRATA_USE_HIP)
        if (place.node < 0) return false;
        constexpr size_t bits = sizeof(unsigned long) * 8;
        std::vector<unsigned long> mask((size_t)place.node / bits + 1, 0);
        mask[(size_t)place.node / bits] = 1ul << ((size_t)place.node % bits);
        // Thread-local allocation policy, not mbind/migration of registered pages.
        // The short-lived helper owns it; the caller's memory policy is unchanged.
        return syscall(SYS_set_mempolicy, 2 /* MPOL_BIND */, mask.data(), mask.size() * bits) == 0;
#else
        (void)place;
        return false;
#endif
    }

    std::function<void()> reader_init(int device) const {
        if (mode_ < 2) return {};
        const Place target = place(device);
        if (target.cpus.empty()) return {};
        return [target, device] {
            if (!bind(target))
                std::fprintf(stderr, "ds4: NUMA staging reader GPU%d could not bind CPU node %d\n", device, target.node);
        };
    }

    static std::map<int, int> sample_nodes(void* ptr, size_t bytes) {
        std::map<int, int> result;
#if defined(__linux__) && !defined(STRATA_USE_HIP)
        if (!ptr || !bytes) return result;
        const size_t page = os::system_page_size();
        const uintptr_t base = (uintptr_t)ptr & ~(uintptr_t)(page - 1);
        const size_t pages = (bytes + ((uintptr_t)ptr - base) + page - 1) / page;
        const size_t count = std::min<size_t>(16, pages);
        std::vector<void*> addresses(count);
        std::vector<int> status(count, -1);
        for (size_t i = 0; i < count; ++i)
            addresses[i] = (void*)(base + (count == 1 ? 0 : i * (pages - 1) / (count - 1)) * page);
        if (syscall(SYS_move_pages, 0, count, addresses.data(), nullptr, status.data(), 0) < 0)
            result[-errno] = (int)count;
        else for (int node : status) ++result[node];
#else
        (void)ptr; (void)bytes;
#endif
        return result;
    }

    cudaError_t alloc(void** ptr, size_t bytes, unsigned flags, int device, const char* role) const {
        if (!mode_) return cudaHostAlloc(ptr, bytes, flags);
        const Place target = place(device);
        if (target.cpus.empty()) {
            std::fprintf(stderr, "ds4: NUMA staging GPU%d %s has no allowed local CPU node; retaining ordinary allocation\n", device, role);
            return cudaHostAlloc(ptr, bytes, flags);
        }
        cudaError_t error = cudaSuccess;
        bool pinned = false;
        try {
            std::thread([&] {
                pinned = bind(target) && bind_memory(target);
                if (!pinned) return;
                error = cudaSetDevice(device);
                if (error == cudaSuccess) error = cudaHostAlloc(ptr, bytes, flags);
                if (error == cudaSuccess) std::memset(*ptr, 0, bytes);
            }).join();
        } catch (const std::system_error&) {
            pinned = false;
        }
        if (!pinned) {
            std::fprintf(stderr, "ds4: NUMA staging GPU%d %s local CPU/memory policy unavailable; retaining ordinary allocation\n", device, role);
            return cudaHostAlloc(ptr, bytes, flags);
        }
        if (error == cudaSuccess) {
            const auto nodes = sample_nodes(*ptr, bytes);
            std::string sampled;
            for (const auto& entry : nodes) sampled += " " + std::to_string(entry.first) + ":" + std::to_string(entry.second);
            std::fprintf(stderr, "ds4: NUMA staging GPU%d role=%s node=%d bytes=%zu cpus=%zu mode=%d sampled=%s\n",
                         device, role, target.node, bytes, target.cpus.size(), mode_, sampled.c_str());
        }
        return error;
    }

private:
    int mode_ = 0;
#if defined(__linux__) && !defined(STRATA_USE_HIP)
    cpu_set_t allowed_{};
    bool allowed_ok_ = false;
#endif
};

} // namespace strata::ds4
