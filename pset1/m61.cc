#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <iostream>

struct m61_memory_buffer {
    char* buffer;
    size_t pos = 0;
    size_t size = 8 << 20; /* 8 MiB */

    m61_memory_buffer();
    ~m61_memory_buffer();
};

static m61_memory_buffer default_buffer;


m61_memory_buffer::m61_memory_buffer() {
    void* buf = mmap(nullptr,    // Place the buffer at a random address
        this->size,              // Buffer should be 8 MiB big
        PROT_WRITE,              // We want to read and write the buffer
        MAP_ANON | MAP_PRIVATE, -1, 0);
                                 // We want memory freshly allocated by the OS
    assert(buf != MAP_FAILED);
    this->buffer = (char*) buf;
}

m61_memory_buffer::~m61_memory_buffer() {
    munmap(this->buffer, this->size);
}

static m61_statistics memory_stats = {
    .nactive = 0, .active_size = 0, .ntotal = 0,
    .total_size = 0, .nfail = 0, .fail_size = 0,
    .heap_min = 0, .heap_max = 0
};

// Struct for storing freed blocks
struct freeBlock {
    char* address;
    size_t sz;
};
// Struct for storing active blocks
struct activeBlock {
    size_t sz;
    const char* file;
    int line;
};

// Constant for trailing guard size (in case it needs changed)
constexpr size_t guardSize = 16;
constexpr unsigned char guardExpression = 0xDD;

// Map for storing active allocations, vector for storing freed allocations
static std::unordered_map<void*, activeBlock> activeAlloc;
static std::vector<freeBlock> freedAlloc;

/// align(size_t sz)
///     Given a size, rounds up to the next multiple of 16
size_t align(size_t sz) {
    return (sz + 15) & ~15;
}

/// fail(size_t sz)
///     Updates memory statistics for a failed allocation
void fail(size_t sz) {
    memory_stats.nfail++;
    memory_stats.fail_size += sz;
}

/// success(size_t sz)
///     Updates memory statistics for a successful allocation
void success(size_t sz) {
    memory_stats.ntotal++;
    memory_stats.nactive++;
    memory_stats.active_size += sz;
    memory_stats.total_size += sz;
}

/// insertFreedAlloc(freeBlock freed)
///     Inserts a freed allocation into the freedAlloc vector, keeping
///     the vector sorted by ptr memory address.
static void insertFreedAlloc(const freeBlock& freed) {
    // Variable to store index where freed is inserted
    size_t i = 0;

    // Handle case where freed is the first freed pointer
    // or freed has a larger address than all other freed pointers
    if (freedAlloc.empty()) {
        freedAlloc.push_back(freed);
    } else if (freedAlloc[freedAlloc.size() - 1].address < freed.address) {
        freedAlloc.push_back(freed);
        i = freedAlloc.size() - 1;
    } else {
        // Insert the freed block before the first pointer that has a larger address
        for (size_t j = 0; j < freedAlloc.size(); ++j) {
            if (freedAlloc[j].address > freed.address) {
                freedAlloc.insert(freedAlloc.begin() + j, freed);
                i = j;
                break;
            }
        }
    }
    // Coalesce with next freed block (if possible)
    if (i < freedAlloc.size() - 1 && freedAlloc[i].address + freedAlloc[i].sz == freedAlloc[i + 1].address) {
        freedAlloc[i].sz += freedAlloc[i + 1].sz;
        freedAlloc.erase(freedAlloc.begin() + i + 1);
    }
    // Coalesce with previous freed block (if possible)
    if (i > 0 && freedAlloc[i - 1].address + freedAlloc[i - 1].sz == freedAlloc[i].address) {
        freedAlloc[i - 1].sz += freedAlloc[i].sz;
        freedAlloc.erase(freedAlloc.begin() + i);
    }
    // Coalesce with default buffer (if possible)
    char* heap_end = default_buffer.buffer + default_buffer.pos;
    freeBlock& last = freedAlloc.back();

    if (last.address + last.sz == heap_end) {
        default_buffer.pos -= last.sz;
        freedAlloc.pop_back();
    }

}

