#include <memory>
#include "pmalloc_win.hpp"

struct FibNode {
    int64_t a;
    int64_t b;
    std::shared_ptr<FibNode> prev;

    FibNode(int64_t a_, int64_t b_, std::shared_ptr<FibNode> p_)
        : a(a_), b(b_), prev(std::move(p_)) {}
};

std::shared_ptr<FibNode> fib(int n) {
    using Alloc = pmalloc::pmallocator_cpp<FibNode>;
    
    if (n == 0) {
        return std::allocate_shared<FibNode>(Alloc{}, 1, 1, nullptr);
    }
    
    auto prev = fib(n - 1);
    int64_t new_a = prev->a + prev->b;
    int64_t new_b = prev->a;
    
    return std::allocate_shared<FibNode>(Alloc{}, new_a, new_b, prev);
}

int main() {
    auto result = fib(25);
    return 0;
}