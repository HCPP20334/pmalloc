#include <thread>
#include <array>
#ifdef _WIN32
#include "pmalloc_win.hpp"
#else
#include "pmalloc_posix.hpp"
#endif
#include <cstdio>
#include <cstdlib>

struct TestObj {
  int32_t i32Value;
  uint64_t u64Tag;
};

static void RunUAF() {
  auto hObj = pmalloc::pptr::ptr<TestObj>(1337, 0xDEADBEEFull);
  std::printf("[0: UAF] created idx=%llu id=%llu val=%d\n",
              (unsigned long long)hObj.idx, (unsigned long long)hObj.id,
              hObj->i32Value);

  pmalloc::pptr::del(hObj);
  std::printf("[0: UAF] deleted, accessing hObj->i32Value...\n");
  std::fflush(stdout);

  int32_t i32Val = hObj->i32Value;
  std::printf("[0: UAF] unreachable: %d\n", i32Val);
}

static void RunDoubleFree() {
  auto hObj = pmalloc::pptr::ptr<TestObj>(42, 0x11223344ull);
  std::printf("[1: DF] created idx=%llu id=%llu\n",
              (unsigned long long)hObj.idx, (unsigned long long)hObj.id);

  bool bFirst = pmalloc::pptr::del(hObj);
  std::printf("[1: DF] first del = %s\n", bFirst ? "true" : "false");

  bool bSecond = pmalloc::pptr::del(hObj);
  std::printf("[1: DF] second del = %s\n", bSecond ? "true" : "false");
}

static void RunDanglingPtr() {
  auto hOwner = pmalloc::pptr::ptr<TestObj>(100, 0xAAAAull);
  auto hDangling = hOwner;

  std::printf("[2: DP] owner   idx=%llu id=%llu ptr=%p val=%d\n",
              (unsigned long long)hOwner.idx, (unsigned long long)hOwner.id,
              (void *)hOwner.p, hOwner->i32Value);

  pmalloc::pptr::del(hOwner, 1);

  auto hReused = pmalloc::pptr::ptr<TestObj>(999, 0xBBBBull);
  std::printf("[2: DP] reused  idx=%llu id=%llu ptr=%p val=%d\n",
              (unsigned long long)hReused.idx, (unsigned long long)hReused.id,
              (void *)hReused.p, hReused->i32Value);

  std::printf("[2: DP] dangling valid=%s get=%p\n",
              hDangling.valid() ? "true" : "false", (void *)hDangling.get());
  std::printf("[2: DP] accessing hDangling->i32Value...\n");
  std::fflush(stdout);

  int32_t i32Val = hDangling->i32Value;
  std::printf("[2: DP] unreachable: %d\n", i32Val);
}

int main(int argc, char **argv) {
  int32_t i32Mode = -1;
  if (argc > 1) {
    i32Mode = std::atoi(argv[1]);
  } else {
    std::printf("Mode (0=UAF, 1=DF, 2=DP): ");
    if (std::scanf("%d", &i32Mode) != 1)
      return 1;
  }

  switch (i32Mode) {
  case 0:
    RunUAF();
    break;
  case 1:
    RunDoubleFree();
    break;
  case 2:
    RunDanglingPtr();
    break;
  default:
    std::fprintf(stderr, "Unknown mode: %d (use 0, 1, or 2)\n", i32Mode);
    return 1;
  }

  return 0;
}
