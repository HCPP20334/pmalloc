// pmalloc benchmark: median of N runs, RSS, and pm_stats() metrics.
//
//   Linux:   g++ -O2 -std=c++17 -DUSE_PM [-DPM_STATS] bench.cpp -o bench -pthread
//            g++ -O2 -std=c++17 bench.cpp -o bench_crt -pthread               (glibc)
//            g++ -O2 -std=c++17 -DUSE_MI bench.cpp -lmimalloc -o bench_mi -pthread
//   Windows: cl /O2 /std:c++17 /DUSE_PM bench.cpp psapi.lib
//
// With -DPM_STATS the per-thread alloc/free counters are also printed.
// pm_stats()/pm_thread_stats() must exist in the header (see pmalloc_posix.hpp).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#define NOMINMAX
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

#if defined(USE_PM)
#if defined(_WIN32)
#include "pmalloc_win.hpp"
#else
#include "pmalloc_posix.hpp"
#endif
#define NAME "pmalloc"
NOINLINE static void* A(size_t n) { return pmalloc::pm_malloc(n); }
NOINLINE static void  F(void* p) { pmalloc::pm_free(p); }
#elif defined(USE_MI)
extern "C" void* mi_malloc(size_t); extern "C" void mi_free(void*);
#define NAME "mimalloc"
NOINLINE static void* A(size_t n) { return mi_malloc(n); }
NOINLINE static void  F(void* p) { mi_free(p); }
#else
#define NAME "crt malloc"
NOINLINE static void* A(size_t n) { return std::malloc(n); }
NOINLINE static void  F(void* p) { std::free(p); }
#endif



using clk = std::chrono::steady_clock;
static double secs(clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); }

struct Rng {
    uint64_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
};

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
static size_t rss_bytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}
#else
#include <unistd.h>
static size_t rss_bytes() {
    long pages = 0, rss = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    if (std::fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
    std::fclose(f);
    return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
}
#endif

static double mb(size_t b) { return double(b) / (1024.0 * 1024.0); }


struct Snap {
    size_t rss = 0;
#if defined(USE_PM)
    pmalloc::Stats st{};
#endif
};

static Snap snap() {
    Snap s;
    s.rss = rss_bytes();
#if defined(USE_PM)
    s.st = pmalloc::pm_stats();
#endif
    return s;
}

static std::string extra(const Snap& before, const Snap& after) {
    char buf[256];
#if defined(USE_PM)
    const auto& a = after.st;
    const auto& b = before.st;
    unsigned long long maps = a.os_maps - b.os_maps;
    unsigned long long lall = a.large_allocs - b.large_allocs;
    unsigned long long lhit = a.large_cache_hits - b.large_cache_hits;
    std::snprintf(buf, sizeof buf,
                  "os %.1fM (peak %.1fM)  maps +%llu  large cache %.1fM",
                  mb(a.os_bytes), mb(a.os_peak_bytes), maps, mb(a.large_cache_bytes));
    std::string s = buf;
    if (lall) {
        std::snprintf(buf, sizeof buf, "  large hit %.0f%%", 100.0 * double(lhit) / double(lall));
        s += buf;
    }
    return s;
#else
    (void)before; (void)after; (void)buf;
    return "";
#endif
}



static double t_pairs(size_t n, size_t sz, size_t& ops) {
    ops = n * 2;
    auto t = clk::now();
    for (size_t i = 0; i < n; ++i) { void* p = A(sz); *(volatile char*)p = 1; F(p); }
    return secs(t);
}

static double t_batch(size_t rounds, size_t count, size_t sz, size_t& ops) {
    ops = rounds * count * 2;
    std::vector<void*> v(count);
    auto t = clk::now();
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < count; ++i) { v[i] = A(sz); *(volatile char*)v[i] = 1; }
        for (size_t i = 0; i < count; ++i) F(v[i]);
    }
    return secs(t);
}

