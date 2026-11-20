#pragma once
// pptr_bench.hpp - benchmark of pmalloc::pptr (handle<T>, ptr<T>(), del) behind one call:
//
//   start_pptr_bench();        // non-blocking, runs on a worker thread
//   pptr_bench_imgui();        // button + progress + table, call every frame inside your window
//   stop_pptr_bench();         // on shutdown
//
// What it measures (ns per operation, lower is better):
//   1. table capacity probe (how many live handles ptr<T>() can give out)
//   2. dereference cost: raw T*  vs  h.get()  vs  h->   (sequential and shuffled, 3 working-set sizes)
//   3. ptr<T>() + del() pairs  vs  pm_malloc + placement new  vs  CRT new/delete
//   4. batch create N / del N
//   5. N threads: ptr+del pairs, shared read through h-> vs raw
//   6. race canary: readers pass valid() while a writer deletes the object. Counts "torn" reads.
//   7. (if the header defines PPTR_HAS_PIN_GUARD) the same traversals / shared reads / race through pin_guard.
//      The pinned race must report 0 torn reads; the process exit code of the console runner depends on it.
//
// Assumes handle<T> has public members p / idx / id (as in your pptr). The pptr header is
//   -DPPTR_BENCH_HEADER=\"...\"   (default: pmalloc_win.hpp on Windows, pmalloc_posix.hpp elsewhere)

#include <algorithm>
#include <array>        // pmalloc_win.hpp includes <array> inside namespace pmalloc: include it first
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <new>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if __has_include("imgui.h")
#include "imgui.h"
#endif

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
// pmalloc_win.hpp uses _ReturnAddress() and pulls <array>/<immintrin.h> in from inside namespace pmalloc:
// they must already be included at global scope.
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif
#define PPTR_HAS_PIN_GUARD // safe mode 
#ifndef PPTR_BENCH_HEADER
#if defined(_WIN32)
#define PPTR_BENCH_HEADER "pmalloc_win.hpp"
#else
#define PPTR_BENCH_HEADER "pmalloc_posix.hpp"
#endif
#endif
#include PPTR_BENCH_HEADER

#if defined(_MSC_VER)
#define PPB_NOINLINE __declspec(noinline)
#define PPB_BARRIER() _ReadWriteBarrier()
#else
#define PPB_NOINLINE __attribute__((noinline))
#define PPB_BARRIER() __asm__ volatile("" ::: "memory")
#endif

#if defined(__clang__) && defined(_MSC_VER)
#define PPB_COMPILER "clang-cl"
#elif defined(__clang__)
#define PPB_COMPILER "clang " __clang_version__
#elif defined(_MSC_VER)
#define PPB_COMPILER "msvc"
#elif defined(__GNUC__)
#define PPB_COMPILER "gcc " __VERSION__
#else
#define PPB_COMPILER "unknown-compiler"
#endif

namespace pptrbench {

namespace pp = pmalloc::pptr;

constexpr size_t kProbeMax = 1u << 20;          // at most this many live objects in the probe
constexpr size_t kTouches  = 10'000'000;        // handle dereferences per traversal measurement

struct Obj {
    uint64_t v[8];                              // 64 bytes: one cache line, like a pmalloc block
    explicit Obj(uint64_t x) { for (int k = 0; k < 8; ++k) v[k] = x + k; }
};
using H = pp::handle<Obj>;

struct Canary {
    std::atomic<uint64_t> a{0xAAAA}, b{0xAAAA};
    ~Canary() { a.store(0xDEAD); b.store(0xDEAD); }
};
using CH = pp::handle<Canary>;

#ifdef PPTR_HAS_PIN_GUARD
constexpr int kPin = 1;
#else
constexpr int kPin = 0;
#endif
constexpr int kHold = 32;   // barriers a reader keeps the object in use (race canary), same for pinned and not

// ------------------------------------------------------------------ shared UI state
struct Row {
    std::string name, extra;
    double med_ms = 0, min_ms = 0, ns_op = 0, mops = 0;
};

struct State {
    std::mutex mx;
    std::vector<Row> rows;
    std::vector<std::string> notes;
    std::string current, compiler;
    std::atomic<bool> running{false}, finished{false}, cancel{false};
    std::atomic<uint64_t> torn{0}, torn_pin{0}, pin_checks{0}, fails{0};
    std::atomic<int> step{0}, total{1};
    int reps = 5, threads = 4, race_ms = 1000;
    std::thread worker;
};
inline State& st() { static State s; return s; }

inline void set_current(const std::string& s) { std::lock_guard<std::mutex> lk(st().mx); st().current = s; }

inline void note(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(st().mx);
    st().notes.emplace_back(buf);
}

using clk = std::chrono::steady_clock;
inline double secs(clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); }
inline volatile uint64_t g_sink;

