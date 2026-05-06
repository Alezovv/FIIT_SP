#include "../include/allocator_sorted_list.h"
#include <../../../common/include/not_implemented.h>

/* _trusted_memory:
    allocator_metadata_size
    block_metadata_size
 */

allocator_header *allocator_sorted_list::get_header() const {
    return reinterpret_cast<allocator_header *>(_trusted_memory);
}

std::mutex &allocator_sorted_list::get_mutex() const { return get_header()->mutex; }

void *allocator_sorted_list::get_first_block_ptr() const {
    return static_cast<char *>(_trusted_memory) + allocator_metadata_size;
}

void allocator_sorted_list::find_first_fit(size_t size, block_header *&target,
                                           block_header *&prev) const noexcept {
    target = nullptr;
    prev = nullptr;

    if (size == 0 || _trusted_memory == nullptr) {
        return;
    }

    auto *const header_ptr = get_header();

    block_header *curr = reinterpret_cast<block_header *>(header_ptr->first_free_block);
    block_header *last = nullptr;

    while (curr != nullptr) {
        if (curr->block_size + block_metadata_size >= size) {
            target = curr;
            prev = last;
            return;
        }

        last = curr;
        curr = reinterpret_cast<block_header *>(curr->next_free_block);
    }
}

void allocator_sorted_list::find_best_fit(size_t size, block_header *&target,
                                          block_header *&prev) const noexcept {
    target = nullptr;
    prev = nullptr;

    if (size == 0 || _trusted_memory == nullptr) return;

    auto *const header_ptr = get_header();
    block_header *curr = reinterpret_cast<block_header *>(header_ptr->first_free_block);
    block_header *last = nullptr;

    size_t min_diff = std::numeric_limits<size_t>::max();

    while (curr != nullptr) {
        size_t current_block_full_size = curr->block_size + block_metadata_size;

        if (current_block_full_size >= size) {
            size_t diff = current_block_full_size - size;

            if (diff < min_diff) {
                min_diff = diff;
                target = curr;
                prev = last;
            }

            if (diff == 0) return;
        }

        last = curr;
        curr = reinterpret_cast<block_header *>(curr->next_free_block);
    }
}

void allocator_sorted_list::find_worst_fit(size_t size, block_header *&target,
                                           block_header *&prev) const noexcept {
    target = nullptr;
    prev = nullptr;

    if (size == 0 || _trusted_memory == nullptr) return;

    auto *const header_ptr = get_header();
    block_header *curr = reinterpret_cast<block_header *>(header_ptr->first_free_block);
    block_header *last = nullptr;

    size_t max_found_size = 0;

    while (curr != nullptr) {
        size_t current_block_full_size = curr->block_size + block_metadata_size;

        if (current_block_full_size >= size) {
            if (current_block_full_size > max_found_size) {
                max_found_size = current_block_full_size;
                target = curr;
                prev = last;
            }
        }

        last = curr;
        curr = reinterpret_cast<block_header *>(curr->next_free_block);
    }
}

allocator_sorted_list::~allocator_sorted_list() {
    if (_trusted_memory != nullptr) {
        auto *header = get_header();

        auto *parent_allocator = header->parent;
        size_t total_memory_size = header->total_size;

        header->mutex.~mutex();

        if (parent_allocator != nullptr) {
            parent_allocator->deallocate(_trusted_memory, total_memory_size);
        } else {
            ::operator delete(_trusted_memory);
        }

        _trusted_memory = nullptr;
    }
}

allocator_sorted_list::allocator_sorted_list(allocator_sorted_list &&other) noexcept {
    _trusted_memory = other._trusted_memory;

    other._trusted_memory = nullptr;
}

allocator_sorted_list &allocator_sorted_list::operator=(allocator_sorted_list &&other) noexcept {
    if (this != &other) {
        this->~allocator_sorted_list();
        _trusted_memory = other._trusted_memory;

        other._trusted_memory = nullptr;
    }

    return *this;
}

allocator_sorted_list::allocator_sorted_list(size_t space_size,
                                             std::pmr::memory_resource *parent_allocator,
                                             allocator_with_fit_mode::fit_mode allocate_fit_mode) {
    std::pmr::memory_resource *actual_parent =
        (parent_allocator != nullptr) ? parent_allocator : std::pmr::get_default_resource();

    size_t alignment = alignof(std::max_align_t);

    size_t header_offset = (allocator_metadata_size + alignment - 1) & ~(alignment - 1);

    size_t total_space = header_offset + space_size;
    total_space = (total_space + alignment - 1) & ~(alignment - 1);

    _trusted_memory = actual_parent->allocate(total_space);

    auto *header = get_header();
    header->parent = actual_parent;
    header->mode = allocate_fit_mode;
    header->total_size = total_space;

    new (&header->mutex) std::mutex();

    void *first_block_addr = static_cast<char *>(_trusted_memory) + header_offset;
    header->first_free_block = first_block_addr;

    auto *first_block = reinterpret_cast<block_header *>(first_block_addr);

    first_block->block_size = total_space - header_offset - block_metadata_size;
    first_block->next_free_block = nullptr;
}

