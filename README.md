# PahomAlloc (pmalloc)

Высокопроизводительный slab-аллокатор памяти общего назначения с локальными TLS-кучами, lock-free очередью межпоточного освобождения и динамическим кэшем крупных сегментов. Разработан специально для игрового движка PahomEngine и требовательных системных сред на C++ и Rust.

```
Внимание! pmalloc_posix.hpp пока в активной доработке. Основной и максимально оптимизированный таргет на данный момент — Windows (pmalloc_win.hpp).
```

Header-only, C++17, всё оформлено в `namespace pmalloc`. Подключайте `pmalloc_win.hpp` на Windows или `pmalloc_posix.hpp` на Linux/macOS. Линковать внешние зависимости не требуется.

---

## Архитектура и особенности

* **Slab-модель сегментов:** память разбита на фиксированные изолированные сегменты по 64 КБ (`kSegmentSize`), выровненные в адресном пространстве. Базовый адрес сегмента вычисляется мгновенной битовой маской `addr & ~(kSegmentSize - 1)` без поиска по деревьям или хеш-таблицам.
* **Нулевая синхронизация на локальном пути:** каждый поток владеет своей кучей (`ThreadHeap`) через TLS (`TlsHeap`). Выделение и освобождение в потоке-владельце выполняются за несколько ассемблерных инструкций с вершиной списка в L1/L2 кэше.
* **Lock-free Remote Free:** межпоточные освобождения аккумулируются в атомарный стек `remote_free` через `compare_exchange_weak` с барьерами памяти `Release`/`Acquire` и переносятся пачкой (`drain()`) при нехватке блоков в локальной куче.
* **Горячий кэш больших блоков:** блоки от 8 КБ обслуживаются массивом размерных бакетов (`LargeBucket`) с динамическим расчётом вместимости `bucket_cap(total)`. Повторные крупные аллокации забираются напрямую из памяти без единого системного вызова в ядро Windows.

---

## Бенчмарки

### 1. Реальная нагрузка: компилятор AST `jrsonnet` (манифест `graalvm`)
Тестирование выполнения тяжелейшего манифеста GraalVM через `hyperfine` (50 прогонов, 3 прогревочных):

| Аллокатор | Среднее время (Mean $\pm \sigma$) | Min | Относительно pmalloc |
|---|---|---|---|
| **pmalloc (PahomAlloc)** | **97.5 ms $\pm$ 1.5 ms** | **94.9 ms** | **базовый (быстрее всех)** |
| **mimalloc** | 101.4 ms $\pm$ 3.0 ms | 98.1 ms | +4.0% медленнее |
| **system (HeapAlloc)** | 134.4 ms $\pm$ 1.9 ms | 132.2 ms | +37.7% (в 1.4x медленнее) |
jrsonnet - pmalloc
jrsonnet2 - mimalloc
jrsonet2-target-sys - System Alloc
<img width="995" height="518" alt="image" src="https://github.com/user-attachments/assets/e0d96443-6ec8-401b-b0a4-0278b455c321" />

> *За счёт меньшей дисперсии ($\sigma = 1.5\text{ ms}$ против $3.0\text{ ms}$ у mimalloc) PahomAlloc удерживает стабильный фреймпейсинг без пиковых задержек и просадок.*

### 2. Синтетические тесты throughput (`clang-cl`, Windows 10, 4 потока)

| Сценарий теста | pmalloc (время / Mops) | mimalloc (время / Mops) | Разница |
|---|---|---|---|
| `pairs 32B x10M` | **62.9 ms (318.0 Mops/s)** | 93.6 ms (213.6 Mops/s) | **pmalloc в 1.5x быстрее** |
| `pairs 1KB x5M` | **31.3 ms (319.5 Mops/s)** | 60.3 ms (165.8 Mops/s) | **pmalloc почти в 2x быстрее** |
| `large 1MB x20k` | **0.3 ms (121.4 Mops/s)** | 14.2 ms (2.8 Mops/s) | **pmalloc более чем в 40x быстрее** |
| `threads mixed (RSS)` | **40.6 MB RSS** | 172.0 MB RSS | **pmalloc компактнее в 4.2 раза** |
| `cross-thread free 2M` | **58.3 ms (68.6 Mops/s)** | 58.9 ms (67.9 Mops/s) | паритет при чистом lock-free |

---

## Базовые функции C++ API

```cpp
#include "pmalloc_win.hpp"

void* p = pmalloc::pm_malloc(256);
void* z = pmalloc::pm_calloc(100, sizeof(int));
p = pmalloc::pm_realloc(p, 1024);
std::size_t n = pmalloc::pm_usable_size(p);
pmalloc::pm_free(p);
pmalloc::pm_free(z);
```

