// bench3.cpp  (bench2 + x3d + locality benchmark)
//
// Windows (MSVC): cl /O2 /std:c++17 /EHsc bench3.cpp
// Linux (GCC):    g++ -O2 -std=c++17 bench3.cpp -o bench3 -pthread
//
// Hardware counters (Linux):
//   perf stat -e L1-dcache-load-misses,LLC-load-misses ./bench3

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "pmalloc_win.hpp"
#else
#include "pmalloc_posix.hpp"
#endif
#include "pmalloc_x3d.hpp"

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// ---------------------------------------------------------------- allocators

struct Pm {
    static constexpr const char* NAME = "pmalloc";
    NOINLINE static void* alloc(std::size_t sz) { return pmalloc::pm_malloc(sz); }
    NOINLINE static void free(void* p, std::size_t) { pmalloc::pm_free(p); }
};

struct X3d {
    static constexpr const char* NAME = "pmalloc + x3d (round to 64)";
    NOINLINE static void* alloc(std::size_t sz) { return x3d::pm_alloc_x3d(sz); }
    NOINLINE static void free(void* p, std::size_t) { x3d::pm_free_x3d(p); }
};

struct Sys {
    static constexpr const char* NAME = "system (malloc/free)";
    NOINLINE static void* alloc(std::size_t sz) { return std::malloc(sz); }
    NOINLINE static void free(void* p, std::size_t) { std::free(p); }
};

// ---------------------------------------------------------------- rng / helpers

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<uint32_t>(s >> 11);
    }
};

template <typename T>
inline void black_box(T& value) {
#if defined(_MSC_VER)
    volatile T* p = &value;
    (void)*p;
#else
    __asm__ volatile("" : "+r"(value));
#endif
}

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define COMPILER_BARRIER() _ReadWriteBarrier()
#else
#define COMPILER_BARRIER() __asm__ volatile("" ::: "memory")
#endif

static volatile uint64_t g_sink;  // consume results so the loops cannot be dropped

using clk = std::chrono::steady_clock;
static double secs(clk::time_point start) {
    return std::chrono::duration<double>(clk::now() - start).count();
}

// ---------------------------------------------------------------- basic benchmarks (as before)

template <typename H>
std::pair<double, size_t> t_pairs(size_t n, size_t sz) {
    auto start = clk::now();
    for (size_t i = 0; i < n; ++i) {
        size_t s = sz;
        black_box(s);
        void* p = H::alloc(s);
        if (p) {
            *static_cast<volatile char*>(p) = 1;
            black_box(p);
            H::free(p, s);
        }
    }
    return {secs(start), n * 2};
}

template <typename H>
std::pair<double, size_t> t_mixed(size_t n, uint64_t seed, uint32_t min_sz, uint32_t span) {
    constexpr size_t SLOTS = 10000;
    struct Slot { void* p; size_t sz; };
    std::vector<Slot> slots(SLOTS, {nullptr, 0});
    Rng r(seed);
    size_t ops = 0;

    auto start = clk::now();
    for (size_t i = 0; i < n; ++i) {
        size_t k = r.next() % SLOTS;
        if (slots[k].p) {
            H::free(slots[k].p, slots[k].sz);
            slots[k] = {nullptr, 0};
            ops++;
        }
        size_t s = min_sz + r.next() % span;
        void* p = H::alloc(s);
        if (p) {
            *static_cast<volatile char*>(p) = 1;
            slots[k] = {p, s};
            ops++;
        }
    }
    double elapsed = secs(start);
    for (auto& slot : slots)
        if (slot.p) H::free(slot.p, slot.sz);
    return {elapsed, ops};
}

template <typename H>
std::pair<double, double> t_remote(size_t n, size_t sz) {
    std::vector<void*> addrs(n);
    for (size_t i = 0; i < n; ++i) addrs[i] = H::alloc(sz);

    auto start_free = clk::now();
    std::thread free_t([&]() {
        for (void* p : addrs)
            if (p) H::free(p, sz);
    });
    free_t.join();
    double free_elapsed = secs(start_free);

    auto start_realloc = clk::now();
    std::vector<void*> again(n);
    for (size_t i = 0; i < n; ++i) again[i] = H::alloc(sz);
    double realloc_elapsed = secs(start_realloc);

    for (void* p : again)
        if (p) H::free(p, sz);
    return {free_elapsed, realloc_elapsed};
}

// ---------------------------------------------------------------- locality benchmark (NEW)
//
// N objects of OBJ bytes, each "used" by reading its first and last word.
// Working sets: 1 / 16 / 64 / 200 MB (L2 / L3 / V-Cache edge / RAM).
//
//   dense        : std::vector<Obj>, the baseline (no pointers)
//   <alloc> seq  : vector<void*> in allocation order  (like vector<unique_ptr<T>>)
//   <alloc> shuf : same pointers shuffled             (fragmented heap / pointer chasing)
//   cross-line   : % of objects that straddle a 64-byte cache line

constexpr size_t OBJ = 48;  // not a multiple of 64 on purpose: x3d rounds it up to 64

struct Obj { uint64_t v[OBJ / 8]; };

