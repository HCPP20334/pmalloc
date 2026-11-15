#!/usr/bin/env bash
set -e
FLAGS="-O2 -mavx2 -pthread"        # или -march=znver3 / -march=native

# mimalloc: статическая сборка из исходников (это C)
gcc -c -O2 -std=c11 -DNDEBUG -mavx2 -pthread -Imimalloc/include \
      mimalloc/src/static.c -o mimalloc_g_l.o

g++ $FLAGS -std=c++17 -DUSE_PM bench.cpp -o bench_linux_old
g++ $FLAGS -std=c++17 -DUSE_MI bench.cpp mimalloc_g_l.o -Imimalloc/include \
        -o bench_g_mimalloc_linux
g++ $FLAGS -std=c++17          bench.cpp -o bench_g_glibc_linux

for b in pmalloc mimalloc glibc; do
  echo "=== $b ==="
  ./bench_g_${b}_linux
done