#include "pmalloc_win.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <latch>
#include <print>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <mimalloc.h>

#ifdef _MSC_VER
#define PM_NOINLINE __declspec(noinline)
#else
#define PM_NOINLINE [[gnu::noinline]]
#endif

using pm_str = std::basic_string<char, std::char_traits<char>, pmalloc::pmallocator_cpp<char>>;
using mi_str = std::basic_string<char, std::char_traits<char>, mi_stl_allocator<char>>;

template <typename K, typename V>
using pm_umap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                                   pmalloc::pmallocator_cpp<std::pair<const K, V>>>;
template <typename K, typename V>
using mi_umap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                                   mi_stl_allocator<std::pair<const K, V>>>;

using cl = std::chrono::steady_clock;
static double secs(cl::time_point a) { return std::chrono::duration<double>(cl::now() - a).count(); }

template <typename S> PM_NOINLINE size_t bench_str(int iters) {
  size_t sink = 0;
  for (int i = 0; i < iters; ++i) {
    S s;
    s.reserve(64 + (i & 255));
    s += "payload_to_bypass_sso_and_force_real_heap_allocation_12345";
    sink += s.back();
  }
  return sink;
}

template <typename MapT> PM_NOINLINE size_t bench_umap(int rounds, int count) {
  size_t sink = 0;
  for (int r = 0; r < rounds; ++r) {
    MapT m;
    for (int i = 0; i < count; ++i) m.emplace(i, i * 3 + r);
    for (int i = 0; i < count; i += 2) { sink += m[i]; m.erase(i); }
  }
  return sink;
}

template <typename Fn> std::pair<double, size_t> run_mt(int threads, Fn &&work) {
  std::latch ready(threads), start(1);
  std::atomic<size_t> total{0};
  std::vector<std::thread> th;
  th.reserve(threads);
  for (int t = 0; t < threads; ++t)
    th.emplace_back([&] { ready.count_down(); start.wait(); total.fetch_add(work(), std::memory_order_relaxed); });
  ready.wait();
  auto c0 = cl::now();
  start.count_down();
  for (auto &x : th) x.join();
  return {secs(c0), total.load()};
}

// ---------- сравнение: прогрев, ротация порядка, медиана ----------
struct Contender {
  const char *name;
  std::function<double(size_t &)> run;   // возвращает секунды, прибавляет к sink
  std::vector<double> t;
};

template <typename F> static double timed(F &&f, size_t &sink) {
  auto a = cl::now();
  sink += f();
  return secs(a);
}

static void compare(const char *label, std::vector<Contender> &cs, int reps = 9) {
  size_t sink = 0;
  for (auto &c : cs) c.run(sink);                           // прогрев каждого
  for (int r = 0; r < reps; ++r)
    for (size_t k = 0; k < cs.size(); ++k) {                // порядок сдвигается каждый круг
      auto &c = cs[(k + r) % cs.size()];
      c.t.push_back(c.run(sink));
    }
  for (auto &c : cs) std::sort(c.t.begin(), c.t.end());
  const double base = cs[0].t[reps / 2];                    // cs[0] = pmalloc
  std::println("{}", label);
  for (auto &c : cs)
    std::println("  {:<9} median {:.4f} s  (min {:.4f}, max {:.4f})  pm faster by {:.2f}x",
                 c.name, c.t[reps / 2], c.t.front(), c.t.back(), c.t[reps / 2] / base);
  std::println("  (sink={})\n", sink);
}

int main() {
  constexpr int strIters = 5'000'000, mapRounds = 20, mapCount = 100'000, threads = 8;

  std::vector<Contender> s1{
      {"pmalloc",  [&](size_t &s) { return timed([&] { return bench_str<pm_str>(strIters); }, s); }, {}},
      {"crt",      [&](size_t &s) { return timed([&] { return bench_str<std::string>(strIters); }, s); }, {}},
      {"mimalloc", [&](size_t &s) { return timed([&] { return bench_str<mi_str>(strIters); }, s); }, {}}};
  compare("[1T] std::string", s1);

  std::vector<Contender> m1{
      {"pmalloc",  [&](size_t &s) { return timed([&] { return bench_umap<pm_umap<int, int>>(mapRounds, mapCount); }, s); }, {}},
      {"crt",      [&](size_t &s) { return timed([&] { return bench_umap<std::unordered_map<int, int>>(mapRounds, mapCount); }, s); }, {}},
      {"mimalloc", [&](size_t &s) { return timed([&] { return bench_umap<mi_umap<int, int>>(mapRounds, mapCount); }, s); }, {}}};
  compare("[1T] unordered_map", m1);

  std::vector<Contender> s8{
      {"pmalloc",  [&](size_t &s) { auto [t, k] = run_mt(threads, [&] { return bench_str<pm_str>(strIters / 2); }); s += k; return t; }, {}},
      {"crt",      [&](size_t &s) { auto [t, k] = run_mt(threads, [&] { return bench_str<std::string>(strIters / 2); }); s += k; return t; }, {}},
      {"mimalloc", [&](size_t &s) { auto [t, k] = run_mt(threads, [&] { return bench_str<mi_str>(strIters / 2); }); s += k; return t; }, {}}};
  compare("[8T] std::string", s8);

  std::vector<Contender> m8{
      {"pmalloc",  [&](size_t &s) { auto [t, k] = run_mt(threads, [&] { return bench_umap<pm_umap<int, int>>(mapRounds / 2, mapCount); }); s += k; return t; }, {}},
      {"crt",      [&](size_t &s) { auto [t, k] = run_mt(threads, [&] { return bench_umap<std::unordered_map<int, int>>(mapRounds / 2, mapCount); }); s += k; return t; }, {}},
      {"mimalloc", [&](size_t &s) { auto [t, k] = run_mt(threads, [&] { return bench_umap<mi_umap<int, int>>(mapRounds / 2, mapCount); }); s += k; return t; }, {}}};
  compare("[8T] unordered_map", m8);
}