| Функция | Поведение |
|---|---|
| `pm_malloc(size)` | `nullptr` при системной ошибке. `size == 0` возвращает валидный указатель на 1 байт. Базовое выравнивание 64 байта (`kAlign`). |
| `pm_calloc(n, size)` | Обнулённая память. При переполнении `n * size` возвращает `nullptr`. Для свежих large-блоков от ОС системный `memset` пропускается. |
| `pm_realloc(p, size)` | `p == nullptr` эквивалентен `pm_malloc`. `size == 0` освобождает блок и возвращает `nullptr`. Если размер укладывается в текущий класс, возвращается тот же указатель. |
| `pm_free(p)` | `nullptr` безопасен. Корректно вызывается из любого потока без дедлоков. |
| `pm_usable_size(p)` | Доступный полезный размер блока до конца сегмента или слота. |

Все функции помечены как `noexcept`.

---

## Выровненные аллокации

```cpp
void* p = pmalloc::pm_malloc_aligned(4096, 128);
pmalloc::pm_free(p);
```

* Выравнивание строго обязано быть степенью двойки. До `kAlign` (64 байта) перенаправляется напрямую в `pm_malloc`.
* Память освобождается обычным `pm_free(p)`. Функция `pm_free_aligned` предоставлена для совместимости.

---

## STL-аллокатор C++

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

Аллокатор stateless, экземпляры всегда эквивалентны (`operator==` возвращает `true`). Для типов с `alignof(T) > kAlign` автоматически используется `pm_malloc_aligned`. При исчерпании памяти выбрасывает `std::bad_alloc`.

---

## Возврат памяти ОС

```cpp
pmalloc::pm_collect();
pmalloc::pm_large_trim();
pmalloc::pm_large_release();
```

* **`pm_collect()`** — обрабатывает кучу вызвавшего потока: освобождает полностью пустые малые сегменты обратно в Windows через `VirtualFree` и выполняет `pm_large_trim()`.
* **`pm_large_trim()`** — обходит кэш крупных сегментов и даёт ядру подсказку `MEM_RESET` на неактивные регионы памяти. Память остаётся закэшированной, но не занимает физические страницы RAM.
* **`pm_large_release()`** — глобально сбрасывает и возвращает системе абсолютно все сегменты из `LargeBucket`.

Вызывайте `pm_collect()` на стыках сцен, в паузах или при минимизации приложения. Избегайте вызова внутри высокочастотного цикла кадра.

---

## Замена глобального `new`/`delete` в C++

Разместите этот блок ровно в одном `.cpp` файле проекта:

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

## Использование как основного аллокатора в Rust

### 1. Си-обёртка (`cpp/pmalloc_allocator.cpp`)

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
    // вывод статистики при необходимости
}

}
```

### 2. Сборка через `build.rs`

Добавьте зависимость `cc` в `Cargo.toml`:
```toml
[build-dependencies]
cc = "1.0"
```

Код `build.rs`:
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

### 3. Регистрация `#[global_allocator]` в Rust

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

## Правила и ограничения

* **Строгая изоляция:** освобождать разрешено только указатели, полученные из `pmalloc`. Передача указателя из стандартного CRT `free()` или `std::free()` приведёт к чтению повреждённых метаданных сегмента.
* **Граница классов:** малые блоки обслуживаются до границы `kSmallMax` (8 КБ) с шагом `kAlign` (64 байта, 128 размерных классов). Всё, что больше 8 КБ, маршрутизируется в `large_alloc`.
* **Жизненный цикл потоков:** при завершении потока сегменты с активными блоками атомарно сбрасываются в глобальный список `g_abandoned` и прозрачно усыновляются (`adopt_abandoned`) другими живыми потоками без утечек.

---

## Конфигурация ядра

Все параметры зафиксированы константами времени компиляции в заголовке файла:

| Константа | Значение | Описание |
|---|---|---|
| `kSegmentSize` | 64 КБ | Размер монолитного слэб-сегмента |
| `kAlign` | 64 байта | Базовый квант выравнивания и шаг размерных классов |
| `kSmallMax` | 8 КБ | Верхний предел малых слэб-аллокаций |
| `kNumClasses` | 128 | Количество размерных бакетов малых блоков |
| `kLargeBuckets` | 45 | Количество степенных бакетов кэша крупных блоков |
| `kLargeCacheMax` | 128 МБ | Предельный суммарный объем закэшированных large-сегментов |
| `kLargeHotBudget` | 16 МБ | Бюджет активных горячих страниц до сброса `MEM_RESET` |
| `kLargeBucketBudget` | 4 МБ | Бюджет памяти на отдельный large-бакет |
| `kLargePerBucketMax` | 512 | Максимальное число свободных сегментов в одном бакете |
