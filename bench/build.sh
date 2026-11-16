#!/usr/bin/env bash
set -e
FLAGS="-O2  -pthread"        # или -march=znver3 / -march=native
g++ $FLAGS -std=c++20 -DUSE_PM bench.cpp -o bench_linux_pm
g++ $FLAGS -std=c++20          bench.cpp -o bench_linux_glibc
g++ $FLAGS -std=c++20          bench2.cpp -o bench2_linux
g++ $FLAGS -std=c++20         pptr_bench_console.cpp -o pptr_bench
./bench_linux_pm && ./bench_linux_glibc && ./pptr_bench