/// findFreeSpace(size_t sz)
///     Finds smallest free space in the vector of freedAlloc that is large
///     enough to accomodate sz
static void* findFreeSpace(size_t sz) {
    // Handle case of empty freedAlloc
    if (freedAlloc.size() == 0) {
        return nullptr;
    }
    // Find the smallest free space that is large enough for sz
    size_t bestIndex = (size_t)-1;
    size_t bestSz = (size_t)-1;
    for (size_t i = 0; i < freedAlloc.size(); ++i) {
        if (freedAlloc[i].sz >= sz && freedAlloc[i].sz < bestSz) {
            bestSz = freedAlloc[i].sz;
            bestIndex = i;
            // If perfect fit is found
            if (bestSz == sz) break;
        }
    }
    // Handle case where no sufficient space was found
    if (bestSz == (size_t)-1) {
        return nullptr;
    }
    void* ptr = freedAlloc[bestIndex].address;
    // Handle case where perfect match was found
    if (bestSz == sz) {
        freedAlloc.erase(freedAlloc.begin() + bestIndex);
    }
    else {
        freedAlloc[bestIndex].address += sz;
        freedAlloc[bestIndex].sz -= sz;
    }
    return ptr;
}

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, int line) {
    (void) file, (void) line;   // avoid uninitialized variable warnings
    // Guard against overflow when finding aligned_sz
    if (sz > SIZE_MAX - 15) {
        fail(sz);
        return nullptr;
    }
    // Find padding needed for 16-byte alignment, then combine with guard
    size_t total = sz + guardSize;
    size_t aligned_total = align(total);
    // Try to fit the allocation into previously freed space
    void* ptr = findFreeSpace(aligned_total);
    // If that fails, try new space
    if (!ptr) {
        // Check if space is available for padded allocation, guarding against
        // overflow when calculating available space
        if (aligned_total <= default_buffer.size - default_buffer.pos) {
            // Space is available, allocate memory
            ptr = &default_buffer.buffer[default_buffer.pos];
            default_buffer.pos += aligned_total;        
        }
        else {
            // Not enough space left in default buffer for allocation
            ptr = nullptr;
        }
    }
    if (ptr) {
        // Update guard space
        std::memset(reinterpret_cast<unsigned char*>(ptr) + sz, guardExpression, guardSize);
        // Handle successful allocation
        success(sz);
        activeAlloc[ptr] = { sz, file, line };
        
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        auto endAddr = addr + aligned_total;
        // Adjust heap_min for intial case or new smallest address
        if (memory_stats.heap_min == 0 || addr < memory_stats.heap_min) {
            memory_stats.heap_min = addr;
        }
        // Adjust heap_max for new largest address
        if (endAddr > memory_stats.heap_max) {
            memory_stats.heap_max = endAddr;
        }
    }
    else {
        // Handle failed allocation
        fail(sz);
    }
    return ptr;
}

/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.

void m61_free(void* ptr, const char* file, int line) {
    // avoid uninitialized variable warnings
    (void) ptr, (void) file, (void) line;
    // Handle nullptr case
    if (ptr == nullptr) {
        return;
    }
    // Handle cases where ptr does not point to an active allocation
    // Three cases: not in heap, double free, invalid free
    char* p = reinterpret_cast<char*>(ptr);
    // Handle not in heap case
    if (p < default_buffer.buffer || p >= default_buffer.buffer + default_buffer.size) {
        std::cerr << "MEMORY BUG: " << file << ":" << line
            << ": invalid free of pointer " << ptr << ", not in heap" << std::endl;
        abort();
    }
    auto it = activeAlloc.find(ptr);
    if (it == activeAlloc.end()) {
        // Handle double frees (ptr was already freed)
        bool doubleFree = false;
        auto itFree = std::find_if(freedAlloc.begin(), freedAlloc.end(),
            [p](const freeBlock& f) 
            { return p >= f.address && p < f.address + f.sz; });
        if (itFree != freedAlloc.end()) {
            doubleFree = true;
        }
        // double-free case where freed memory was coalesced with heap
        char* max = reinterpret_cast<char*>(memory_stats.heap_max);
        if (p >= default_buffer.buffer + default_buffer.pos && p < max) {
            doubleFree = true;
        }
        if (doubleFree) {
            std::cerr << "MEMORY BUG: " << file << ":" << line
                << ": invalid free of pointer " << ptr << ", double free" << std::endl;
            abort();
        }
        else { // Handle invalid frees (ptr was never allocated)
            std::cerr << "MEMORY BUG: " << file << ":" << line
                << ": invalid free of pointer " << ptr << ", not allocated" << std::endl;
            abort();
        }
    }
    // Check trailing guard
    unsigned char* pUnsigned = reinterpret_cast<unsigned char*>(ptr);
    for (size_t i = 0; i < guardSize; ++i) {
        if (pUnsigned[activeAlloc[ptr].sz + i] != guardExpression) {
            std::cerr << "MEMORY BUG: " << file << ":" << line
                << ": detected wild write during free of pointer " << ptr
                << std::endl;
            abort();
        }
    }
    
    // Handle successful case
    --memory_stats.nactive;
    memory_stats.active_size -= activeAlloc[ptr].sz;
    insertFreedAlloc({ static_cast<char*>(ptr), align(activeAlloc[ptr].sz + guardSize) });
    activeAlloc.erase(it);
}

/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.

void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    // Guard against multiplication overflow
    if (count != 0 && sz > SIZE_MAX / count) {
        memory_stats.nfail++;
        memory_stats.fail_size += static_cast<unsigned long long>(count) * static_cast<unsigned long long>(sz);
        return nullptr;
    }
    void* ptr = m61_malloc(count * sz, file, line);
    if (ptr) {
        memset(ptr, 0, count * sz);
    }
    return ptr;
}


/// m61_get_statistics()
///    Return the current memory statistics.

m61_statistics m61_get_statistics() {
    return memory_stats;
}


/// m61_print_statistics()
///    Prints the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats = m61_get_statistics();
    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Prints a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    for (const auto& pair : activeAlloc) {
        std::cout << "LEAK CHECK: " << pair.second.file << ":" 
            << pair.second.line << ": allocated object " << pair.first
            << " with size " << pair.second.sz << std::endl;
    }
}


