@echo off
echo ----------------------------------------
echo [2/3] Building with Clang-CL...
echo ----------------------------------------
del /q *.exe 2>nul

clang-cl /O2 /std:c++17 /DUSE_PM /arch:AVX2 bench.cpp /Febench_clang_pm.exe
clang-cl /O2 /std:c++17 /DUSE_MI /arch:AVX2 bench.cpp mimalloc_clang.obj /Imimalloc/include advapi32.lib /Febench_clang_mi.exe
clang-cl /O2  /arch:AVX2 bench.cpp /Febench_clang_crt.exe

pause
