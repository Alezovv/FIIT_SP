#include "../include/allocator_global_heap.h"
#include <not_implemented.h>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::mutex global_heap_mutex;

allocator_global_heap::allocator_global_heap() = default;

[[nodiscard]] void *allocator_global_heap::do_allocate_sm(size_t size) {
    std::lock_guard<std::mutex> lock(global_heap_mutex);
    void *ptr = ::operator new(size);
    std::cout << "Allocated " << size << " bytes at " << ptr << std::endl;
    return ptr;
}

void allocator_global_heap::do_deallocate_sm(void *at) {
    if (at == nullptr) return;

    std::lock_guard<std::mutex> lock(global_heap_mutex);

    ::operator delete(at);
    std::cout << "Deallocated memory at " << at << std::endl;
}

allocator_global_heap::~allocator_global_heap() = default;

allocator_global_heap::allocator_global_heap(const allocator_global_heap &other) = default;

allocator_global_heap &allocator_global_heap::operator=(const allocator_global_heap &other) {
    return *this;
}

bool allocator_global_heap::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return dynamic_cast<const allocator_global_heap *>(&other) != nullptr;
}

allocator_global_heap::allocator_global_heap(allocator_global_heap &&other) noexcept = default;

allocator_global_heap &allocator_global_heap::operator=(allocator_global_heap &&other) noexcept {
    return *this;
}