inline std::string count_str(size_t n) {
    char b[32];
    if (n % (1u << 20) == 0) std::snprintf(b, sizeof b, "%zuM", n >> 20);
    else if (n % 1024 == 0)  std::snprintf(b, sizeof b, "%zuK", n >> 10);
    else                     std::snprintf(b, sizeof b, "%zu", n);
    return b;
}

// ------------------------------------------------------------------ allocation shims (noinline on purpose)
PPB_NOINLINE inline H    pp_new(uint64_t x) { return pp::ptr<Obj>(x); }
PPB_NOINLINE inline void pp_del(H& h)       { pp::del(h); }
PPB_NOINLINE inline Obj* raw_new(uint64_t x) {
    void* m = pmalloc::pm_malloc(sizeof(Obj));
    return m ? new (m) Obj(x) : nullptr;
}
PPB_NOINLINE inline void raw_del(Obj* o) { o->~Obj(); pmalloc::pm_free(o); }
PPB_NOINLINE inline Obj* crt_new(uint64_t x) { return new Obj(x); }
PPB_NOINLINE inline void crt_del(Obj* o)     { delete o; }

// ------------------------------------------------------------------ traversal (deref cost)
// `ord` is an index permutation: identity = sequential, shuffled = pointer chasing.
PPB_NOINLINE inline double trav_raw(Obj* const* r, const uint32_t* ord, size_t n, size_t inner, size_t& ops) {
    uint64_t sum = 0; ops = n * inner;
    auto t = clk::now();
    for (size_t k = 0; k < inner; ++k) { PPB_BARRIER(); for (size_t i = 0; i < n; ++i) sum += r[ord[i]]->v[0]; }
    double d = secs(t); g_sink = sum; return d;
}
PPB_NOINLINE inline double trav_get(const H* h, const uint32_t* ord, size_t n, size_t inner, size_t& ops) {
    uint64_t sum = 0; ops = n * inner;
    auto t = clk::now();
    for (size_t k = 0; k < inner; ++k) {
        PPB_BARRIER();
        for (size_t i = 0; i < n; ++i) { Obj* o = h[ord[i]].get(); sum += o ? o->v[0] : 0; }
    }
    double d = secs(t); g_sink = sum; return d;
}
PPB_NOINLINE inline double trav_arrow(const H* h, const uint32_t* ord, size_t n, size_t inner, size_t& ops) {
    uint64_t sum = 0; ops = n * inner;
    auto t = clk::now();
    for (size_t k = 0; k < inner; ++k) { PPB_BARRIER(); for (size_t i = 0; i < n; ++i) sum += h[ord[i]]->v[0]; }
    double d = secs(t); g_sink = sum; return d;
}

#ifdef PPTR_HAS_PIN_GUARD
PPB_NOINLINE inline double trav_pin(const H* h, const uint32_t* ord, size_t n, size_t inner, size_t& ops) {
    uint64_t sum = 0; ops = n * inner;
    auto t = clk::now();
    for (size_t k = 0; k < inner; ++k) {
        PPB_BARRIER();
        for (size_t i = 0; i < n; ++i) { pp::pin_guard<Obj> g(h[ord[i]]); sum += g ? g->v[0] : 0; }
    }
    double d = secs(t); g_sink = sum; return d;
}
#endif

