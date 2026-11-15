#pragma once
/*
 **** EXTENSION FOR AMD CPU WITH X3D ****
 FILE: pmalloc_x3d_fixed.hpp

 ~! Константы в pmalloc_*.hpp:
 ~ constexpr std::size_t kLargeCacheMax  = 96 * 1024 * 1024;
 ~ constexpr std::size_t kLargeHotBudget = 32 * 1024 * 1024;

 Использование:
   x3d::init();                          // один раз при старте, до запуска потоков
   void* p = x3d::pm_alloc_x3d(238);
   x3d::pm_free_x3d(p);

   x3d::pl.x_pull<5000>();               // пул блоков по 5000 Б на половину L3 (по умолчанию)
   void* b = x3d::pl.get(0);             // блок пула; НЕ освобождать через pm_free_x3d
   x3d::pl.clear();                      // вернуть пул (вызвать явно до выхода из main)

   === EN =====
    ~! constexpr's в pmalloc_*.hpp:
 ~ constexpr std::size_t kLargeCacheMax  = 96 * 1024 * 1024;
 ~ constexpr std::size_t kLargeHotBudget = 32 * 1024 * 1024;

 usage:
   x3d::init();                          // only to launch one call to App
   void* p = x3d::pm_alloc_x3d(238);
   x3d::pm_free_x3d(p);

   x3d::pl.x_pull<5000>();               // pool blocks on 5000 B  count = (L3cache / size_block)
   void* b = x3d::pl.get(0);             // block pull. Not use pm_free_x3d!
   x3d::pl.clear();                      // collect pull
*/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include "pmalloc_win.hpp"
#include <intrin.h>
inline void x3d_cpuid_(unsigned leaf, int r[4]) { __cpuid(r, static_cast<int>(leaf)); }
#else
#include "pmalloc_posix.hpp"
#include <cpuid.h>
inline void x3d_cpuid_(unsigned leaf, int r[4]) { __cpuid(leaf, r[0], r[1], r[2], r[3]); }
#endif

static_assert(pmalloc::kHeader % 64 == 0, "segment header must keep the first block 64-byte aligned");

namespace x3d {

    struct flags {
        uint8_t u8AMD_CPU = 0;                    // AuthenticAMD
        uint8_t u8AMD_CPU_X3D_ENABLE = 0;         // it's "X3D"
        uint8_t u8AMD_CPU_ALIGN_MEMORY_PATTERN = 0; // align block (64)
    };
    inline flags f;

    struct data {
        uint64_t MB = 1024ull * 1024ull;
        uint64_t u64X3D_MEMORY_CACHE = 96ull * 1024ull * 1024ull;
        std::atomic<uint64_t> u64CacheMiss{ 0 };    // cache miss
    };
    inline data d;

    // one call to app!!!
    inline void init(bool align64 = true) noexcept {
        int r[4] = {};
        x3d_cpuid_(0, r);
        char vendor[13] = {};
        std::memcpy(vendor + 0, &r[1], 4);
        std::memcpy(vendor + 4, &r[3], 4);
        std::memcpy(vendor + 8, &r[2], 4);
        f.u8AMD_CPU = std::strcmp(vendor, "AuthenticAMD") == 0;

        char brand[49] = {};
        x3d_cpuid_(0x80000000u, r);
        if (static_cast<unsigned>(r[0]) >= 0x80000004u) {
            for (int i = 0; i < 3; ++i) {
                x3d_cpuid_(0x80000002u + i, r);
                std::memcpy(brand + i * 16, r, 16);
            }
        }
        f.u8AMD_CPU_X3D_ENABLE = f.u8AMD_CPU && std::strstr(brand, "X3D") != nullptr;
        f.u8AMD_CPU_ALIGN_MEMORY_PATTERN = align64;
    }
    inline std::size_t round_size_(std::size_t sz) noexcept {
        if (!f.u8AMD_CPU_ALIGN_MEMORY_PATTERN || sz <= 32) return sz;
        return (sz + 63) & ~static_cast<std::size_t>(63);
    }
    inline void* pm_alloc_x3d(std::size_t sz) noexcept {
        if (sz > d.u64X3D_MEMORY_CACHE) d.u64CacheMiss.fetch_add(1, std::memory_order_relaxed);
        return pmalloc::pm_malloc(round_size_(sz));
    }

    inline void pm_free_x3d(void* p) noexcept {
        pmalloc::pm_free(p);
    }

    struct pool {
        std::vector<void*> _ps;
        std::size_t cap = 0;
        std::size_t block = 0;

        pool() = default;
        pool(const pool&) = delete;
        pool& operator=(const pool&) = delete;
        template <std::size_t sz_max>
        bool x_pull(std::uint64_t budget = 0) {
            static_assert(sz_max > 0, "block size must be > 0");
            clear();
            if (budget == 0) budget = d.u64X3D_MEMORY_CACHE / 2;
            std::size_t count = static_cast<std::size_t>(budget / sz_max);
            _ps.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                void* p = pmalloc::pm_malloc(round_size_(sz_max));
                if (!p) { clear(); return false; }
                _ps.push_back(p);
            }
            block = sz_max;
            cap = _ps.size();
            return true;
        }

        void* get(std::size_t index) const noexcept {
            return index < cap ? _ps[index] : nullptr;
        }

        void clear() noexcept {
            for (void* p : _ps) pmalloc::pm_free(p);
            _ps.clear();
            cap = 0;
            block = 0;
        }
    };
    inline pool pl;

}  // namespace x3d
