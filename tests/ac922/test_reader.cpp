#include "ds4_engram_io.hpp"
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

static std::atomic<int> fault_mode{0};
static std::mutex fd_mutex;
static std::set<int> direct_fds;
extern "C" int __real_open(const char*, int, ...);
extern "C" ssize_t __real_pread(int, void*, size_t, off_t);
extern "C" int __real_close(int);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if ((flags & O_DIRECT) && fault_mode.load() == 1) { errno = EOPNOTSUPP; return -1; }
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
    const int fd = __real_open(path, flags, mode);
    if (fd >= 0 && (flags & O_DIRECT)) { std::lock_guard<std::mutex> lock(fd_mutex); direct_fds.insert(fd); }
    return fd;
}
extern "C" ssize_t __wrap_pread(int fd, void* dst, size_t n, off_t off) {
    if (fault_mode.load() == 2) {
        std::lock_guard<std::mutex> lock(fd_mutex);
        if (direct_fds.count(fd)) { errno = EIO; return -1; }
    }
    return __real_pread(fd, dst, n, off);
}
extern "C" int __wrap_close(int fd) {
    { std::lock_guard<std::mutex> lock(fd_mutex); direct_fds.erase(fd); }
    return __real_close(fd);
}
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
using namespace strata::ds4;
int main(int argc, char** argv) {
    require(argc == 2, "fixture path required");
    std::vector<uint8_t> expected(65536 + 27);
    for (size_t i = 0; i < expected.size(); ++i) expected[i] = uint8_t((i * 73 + i / 19) % 251);
    { std::ofstream file(argv[1], std::ios::binary); file.write((char*) expected.data(), expected.size()); require(bool(file), "fixture write"); }
    const std::vector<uint64_t> offsets{0, 1, 3886, 4000, 4095, 8191, 16383, expected.size() - 210};
    for (int mode : {0, 1, 2}) {
        fault_mode.store(mode);
        EgReader reader; reader.start({argv[1]}, 4);
        EgBatch prefill, decode;
        std::vector<std::vector<uint8_t>> a(offsets.size(), std::vector<uint8_t>(210));
        auto b = a;
        for (size_t i = 0; i < offsets.size(); ++i) {
            EgRead row; row.file = 0; row.off = offsets[i]; row.n = 210; row.dst = a[i].data();
            prefill.reads.push_back(row); row.dst = b[i].data(); decode.reads.push_back(row);
        }
        prefill.prefill_direct = true;
        reader.submit(prefill); reader.submit(decode); reader.wait(prefill); reader.wait(decode);
        for (size_t i = 0; i < offsets.size(); ++i) {
            require(prefill.ok[i] == 1 && decode.ok[i] == 1, "read failed");
            require(std::memcmp(a[i].data(), expected.data() + offsets[i], 210) == 0, "prefill bytes differ");
            require(a[i] == b[i], "decode bytes differ");
        }
        const auto stats = reader.direct_stats();
        require(stats.rows == (mode == 0 ? offsets.size() : 0), "decode used direct I/O or wrong success count");
        require(stats.fallback == (mode == 0 ? 0 : offsets.size()), "wrong fallback count");
        require(mode != 1 || (stats.open_fail > 0 && stats.read_fail == 0), "open-failure path not exercised");
        require(mode != 2 || (stats.read_fail > 0 && stats.open_fail == 0), "read-failure path not exercised");
        prefill.clear(); require(!prefill.prefill_direct, "batch policy leaked after clear");
        reader.stop();
        std::cout << "PASS mode=" << mode << " exact_rows=" << offsets.size() * 2
                  << " direct=" << stats.rows << " fallback=" << stats.fallback << '\n';
    }
    std::cout << "ALL_READER_TESTS_PASSED\n";
}