// ------------------------------------------------------------------ create / destroy
inline double t_pairs_pp(size_t n, size_t& ops) {
    ops = n * 2; uint64_t sum = 0; auto t = clk::now();
    for (size_t i = 0; i < n; ++i) {
        H h = pp_new(i);
        if (!h.p) { ++st().fails; continue; }
        sum += h->v[0]; pp_del(h);
    }
    double d = secs(t); g_sink = sum; return d;
}
inline double t_pairs_raw(size_t n, size_t& ops) {
    ops = n * 2; uint64_t sum = 0; auto t = clk::now();
    for (size_t i = 0; i < n; ++i) { Obj* o = raw_new(i); sum += o->v[0]; raw_del(o); }
    double d = secs(t); g_sink = sum; return d;
}
inline double t_pairs_crt(size_t n, size_t& ops) {
    ops = n * 2; uint64_t sum = 0; auto t = clk::now();
    for (size_t i = 0; i < n; ++i) { Obj* o = crt_new(i); sum += o->v[0]; crt_del(o); }
    double d = secs(t); g_sink = sum; return d;
}
inline double t_batch_pp(size_t rounds, size_t count, size_t& ops) {
    ops = rounds * count * 2;
    std::vector<H> v(count);
    auto t = clk::now();
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < count; ++i) { v[i] = pp_new(i); if (!v[i].p) ++st().fails; }
        for (size_t i = 0; i < count; ++i) if (v[i].p) { pp_del(v[i]); v[i] = H{}; }
    }
    return secs(t);
}
inline double t_batch_raw(size_t rounds, size_t count, size_t& ops) {
    ops = rounds * count * 2;
    std::vector<Obj*> v(count);
    auto t = clk::now();
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < count; ++i) v[i] = raw_new(i);
        for (size_t i = 0; i < count; ++i) raw_del(v[i]);
    }
    return secs(t);
}

// ------------------------------------------------------------------ multithreaded
template <typename Fn>
inline double run_threads(int nt, Fn fn) {
    std::vector<std::thread> th;
    auto t = clk::now();
    for (int i = 0; i < nt; ++i) th.emplace_back([=] { fn(i); });
    for (auto& x : th) x.join();
    return secs(t);
}
inline double t_threads_pp(size_t per, size_t& ops) {
    int nt = st().threads; ops = per * 2 * size_t(nt);
    return run_threads(nt, [=](int) {
        uint64_t sum = 0;
        for (size_t i = 0; i < per; ++i) {
            H h = pp_new(i);
            if (!h.p) { ++st().fails; continue; }
            sum += h->v[0]; pp_del(h);
        }
        g_sink = sum;
    });
}
inline double t_threads_raw(size_t per, size_t& ops) {
    int nt = st().threads; ops = per * 2 * size_t(nt);
    return run_threads(nt, [=](int) {
        uint64_t sum = 0;
        for (size_t i = 0; i < per; ++i) { Obj* o = raw_new(i); sum += o->v[0]; raw_del(o); }
        g_sink = sum;
    });
}
inline double t_shared_arrow(const std::vector<H>& hs, size_t touches, size_t& ops) {
    int nt = st().threads; size_t n = hs.size(), rounds = std::max<size_t>(1, touches / n);
    ops = rounds * n * size_t(nt);
    return run_threads(nt, [&, rounds, n](int) {
        uint64_t sum = 0;
        for (size_t r = 0; r < rounds; ++r) { PPB_BARRIER(); for (size_t i = 0; i < n; ++i) sum += hs[i]->v[0]; }
        g_sink = sum;
    });
}
#ifdef PPTR_HAS_PIN_GUARD
inline double t_shared_pin(const std::vector<H>& hs, size_t touches, size_t& ops) {
    int nt = st().threads; size_t n = hs.size(), rounds = std::max<size_t>(1, touches / n);
    ops = rounds * n * size_t(nt);
    return run_threads(nt, [&, rounds, n](int) {
        uint64_t sum = 0;
        for (size_t r = 0; r < rounds; ++r) {
            PPB_BARRIER();
            for (size_t i = 0; i < n; ++i) { pp::pin_guard<Obj> g(hs[i]); sum += g ? g->v[0] : 0; }
        }
        g_sink = sum;
    });
}
#endif
inline double t_shared_raw(const std::vector<Obj*>& rs, size_t touches, size_t& ops) {
    int nt = st().threads; size_t n = rs.size(), rounds = std::max<size_t>(1, touches / n);
    ops = rounds * n * size_t(nt);
    return run_threads(nt, [&, rounds, n](int) {
        uint64_t sum = 0;
        for (size_t r = 0; r < rounds; ++r) { PPB_BARRIER(); for (size_t i = 0; i < n; ++i) sum += rs[i]->v[0]; }
        g_sink = sum;
    });
}