static inline uint64_t touch(const void* p) {
    auto* q = static_cast<const uint64_t*>(p);
    return q[0] + q[OBJ / 8 - 1];
}

static void init_obj(void* p, size_t i) {
    auto* q = static_cast<uint64_t*>(p);
    for (size_t k = 0; k < OBJ / 8; ++k) q[k] = i * 3 + k;
}

// ns per object, out = checksum of one pass
NOINLINE static double traverse(const std::vector<void*>& v, size_t reps, uint64_t& out) {
    uint64_t sum = 0;
    for (void* p : v) sum += touch(p);  // warm-up pass
    sum = 0;
    auto start = clk::now();
    for (size_t r = 0; r < reps; ++r) {
        COMPILER_BARRIER();  // forbid hoisting loads out of the rep loop
        for (void* p : v) sum += touch(p);
    }
    double e = secs(start);
    out = sum / reps;
    return e / (double(reps) * double(v.size())) * 1e9;
}

NOINLINE static double traverse_dense(const std::vector<Obj>& v, size_t reps, uint64_t& out) {
    uint64_t sum = 0;
    for (const Obj& o : v) sum += touch(&o);
    sum = 0;
    auto start = clk::now();
    for (size_t r = 0; r < reps; ++r) {
        const Obj* d = v.data();
        black_box(d);
        COMPILER_BARRIER();  // otherwise clang folds the whole rep loop to one pass
        for (size_t i = 0; i < v.size(); ++i) sum += touch(&d[i]);
    }
    double e = secs(start);
    out = sum / reps;
    return e / (double(reps) * double(v.size())) * 1e9;
}

static void report(const char* name, const std::vector<void*>& ptrs, size_t reps) {
    size_t cross = 0;
    for (void* p : ptrs)
        if ((reinterpret_cast<uintptr_t>(p) & 63u) + OBJ > 64) ++cross;

    uint64_t s1 = 0, s2 = 0;
    double t_seq = traverse(ptrs, reps, s1);

    std::vector<void*> sh = ptrs;
    std::mt19937_64 g(12345);
    std::shuffle(sh.begin(), sh.end(), g);
    double t_shuf = traverse(sh, reps, s2);

    if (s1 != s2) std::printf("  !! checksum mismatch for %s\n", name);
    g_sink = s1 + s2;
    std::printf("  %-30s seq %7.2f ns   shuf %7.2f ns   cross-line %5.1f%%\n",
                name, t_seq, t_shuf, 100.0 * double(cross) / double(ptrs.size()));
}

template <typename H>
static void locality_alloc(size_t n, size_t reps) {
    std::vector<void*> ptrs(n);
    for (size_t i = 0; i < n; ++i) {
        void* p = H::alloc(OBJ);
        if (!p) { std::fprintf(stderr, "alloc failed\n"); std::abort(); }
        init_obj(p, i);
        ptrs[i] = p;
    }
    report(H::NAME, ptrs, reps);
    for (void* p : ptrs) H::free(p, OBJ);
}

static void locality_pool(size_t n, size_t reps) {
    if (!x3d::pl.x_pull<OBJ>(static_cast<std::uint64_t>(n) * OBJ) || x3d::pl.cap < n) {
        std::printf("  x3d::pl pool: not enough memory, skipped\n");
        x3d::pl.clear();
        return;
    }
    std::vector<void*> ptrs(n);
    for (size_t i = 0; i < n; ++i) {
        ptrs[i] = x3d::pl.get(i);
        init_obj(ptrs[i], i);
    }
    report("x3d::pl pool (blocks 64B)", ptrs, reps);
    x3d::pl.clear();
}

static void run_locality() {
    std::printf("\n== locality: %zu-byte objects ==\n", OBJ);
    const size_t sets_mb[] = {1, 16, 64, 200};
    constexpr size_t TOTAL_TOUCHES = 30'000'000;

    for (size_t mb : sets_mb) {
        size_t n = mb * 1024 * 1024 / OBJ;
        size_t reps = std::max<size_t>(2, TOTAL_TOUCHES / n);
        std::printf("\n-- working set %zu MB (%zu objects, %zu passes) --\n", mb, n, reps);

        {
            std::vector<Obj> d(n);
            for (size_t i = 0; i < n; ++i) init_obj(&d[i], i);
            uint64_t s = 0;
            double t = traverse_dense(d, reps, s);
            g_sink = s;
            std::printf("  %-30s seq %7.2f ns\n", "dense std::vector<Obj>", t);
        }
        locality_alloc<Pm>(n, reps);
        locality_alloc<X3d>(n, reps);
        locality_pool(n, reps);
        locality_alloc<Sys>(n, reps);
    }
}

// ---------------------------------------------------------------- correctness

static void check(void* p, size_t sz, uint8_t tag) {
    auto* s = static_cast<uint8_t*>(p);
    for (size_t i = 0; i < sz; ++i) {
        if (s[i] != tag) {
            std::cerr << "memory corrupted (size " << sz << ")\n";
            std::abort();
        }
    }
}

