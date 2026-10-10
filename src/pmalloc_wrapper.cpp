/*

pmalloc_wrapper.cpp

to compile .lib
Only Windows

*/

#include "pmalloc_win.hpp"

extern "C" {
    __declspec(dllexport) void* pm_c_alloc(std::size_t size, std::size_t align) {
        return pmalloc::pm_malloc_aligned(size, align);
    }

    __declspec(dllexport) void pm_c_dealloc(void* ptr, std::size_t size, std::size_t align) {
        (void)size;
        (void)align;
        pmalloc::pm_free(ptr);
    }

    __declspec(dllexport) void* pm_c_realloc(void* ptr, std::size_t old_size, std::size_t new_size, std::size_t align) {
        (void)old_size;
        (void)align;
        return pmalloc::pm_realloc(ptr, new_size);
    }
}