// ------------------------------------------------------------------ runner: median of reps after a warm-up
inline void run(const std::string& name, const std::function<double(size_t&)>& fn, const std::string& extra = {}) {
    State& S = st();
    if (S.cancel) return;
    set_current(name);
    size_t ops = 0;
    fn(ops);                                              // warm-up, not counted
    std::vector<double> t;
    for (int i = 0; i < S.reps && !S.cancel; ++i) t.push_back(fn(ops));
    if (t.empty()) return;
    std::sort(t.begin(), t.end());
    double med = t[t.size() / 2];
    Row r;
    r.name = name; r.extra = extra;
    r.med_ms = med * 1e3; r.min_ms = t.front() * 1e3;
    r.ns_op = med * 1e9 / double(ops);
    r.mops = double(ops) / med / 1e6;
    { std::lock_guard<std::mutex> lk(S.mx); S.rows.push_back(std::move(r)); }
    ++S.step;
}

// ------------------------------------------------------------------ race canary
// Readers copy the published handle, then either pass valid() through get() (unpinned) or take a
// pin_guard (pinned), and read the object twice: right away and after a short hold (kHold barriers).
// The writer deletes the object right after publishing it. A reader that got the object and then saw
// a destroyed (0xDEAD) or recycled block is a "torn" read: the check-then-use window is real.
// Unpinned: torn reads are expected. Pinned: torn reads must be 0.
inline void race_test(int ms, int readers, bool pinned) {
    State& S = st();
    struct alignas(64) Ctr { uint64_t checks = 0, stale = 0, torn = 0; };
    std::vector<Ctr> ctr(static_cast<size_t>(readers));
    std::atomic<uint64_t> pub_p{0}, pub_idx{0}, pub_id{0}, replaced{0}, wfail{0};
    std::atomic<bool> stop{false};

    std::thread writer([&] {
        // ballast of the same size class keeps the segment non-empty, so pm_free never unmaps it
        std::vector<void*> ballast;
        for (int i = 0; i < 2048; ++i) if (void* b = pmalloc::pm_malloc(sizeof(Canary))) ballast.push_back(b);
        while (!stop.load(std::memory_order_relaxed)) {
            CH h = pp::ptr<Canary>();
            if (!h.p) { ++wfail; std::this_thread::yield(); continue; }
            pub_p.store(reinterpret_cast<uintptr_t>(h.p), std::memory_order_release);
            pub_idx.store(uint64_t(h.idx), std::memory_order_release);
            pub_id.store(uint64_t(h.id), std::memory_order_release);
            for (int k = 0; k < 200; ++k) PPB_BARRIER();   // keep it valid for a short while
            pp::del(h);                                     // pinned: waits until the readers let go
            ++replaced;
        }
        pub_p.store(0, std::memory_order_release);
        for (void* b : ballast) pmalloc::pm_free(b);
    });

    std::vector<std::thread> rd;
    for (int i = 0; i < readers; ++i)
        rd.emplace_back([&, i] {
            Ctr& c = ctr[size_t(i)];
            while (!stop.load(std::memory_order_relaxed)) {
                CH h;                                        // may mix fields of two epochs: valid()/pin reject those
                h.p   = reinterpret_cast<Canary*>(uintptr_t(pub_p.load(std::memory_order_acquire)));
                h.idx = static_cast<decltype(h.idx)>(pub_idx.load(std::memory_order_acquire));
                h.id  = static_cast<decltype(h.id)>(pub_id.load(std::memory_order_acquire));
                uint64_t a, b, a2, b2;
                if (pinned) {
#ifdef PPTR_HAS_PIN_GUARD
                    pp::pin_guard<Canary> g(h);
                    if (!g) { ++c.stale; continue; }
                    ++c.checks;
                    a = g->a.load(std::memory_order_acquire); b = g->b.load(std::memory_order_acquire);
                    for (int k = 0; k < kHold; ++k) PPB_BARRIER();
                    a2 = g->a.load(std::memory_order_acquire); b2 = g->b.load(std::memory_order_acquire);
#else
                    return;
#endif
                } else {
                    Canary* r = h.get();
                    if (!r) { ++c.stale; continue; }
                    ++c.checks;
                    a = r->a.load(std::memory_order_acquire); b = r->b.load(std::memory_order_acquire);
                    for (int k = 0; k < kHold; ++k) PPB_BARRIER();
                    a2 = r->a.load(std::memory_order_acquire); b2 = r->b.load(std::memory_order_acquire);
                }
                if (a != 0xAAAA || b != 0xAAAA || a2 != 0xAAAA || b2 != 0xAAAA) ++c.torn;
            }
        });

    for (int waited = 0; waited < ms && !S.cancel; waited += 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop = true;
    writer.join();
    for (auto& t : rd) t.join();

    uint64_t checks = 0, stale = 0, torn = 0;
    for (auto& c : ctr) { checks += c.checks; stale += c.stale; torn += c.torn; }
    const char* kind = pinned ? "pinned (pin_guard)" : "unpinned (h.get())";
    note("race canary, %s: 1 writer + %d reader(s), %d ms", kind, readers, ms);
    note("  create+del cycles %llu, reads that got the object %llu, rejected %llu",
         (unsigned long long)replaced.load(), (unsigned long long)checks, (unsigned long long)stale);
    if (wfail) note("  writer: ptr<T>() returned empty %llu times (table exhausted)", (unsigned long long)wfail.load());
    if (pinned) { S.torn_pin = torn; S.pin_checks = checks; } else S.torn = torn;
    if (checks == 0) {
        note("  INCONCLUSIVE: no read got the object, so this run proves nothing%s",
             std::thread::hardware_concurrency() < 2 ? " (single hardware thread: readers and writer never overlap)" : "");
    } else if (pinned) {
        if (torn) note("  PIN FAILED: %llu torn reads out of %llu pinned reads (must be 0)",
                       (unsigned long long)torn, (unsigned long long)checks);
        else      note("  PIN OK: 0 torn reads out of %llu pinned reads (absence of hits is evidence, not a proof)",
                       (unsigned long long)checks);
    } else if (torn) {
        note("  TORN reads: %llu = %.3g per 1e9 passed checks, %.3g per 1e9 attempts -> the check-then-use window is real",
             (unsigned long long)torn, double(torn) * 1e9 / double(checks),
             double(torn) * 1e9 / double(checks + stale));
    } else {
        note("  torn reads: 0 -> no violation observed in this run (window is narrow; absence of hits is not a proof)");
    }
}

// ------------------------------------------------------------------ main sequence
inline void bench_main() {
    State& S = st();
    { std::lock_guard<std::mutex> lk(S.mx); S.compiler = PPB_COMPILER; }

    // 1. capacity probe (the objects are reused as the dereference working set)
    set_current("probe table capacity");
    std::vector<H> hs;
    for (size_t i = 0; i < kProbeMax && !S.cancel; ++i) {
        H h = pp_new(i);
        if (!h.p) break;
        hs.push_back(h);
    }
    const size_t cap = hs.size();
    note("table capacity: %s%zu live handles (probe limit %zu)", cap >= kProbeMax ? ">= " : "", cap, kProbeMax);
    if (cap == 0) {
        note("ptr<T>() returned an empty handle at once: nothing to measure");
        set_current("done"); S.finished = true; S.running = false; return;
    }

    if (std::thread::hardware_concurrency() < 2)
        note("only %u hardware thread: multithreaded rows and the race tests are not meaningful", std::thread::hardware_concurrency());
    if (cap < 1024)
        note("table is tiny (%zu slots): working sets are L1-hot, and ptr<T>() scans slots linearly, so its cost grows with occupancy (see the batch rows)", cap);

    // working sets: from L1-hot up to the whole table
    std::vector<size_t> sizes;
    for (size_t want : {size_t(16), size_t(1) << 10, size_t(1) << 16, size_t(1) << 20})
        if (want <= cap) sizes.push_back(want);
    if (cap >= 16 && (sizes.empty() || sizes.back() != cap)) sizes.push_back(cap);

    // batch sizes: low, half and full occupancy (shows how ptr<T>() scales with live objects)
    std::vector<size_t> batch_sizes;
    for (size_t n : {size_t(16), cap / 2, cap}) {
        n = std::min<size_t>(n, 65536);
        if (n >= 16 && (batch_sizes.empty() || batch_sizes.back() < n)) batch_sizes.push_back(n);
    }
    const size_t shared_n = std::min<size_t>(cap, 4096);
    const bool do_shared = shared_n >= 16;
    S.total = 1 + int(sizes.size()) * (5 + kPin) + 3 + int(batch_sizes.size()) * 2 + 2 + (do_shared ? 2 + kPin : 0) + 1 + kPin;
    ++S.step;                                                // probe done

    // 2. dereference cost
    for (size_t n : sizes) {
        if (S.cancel) break;
        std::vector<uint32_t> seq(n), shuf;
        std::iota(seq.begin(), seq.end(), 0u);
        shuf = seq;
        std::mt19937_64 g(12345);
        std::shuffle(shuf.begin(), shuf.end(), g);
        std::vector<Obj*> raw(n);
        for (size_t i = 0; i < n; ++i) raw[i] = hs[i].p;
        const size_t inner = std::max<size_t>(1, kTouches / n);
        const std::string tag = " [" + count_str(n) + "]";
        const std::string ex = "working set " + count_str(n) + " objects x 64B";
        run("raw T* seq" + tag,       [&](size_t& o) { return trav_raw(raw.data(), seq.data(), n, inner, o); }, ex);
        run("h.get() seq" + tag,      [&](size_t& o) { return trav_get(hs.data(), seq.data(), n, inner, o); }, ex);
        run("h-> seq" + tag,          [&](size_t& o) { return trav_arrow(hs.data(), seq.data(), n, inner, o); }, ex);
        run("raw T* shuffled" + tag,  [&](size_t& o) { return trav_raw(raw.data(), shuf.data(), n, inner, o); }, ex);
        run("h-> shuffled" + tag,     [&](size_t& o) { return trav_arrow(hs.data(), shuf.data(), n, inner, o); }, ex);
#ifdef PPTR_HAS_PIN_GUARD
        run("pin_guard seq" + tag,    [&](size_t& o) { return trav_pin(hs.data(), seq.data(), n, inner, o); }, ex);
#endif
    }
    for (auto& h : hs) if (h.p) pp_del(h);
    hs.clear(); hs.shrink_to_fit();

    // 3. create + destroy
    run("ptr<T>() + del",            [&](size_t& o) { return t_pairs_pp(2000000, o); }, "ops = create + destroy");
    run("pm_malloc + new + free",    [&](size_t& o) { return t_pairs_raw(2000000, o); });
    run("CRT new + delete",          [&](size_t& o) { return t_pairs_crt(2000000, o); });
    for (size_t n : batch_sizes) {
        if (S.cancel) break;
        const size_t rounds = std::max<size_t>(1, 2000000 / n);
        const std::string tag = " [" + count_str(n) + " live]";
        run("batch ptr<T>() / del" + tag,    [&](size_t& o) { return t_batch_pp(rounds, n, o); },
            n == cap ? "table full: last ptr<T>() scans every slot" : "");
        run("batch pm_malloc / free" + tag,  [&](size_t& o) { return t_batch_raw(rounds, n, o); });
    }

    // 4. threads
    const std::string tt = " x" + std::to_string(S.threads) + " threads";
    run("ptr<T>() + del" + tt,          [&](size_t& o) { return t_threads_pp(1000000, o); }, "each thread: 1 live object at a time");
    run("pm_malloc + new + free" + tt,  [&](size_t& o) { return t_threads_raw(1000000, o); });
    if (do_shared && !S.cancel) {
        std::vector<H> shs; std::vector<Obj*> srs;
        for (size_t i = 0; i < shared_n; ++i) { H h = pp_new(i); if (!h.p) break; shs.push_back(h); srs.push_back(h.p); }
        run("shared read h->" + tt,  [&](size_t& o) { return t_shared_arrow(shs, 10000000, o); }, "read-only, " + count_str(shs.size()) + " shared objects");
#ifdef PPTR_HAS_PIN_GUARD
        run("shared read pin_guard" + tt, [&](size_t& o) { return t_shared_pin(shs, 10000000, o); }, "read-only, fetch_add/fetch_sub on pins[idx]");
#endif
        run("shared read raw T*" + tt, [&](size_t& o) { return t_shared_raw(srs, 10000000, o); });
        for (auto& h : shs) if (h.p) pp_del(h);
    }
    if (S.fails) note("ptr<T>() returned empty %llu times during the timed tests (table exhausted/contended)",
                      (unsigned long long)S.fails.load());

    // 5. race canary
    if (!S.cancel) {
        set_current("race canary");
        const int rdrs = std::max(1, S.threads - 1);
        race_test(S.race_ms, rdrs, false);
        ++S.step;
#ifdef PPTR_HAS_PIN_GUARD
        if (!S.cancel) { set_current("race canary (pinned)"); race_test(S.race_ms, rdrs, true); ++S.step; }
#else
        note("pin_guard not found in the pmalloc header (PPTR_HAS_PIN_GUARD is not defined): pinned tests skipped");
#endif
    }
    set_current(S.cancel ? "cancelled" : "done");
    S.finished = true;
    S.running = false;
}

}  // namespace pptrbench