static double mixed_core(size_t n, uint64_t seed, bool check, bool& ok, uint32_t min_sz, uint32_t span) {
    const size_t slots = 10000;
    std::vector<unsigned char*> p(slots, nullptr);
    std::vector<uint32_t> sz(slots, 0);
    Rng r{seed};
    auto t = clk::now();
    for (size_t i = 0; i < n; ++i) {
        size_t k = r.next() % slots;
        if (p[k]) {
            if (check) {
                unsigned char tag = (unsigned char)(k & 0xff);
                if (p[k][0] != tag || p[k][sz[k] - 1] != tag) ok = false;
            }
            F(p[k]);
        }
        uint32_t s = min_sz + r.next() % span;
        p[k] = (unsigned char*)A(s);
        sz[k] = s;
        if (check) { p[k][0] = (unsigned char)(k & 0xff); p[k][s - 1] = (unsigned char)(k & 0xff); }
        else *(volatile char*)p[k] = 1;
    }
    double d = secs(t);
    for (auto q : p) if (q) F(q);
    return d;
}

static double t_mixed(size_t n, uint32_t min_sz, uint32_t span, size_t& ops) {
    ops = n * 2;
    bool ok = true;
    return mixed_core(n, 42, false, ok, min_sz, span);
}

static int g_threads = 4;

static double t_threads(size_t per_thread, size_t& ops) {
    ops = per_thread * 2 * (size_t)g_threads;
    std::vector<std::thread> th;
    auto t = clk::now();
    for (int i = 0; i < g_threads; ++i)
        th.emplace_back([=] { bool ok = true; mixed_core(per_thread, 1234 + i, false, ok, 16, 2033); });
    for (auto& x : th) x.join();
    return secs(t);
}


static inline void spin_wait(unsigned& n) {
    if (++n > 64) { std::this_thread::yield(); n = 0; } 
}

static double t_cross(size_t n, bool& ok, size_t& ops) {
    ops = n * 2;
    constexpr size_t kRing = 1 << 12;
    std::vector<std::atomic<void*>> ring(kRing);
    for (auto& s : ring) s.store(nullptr, std::memory_order_relaxed);
    auto t = clk::now();
    std::thread prod([&] {
        for (size_t i = 0; i < n; ++i) {
            void* p = A(64);
            *(uint64_t*)p = i;
            auto& slot = ring[i & (kRing - 1)];
            for (unsigned w = 0; slot.load(std::memory_order_acquire) != nullptr;) spin_wait(w);
            slot.store(p, std::memory_order_release);
        }
    });
    std::thread cons([&] {
        for (size_t i = 0; i < n; ++i) {
            auto& slot = ring[i & (kRing - 1)];
            void* p;
            for (unsigned w = 0; (p = slot.load(std::memory_order_acquire)) == nullptr;) spin_wait(w);
            if (*(uint64_t*)p != i) ok = false;
            slot.store(nullptr, std::memory_order_release);
            F(p);
        }
    });
    prod.join(); cons.join();
    return secs(t);
}

static double t_large(size_t n, size_t sz, size_t& ops) {
    ops = n * 2;
    auto t = clk::now();
    for (size_t i = 0; i < n; ++i) { char* p = (char*)A(sz); p[0] = 1; p[sz - 1] = 1; F(p); }
    return secs(t);
}



static int g_reps = 5;

static void run(const char* name, const std::function<double(size_t&)>& fn) {
    size_t ops = 0;
    fn(ops);                                   // warm-up, not counted
    Snap before = snap();
    std::vector<double> t;
    for (int i = 0; i < g_reps; ++i) t.push_back(fn(ops));
    std::sort(t.begin(), t.end());
    double med = t[t.size() / 2];
    Snap after = snap();
    std::printf("  %-26s %8.1f ms  (min %7.1f)  %7.1f Mops/s  rss %7.1fM  %s\n",
                name, med * 1e3, t.front() * 1e3, double(ops) / med / 1e6,
                mb(after.rss), extra(before, after).c_str());
}

