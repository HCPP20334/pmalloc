#!/usr/bin/env bash
set -e
FLAGS="-O2  -pthread"        # или -march=znver3 / -march=native
g++ $FLAGS -std=c++17 -DUSE_PM bench.cpp -o bench_linux_pm
g++ $FLAGS -std=c++17          bench.cpp -o bench_linux_glibc

./bench_linux_pm && ./bench_linux_glibc
