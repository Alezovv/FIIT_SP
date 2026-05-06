#include "../include/allocator_boundary_tags.h"
#include <not_implemented.h>

allocator_header *allocator_boundary_tags::get_header() const {
    return reinterpret_cast<allocator_header *>(_trusted_memory);
}

block_meta *allocator_boundary_tags::get_block_meta(void *payload) const {
    if (payload != nullptr) {
        return reinterpret_cast<block_meta *>(payload) - 1;
    }

    return reinterpret_cast<block_meta *>(get_header() + 1);
}

block_meta *allocator_boundary_tags::get_next_phys_block(block_meta *current) const {
    if (current == nullptr) return nullptr;
    char *next_addr = reinterpret_cast<char *>(current) + current->block_size;

    char *end = reinterpret_cast<char *>(get_header()) + get_header()->total_size;

    if (next_addr >= end) return nullptr;
    return reinterpret_cast<block_meta *>(next_addr);
}

allocator_boundary_tags::~allocator_boundary_tags() {
    if (_trusted_memory == nullptr) return;
    auto *header = get_header();

    header->mutex.~mutex();
    std::pmr::memory_resource *parent = header->parent;
    size_t total_size = header->total_size;

    parent->deallocate(_trusted_memory, total_size);
    _trusted_memory = nullptr;
}

allocator_boundary_tags::allocator_boundary_tags(allocator_boundary_tags &&other) noexcept
    : _trusted_memory(other._trusted_memory) {
    other._trusted_memory = nullptr;
}

allocator_boundary_tags &allocator_boundary_tags::operator=(
    allocator_boundary_tags &&other) noexcept {
    if (this != &other) {
        this->~allocator_boundary_tags();

        _trusted_memory = other._trusted_memory;

        other._trusted_memory = nullptr;
    }
    return *this;
}

/** If parent_allocator* == nullptr you should use std::pmr::get_default_resource()
 */
// 0...header_size -> header_size...header_size + 32 -> header_size + 32...
// alloc_header    -> block_meta                     -> payload -> block_meta2 -> ...
allocator_boundary_tags::allocator_boundary_tags(
    size_t space_size, std::pmr::memory_resource *parent_allocator,
    allocator_with_fit_mode::fit_mode allocate_fit_mode) {
    std::pmr::memory_resource *actual_parent =
        (parent_allocator != nullptr) ? parent_allocator : std::pmr::get_default_resource();

    size_t header_size = sizeof(allocator_header);
    size_t total_size = header_size + space_size;

    _trusted_memory = actual_parent->allocate(total_size);

    auto *alloc_header = get_header();
    alloc_header->parent = actual_parent;
    alloc_header->mode = allocate_fit_mode;
    alloc_header->total_size = total_size;
    new (&alloc_header->mutex) std::mutex();

    block_meta *first_block = reinterpret_cast<block_meta *>(alloc_header + 1);

    first_block->block_size = space_size;
    first_block->next_free_block = nullptr;
    first_block->parent_allocator = nullptr;
    first_block->prev_block = nullptr;

    alloc_header->first_free_block = first_block;
}

[[nodiscard]] void *allocator_boundary_tags::do_allocate_sm(size_t bytes) {
    std::lock_guard<std::mutex> lock(get_header()->mutex);
    auto *header = get_header();

    size_t required = bytes + sizeof(block_meta);

    block_meta *chosen = nullptr;
    block_meta *chosen_prev_free = nullptr;

    block_meta *curr = reinterpret_cast<block_meta *>(header->first_free_block);
    block_meta *prev_free = nullptr;

    while (curr != nullptr) {
        if (curr->block_size >= required) {
            if (header->mode == fit_mode::first_fit) {
                chosen = curr;
                chosen_prev_free = prev_free;
                break;
            } else if (header->mode == fit_mode::the_best_fit) {
                if (!chosen || curr->block_size < chosen->block_size) {
                    chosen = curr;
                    chosen_prev_free = prev_free;
                }
            } else if (header->mode == fit_mode::the_worst_fit) {
                if (!chosen || curr->block_size > chosen->block_size) {
                    chosen = curr;
                    chosen_prev_free = prev_free;
                }
            }
        }
        prev_free = curr;
        curr = reinterpret_cast<block_meta *>(curr->next_free_block);
    }

    if (!chosen) throw std::bad_alloc();

    size_t remain = chosen->block_size - required;

    if (remain >= sizeof(block_meta)) {
        block_meta *new_block =
            reinterpret_cast<block_meta *>(reinterpret_cast<char *>(chosen) + required);

        new_block->block_size = remain;
        new_block->parent_allocator = nullptr;
        new_block->prev_block = chosen;

        block_meta *next_phys = get_next_phys_block(new_block);
        if (next_phys != nullptr) {
            next_phys->prev_block = new_block;
        }

        new_block->next_free_block = chosen->next_free_block;
        if (chosen_prev_free != nullptr)
            chosen_prev_free->next_free_block = new_block;
        else
            header->first_free_block = new_block;

        chosen->block_size = required;
    } else {
        if (chosen_prev_free != nullptr)
            chosen_prev_free->next_free_block = chosen->next_free_block;
        else
            header->first_free_block = chosen->next_free_block;
    }

    chosen->parent_allocator = this;
    chosen->next_free_block = nullptr;

    return chosen + 1;
}

