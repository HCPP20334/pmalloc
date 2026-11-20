// bench_cmp.cpp - pmalloc vs jemalloc vs tcmalloc (gperftools) vs mimalloc vs glibc.
//
// One allocator per binary (-DUSE_PM / -DUSE_JE / -DUSE_TC / -DUSE_MI / nothing = glibc).
// Every scenario is meant to run in its OWN process (--only <name>), so the reported
// peak RSS belongs to that scenario alone (getrusage ru_maxrss), not to the tests before it.
// run.sh does that and prints a comparison table.
//
//   ./bench_pm --list
//   ./bench_pm --only pairs32 --reps 7 --threads 4
//   ./bench_pm --check            # integrity + cross-thread payload (exit code 1 on failure)
//
// Output line (machine readable):
//   RESULT <scenario> <allocator> <median_ms> <min_ms> <mops> <peak_rss_mb> <note>

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

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <sys/resource.h>
#endif

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

// ---------------------------------------------------------------- allocator under test
// All allocators are called through NOINLINE wrappers with a direct API call, so none of them
// gets inlined into the benchmark loop (pmalloc is header-only, the others are libraries).
#if defined(USE_PM)
#ifdef _WIN32
#include "pmalloc_win.hpp"
#else
#include "pmalloc_posix.hpp"
#endif
#define NAME "pmalloc"
NOINLINE static void* A(size_t n) { return pmalloc::pm_malloc(n); }
NOINLINE static void  F(void* p)  { pmalloc::pm_free(p); }
#elif defined(USE_JE)
extern "C" void* mallocx(size_t, int);
extern "C" void  dallocx(void*, int);
#define NAME "jemalloc"
NOINLINE static void* A(size_t n) { return mallocx(n, 0); }
NOINLINE static void  F(void* p)  { dallocx(p, 0); }
#elif defined(USE_TC)
extern "C" void* tc_malloc(size_t);
extern "C" void  tc_free(void*);
#define NAME "tcmalloc"
NOINLINE static void* A(size_t n) { return tc_malloc(n); }
NOINLINE static void  F(void* p)  { tc_free(p); }
#elif defined(USE_MI)
extern "C" void* mi_malloc(size_t);
extern "C" void  mi_free(void*);
#define NAME "mimalloc"
NOINLINE static void* A(size_t n) { return mi_malloc(n); }
NOINLINE static void  F(void* p)  { mi_free(p); }
#else
#define NAME "glibc"
NOINLINE static void* A(size_t n) { return std::malloc(n); }
NOINLINE static void  F(void* p)  { std::free(p); }
#endif

// ---------------------------------------------------------------- helpers
using clk = std::chrono::steady_clock;
static double secs(clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); }

struct Rng {
    uint64_t s;
    uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)(s >> 11); }
};

static int g_threads = 4;
static int g_reps = 5;

static double peak_rss_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return double(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return double(ru.ru_maxrss) / 1024.0;
#endif
}

static inline void touch_all(void* p, size_t n) {   // one write per page + last byte
    volatile char* c = static_cast<volatile char*>(p);
    for (size_t i = 0; i < n; i += 4096) c[i] = 1;
    c[n - 1] = 1;
}

// ---------------------------------------------------------------- scenarios
// each returns elapsed seconds and sets ops (alloc + free counted separately)

static double s_pairs(size_t n, size_t sz, size_t& ops) {
    ops = n * 2;
    auto t = clk::now();
    for (size_t i = 0; i < n; ++i) { void* p = A(sz); *(volatile char*)p = 1; F(p); }
    return secs(t);
}

static double s_batch(size_t rounds, size_t count, size_t sz, size_t& ops) {
    ops = rounds * count * 2;
    std::vector<void*> v(count);
    auto t = clk::now();
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < count; ++i) { v[i] = A(sz); *(volatile char*)v[i] = 1; }
        for (size_t i = 0; i < count; ++i) F(v[i]);
    }
    return secs(t);
}

// random sizes in [min_sz, min_sz+span), 10k live slots; optional tag check
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

static double s_mixed(size_t n, uint32_t min_sz, uint32_t span, size_t& ops) {
    ops = n * 2;
    bool ok = true;
    return mixed_core(n, 42, false, ok, min_sz, span);
}

// N independent threads, each with its own mixed workload (thread-local fast path scaling)
static double s_mt_mixed(size_t per_thread, size_t& ops) {
    ops = per_thread * 2 * (size_t)g_threads;
    std::vector<std::thread> th;
    auto t = clk::now();
    for (int i = 0; i < g_threads; ++i)
        th.emplace_back([=] { bool ok = true; mixed_core(per_thread, 1234 + i, false, ok, 16, 2033); });
    for (auto& x : th) x.join();
    return secs(t);
}

// REMOTE FREE THROUGHPUT: owner thread allocates `count` blocks, then g_threads other threads
// free slices of them in parallel (every free is cross-thread), then the owner allocates again
// (it has to reclaim what was remotely freed). This is what the old ring test did NOT measure.
static double s_xfree_batch(size_t rounds, size_t count, size_t sz, size_t& ops) {
    ops = rounds * count * 2;
    std::vector<void*> v(count);
    auto t = clk::now();
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < count; ++i) { v[i] = A(sz); *(volatile char*)v[i] = 1; }
        std::vector<std::thread> th;
        for (int k = 0; k < g_threads; ++k)
            th.emplace_back([&, k] {
                size_t lo = count * (size_t)k / (size_t)g_threads, hi = count * (size_t)(k + 1) / (size_t)g_threads;
                for (size_t i = lo; i < hi; ++i) F(v[i]);
            });
        for (auto& x : th) x.join();
    }
    return secs(t);
}