// ====================================================================== public API
inline void stop_pptr_bench() {
    auto& S = pptrbench::st();
    S.cancel = true;
    if (S.worker.joinable()) S.worker.join();
    S.running = false;
}
inline void cancel_pptr_bench() { pptrbench::st().cancel = true; }
inline bool pptr_bench_running() { return pptrbench::st().running.load(); }
inline float pptr_bench_progress() {
    auto& S = pptrbench::st();
    return float(std::min(S.step.load(), S.total.load())) / float(std::max(1, S.total.load()));
}

// reps = runs per test (median), threads = 0 -> auto (up to 8), race_ms = length of the race test
inline bool start_pptr_bench(int reps = 5, int threads = 0, int race_ms = 1000) {
    auto& S = pptrbench::st();
    if (S.running.exchange(true)) return false;
    if (S.worker.joinable()) S.worker.join();
    static struct Joiner { ~Joiner() { stop_pptr_bench(); } } joiner;
    S.reps = reps < 1 ? 1 : reps;
    unsigned hw = std::thread::hardware_concurrency();
    S.threads = threads > 0 ? threads : (hw >= 8 ? 8 : (hw >= 2 ? int(hw) : 2));
    S.race_ms = race_ms < 100 ? 100 : race_ms;
    S.step = 0; S.total = 1;
    S.cancel = false; S.finished = false; S.torn = 0; S.torn_pin = 0; S.pin_checks = 0; S.fails = 0;
    {
        std::lock_guard<std::mutex> lk(S.mx);
        S.rows.clear(); S.notes.clear(); S.current = "starting";
    }
    S.worker = std::thread([] { pptrbench::bench_main(); });
    return true;
}

