// pmalloc - PahomEngine memory Allocator (POSIX / Linux / macOS)
// created: 19:52 , 06.10.26

#pragma once

#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef PM_STATS
#define PM_STAT(x) x
#else
#define PM_STAT(x) ((void)0)
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define PM_NOINLINE __declspec(noinline)
#else
#define PM_NOINLINE __attribute__((noinline))
#endif

namespace pmalloc {

constexpr std::size_t kSegmentSize   = 64 * 1024;
constexpr std::size_t kAlign         = 64;
constexpr std::size_t kSmallMax      = 8 * 1024;
constexpr std::size_t kNumClasses    = kSmallMax / kAlign;
constexpr std::size_t kLargeClass    = kNumClasses;
constexpr std::uint32_t kSegmentMagic = 0x504D5347u;
constexpr std::uint32_t kLargeMagic   = 0x504D4C47u;
constexpr std::size_t kLargeMinBucket = 16 * 1024;
constexpr std::size_t kLargeBuckets   = 45;
constexpr std::size_t kLargePerBucket = 8;
constexpr std::size_t kLargeCacheMax  = 128 * 1024 * 1024;
constexpr std::size_t kLargeHotBudget = 16 * 1024 * 1024;   // не используется (оставлено для совместимости с win)
constexpr std::uint32_t kFlagFresh    = 1u;
constexpr std::uint32_t kFlagHot      = 2u;
constexpr std::size_t kLargeBucketBudget = 4 * 1024 * 1024;   // целевой объём кэша на бакет
constexpr std::size_t kLargePerBucketMax = 512;               // потолок числа блоков на бакет
constexpr std::size_t kSegmentCacheBytes = 16 * 1024 * 1024;  // общий лимит кэша пустых малых сегментов

struct FreeNode {
    FreeNode* next;
};

struct ThreadHeap;

struct Segment {
    std::uint32_t magic;
    std::uint32_t size_class;
    std::uint32_t block_size;
    std::uint32_t reserved;
    FreeNode*     free_list;
    std::atomic<FreeNode*>   remote_free;
    std::atomic<ThreadHeap*> owner;
    std::size_t   used;
    std::size_t   total_bytes;
    Segment*      next;
};

struct ThreadHeap {
    Segment* segments[kNumClasses]{};
    Segment* active[kNumClasses]{};
#ifdef PM_STATS
    std::uint64_t n_alloc  = 0;   
    std::uint64_t n_free   = 0;   
    std::uint64_t n_remote = 0;   
#endif
};

constexpr std::size_t align_up(std::size_t v, std::size_t a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

constexpr std::size_t kHeader = align_up(sizeof(Segment), kAlign);

inline std::atomic<Segment*> g_abandoned{nullptr};

// Выравниваем для предотвращения ложного разделения кэш-линий в многопоточной среде
struct alignas(64) SegmentCacheNode {
    Segment* head;
    std::atomic<std::size_t> count;
    std::atomic<int> lock;
};

inline SegmentCacheNode g_seg_cache[kNumClasses];
inline std::atomic<std::size_t> g_seg_cache_bytes{0};

inline std::size_t page_size() noexcept {
    static const std::size_t ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return ps;
}

inline std::atomic<std::size_t>   g_os_bytes{0};    
inline std::atomic<std::size_t>   g_os_peak{0};    
inline std::atomic<std::uint64_t> g_os_maps{0};      
inline std::atomic<std::uint64_t> g_os_unmaps{0};    
inline std::atomic<std::uint64_t> g_large_allocs{0}; 
inline std::atomic<std::uint64_t> g_large_hits{0};   
inline std::atomic<std::uint64_t> g_dead_alloc{0}, g_dead_free{0}, g_dead_remote{0};

inline void os_free(void* p, std::size_t bytes) noexcept {
    ::munmap(p, bytes);
    g_os_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    g_os_unmaps.fetch_add(1, std::memory_order_relaxed);
}

inline void os_advise_free(void* p, std::size_t bytes) noexcept {
#if defined(MADV_FREE)
    if (::madvise(p, bytes, MADV_FREE) == 0) return;
#endif
    ::madvise(p, bytes, MADV_DONTNEED);
}

inline void* os_alloc_aligned(std::size_t bytes, std::size_t align) noexcept {
    if (bytes > SIZE_MAX - align) return nullptr;
    std::size_t len = bytes + align;
    void* m = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return nullptr;
    char* raw  = static_cast<char*>(m);
    char* base = reinterpret_cast<char*>(
        align_up(reinterpret_cast<std::uintptr_t>(raw), align));
    if (base > raw) ::munmap(raw, static_cast<std::size_t>(base - raw));
    char* tail   = base + bytes;
    char* rawend = raw + len;
    if (rawend > tail) ::munmap(tail, static_cast<std::size_t>(rawend - tail));
    
    std::size_t now = g_os_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    std::size_t pk  = g_os_peak.load(std::memory_order_relaxed);
    while (now > pk && !g_os_peak.compare_exchange_weak(pk, now, std::memory_order_relaxed)) {
    }
    g_os_maps.fetch_add(1, std::memory_order_relaxed);

    return base;
}

inline std::size_t classify(std::size_t size) noexcept {
    if (size == 0) return 0;
    if (size > kSmallMax) return kLargeClass;
    return (size + kAlign - 1) / kAlign - 1;
}

inline std::size_t block_size_of(std::size_t ci) noexcept {
    return (ci + 1) * kAlign;
}

inline void drain(Segment* s) noexcept {
    FreeNode* r = s->remote_free.exchange(nullptr, std::memory_order_acquire);
    if (!r) return;
    std::size_t n = 1;
    FreeNode* tail = r;
    while (tail->next) {
        tail = tail->next;
        ++n;
    }
    tail->next   = s->free_list;
    s->free_list = r;
    s->used -= n;
}

inline Segment* segment_cache_pop(std::size_t ci) noexcept {
    SegmentCacheNode& sc = g_seg_cache[ci];
    while (sc.lock.exchange(1, std::memory_order_acquire)) {
        sched_yield(); // Уступаем процессор, если блокировка занята
    }
    Segment* s = sc.head;
    if (s) {
        sc.head = s->next;
        sc.count.fetch_sub(1, std::memory_order_relaxed);
        g_seg_cache_bytes.fetch_sub(kSegmentSize, std::memory_order_relaxed);
    }
    sc.lock.store(0, std::memory_order_release);
    return s;
}

inline void segment_cache_push(Segment* s, std::size_t ci) noexcept {
    if (g_seg_cache_bytes.fetch_add(kSegmentSize, std::memory_order_relaxed) + kSegmentSize
        > kSegmentCacheBytes) {
        g_seg_cache_bytes.fetch_sub(kSegmentSize, std::memory_order_relaxed);
        os_free(s, kSegmentSize);
        return;
    }
    SegmentCacheNode& sc = g_seg_cache[ci];
    while (sc.lock.exchange(1, std::memory_order_acquire)) {
        sched_yield();
    }
    s->next = sc.head;
    sc.head = s;
    sc.count.fetch_add(1, std::memory_order_relaxed);
    sc.lock.store(0, std::memory_order_release);
}

/// Возвращает ОС все сегменты из кэша пустых малых сегментов.
inline void pm_segment_cache_release() noexcept {
    for (std::size_t ci = 0; ci < kNumClasses; ++ci) {
        SegmentCacheNode& sc = g_seg_cache[ci];
        while (sc.lock.exchange(1, std::memory_order_acquire)) {
            sched_yield();
        }
        Segment* list = sc.head;
        sc.head = nullptr;
        sc.count.store(0, std::memory_order_relaxed);
        sc.lock.store(0, std::memory_order_release);
        while (list) {
            Segment* next = list->next;
            g_seg_cache_bytes.fetch_sub(kSegmentSize, std::memory_order_relaxed);
            os_free(list, kSegmentSize);
            list = next;
        }
    }
}

inline void heap_release(ThreadHeap& h) noexcept {
#ifdef PM_STATS
    g_dead_alloc.fetch_add(h.n_alloc, std::memory_order_relaxed);
    g_dead_free.fetch_add(h.n_free, std::memory_order_relaxed);
    g_dead_remote.fetch_add(h.n_remote, std::memory_order_relaxed);
    h.n_alloc = h.n_free = h.n_remote = 0;
#endif
    for (std::size_t ci = 0; ci < kNumClasses; ++ci) {
        Segment* s = h.segments[ci];
        while (s) {
            Segment* next = s->next;
            drain(s);
            if (s->used == 0) {
                segment_cache_push(s, ci);
            } else {
                s->owner.store(nullptr, std::memory_order_release);
                Segment* head = g_abandoned.load(std::memory_order_relaxed);
                do {
                    s->next = head;
                } while (!g_abandoned.compare_exchange_weak(
                    head, s, std::memory_order_release, std::memory_order_relaxed));
            }
            s = next;
        }
        h.segments[ci] = nullptr;
        h.active[ci]   = nullptr;
    }
}

struct TlsHeap {
    ThreadHeap heap{};
    ~TlsHeap() noexcept { heap_release(heap); }
};

inline ThreadHeap& tls_heap() noexcept {
    thread_local TlsHeap t;
    return t.heap;
}

PM_NOINLINE inline Segment* segment_new(ThreadHeap& h, std::size_t ci) noexcept {
    Segment* seg = segment_cache_pop(ci);
    if (seg) {
        // Оптимизация: сегмент уже содержит готовый free_list, так как он попадает 
        // в кэш только при used == 0. Не нужно пересобирать список в O(N).
        seg->owner.store(&h, std::memory_order_relaxed);
        seg->next      = h.segments[ci];
        h.segments[ci] = seg;
        return seg;
    }

    std::size_t bs = block_size_of(ci);
    if (kHeader + bs > kSegmentSize) return nullptr;

    void* raw = os_alloc_aligned(kSegmentSize, kSegmentSize);
    if (!raw) return nullptr;

    seg = new (raw) Segment{};
    seg->magic       = kSegmentMagic;
    seg->size_class  = static_cast<std::uint32_t>(ci);
    seg->block_size  = static_cast<std::uint32_t>(bs);
    seg->free_list   = nullptr;
    seg->used        = 0;
    seg->total_bytes = kSegmentSize;
    seg->owner.store(&h, std::memory_order_relaxed);

    std::size_t count = (kSegmentSize - kHeader) / bs;
    char* start = static_cast<char*>(raw) + kHeader;
    for (std::size_t k = count; k-- > 0;) {
        auto* node = reinterpret_cast<FreeNode*>(start + k * bs);
        node->next = seg->free_list;
        seg->free_list = node;
    }

    seg->next      = h.segments[ci];
    h.segments[ci] = seg;
    return seg;
}

PM_NOINLINE inline void adopt_abandoned(ThreadHeap& h) noexcept {
    Segment* list = g_abandoned.exchange(nullptr, std::memory_order_acquire);
    while (list) {
        Segment* next = list->next;
        list->owner.store(&h, std::memory_order_release);
        drain(list);
        std::size_t ci = list->size_class;
        if (list->used == 0) {
            segment_cache_push(list, ci);
        } else {
            list->next     = h.segments[ci];
            h.segments[ci] = list;
        }
        list = next;
    }
}

inline Segment* find_segment(ThreadHeap& h, std::size_t ci) noexcept {
    for (Segment* s = h.segments[ci]; s; s = s->next) {
        if (!s->free_list) drain(s);
        if (s->free_list) return s;
    }
    return nullptr;
}

PM_NOINLINE inline Segment* refill(ThreadHeap& h, std::size_t ci) noexcept {
    Segment* s = find_segment(h, ci);
    if (!s) {
        adopt_abandoned(h);
        s = find_segment(h, ci);
    }
    if (!s) s = segment_new(h, ci);
    if (s) h.active[ci] = s;
    return s;
}

inline Segment* segment_of(const void* p) noexcept {
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(p);
    std::uintptr_t base = addr & ~(static_cast<std::uintptr_t>(kSegmentSize) - 1);
    auto* seg = reinterpret_cast<Segment*>(base);
    if (seg->magic != kSegmentMagic && seg->magic != kLargeMagic) return nullptr;
    return seg;
}

inline char* block_start(Segment* seg, void* p) noexcept {
    char* first = reinterpret_cast<char*>(seg) + kHeader;
    std::size_t off = static_cast<std::size_t>(static_cast<char*>(p) - first);
    return first + (off / seg->block_size) * seg->block_size;
}

// Выравниваем кэш больших бакетов
struct alignas(64) LargeBucket {
    std::atomic<int> lock{0};
    Segment*         head  = nullptr;
    std::size_t      count = 0;
};

inline LargeBucket g_large[kLargeBuckets];
inline std::atomic<std::size_t> g_large_bytes{0};
inline std::atomic<std::size_t> g_large_hot{0};

inline void bucket_lock(LargeBucket& b) noexcept {
    while (b.lock.exchange(1, std::memory_order_acquire)) {
        sched_yield();
    }
}

inline std::size_t bucket_cap(std::size_t total) noexcept {
    std::size_t c = kLargeBucketBudget / total;
    return c < kLargePerBucket ? kLargePerBucket
                               : (c > kLargePerBucketMax ? kLargePerBucketMax : c);
}

inline void bucket_unlock(LargeBucket& b) noexcept {
    b.lock.store(0, std::memory_order_release);
}

inline std::size_t large_round(std::size_t n, std::size_t& idx) noexcept {
    if (n <= kLargeMinBucket) {
        idx = 0;
        return kLargeMinBucket;
    }
    std::size_t m = 0;
    for (std::size_t x = n - 1; x >>= 1;) ++m;
    if (m > 24) {
        idx = kLargeBuckets;
        return n;
    }
    std::size_t step = std::size_t(1) << (m - 2);
    std::size_t k    = (n + step - 1) / step;
    idx = 1 + (m - 14) * 4 + (k - 5);
    return k * step;
}

inline Segment* large_pop(std::size_t idx) noexcept {
    LargeBucket& b = g_large[idx];
    bucket_lock(b);
    Segment* s = b.head;
    if (s) {
        b.head = s->next;
        b.count--;
        g_large_bytes.fetch_sub(s->total_bytes, std::memory_order_relaxed);
        if (s->reserved & kFlagHot)
            g_large_hot.fetch_sub(s->total_bytes, std::memory_order_relaxed);
    }
    bucket_unlock(b);
    if (s) {
        s->next     = nullptr;
        s->reserved = 0;
    }
    return s;
}

inline void large_release(Segment* seg) noexcept {
    std::size_t idx   = seg->block_size;
    std::size_t total = seg->total_bytes;
    if (idx >= kLargeBuckets ||
        g_large_bytes.load(std::memory_order_relaxed) + total > kLargeCacheMax) {
        os_free(seg, total);
        return;
    }

    LargeBucket& b = g_large[idx];
    bucket_lock(b);
    if (b.count >= bucket_cap(total)) {
        bucket_unlock(b);
        os_free(seg, total);
        return;
    }
    seg->reserved = kFlagHot;
    seg->next     = b.head;
    b.head        = seg;
    b.count++;
    g_large_bytes.fetch_add(total, std::memory_order_relaxed);
    g_large_hot.fetch_add(total, std::memory_order_relaxed);
    bucket_unlock(b);
}

inline void pm_large_trim() noexcept {
    std::size_t ps = page_size();
    for (std::size_t i = 0; i < kLargeBuckets; ++i) {
        LargeBucket& b = g_large[i];
        bucket_lock(b);
        for (Segment* s = b.head; s; s = s->next) {
            if (s->reserved & kFlagHot) {
                if (s->total_bytes > ps)
                    os_advise_free(reinterpret_cast<char*>(s) + ps, s->total_bytes - ps);
                s->reserved &= ~kFlagHot;
                g_large_hot.fetch_sub(s->total_bytes, std::memory_order_relaxed);
            }
        }
        bucket_unlock(b);
    }
}

inline void pm_large_release() noexcept {
    for (std::size_t i = 0; i < kLargeBuckets; ++i) {
        LargeBucket& b = g_large[i];
        bucket_lock(b);
        Segment* list = b.head;
        b.head  = nullptr;
        b.count = 0;
        for (Segment* s = list; s; s = s->next) {
            g_large_bytes.fetch_sub(s->total_bytes, std::memory_order_relaxed);
            if (s->reserved & kFlagHot)
                g_large_hot.fetch_sub(s->total_bytes, std::memory_order_relaxed);
        }
        bucket_unlock(b);
        while (list) {
            Segment* next = list->next;
            os_free(list, list->total_bytes);
            list = next;
        }
    }
}

PM_NOINLINE inline void* large_alloc(std::size_t size, std::size_t alignment) noexcept {
    std::size_t offset = align_up(kHeader, alignment);
    if (offset >= kSegmentSize) return nullptr;
    std::size_t ps = page_size();
    if (size > SIZE_MAX - offset - ps) return nullptr;

    std::size_t idx;
    std::size_t total = align_up(large_round(offset + size, idx), ps);

    g_large_allocs.fetch_add(1, std::memory_order_relaxed);
    if (idx < kLargeBuckets) {
        Segment* c = large_pop(idx);
        if (c) {
            g_large_hits.fetch_add(1, std::memory_order_relaxed);
            return reinterpret_cast<char*>(c) + offset;
        }
    }

    void* raw = os_alloc_aligned(total, kSegmentSize);
    if (!raw) return nullptr;

    auto* seg = new (raw) Segment{};
    seg->magic       = kLargeMagic;
    seg->size_class  = static_cast<std::uint32_t>(kLargeClass);
    seg->block_size  = static_cast<std::uint32_t>(idx);
    seg->reserved    = kFlagFresh;
    seg->total_bytes = total;
    return static_cast<char*>(raw) + offset;
}

inline void* pm_malloc(std::size_t size) noexcept {
    if (size == 0) size = 1;
    if (size > kSmallMax) return large_alloc(size, kAlign);

    std::size_t ci = classify(size);
    ThreadHeap& h  = tls_heap();

    Segment* seg = h.active[ci];
    if (!seg || !seg->free_list) {
        seg = refill(h, ci);
        if (!seg) return nullptr;
    }

    FreeNode* node = seg->free_list;
    seg->free_list = node->next;
    seg->used++;
    PM_STAT(h.n_alloc++);
    return node;
}

inline std::size_t pm_usable_size(void* p) noexcept {
    if (!p) return 0;
    Segment* seg = segment_of(p);
    if (!seg) return 0;
    char* end;
    if (seg->magic == kLargeMagic) {
        end = reinterpret_cast<char*>(seg) + seg->total_bytes;
    } else {
        end = block_start(seg, p) + seg->block_size;
    }
    return static_cast<std::size_t>(end - static_cast<char*>(p));
}

inline void pm_free(void* p) noexcept {
    if (!p) return;

    Segment* seg = segment_of(p);
    if (!seg) return;

    if (seg->magic == kLargeMagic) {
        large_release(seg);
        return;
    }

    auto* node = reinterpret_cast<FreeNode*>(block_start(seg, p));
    ThreadHeap& h = tls_heap();

    if (seg->owner.load(std::memory_order_acquire) == &h) {
        node->next     = seg->free_list;
        seg->free_list = node;
        seg->used--;
        PM_STAT(h.n_free++);
    } else {
        PM_STAT(h.n_remote++);
        FreeNode* head = seg->remote_free.load(std::memory_order_relaxed);
        do {
            node->next = head;
        } while (!seg->remote_free.compare_exchange_weak(
            head, node, std::memory_order_release, std::memory_order_relaxed));
    }
}

inline void* pm_calloc(std::size_t n, std::size_t size) noexcept {
    if (size != 0 && n > SIZE_MAX / size) return nullptr;
    std::size_t total = n * size;
    void* p = pm_malloc(total);
    if (!p) return nullptr;
    Segment* seg = segment_of(p);
    if (seg && seg->magic == kLargeMagic && (seg->reserved & kFlagFresh)) return p;
    std::memset(p, 0, total);
    return p;
}

inline void* pm_realloc(void* p, std::size_t new_size) noexcept {
    if (!p) return pm_malloc(new_size);
    if (new_size == 0) { pm_free(p); return nullptr; }

    std::size_t old_size = pm_usable_size(p);
    if (new_size <= old_size) {
        if (new_size >= old_size / 2) return p;
        Segment* seg = segment_of(p);
        if (seg && seg->magic == kSegmentMagic && classify(new_size) == seg->size_class)
            return p;
    }

    void* np = pm_malloc(new_size);
    if (!np) return nullptr;
    std::size_t copy = old_size < new_size ? old_size : new_size;
    std::memcpy(np, p, copy);
    pm_free(p);
    return np;
}

inline void* pm_malloc_aligned(std::size_t size, std::size_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;   // только степень двойки
    if (alignment <= kAlign) return pm_malloc(size);
    if (size == 0) size = 1;
    if (size > SIZE_MAX - alignment) return nullptr;

    std::size_t total = size + alignment;
    if (total <= kSmallMax) {
        void* raw = pm_malloc(total);
        if (!raw) return nullptr;
        std::uintptr_t aligned = align_up(reinterpret_cast<std::uintptr_t>(raw), alignment);
        return reinterpret_cast<void*>(aligned);
    }
    return large_alloc(size, alignment);
}

inline void pm_free_aligned(void* p) noexcept {
    pm_free(p);
}

inline void pm_collect() noexcept {
    ThreadHeap& h = tls_heap();
    for (std::size_t ci = 0; ci < kNumClasses; ++ci) {
        Segment* prev = nullptr;
        Segment* cur  = h.segments[ci];
        while (cur) {
            Segment* next = cur->next;
            drain(cur);
            if (cur->used == 0) {
                if (prev) prev->next = next;
                else      h.segments[ci] = next;
                if (h.active[ci] == cur) h.active[ci] = nullptr;
                os_free(cur, kSegmentSize);
            } else {
                prev = cur;
            }
            cur = next;
        }
    }
    pm_segment_cache_release();
    pm_large_trim();
}

struct Stats {
    std::size_t   os_bytes;          
    std::size_t   os_peak_bytes;      
    std::size_t   large_cache_bytes;   
    std::size_t   large_cache_hot;    
    std::uint64_t os_maps;            
    std::uint64_t os_unmaps;          
    std::uint64_t large_allocs;       
    std::uint64_t large_cache_hits;   
    std::uint64_t exited_allocs;
    std::uint64_t exited_frees;
    std::uint64_t exited_remote_frees;
};

inline Stats pm_stats() noexcept {
    Stats s{};
    s.os_bytes            = g_os_bytes.load(std::memory_order_relaxed);
    s.os_peak_bytes       = g_os_peak.load(std::memory_order_relaxed);
    s.large_cache_bytes   = g_large_bytes.load(std::memory_order_relaxed);
    s.large_cache_hot     = g_large_hot.load(std::memory_order_relaxed);
    s.os_maps             = g_os_maps.load(std::memory_order_relaxed);
    s.os_unmaps           = g_os_unmaps.load(std::memory_order_relaxed);
    s.large_allocs        = g_large_allocs.load(std::memory_order_relaxed);
    s.large_cache_hits    = g_large_hits.load(std::memory_order_relaxed);
    s.exited_allocs       = g_dead_alloc.load(std::memory_order_relaxed);
    s.exited_frees        = g_dead_free.load(std::memory_order_relaxed);
    s.exited_remote_frees = g_dead_remote.load(std::memory_order_relaxed);
    return s;
}

struct ThreadStats {
    std::size_t   segments;        
    std::size_t   reserved_bytes;  
    std::size_t   live_blocks;     
    std::size_t   live_bytes;      
    double        utilization;     
    std::uint64_t allocs;          
    std::uint64_t frees;           
    std::uint64_t remote_frees;    
};

inline ThreadStats pm_thread_stats() noexcept {
    ThreadStats t{};
    ThreadHeap& h = tls_heap();
    for (std::size_t ci = 0; ci < kNumClasses; ++ci) {
        for (Segment* s = h.segments[ci]; s; s = s->next) {
            drain(s);
            ++t.segments;
            t.live_blocks += s->used;
            t.live_bytes  += s->used * s->block_size;
        }
    }
    t.reserved_bytes = t.segments * kSegmentSize;
    t.utilization    = t.reserved_bytes ? double(t.live_bytes) / double(t.reserved_bytes) : 0.0;
#ifdef PM_STATS
    t.allocs = h.n_alloc;
    t.frees  = h.n_free;
    t.remote_frees = h.n_remote;
#endif
    return t;
}

template <typename T>
struct pmallocator_cpp {
    using value_type = T;

    pmallocator_cpp() noexcept = default;
    template <typename U>
    pmallocator_cpp(const pmallocator_cpp<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n > SIZE_MAX / sizeof(T)) throw std::bad_alloc();
        void* p = alignof(T) > kAlign
            ? pm_malloc_aligned(n * sizeof(T), alignof(T))
            : pm_malloc(n * sizeof(T));
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, std::size_t) noexcept {
        pm_free(p);
    }

    template <typename U>
    bool operator==(const pmallocator_cpp<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const pmallocator_cpp<U>&) const noexcept { return false; }
};
#if defined(_MSC_VER)
#include <intrin.h>
#define PPTR_RETADDR() _ReturnAddress()
#else
#define PPTR_RETADDR() __builtin_return_address(0)
#endif
#define free_mem(p) pm_free(p)
#define malloc_mem(p) pm_malloc(p)
#define collect_mem() pm_collect()
namespace pptr {
    constexpr uint64_t max_ptrs = 128;
    inline std::atomic<uint64_t> id_counter{ 1 };
    inline uint64_t set_id() {
        return id_counter.fetch_add(1, std::memory_order_relaxed);
    }
    struct ptr_data {
        std::array<std::atomic<void*>, max_ptrs>    addr{};
        std::array<std::atomic<uint64_t>, max_ptrs> id_ptr{};
        std::array<std::atomic<uint32_t>, max_ptrs> pins{};
    };
    inline ptr_data ps;
    inline thread_local uint64_t tls_start_idx = 0;
    inline std::atomic<void*> garbage_head{ nullptr };
    inline void flush_garbage() {
        void* curr = garbage_head.exchange(nullptr, std::memory_order_acquire);
        while (curr) {
            void* next = *static_cast<void**>(curr);
            free_mem(curr);
            curr = next;
        }
    }
    template <typename T>
    struct handle {
        T* p = nullptr;
        uint64_t idx = 0;
        uint64_t id = 0;
        bool valid() const {
            return p && id != 0 && idx < max_ptrs &&
                ps.id_ptr[idx].load(std::memory_order_acquire) == id &&
                ps.addr[idx].load(std::memory_order_acquire) == static_cast<void*>(p);
        }
        T* get() const {
            return valid() ? p : nullptr;
        }
        T* operator->() const {
            T* r = get();
            if (!r) {
                #if defined(_WIN32)
                void* base = (void*)GetModuleHandleW(nullptr);
                std::fprintf(stderr,
                    "pptr: UAF detected [ idx=%llu id=%llu, used from %p ]\nmodule base %p, rva 0x%llx\ncheck llvm-symbolizer --relative-address --obj=app.exe <rva>\n",
                    (unsigned long long)idx, (unsigned long long)id, PPTR_RETADDR(), base,
                    (unsigned long long)((char*)PPTR_RETADDR() - (char*)base));
                #else
                std::fprintf(stderr, "pptr: UAF detected [ idx=%llu id=%llu, used from %p ]\n",(unsigned long long)idx, (unsigned long long)id, PPTR_RETADDR());
                std::printf("abort()!\n");
                std::abort(); 
                #endif
                std::fflush(stderr);
                std::abort();
            }
            return r;
        }
        explicit operator bool() const { return valid(); }
        bool operator==(std::nullptr_t) const { return !valid(); }
        bool operator!=(std::nullptr_t) const { return valid(); }
    };
    template <typename T>
    class pin_guard {
        T* p_ = nullptr;
        uint64_t idx_ = 0;
    public:
        explicit pin_guard(const handle<T>& h) {
            if (!h.p || h.id == 0 || h.idx >= max_ptrs) return;

            ps.pins[h.idx].fetch_add(1, std::memory_order_seq_cst);   // 1. заявляем пин
            if (ps.id_ptr[h.idx].load(std::memory_order_seq_cst) == h.id &&
                ps.addr[h.idx].load(std::memory_order_seq_cst) == static_cast<void*>(h.p)) {  // 2. перепроверяем
                p_ = h.p;
                idx_ = h.idx;
            }
            else {
                ps.pins[h.idx].fetch_sub(1, std::memory_order_release);
            }
        }
        ~pin_guard() {
            if (p_) ps.pins[idx_].fetch_sub(1, std::memory_order_release);
        }
        pin_guard(const pin_guard&) = delete;
        pin_guard& operator=(const pin_guard&) = delete;
        pin_guard(pin_guard&& o) noexcept : p_(o.p_), idx_(o.idx_) { o.p_ = nullptr; }