[[nodiscard]] void *allocator_sorted_list::do_allocate_sm(size_t size) {
    std::lock_guard<std::mutex> lock(get_mutex());

    if (size == 0) {
        throw std::logic_error("Allocation size cannot be zero");
    }

    size_t alignment = alignof(std::max_align_t);
    size_t user_size = (size + alignment - 1) & ~(alignment - 1);
    size_t total_needed = user_size + block_metadata_size;

    auto *const header_ptr = get_header();
    block_header *target = nullptr;
    block_header *prev = nullptr;

    // Поиск подходящего блока в зависимости от режима
    switch (header_ptr->mode) {
        case allocator_with_fit_mode::fit_mode::first_fit:
            find_first_fit(total_needed, target, prev);
            break;
        case allocator_with_fit_mode::fit_mode::the_best_fit:
            find_best_fit(total_needed, target, prev);
            break;
        case allocator_with_fit_mode::fit_mode::the_worst_fit:
            find_worst_fit(total_needed, target, prev);
            break;
        default:
            find_first_fit(total_needed, target, prev);
            break;
    }

    if (target == nullptr) {
        throw std::bad_alloc();
    }

    // Если после отрезания остается достаточно места для нового заголовка и минимальных данных
    if (target->block_size + block_metadata_size >=
        total_needed + block_metadata_size + alignment) {
        // Создаем новый свободный блок из остатка
        auto *new_free_block =
            reinterpret_cast<block_header *>(reinterpret_cast<char *>(target) + total_needed);

        new_free_block->block_size = target->block_size - total_needed;
        new_free_block->next_free_block = target->next_free_block;

        target->block_size = user_size;

        if (prev == nullptr) {
            header_ptr->first_free_block = new_free_block;
        } else {
            prev->next_free_block = new_free_block;
        }
    } else {
        // Блок забирается целиком
        if (prev == nullptr) {
            header_ptr->first_free_block = target->next_free_block;
        } else {
            prev->next_free_block = target->next_free_block;
        }
    }

    // ВАЖНО: В занятом блоке поле next_free_block теперь хранит указатель на аллокатор
    target->next_free_block = _trusted_memory;

    return reinterpret_cast<void *>(target + 1);
}

allocator_sorted_list::allocator_sorted_list(const allocator_sorted_list &other) {
    throw std::logic_error("Copying of allocator_sorted_list is prohibited.");
}

allocator_sorted_list &allocator_sorted_list::operator=(const allocator_sorted_list &other) {
    if (this != &other) {
        throw std::logic_error("Copying of allocator_sorted_list is prohibited.");
    }
    return *this;
}

bool allocator_sorted_list::do_is_equal(const std::pmr::memory_resource &other) const noexcept {
    return this == &other;
}

void allocator_sorted_list::do_deallocate_sm(void *at) {
    if (at == nullptr) return;

    // Получаем заголовок блока по адресу пользователя
    auto *target_block =
        reinterpret_cast<block_header *>(static_cast<char *>(at) - block_metadata_size);

    // ПРОВЕРКА: смотрим в поле, где мы сохранили указатель на аллокатор
    if (target_block->next_free_block != _trusted_memory) {
        throw std::logic_error("This block does not belong to this allocator instance!");
    }

    std::lock_guard<std::mutex> lock(get_mutex());

    auto *const header_ptr = get_header();

    // Ищем место для вставки блока обратно в сортированный по адресам список
    block_header *curr = reinterpret_cast<block_header *>(header_ptr->first_free_block);
    block_header *prev = nullptr;

    while (curr != nullptr && curr < target_block) {
        prev = curr;
        curr = reinterpret_cast<block_header *>(curr->next_free_block);
    }

    // Возвращаем блок в список (здесь поле next_free_block перезаписывается реальным указателем
    // списка)
    target_block->next_free_block = curr;
    if (prev == nullptr) {
        header_ptr->first_free_block = target_block;
    } else {
        prev->next_free_block = target_block;
    }

    // Попытка склеить со следующим свободным блоком
    if (curr != nullptr &&
        reinterpret_cast<char *>(target_block) + target_block->block_size + block_metadata_size ==
            reinterpret_cast<char *>(curr)) {
        target_block->block_size += curr->block_size + block_metadata_size;
        target_block->next_free_block = curr->next_free_block;
    }

    // Попытка склеить с предыдущим свободным блоком
    if (prev != nullptr &&
        reinterpret_cast<char *>(prev) + prev->block_size + block_metadata_size ==
            reinterpret_cast<char *>(target_block)) {
        prev->block_size += target_block->block_size + block_metadata_size;
        prev->next_free_block = target_block->next_free_block;
    }
}