// Allocates a lot, frees 90% at random, then asks the allocator to give memory back.
static void fragmentation() {
    const size_t n = 800000, sz = 256;       
    std::vector<void*> v(n);
    size_t rss0 = rss_bytes();
    for (size_t i = 0; i < n; ++i) { v[i] = A(sz); *(volatile char*)v[i] = 1; }
    size_t rss_full = rss_bytes();
    Rng r{99};
    std::vector<void*> keep;
    for (size_t i = 0; i < n; ++i) {
        if (r.next() % 10 == 0) keep.push_back(v[i]);
        else F(v[i]);
    }
    size_t rss_frag = rss_bytes();
    std::printf("  fragmentation (256B x800k, keep 10%%)\n");
    std::printf("    rss: start %.1fM  full %.1fM  after free 90%% %.1fM", mb(rss0), mb(rss_full), mb(rss_frag));
#if defined(USE_PM)
    {
        auto t = pmalloc::pm_thread_stats();
        std::printf("\n    pm_thread_stats: %zu segments, live %.1fM / reserved %.1fM, utilization %.1f%%",
                    t.segments, mb(t.live_bytes), mb(t.reserved_bytes), t.utilization * 100.0);
        pmalloc::pm_collect();
        auto t2 = pmalloc::pm_thread_stats();
        auto s = pmalloc::pm_stats();
        std::printf("\n    after pm_collect: %zu segments, utilization %.1f%%, os %.1fM, rss %.1fM",
                    t2.segments, t2.utilization * 100.0, mb(s.os_bytes), mb(rss_bytes()));
    }
#endif
    std::printf("\n");
    for (void* p : keep) F(p);
}

int main(int argc, char** argv) {
    unsigned hw = std::thread::hardware_concurrency();
    g_threads = hw >= 4 ? 4 : (hw ? (int)hw : 1);
    if (argc > 1) g_reps = std::max(1, std::atoi(argv[1]));        
    if (argc > 2) g_threads = std::max(1, std::atoi(argv[2]));

    std::printf("%s   (median of %d runs after warm-up, %d threads for MT tests)\n", NAME, g_reps, g_threads);
    Snap start = snap();
    std::printf("  start: rss %.1fM\n\n", mb(start.rss));

    run("pairs 32B x10M",      [](size_t& o) { return t_pairs(10000000, 32, o); });
    run("pairs 1KB x5M",       [](size_t& o) { return t_pairs(5000000, 1024, o); });
    run("batch 100k x64B x50", [](size_t& o) { return t_batch(50, 100000, 64, o); });
    run("mixed 16..2KB x5M",   [](size_t& o) { return t_mixed(5000000, 16, 2033, o); });
    run("mixed 16B..32KB x1M", [](size_t& o) { return t_mixed(1000000, 16, 32768, o); });
    run("threads mixed 3M ea", [](size_t& o) { return t_threads(3000000, o); });
    bool cross_ok = true;
    run("cross-thread free 2M", [&](size_t& o) { return t_cross(2000000, cross_ok, o); });
    run("large 64KB x200k",    [](size_t& o) { return t_large(200000, 64 * 1024, o); });
    run("large 1MB x20k",      [](size_t& o) { return t_large(20000, 1 << 20, o); });

   
    bool ok = true;
    mixed_core(2000000, 7, true, ok, 16, 2033);
    std::printf("\n  integrity (tagged blocks)   %s\n", ok ? "ok" : "CORRUPTION");
    std::printf("  cross-thread payload        %s\n\n", cross_ok ? "ok" : "BAD");

    fragmentation();

#if defined(USE_PM)
    pmalloc::Stats s = pmalloc::pm_stats();
    std::printf("\n  final pm_stats: os %.1fM (peak %.1fM)  maps %llu  unmaps %llu  large cache %.1fM (hot %.1fM)\n",
                mb(s.os_bytes), mb(s.os_peak_bytes),
                (unsigned long long)s.os_maps, (unsigned long long)s.os_unmaps,
                mb(s.large_cache_bytes), mb(s.large_cache_hot));
    std::printf("  large: %llu allocs, %llu served from cache\n",
                (unsigned long long)s.large_allocs, (unsigned long long)s.large_cache_hits);
#ifdef PM_STATS
    auto t = pmalloc::pm_thread_stats();
    std::printf("  main thread: %llu allocs, %llu frees, %llu remote frees   exited threads: %llu / %llu / %llu\n",
                (unsigned long long)t.allocs, (unsigned long long)t.frees, (unsigned long long)t.remote_frees,
                (unsigned long long)s.exited_allocs, (unsigned long long)s.exited_frees,
                (unsigned long long)s.exited_remote_frees);
#endif
#endif
    return (ok && cross_ok) ? 0 : 1;
}