static void selftest() {
    const size_t sizes[] = {1, 15, 16, 17, 100, 1000, 8191, 8192, 8193, 20000, 100000, 1 << 20, 3 << 20};

    for (size_t round = 0; round < 3; ++round) {
        struct Live { void* p; size_t sz; uint8_t tag; };
        std::vector<Live> live;
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            for (size_t j = 0; j < 3; ++j) {
                uint8_t tag = static_cast<uint8_t>(((i * 3 + j + round) & 0xFF) | 1);
                void* p = pmalloc::pm_malloc(sizes[i]);
                if (!p) { std::cerr << "alloc failed\n"; std::abort(); }
                if (reinterpret_cast<uintptr_t>(p) % 16 != 0) { std::cerr << "misaligned\n"; std::abort(); }
                std::memset(p, tag, sizes[i]);
                live.push_back({p, sizes[i], tag});
            }
        }
        for (auto& item : live) {
            check(item.p, item.sz, item.tag);
            pmalloc::pm_free(item.p);
        }
    }

    {
        struct LiveSmall { void* p; uint8_t tag; };
        std::vector<LiveSmall> v;
        for (size_t i = 0; i < 20000; ++i) {
            void* p = pmalloc::pm_malloc(48);
            if (!p) std::abort();
            uint8_t tag = static_cast<uint8_t>(i & 0xFF);
            std::memset(p, tag, 48);
            v.push_back({p, tag});
        }
        for (auto it = v.rbegin(); it != v.rend(); ++it) {
            check(it->p, 48, it->tag);
            pmalloc::pm_free(it->p);
        }
    }

    {
        std::vector<void*> addrs(1000);
        std::thread t([&]() {
            for (size_t i = 0; i < 1000; ++i) {
                void* p = pmalloc::pm_malloc(64);
                std::memset(p, 0xAB, 64);
                addrs[i] = p;
            }
        });
        t.join();
        for (void* a : addrs) {
            check(a, 64, 0xAB);
            pmalloc::pm_free(a);
        }
        std::vector<void*> again(100000);
        for (size_t i = 0; i < 100000; ++i) again[i] = pmalloc::pm_malloc(64);
        for (void* p : again) pmalloc::pm_free(p);
    }

    // NEW: x3d must give 64-byte aligned blocks for sz > 32 (up to the small-class limit)
    {
        const size_t xs[] = {33, 48, 64, 65, 100, 238, 1000, 5000, 8000};
        for (size_t sz : xs) {
            std::vector<void*> v;
            for (int i = 0; i < 2000; ++i) {
                void* p = x3d::pm_alloc_x3d(sz);
                if (!p) std::abort();
                if (reinterpret_cast<uintptr_t>(p) % 64 != 0) {
                    std::cerr << "x3d: block not 64-aligned (size " << sz << ")\n";
                    std::abort();
                }
                std::memset(p, 0x5A, sz);
                v.push_back(p);
            }
            for (void* p : v) { check(p, sz, 0x5A); x3d::pm_free_x3d(p); }
        }
    }

    std::cout << "selftest ok\n";
}

// ---------------------------------------------------------------- main

static double mops(size_t ops, double t) { return double(ops) / t / 1e6; }

template <typename H>
void run_bench() {
    std::printf("\n== %s ==\n", H::NAME);

    auto [t1, ops1] = t_pairs<H>(5000000, 32);
    std::printf("pairs 32B x5M:       %8.1f ms (%6.1f Mops/s)\n", t1 * 1e3, mops(ops1, t1));

    auto [t2, ops2] = t_mixed<H>(2000000, 42, 16, 2033);
    std::printf("mixed 16..2KB x2M:   %8.1f ms (%6.1f Mops/s)\n", t2 * 1e3, mops(ops2, t2));

    auto [f, a] = t_remote<H>(1000000, 64);
    std::printf("remote 64B x1M:      free %7.1f ms (%6.1f Mops/s), realloc %7.1f ms (%6.1f Mops/s)\n",
                f * 1e3, mops(1000000, f), a * 1e3, mops(1000000, a));
}

int main() {
    std::cout << "pmalloc C++ benchmark starting...\n";
    x3d::init(true);
    std::printf("cpu: AMD=%d X3D=%d align64=%d\n",
                int(x3d::f.u8AMD_CPU), int(x3d::f.u8AMD_CPU_X3D_ENABLE), int(x3d::f.u8AMD_CPU_ALIGN_MEMORY_PATTERN));

    selftest();

    run_bench<Pm>();
    run_bench<X3d>();
    run_bench<Sys>();

    run_locality();

    x3d::pl.clear();

    auto s = pmalloc::pm_stats();
    std::printf("\nStats { os_bytes: %zu, os_peak: %zu, os_maps: %llu, os_unmaps: %llu, large_allocs: %llu, large_hits: %llu, large_cached_bytes: %zu }\n",
                s.os_bytes, s.os_peak_bytes,
                static_cast<unsigned long long>(s.os_maps),
                static_cast<unsigned long long>(s.os_unmaps),
                static_cast<unsigned long long>(s.large_allocs),
                static_cast<unsigned long long>(s.large_cache_hits),
                s.large_cache_bytes);

    std::cout << "Done!\n";
    return 0;
}