// PRODUCER/CONSUMER through a ring: bound by the hand-off itself, so allocators look alike.
// Kept only as a sanity check that a remote free path does not collapse; do not read speed from it.
static inline void spin_wait(unsigned& n) { if (++n > 64) { std::this_thread::yield(); n = 0; } }
static double s_xfree_ring(size_t n, bool& ok, size_t& ops) {
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

// large blocks: touch2 = first+last byte only (measures allocator bookkeeping / cache),
//               full  = write every page (measures what a real user pays, incl. page faults on fresh memory)
static double s_large(size_t n, size_t sz, bool full, size_t& ops) {
    ops = n * 2;
    auto t = clk::now();
    for (size_t i = 0; i < n; ++i) {
        char* p = (char*)A(sz);
        if (full) touch_all(p, sz); else { p[0] = 1; p[sz - 1] = 1; }
        F(p);
    }
    return secs(t);
}

// live-set growth: allocate N blocks and keep them (stresses segment/span lookup when the heap is big)
static double s_grow(size_t count, size_t sz, size_t& ops) {
    ops = count * 2;
    std::vector<void*> v(count);
    auto t = clk::now();
    for (size_t i = 0; i < count; ++i) { v[i] = A(sz); *(volatile char*)v[i] = 1; }
    for (size_t i = 0; i < count; ++i) F(v[i]);
    return secs(t);
}

// ---------------------------------------------------------------- registry
struct Scen { const char* name; std::function<double(size_t&)> fn; const char* note; };

static bool g_cross_ok = true;

static std::vector<Scen> scenarios() {
    return {
        {"pairs32",       [](size_t& o) { return s_pairs(10000000, 32, o); },            "best case: LIFO reuse of one block"},
        {"pairs1k",       [](size_t& o) { return s_pairs(5000000, 1024, o); },           ""},
        {"batch64",       [](size_t& o) { return s_batch(50, 100000, 64, o); },          ""},
        {"mixed_small",   [](size_t& o) { return s_mixed(5000000, 16, 2033, o); },       "16..2KB, 10k live"},
        {"mixed_big",     [](size_t& o) { return s_mixed(1000000, 16, 32768, o); },      "16B..32KB, 10k live"},
        {"mt_mixed",      [](size_t& o) { return s_mt_mixed(3000000, o); },              "N threads, no sharing"},
        {"xfree_batch",   [](size_t& o) { return s_xfree_batch(20, 200000, 64, o); },    "N threads free blocks of another thread"},
        {"xfree_ring",    [](size_t& o) { return s_xfree_ring(2000000, g_cross_ok, o); },"hand-off bound, not an allocator test"},
        {"grow_256",      [](size_t& o) { return s_grow(800000, 256, o); },              "800k live blocks (refill/segment scan)"},
        {"grow_64",       [](size_t& o) { return s_grow(3000000, 64, o); },              "3M live blocks"},
        {"large64k_full", [](size_t& o) { return s_large(100000, 64 * 1024, true, o); }, "every page written"},
        {"large1m_touch2",[](size_t& o) { return s_large(20000, 1 << 20, false, o); },   "2 bytes written: cache hit best case"},
        {"large1m_full",  [](size_t& o) { return s_large(5000, 1 << 20, true, o); },     "every page written"},
        {"large8m_full",  [](size_t& o) { return s_large(1000, 8 << 20, true, o); },     "every page written"},
    };
}

static int check_mode() {
    bool ok = true;
    mixed_core(2000000, 7, true, ok, 16, 2033);
    mixed_core(300000, 9, true, ok, 16, 40000);
    bool cross = true; size_t ops = 0;
    s_xfree_ring(500000, cross, ops);
    // aligned pointer sanity for small sizes
    for (size_t sz : {1u, 8u, 16u, 33u, 64u, 100u, 4096u}) {
        void* p = A(sz);
        if (reinterpret_cast<uintptr_t>(p) % 16 != 0) { std::printf("misaligned %zu\n", (size_t)sz); ok = false; }
        F(p);
    }
    std::printf("CHECK %s integrity=%s cross-thread=%s\n", NAME, ok ? "ok" : "CORRUPTION", cross ? "ok" : "BAD");
    return (ok && cross) ? 0 : 1;
}

int main(int argc, char** argv) {
    const char* only = nullptr;
    bool list = false, check = false;
    unsigned hw = std::thread::hardware_concurrency();
    g_threads = hw >= 4 ? 4 : (hw ? (int)hw : 1);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") list = true;
        else if (a == "--check") check = true;
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--reps" && i + 1 < argc) g_reps = std::max(1, std::atoi(argv[++i]));
        else if (a == "--threads" && i + 1 < argc) g_threads = std::max(1, std::atoi(argv[++i]));
    }
    auto sc = scenarios();
    if (list) { for (auto& s : sc) std::printf("%s\n", s.name); return 0; }
    if (check) return check_mode();
    if (!only) { std::fprintf(stderr, "use --only <scenario> (see --list) or --check\n"); return 2; }

    for (auto& s : sc) {
        if (std::strcmp(s.name, only) != 0) continue;
        size_t ops = 0;
        s.fn(ops);                                   // warm-up (counted in peak RSS, not in time)
        std::vector<double> t;
        for (int i = 0; i < g_reps; ++i) t.push_back(s.fn(ops));
        std::sort(t.begin(), t.end());
        double med = t[t.size() / 2];
        std::printf("RESULT %s %s %.2f %.2f %.1f %.1f %s\n", s.name, NAME, med * 1e3, t.front() * 1e3,
                    double(ops) / med / 1e6, peak_rss_mb(), s.note);
        return 0;
    }
    std::fprintf(stderr, "unknown scenario: %s\n", only);
    return 2;
}
