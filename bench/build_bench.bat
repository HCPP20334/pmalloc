@echo off
echo ----------------------------------------
echo [2/3] Building with Clang-CL...
echo ----------------------------------------
clang-cl /O2 /std:c++17 /DUSE_PM bench.cpp /Febench_clang_llvm_pmalloc.exe
clang-cl /O2 /std:c++17 /DUSE_MI bench.cpp mimalloc_clang.obj /Imimalloc/include advapi32.lib /Febench_clang_llvm_mimalloc.exe
echo.
echo ----------------------------------------
echo Running Benchmarks...
echo ----------------------------------------

echo - clang-llvm
if exist bench_clang_llvm_pmalloc.exe bench_clang_llvm_pmalloc.exe
if exist bench_clang_llvm_mimalloc.exe bench_clang_llvm_mimalloc.exe
pause
