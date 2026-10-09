# pmalloc: использование

Header-only, C++17, всё в `namespace pmalloc`. Подключайте `pmalloc.hpp` на Windows или `pmalloc_posix.hpp` на Linux/macOS. Линковать ничего не нужно.

## Базовые функции

```cpp
#include "pmalloc.hpp"

void* p = pmalloc::pm_malloc(256);
void* z = pmalloc::pm_calloc(100, sizeof(int));
p = pmalloc::pm_realloc(p, 1024);
std::size_t n = pmalloc::pm_usable_size(p);
pmalloc::pm_free(p);
pmalloc::pm_free(z);
```

| Функция | Поведение |
|---|---|
| `pm_malloc(size)` | `nullptr` при ошибке. `size == 0` даёт валидный уникальный указатель. Выравнивание 16 байт |
| `pm_calloc(n, size)` | Обнулённая память. Переполнение `n * size` даёт `nullptr` |
| `pm_realloc(p, size)` | `p == nullptr` равно `pm_malloc`. `size == 0` освобождает `p` и возвращает `nullptr`. При ошибке `p` остаётся валидным |
| `pm_free(p)` | `nullptr` допустим. Можно вызывать из любого потока |
| `pm_usable_size(p)` | Сколько байт доступно от `p` до конца блока (не меньше запрошенного) |

Все функции `noexcept` и ошибки сообщают только через `nullptr`.

## Выровненные аллокации

```cpp
void* p = pmalloc::pm_malloc_aligned(4096, 64);
pmalloc::pm_free(p);
```

- Выравнивание должно быть степенью двойки. До 16 байт это обычный `pm_malloc`.
- Максимум 32 КБ. Для 64 КБ и больше вернётся `nullptr`.
- Освобождать можно обычным `pm_free`. `pm_free_aligned` оставлен только для симметрии и делает то же самое.

## STL-аллокатор

```cpp
#include <vector>
#include <unordered_map>

std::vector<int, pmalloc::pmallocator_cpp<int>> v;
v.reserve(1000);

using Map = std::unordered_map<
    int, float, std::hash<int>, std::equal_to<int>,
    pmalloc::pmallocator_cpp<std::pair<const int, float>>>;
Map m;
```

Аллокатор без состояния, все экземпляры равны. Типы с `alignof(T) > 16` автоматически идут через `pm_malloc_aligned`. При ошибке бросает `std::bad_alloc`.

## Возврат памяти ОС

```cpp
pmalloc::pm_collect();
pmalloc::pm_large_trim();
pmalloc::pm_large_release();
```

- **`pm_collect()`** работает с кучей текущего потока: отдаёт ОС полностью пустые сегменты малых блоков. Заодно вызывает `pm_large_trim()`.
- **`pm_large_trim()`** переводит «горячие» регионы кэша large-блоков в холодные (`MADV_FREE` или `MEM_RESET`). Память остаётся закэшированной и переиспользуемой, но ОС может её забрать.
- **`pm_large_release()`** полностью выгружает кэш large-блоков. Глобальная функция, затрагивает все потоки.

Вызывайте `pm_collect()` в тихие моменты: между уровнями, при сворачивании приложения, раз в несколько секунд в простое. Не каждый кадр.

## Замена глобального `new`/`delete`

```cpp
#include <new>

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

Положите это ровно в одну `.cpp` в exe. Не мешайте с DLL, которые собраны со своим CRT и своим `new`: указатель от одного аллокатора нельзя освобождать другим.

## Правила и ограничения

- Освобождать можно только то, что выдал pmalloc. `pm_free` на чужом указателе читает память по маске адреса и может упасть.
- Нельзя смешивать с CRT: `pm_malloc` + `free()` или `malloc` + `pm_free` это порча памяти.
- Блок можно освободить из другого потока: он уйдёт в очередь сегмента-владельца и будет подобран при его следующей аллокации или `pm_collect`.
- При завершении потока его сегменты с живыми блоками передаются глобальному списку, и их подхватит другой поток. Аллокации из деструкторов `thread_local` в этом же потоке работают, но их сегменты утекут.
- Малые блоки: до 8 КБ, 512 классов с шагом 16 байт. Всё больше идёт через large-путь с округлением до бакета (до +25%) и кэшем.
- `pm_calloc` на large-блоке из кэша делает `memset`, на свежем от ОС пропускает.

## Настройки

Лежат в константах в начале файла и меняются до сборки.

| Константа | Значение | Смысл |
|---|---|---|
| `kSmallMax` | 8 КБ | Граница малых блоков |
| `kLargeCacheMax` | 128 МБ | Максимум в кэше large-регионов |
| `kLargeHotBudget` | 16 МБ | Сколько держать «горячим» без `MADV_FREE`/`MEM_RESET` |
| `kLargePerBucket` | 8 | Регионов в одном бакете кэша |