        explicit operator bool() const { return p_ != nullptr; }
        T* operator->() const { return p_; }
        T& operator*()  const { return *p_; }
    };
    template <typename T, typename... Args>
    handle<T> ptr(Args&&... args) {
        constexpr size_t alloc_size =
            sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
        void* mem = malloc_mem(alloc_size);
        if (!mem) return {};
        uint64_t start = tls_start_idx;
        for (uint64_t i = start; i < start + max_ptrs; ++i) {
            uint64_t k = i % max_ptrs;
            if (ps.addr[k].load(std::memory_order_relaxed) != nullptr) continue;
            void* expected = nullptr;
            if (ps.addr[k].compare_exchange_strong(expected, mem, std::memory_order_acq_rel)) {
                T* obj;
                try {
                    obj = std::construct_at(static_cast<T*>(mem), std::forward<Args>(args)...);
                }
                catch (...) {
                    ps.addr[k].store(nullptr, std::memory_order_release);
                    pm_free(mem);
                    throw;
                }
                uint64_t id = set_id();
                ps.id_ptr[k].store(id, std::memory_order_release);
                tls_start_idx = (k + 1) % max_ptrs;
                return { obj, k, id };
            }
        }
        pm_free(mem);
        return {};
    }
    template <typename T>
    bool del(handle<T>& h, uint8_t is_nullptr_set = 0, uint8_t is_collect = 0) {
        if (!h.p || h.idx >= max_ptrs) return false;
        uint64_t expected = h.id;
        if (!ps.id_ptr[h.idx].compare_exchange_strong(
            expected, 0, std::memory_order_seq_cst)) {
                  #if defined(_WIN32)
            void* base = (void*)GetModuleHandleW(nullptr);
            std::fprintf(stderr, "pptr: Warn! After del call to Deleted pointer (Double Free) detected! return false rva=0x%llx module_stack_base=%p ptr=%p", (unsigned long long)((char*)PPTR_RETADDR() - (char*)base),base,h.get());
          #else
            std::fprintf(stderr, "pptr: Warn! After del call to Deleted pointer (Double Free) detected! return false addr=%p ptr=%p", PPTR_RETADDR(), h.get());
#endif
            return false;
        }
        while (ps.pins[h.idx].load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
        T* obj = h.p;
        if constexpr (!std::is_trivially_destructible_v<T>) {
            obj->~T();
        }
        ps.addr[h.idx].store(nullptr, std::memory_order_release);
        void* mem = static_cast<void*>(obj);
        void** node = static_cast<void**>(mem);
        void* old_head = garbage_head.load(std::memory_order_relaxed);
        do {
            *node = old_head;
        } while (!garbage_head.compare_exchange_weak(
            old_head, mem, std::memory_order_release, std::memory_order_relaxed));
        if (is_collect) collect_mem();
        if (is_nullptr_set) h = {};
        return true;
    }
}
}