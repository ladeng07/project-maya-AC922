#pragma once
#include "ds4_os.hpp"
#include <fstream>
#include <limits>

namespace strata::ds4::host {

inline std::string read_file(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

inline std::vector<int> cpu_list(const std::string& text) {
    std::vector<int> result;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (part.empty() || part == "\n") continue;
        const size_t dash = part.find('-');
        const int first = std::atoi(part.c_str());
        const int last = dash == std::string::npos ? first : std::atoi(part.c_str() + dash + 1);
        for (int value = first; value <= last; ++value) result.push_back(value);
    }
    return result;
}

struct Topology {
    std::vector<int> ids;
    std::vector<std::vector<os::Core>> cores;
};

inline Topology topology() {
    Topology result;
#if defined(_WIN32)
    result.cores = os::node_core_threads();
    for (size_t i = 0; i < result.cores.size(); ++i) result.ids.push_back((int)i);
#else
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    const bool have_mask = sched_getaffinity(0, sizeof allowed, &allowed) == 0;
    const auto usable = [&](int cpu) {
        return cpu >= 0 && cpu < CPU_SETSIZE && (!have_mask || CPU_ISSET(cpu, &allowed));
    };
    const auto pcores = cpu_list(read_file("/sys/devices/cpu_core/cpus"));
    const bool hybrid = !pcores.empty() && !read_file("/sys/devices/cpu_atom/cpus").empty();
    // Node IDs are sparse on AC922 (CPU nodes 0 and 8, GPU memory nodes 250..255).
    for (int node : cpu_list(read_file("/sys/devices/system/node/online"))) {
        std::vector<os::Core> cores;
        std::vector<int> seen;
        for (int cpu : cpu_list(read_file("/sys/devices/system/node/node" + std::to_string(node) + "/cpulist"))) {
            if (!usable(cpu)) continue;
            auto siblings = cpu_list(read_file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list"));
            if (siblings.empty()) siblings = {cpu};
            if (std::find(seen.begin(), seen.end(), siblings.front()) != seen.end()) continue;
            seen.push_back(siblings.front());
            os::Core core;
            for (int sibling : siblings) if (usable(sibling)) core.cpus.push_back(sibling);
            if (core.cpus.empty()) continue;
            core.eff = hybrid && std::find(pcores.begin(), pcores.end(), core.cpus.front()) != pcores.end() ? 1 : 0;
            cores.push_back(core);
        }
        if (!cores.empty()) {
            result.ids.push_back(node);
            result.cores.push_back(std::move(cores));
        }
    }
    if (result.cores.empty() && have_mask) {
        std::vector<os::Core> cores;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &allowed)) cores.push_back({{cpu}, 0});
        if (!cores.empty()) { result.ids.push_back(-1); result.cores.push_back(std::move(cores)); }
    }
#endif
    if (result.cores.empty()) {
        std::vector<os::Core> cores;
        for (unsigned cpu = 0; cpu < std::max(1u, std::thread::hardware_concurrency() / 2); ++cpu)
            cores.push_back({{(int)cpu}, 0});
        result.ids.push_back(-1);
        result.cores.push_back(std::move(cores));
    }
    return result;
}

inline int part_node(const std::vector<int>& ids, int part, int parts) {
    if (ids.empty() || part < 0) return -1;
    // lane_cfgs splits the first socket's cores when more parts than sockets are requested.
    if (parts > (int)ids.size()) return ids.front();
    return part < (int)ids.size() ? ids[(size_t)part] : -1;
}

inline uint64_t node_mem_kb(const std::string& text, const std::string& name) {
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto pos = line.find(name + ":");
        if (pos != std::string::npos) return std::strtoull(line.c_str() + pos + name.size() + 1, nullptr, 10);
    }
    return 0;
}

// POWER NVLink GPU memory is exposed as CPU-less Linux NUMA nodes. Before CUDA fills
// it, its free bytes inflate MemAvailable and must not budget private CPU expert copies.
inline uint64_t power_host_available(uint64_t available) {
#if defined(__powerpc64__) && !defined(_WIN32)
    if (available == ~uint64_t{0}) return available;
    uint64_t cpu_total = 0, device_free = 0;
    for (int node : cpu_list(read_file("/sys/devices/system/node/online"))) {
        const std::string path = "/sys/devices/system/node/node" + std::to_string(node);
        const std::string memory = read_file(path + "/meminfo");
        if (cpu_list(read_file(path + "/cpulist")).empty())
            device_free += node_mem_kb(memory, "MemFree") << 10;
        else cpu_total += node_mem_kb(memory, "MemTotal") << 10;
    }
    if (cpu_total > 0)
        return std::min(cpu_total, available > device_free ? available - device_free : uint64_t{0});
#endif
    return available;
}

} // namespace strata::ds4::host
