# PahomAlloc (pmalloc)

High-performance general-purpose slab memory allocator featuring thread-local (TLS) heaps, a lock-free remote deallocation queue, and dynamic caching for large segments. Engineered specifically for PahomEngine and throughput-critical systems in C++ and Rust.

```
Warning: pmalloc_posix.hpp is actively being stabilized. The primary, highly tuned production target is currently Windows (pmalloc_win.hpp).
```

Header-only, C++17, encapsulated within `namespace pmalloc`. Include `pmalloc_win.hpp` on Windows or `pmalloc_posix.hpp` on Linux/macOS. No external library linking required.

---

## Architecture & Design Highlights

- **Segment-Based Slab Model:** Memory is partitioned into fixed 64 KB segments (`kSegmentSize`) aligned to their size boundary. Base segment addresses are resolved via an instantaneous bitwise mask `addr & ~(kSegmentSize - 1)`, completely eliminating hash tables and lookup trees.
- **Zero Synchronization on Fast Path:** Each thread owns its thread-local heap (`ThreadHeap`) through TLS (`TlsHeap`). Allocation and deallocation within the owning thread complete in a handful of assembly instructions with top-of-stack pointers kept hot in L1/L2 cache.
- **Lock-Free Remote Free:** Cross-thread deallocations push onto an atomic `remote_free` singly linked stack via `compare_exchange_weak` with `Release`/`Acquire` memory fences. The owning thread drains these nodes in bulk (`drain()`) whenever local freelists require replenishment.
- **Dynamic Hot-Cache for Large Blocks:** Allocations larger than 8 KB map to power-scaled cache buckets (`LargeBucket`) governed by a dynamic capacity cap (`bucket_cap(total)`). Repeated large buffers are recycled in-place without triggering kernel transitions or page table updates.

---

## Benchmarks

### 1. Real-World Workload: `jrsonnet` AST Evaluator (GraalVM Manifest)
Execution benchmark compiling the complex GraalVM jsonnet specification measured with `hyperfine` (50 runs, 3 warmup runs):

| Allocator | Mean Wall Time ($\text{Mean} \pm \sigma$) | Min | Relative Speed |
|---|---|---|---|
| **pmalloc (PahomAlloc)** | **97.5 ms $\pm$ 1.5 ms** | **94.9 ms** | **Baseline (Fastest)** |
| **mimalloc** | 101.4 ms $\pm$ 3.0 ms | 98.1 ms | +4.0% slower |
| **system (HeapAlloc)** | 134.4 ms $\pm$ 1.9 ms | 132.2 ms | +37.7% slower (1.4x) |

*jrsonnet binary mapping:*
- `jrsonnet` — compiled with `pmalloc`
- `jrsonnet2` — compiled with `mimalloc`
- `jrsonnet2-target-sys` — compiled with system `HeapAlloc`

<img width="995" height="518" alt="jrsonnet realworld benchmark result" src="https://github.com/user-attachments/assets/e0d96443-6ec8-401b-b0a4-0278b455c321" />

> *Benefiting from tighter variance ($\sigma = 1.5\text{ ms}$ vs $3.0\text{ ms}$ for mimalloc), PahomAlloc provides superior latency predictability and frame pacing without tail degradation.*

### 2. Microbenchmarks Throughput (`clang-cl`, Windows 10, 4 Threads)

| Benchmark Scenario | pmalloc (Time / Mops) | mimalloc (Time / Mops) | Delta |
|---|---|---|---|
| `pairs 32B x10M` | **62.9 ms (318.0 Mops/s)** | 93.6 ms (213.6 Mops/s) | **pmalloc is 1.5x faster** |
| `pairs 1KB x5M` | **31.3 ms (319.5 Mops/s)** | 60.3 ms (165.8 Mops/s) | **pmalloc is ~2x faster** |
| `large 1MB x20k` | **0.3 ms (121.4 Mops/s)** | 14.2 ms (2.8 Mops/s) | **pmalloc is >40x faster** |
| `threads mixed (RSS)` | **40.6 MB RSS** | 172.0 MB RSS | **pmalloc uses 4.2x less RAM** |
| `cross-thread free 2M` | **58.3 ms (68.6 Mops/s)** | 58.9 ms (67.9 Mops/s) | Parity on pure atomic operations |

---

## Core C++ API

```cpp
#include "pmalloc_win.hpp"

void* p = pmalloc::pm_malloc(256);
void* z = pmalloc::pm_calloc(100, sizeof(int));
p = pmalloc::pm_realloc(p, 1024);
std::size_t n = pmalloc::pm_usable_size(p);
pmalloc::pm_free(p);
pmalloc::pm_free(z);
```

| Function | Behavior |
|---|---|
| `pm_malloc(size)` | Returns `nullptr` on system allocation failure. `size == 0` returns a valid unique pointer. Default 64-byte alignment (`kAlign`). |
| `pm_calloc(n, size)` | Zero-initialized memory block. Arithmetic overflow of `n * size` returns `nullptr`. Bypasses `memset` on fresh pages direct from OS. |
| `pm_realloc(p, size)` | `p == nullptr` behaves identically to `pm_malloc`. `size == 0` frees memory and returns `nullptr`. If requested size fits the existing slab class, returns `p` unchanged. |
| `pm_free(p)` | Safely handles `nullptr`. Callable cross-thread without lock contention. |
| `pm_usable_size(p)` | Returns total usable capacity of the block up to segment or slot boundary. |

All API functions are `noexcept`.

---

## Aligned Allocations

```cpp
void* p = pmalloc::pm_malloc_aligned(4096, 128);
pmalloc::pm_free(p);
```