inline void allocator_sorted_list::set_fit_mode(allocator_with_fit_mode::fit_mode mode) {
    std::lock_guard<std::mutex> lock(get_mutex());
    get_header()->mode = mode;
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info()
    const noexcept {
    std::lock_guard<std::mutex> lock(this->get_mutex());

    return get_blocks_info_inner();
}

std::vector<allocator_test_utils::block_info> allocator_sorted_list::get_blocks_info_inner() const {
    std::vector<allocator_test_utils::block_info> result;

    for (auto it = begin(); it != end(); ++it) {
        allocator_test_utils::block_info info;

        info.block_size = it.size();
        info.is_block_occupied = it.occupied();

        result.push_back(info);
    }

    return result;
}

#pragma region sorted_free_iterator

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_begin() const noexcept {
    return sorted_free_iterator(get_header()->first_free_block);
}

allocator_sorted_list::sorted_free_iterator allocator_sorted_list::free_end() const noexcept {
    return sorted_free_iterator(nullptr);
}

bool allocator_sorted_list::sorted_free_iterator::operator==(
    const allocator_sorted_list::sorted_free_iterator &other) const noexcept {
    return _free_ptr == other._free_ptr;
}

bool allocator_sorted_list::sorted_free_iterator::operator!=(
    const allocator_sorted_list::sorted_free_iterator &other) const noexcept {
    return _free_ptr != other._free_ptr;
}

// идет к след -> возвращаем
allocator_sorted_list::sorted_free_iterator &
allocator_sorted_list::sorted_free_iterator::operator++() & noexcept {
    if (_free_ptr == nullptr) {
        return *this;
    }

    auto *header = reinterpret_cast<block_header *>(_free_ptr);

    _free_ptr = header->next_free_block;
    return *this;
}

// копирует текущий -> идет к след -> возвращает копию
allocator_sorted_list::sorted_free_iterator allocator_sorted_list::sorted_free_iterator::operator++(
    int n) {
    sorted_free_iterator temp = *this;

    this->operator++();

    return temp;
}

size_t allocator_sorted_list::sorted_free_iterator::size() const noexcept {
    if (_free_ptr == nullptr) return 0;

    return reinterpret_cast<block_header *>(_free_ptr)->block_size;
}

void *allocator_sorted_list::sorted_free_iterator::operator*() const noexcept {
    if (_free_ptr == nullptr) return nullptr;

    return static_cast<char *>(_free_ptr) + block_metadata_size;
}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator() : _free_ptr(nullptr) {}

allocator_sorted_list::sorted_free_iterator::sorted_free_iterator(void *trusted)
    : _free_ptr(trusted) {}

#pragma endrigon sorted_free_iterator

#pragma region sorted_iterator

bool allocator_sorted_list::sorted_iterator::operator==(
    const allocator_sorted_list::sorted_iterator &other) const noexcept {
    return _current_ptr == other._current_ptr;
}

bool allocator_sorted_list::sorted_iterator::operator!=(
    const allocator_sorted_list::sorted_iterator &other) const noexcept {
    return _current_ptr != other._current_ptr;
}

allocator_sorted_list::sorted_iterator &allocator_sorted_list::sorted_iterator::operator++()
    & noexcept {
    if (_current_ptr == nullptr) return *this;

    auto *curr_block = reinterpret_cast<block_header *>(_current_ptr);

    if (_current_ptr == _free_ptr) {
        _free_ptr = curr_block->next_free_block;
    }

    void *next_block =
        static_cast<char *>(_current_ptr) + curr_block->block_size + block_metadata_size;

    auto *alloc_header = reinterpret_cast<allocator_header *>(_trusted_memory);
    void *mem_end = static_cast<char *>(_trusted_memory) + alloc_header->total_size;

    _current_ptr = (next_block < mem_end) ? next_block : nullptr;

    return *this;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::sorted_iterator::operator++(int n) {
    sorted_iterator temp = *this;

    ++(*this);

    return temp;
}

size_t allocator_sorted_list::sorted_iterator::size() const noexcept {
    if (_current_ptr == nullptr) return 0;

    return reinterpret_cast<block_header *>(_current_ptr)->block_size;
}

void *allocator_sorted_list::sorted_iterator::operator*() const noexcept {
    if (_current_ptr == nullptr) return nullptr;

    return static_cast<char *>(_current_ptr) + block_metadata_size;
}

allocator_sorted_list::sorted_iterator::sorted_iterator()
    : _free_ptr(nullptr), _current_ptr(nullptr), _trusted_memory(nullptr) {}

allocator_sorted_list::sorted_iterator::sorted_iterator(void *trusted) {
    _trusted_memory = trusted;

    if (_trusted_memory == nullptr) {
        _current_ptr = nullptr;
        _free_ptr = nullptr;
        return;
    }

    auto *header = reinterpret_cast<allocator_header *>(_trusted_memory);

    size_t alignment = alignof(std::max_align_t);
    size_t header_offset = (allocator_metadata_size + alignment - 1) & ~(alignment - 1);

    _current_ptr = static_cast<char *>(_trusted_memory) + header_offset;

    _free_ptr = header->first_free_block;
}

bool allocator_sorted_list::sorted_iterator::occupied() const noexcept {
    return _current_ptr != _free_ptr;
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::begin() const noexcept {
    return sorted_iterator(_trusted_memory);
}

allocator_sorted_list::sorted_iterator allocator_sorted_list::end() const noexcept {
    return sorted_iterator();
}

#pragma endregion sorted_iterator