inline std::string pptr_bench_report() {
    auto& S = pptrbench::st();
    std::lock_guard<std::mutex> lk(S.mx);
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof buf, "pptr  [%s]  median of %d runs, %d threads, ns/op counts create+destroy as 2 ops\n",
                  S.compiler.c_str(), S.reps, S.threads);
    out += buf;
    for (const auto& r : S.rows) {
        std::snprintf(buf, sizeof buf, "  %-34s %7.2f ns/op %9.1f Mops/s  median %8.1f ms (min %7.1f)  %s\n",
                      r.name.c_str(), r.ns_op, r.mops, r.med_ms, r.min_ms, r.extra.c_str());
        out += buf;
    }
    for (const auto& n : S.notes) { out += "  "; out += n; out += "\n"; }
    return out;
}

inline void pptr_bench_imgui() {
#if defined(IMGUI_VERSION)
    auto& S = pptrbench::st();
    const bool running = pptr_bench_running();
    if (!running) { if (ImGui::Button("Start pptr benchmark")) start_pptr_bench(); }
    else          { if (ImGui::Button("Cancel pptr")) cancel_pptr_bench(); }
    ImGui::SameLine();
    if (ImGui::Button("Copy report")) ImGui::SetClipboardText(pptr_bench_report().c_str());
    ImGui::TextDisabled("Uses several threads and up to ~100 MB: frame rate drops while it runs.");

    std::string cur;
    std::vector<pptrbench::Row> rows;
    std::vector<std::string> notes;
    {
        std::lock_guard<std::mutex> lk(S.mx);
        cur = S.current; rows = S.rows; notes = S.notes;
    }
    char label[160];
    std::snprintf(label, sizeof label, "%s  (%d/%d)", cur.c_str(), S.step.load(), S.total.load());
    if (running || S.finished) ImGui::ProgressBar(pptr_bench_progress(), ImVec2(-1.f, 0.f), label);

    if (!rows.empty() &&
        ImGui::BeginTable("pptr_rows", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("test");
        ImGui::TableSetupColumn("ns/op");
        ImGui::TableSetupColumn("Mops/s");
        ImGui::TableSetupColumn("median ms");
        ImGui::TableSetupColumn("min ms");
        ImGui::TableSetupColumn("details");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", r.ns_op);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f", r.mops);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f", r.med_ms);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f", r.min_ms);
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(r.extra.c_str());
        }
        ImGui::EndTable();
    }
    for (const auto& n : notes) {
        if (n.find("TORN") != std::string::npos || n.find("PIN FAILED") != std::string::npos) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", n.c_str());
        else ImGui::TextUnformatted(n.c_str());
    }
#endif
}