- Alignment must be an integer power of two. Alignments $\le$ `kAlign` (64 bytes) route directly to `pm_malloc`.
- Deallocate with standard `pm_free(p)`. `pm_free_aligned` is exported for API parity.

---

## C++ STL Allocator

```cpp
#include <vector>
#include <unordered_map>
#include "pmalloc_win.hpp"

std::vector<int, pmalloc::pmallocator_cpp<int>> v;
v.reserve(1000);

using Map = std::unordered_map<
    int, float, std::hash<int>, std::equal_to<int>,
    pmalloc::pmallocator_cpp<std::pair<const int, float>>>;
Map m;
```

Stateless allocator where all instances compare equal. Types requiring `alignof(T) > kAlign` automatically dispatch through `pm_malloc_aligned`. Throws `std::bad_alloc` when allocation fails.

---

## Memory Reclaim & Trimming

```cpp
pmalloc::pm_collect();
pmalloc::pm_large_trim();
pmalloc::pm_large_release();
```

- **`pm_collect()`** — Scans calling thread's heap, releases empty 64 KB segments back to Windows via `VirtualFree`, and invokes `pm_large_trim()`.
- **`pm_large_trim()`** — Traverses the large cache buckets and hints `MEM_RESET` on idle pages. Keeps virtual address ranges pooled while discarding resident physical RAM frames.
- **`pm_large_release()`** — Globally purges all cached large segment allocations across all buckets.

Call `pm_collect()` during frame transitions, scene changes, or background idle loops. Avoid invoking it inside the core render loop.

---

## Global `operator new`/`delete` Override (C++)

Paste this implementation into exactly one translation unit (`.cpp`) in your application:

```cpp
#include <new>
#include "pmalloc_win.hpp"

void* operator new(std::size_t n) {
    void* p = pmalloc::pm_malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = pmalloc::pm_malloc_aligned(n, static_cast<std::size_t>(a));
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }

void operator delete(void* p) noexcept { pmalloc::pm_free(p); }
void operator delete[](void* p) noexcept { pmalloc::pm_free(p); }
void operator delete(void* p, std::size_t) noexcept { pmalloc::pm_free(p); }
void operator delete[](void* p, std::size_t) noexcept { pmalloc::pm_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { pmalloc::pm_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { pmalloc::pm_free(p); }
```

---

## Usage as Global Allocator in Rust

### 1. C FFI Wrapper (`cpp/pmalloc_allocator.cpp`)

```cpp
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

void pm_c_stats() {
    auto s = pmalloc::pm_stats();
    // emit telemetry if needed
}

}
```

### 2. Compilation via `build.rs`

Declare `cc` in your `Cargo.toml`:
```toml
[build-dependencies]
cc = "1.0"
```

Configure `build.rs`:
```rust
fn main() {
    println!("cargo:rerun-if-changed=cpp/pmalloc_allocator.cpp");
    println!("cargo:rerun-if-changed=cpp/pmalloc_win.hpp");

    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .opt_level(3)
        .file("cpp/pmalloc_allocator.cpp")
        .compile("pmalloc");
}
```

### 3. Registering `#[global_allocator]` in Rust

```rust
use std::alloc::{GlobalAlloc, Layout};

#[link(name = "pmalloc", kind = "static")]
unsafe extern "C" {
    fn pm_c_alloc(size: usize, align: usize) -> *mut u8;
    fn pm_c_dealloc(ptr: *mut u8, size: usize, align: usize);
    fn pm_c_realloc(ptr: *mut u8, old_size: usize, new_size: usize, align: usize) -> *mut u8;
    fn pm_c_stats();
}

pub struct PahomAllocator;

impl PahomAllocator {
    pub fn print_stats() {
        unsafe { pm_c_stats() };
    }
}

unsafe impl GlobalAlloc for PahomAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        unsafe { pm_c_alloc(layout.size(), layout.align()) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { pm_c_dealloc(ptr, layout.size(), layout.align()) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        unsafe { pm_c_realloc(ptr, layout.size(), new_size, layout.align()) }
    }
}

#[global_allocator]
static GLOBAL: PahomAllocator = PahomAllocator;
```

---

## Constraints & Usage Rules

- **Strict Arena Isolation:** Only pass pointers originating from `pmalloc` into `pm_free`. Supplying pointers from foreign allocators (e.g., CRT `malloc`) triggers segment boundary miscalculations and access violations.
- **Slab Boundary:** Requests $\le$ `kSmallMax` (8 KB) map to uniform slab classes stepped by `kAlign` (64 bytes, 128 classes). Allocations exceeding 8 KB transition to dynamic large-bucket recycling.
- **Thread Lifetime & Segment Adoption:** Dying threads flush segments retaining live allocations into atomic global storage `g_abandoned`. Active threads absorb (`adopt_abandoned`) these segments seamlessly on demand without memory leaks.

---

## Configuration Reference

Tunable parameters are exposed as compile-time constants in the header file:

| Constant | Value | Description |
|---|---|---|
| `kSegmentSize` | 64 KB | Slab segment allocation granularity |
| `kAlign` | 64 bytes | Fundamental alignment quantum and slab step size |
| `kSmallMax` | 8 KB | Maximum threshold for small slab allocations |
| `kNumClasses` | 128 | Total slab size class bins |
| `kLargeBuckets` | 45 | Quantized large block cache buckets |
| `kLargeCacheMax` | 128 MB | Upper memory bound for cached large segments |
| `kLargeHotBudget` | 16 MB | Active memory ceiling prior to `MEM_RESET` trimming |
| `kLargeBucketBudget` | 4 MB | Memory budget cap per individual large bucket |
| `kLargePerBucketMax` | 512 | Maximum cached segment instances allowed in one bucket |