void allocator_boundary_tags::do_deallocate_sm(void *at) {
    if (at == nullptr) return;
    std::lock_guard<std::mutex> lock(get_header()->mutex);

    block_meta *block = reinterpret_cast<block_meta *>(at) - 1;
    auto *header = get_header();

    if (block->parent_allocator != this) {
        throw std::invalid_argument("wrong pointer");
    }

    block->parent_allocator = nullptr;

    block_meta *prev_phys = reinterpret_cast<block_meta *>(block->prev_block);
    block_meta *next_phys = get_next_phys_block(block);

    if (next_phys != nullptr && next_phys->parent_allocator == nullptr) {
        block_meta *f_curr = reinterpret_cast<block_meta *>(header->first_free_block);
        block_meta *f_prev = nullptr;
        while (f_curr != nullptr && f_curr != next_phys) {
            f_prev = f_curr;
            f_curr = reinterpret_cast<block_meta *>(f_curr->next_free_block);
        }
        if (f_curr != nullptr) {
            if (f_prev != nullptr)
                f_prev->next_free_block = f_curr->next_free_block;
            else
                header->first_free_block = f_curr->next_free_block;
        }

        block->block_size += next_phys->block_size;

        block_meta *after_next = get_next_phys_block(block);
        if (after_next != nullptr) after_next->prev_block = block;
    }

    if (prev_phys != nullptr && prev_phys->parent_allocator == nullptr) {
        prev_phys->block_size += block->block_size;

        block_meta *after_prev = get_next_phys_block(prev_phys);
        if (after_prev != nullptr) after_prev->prev_block = prev_phys;

    } else {
        block->next_free_block = reinterpret_cast<block_meta *>(header->first_free_block);
        header->first_free_block = block;
    }
}

inline void allocator_boundary_tags::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    std::lock_guard<std::mutex> lock(this->get_header()->mutex);
    get_header()->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info() const {
    std::lock_guard<std::mutex> lock(this->get_header()->mutex);

    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_boundary_tags::get_blocks_info_inner()
    const {
    std::vector<allocator_test_utils::block_info> result;

    for (auto it = begin(); it != end(); ++it) {
        allocator_test_utils::block_info info;

        info.block_size = it.size();
        info.is_block_occupied = it.occupied();

        result.push_back(info);
    }

    return result;
}

allocator_boundary_tags::allocator_boundary_tags(const allocator_boundary_tags &other) {
    throw std::logic_error("Copying of allocator_sorted_list is prohibited.");
}

allocator_boundary_tags &allocator_boundary_tags::operator=(const allocator_boundary_tags &other) {
    throw std::logic_error("Copying of allocator_sorted_list is prohibited.");
}

bool allocator_boundary_tags::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return this == &other;
}

#pragma region boundary_iterator

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::begin() const noexcept {
    return boundary_iterator(_trusted_memory);
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::end() const noexcept {
    return boundary_iterator();
}

bool allocator_boundary_tags::boundary_iterator::operator==(
    const allocator_boundary_tags::boundary_iterator &other) const noexcept {
    return _occupied_ptr == other._occupied_ptr;
}

bool allocator_boundary_tags::boundary_iterator::operator!=(
    const allocator_boundary_tags::boundary_iterator &other) const noexcept {
    return !(*this == other);
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator++()
    & noexcept {
    if (_occupied_ptr == nullptr) return *this;
    size_t current_block_size = size();

    void *next_block_ptr = reinterpret_cast<char *>(_occupied_ptr) + current_block_size;

    auto *header = reinterpret_cast<allocator_header *>(_trusted_memory);
    void *end = reinterpret_cast<char *>(_trusted_memory) + header->total_size;

    if (next_block_ptr >= end) {
        _occupied_ptr = nullptr;
        _occupied = false;
    } else {
        _occupied_ptr = next_block_ptr;
        _occupied = occupied();
    }

    return *this;
}

allocator_boundary_tags::boundary_iterator &allocator_boundary_tags::boundary_iterator::operator--()
    & noexcept {
    if (_occupied_ptr == nullptr) {
        return *this;
    }

    auto *current_meta = reinterpret_cast<block_meta *>(_occupied_ptr);

    if (current_meta->prev_block == nullptr) {
        _occupied_ptr = nullptr;
        _occupied = false;
    } else {
        _occupied_ptr = current_meta->prev_block;
        _occupied = occupied();
    }

    return *this;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator++(
    int n) {
    boundary_iterator tmp = *this;
    ++(*this);

    return tmp;
}

allocator_boundary_tags::boundary_iterator allocator_boundary_tags::boundary_iterator::operator--(
    int n) {
    boundary_iterator tmp = *this;
    --(*this);

    return tmp;
}

size_t allocator_boundary_tags::boundary_iterator::size() const noexcept {
    if (_occupied_ptr == nullptr) return 0;

    return reinterpret_cast<block_meta *>(_occupied_ptr)->block_size;
}

bool allocator_boundary_tags::boundary_iterator::occupied() const noexcept {
    if (_occupied_ptr == nullptr) return false;

    return reinterpret_cast<block_meta *>(_occupied_ptr)->parent_allocator != nullptr;
}

void *allocator_boundary_tags::boundary_iterator::operator*() const noexcept { return get_ptr(); }

// конец
allocator_boundary_tags::boundary_iterator::boundary_iterator()
    : _occupied_ptr(nullptr), _occupied(false), _trusted_memory(nullptr) {}

// начало
allocator_boundary_tags::boundary_iterator::boundary_iterator(void *trusted)
    : _trusted_memory(trusted) {
    if (_trusted_memory == nullptr) {
        _occupied_ptr = nullptr;
        _occupied = false;
        return;
    }

    auto *header = reinterpret_cast<allocator_header *>(_trusted_memory);
    _occupied_ptr = header + 1;

    _occupied = occupied();
}

void *allocator_boundary_tags::boundary_iterator::get_ptr() const noexcept {
    if (_occupied_ptr == nullptr) return nullptr;

    return reinterpret_cast<block_meta *>(_occupied_ptr) + 1;
}
