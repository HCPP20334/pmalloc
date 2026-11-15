// Console runner for pptr_bench.hpp
//
//   Windows (clang-cl): clang-cl /O2 /MT /std:c++17 /DNDEBUG /arch:AVX2 pptr_bench_console.cpp /Fepptr_bench.exe
//   Windows (MSVC):     cl /O2 /MT /std:c++17 /EHsc /DNDEBUG pptr_bench_console.cpp /Fepptr_bench.exe
//   Linux:              g++ -O2 -mavx2 -pthread -std=c++17 -DNDEBUG pptr_bench_console.cpp -o pptr_bench
//
//   usage: pptr_bench [reps=5] [threads=0(auto)] [race_ms=1000]
//
// The pptr header is pmalloc_win.hpp / pmalloc_posix.hpp; override with
//   -DPPTR_BENCH_HEADER=\"my_header.hpp\"   (clang-cl: /DPPTR_BENCH_HEADER=\"my_header.hpp\")

#include "pptr_bench.hpp"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    int reps    = argc > 1 ? std::atoi(argv[1]) : 5;
    int threads = argc > 2 ? std::atoi(argv[2]) : 0;
    int race_ms = argc > 3 ? std::atoi(argv[3]) : 1000;

    std::printf("pptr benchmark (reps=%d, threads=%s, race=%d ms)\n", reps,
                threads > 0 ? std::to_string(threads).c_str() : "auto", race_ms);

    if (!start_pptr_bench(reps, threads, race_ms)) {
        std::fprintf(stderr, "could not start\n");
        return 2;
    }

    while (pptr_bench_running()) {
        auto& S = pptrbench::st();
        std::string cur;
        { std::lock_guard<std::mutex> lk(S.mx); cur = S.current; }
        std::printf("\r  [%3.0f%%] %-60.60s", pptr_bench_progress() * 100.0f, cur.c_str());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    std::printf("\r%-72s\r", "");
    stop_pptr_bench();

    std::fputs(pptr_bench_report().c_str(), stdout);

    // non-zero exit if reads were torn, handy for CI: a closed window must give 0
    return pptrbench::st().torn.load() ? 1 : 0;
}