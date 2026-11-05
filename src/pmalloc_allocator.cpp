#include <cstddef>
#if defined(_WIN32)
    #include "pmalloc_win.hpp"
#else
    #include "pmalloc_posix.hpp"
#endif

extern "C" {

void* pm_c_alloc(size_t size, size_t align) {
    if (align <= pmalloc::kAlign) {
        return pmalloc::pm_malloc(size);
    } else {
        return pmalloc::pm_malloc_aligned(size, align);
    }
}

void pm_c_dealloc(void* ptr, size_t size, size_t align) {
    (void)size;
    (void)align;
    pmalloc::pm_free(ptr);
}

void* pm_c_realloc(void* ptr, size_t old_size, size_t new_size, size_t align) {
    (void)old_size;
    (void)align;
    return pmalloc::pm_realloc(ptr, new_size);
}
#include <cstdio>

void pm_c_stats() {
    // auto s = pmalloc::pm_stats();
    // std::fprintf(stderr,
    //     "os_bytes=%zu\n os_peak=%zu\n large_cache=%zu\n large_hot=%zu\n"
    //     "os_maps=%zu\n os_unmaps=%zu\n large_allocs=%zu\n large_hits=%zu\n"
    //     "exited_allocs=%zu\n exited_frees=%zu\n exited_remote=%zu\n"
    //     "segments_used=%zu\n segments_freed=%zu\n segments_live=%zu\n"
    //     "reserved=%zu\n live_bytes=%zu (%.1f%%)\n carved_bytes=%zu (%.1f%%)\n fragmentation=%.1f%%\n"
    //     "n_alloc=%zu\n n_free=%zu\n n_remote=%zu\n outstanding=%lld\n"
    //     "refill_calls=%zu\n find_steps=%zu\n steps_per_refill=%.2f\n",
    //     (size_t)s.os_bytes, (size_t)s.os_peak_bytes,
    //     (size_t)s.large_cache_bytes, (size_t)s.large_cache_hot,
    //     (size_t)s.os_maps, (size_t)s.os_unmaps,
    //     (size_t)s.large_allocs, (size_t)s.large_cache_hits,
    //     (size_t)s.exited_allocs, (size_t)s.exited_frees, (size_t)s.exited_remote_frees,
    //     (size_t)s.segment_used, (size_t)s.segments_freed,
    //     (size_t)(s.segment_used - s.segments_freed),
    //     s.reserved_bytes,
    //     s.live_bytes, s.live_ratio * 100.0,
    //     s.carved_bytes, s.carved_ratio * 100.0,
    //     s.fragmentation * 100.0,
    //     (size_t)s.n_alloc, (size_t)s.n_free, (size_t)s.n_remote,
    //     (long long)s.n_alloc - (long long)s.n_free - (long long)s.n_remote,
    //     (size_t)s.refill_calls, (size_t)s.find_steps,
    //     s.refill_calls ? double(s.find_steps) / double(s.refill_calls) : 0.0);
}

}