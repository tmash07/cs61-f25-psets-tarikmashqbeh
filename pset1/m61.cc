#include "m61.hh"
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <sys/mman.h>

// Memory buffer structure modeled after a linked list node (for list of buffers)
struct m61_memory_buffer {
    char* buffer;                       // Pointer to the memory mapped region of the buffer
    size_t pos = 0;                     // Bytes used in the buffer
    size_t size;                        // Total size of the buffer
    m61_memory_buffer* next = nullptr;  // Next buffer in the linked list

    m61_memory_buffer(size_t sz);
    ~m61_memory_buffer();
};

static m61_memory_buffer* buffer_list = nullptr;
static constexpr size_t DEFAULT_BUFFER_SIZE = 8 << 20; // 8 MiB

// Constructor for m61_memory_buffer, creates a new buffer of size sz, returns nullptr if failed
m61_memory_buffer::m61_memory_buffer(size_t sz) : size(sz) {
    void* buf = mmap(nullptr, this->size, PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
    this->buffer = (buf == MAP_FAILED) ? nullptr : (char*)buf; // Handle failure of mmap
}

// Destructor for m61_memory_buffer, releases the buffer
m61_memory_buffer::~m61_memory_buffer() {
    if (this->buffer) { // Ensure buffer exists before release
        munmap(this->buffer, this->size);
    }
}

/// Structs for handling internal metadata
/// Each allocation has the layout: [Header (48 bytes)] [Data] [Padding with 0xBD guard] [Footer (8 bytes)]

// Struct for header of an allocation (48 bytes)
// Stores internal metadata for each allocation
struct m61_header {
    size_t size;            // Total block size (power of 2 for buddy system)
    size_t user_size;       // Requested allocation size
    const char* file;       // Source file (for error reporting)
    m61_header* self;       // Self-pointer for validity checking
    int line;               // Source line (for error reporting)
    int status;             // 1 = allocated, 0 = free
    unsigned int magic;     // Magic number for corruption detection
    char padding[4];        // Padding to ensure 48-byte header
};

// Struct for footer of an allocation (8 bytes)
// Used to detect wild writes beyond the end of an allocation
struct m61_footer {
    size_t size;            // Duplicate of total block size
};

// Struct for free list node
struct m61_freenode {
    m61_freenode* next;
    m61_freenode* prev;
};

static constexpr unsigned int MAGIC = 0xDEADBEEF; // Magic number to detect corruption
static constexpr size_t HEAD_SZ = sizeof(m61_header);  // 48 bytes
static constexpr size_t FOOT_SZ = sizeof(m61_footer);  // 8 bytes

/// Buddy allocation system works using free lists.
/// Free blocks are stored in separate free lists (one per order).
/// Allocation splits larger blocks in half until desired size is reached.
/// Freeing coalesces buddy pairs into larger blocks using freenodes and order.

static constexpr int MIN_ORDER = 6;
static constexpr int MAX_ORDER = 23;
static constexpr int NUM_ORDERS = MAX_ORDER + 1;

static m61_freenode* free_lists[NUM_ORDERS];

static m61_statistics memory_stats = {
    .nactive = 0, .active_size = 0, .ntotal = 0,
    .total_size = 0, .nfail = 0, .fail_size = 0,
    .heap_min = 0, .heap_max = 0
};

// Return the smallest order whose block size >= sz
static int order_for_size(size_t sz) {
    int order = MIN_ORDER;
    size_t block_size = (size_t)1 << order;
    while (block_size < sz && order < 64) {
        ++order;
        block_size <<= 1;
    }
    return order;
}

// Return the block size for a given order
static inline size_t size_for_order(int order) {
    return (size_t)1 << order;
}

// Insert a free block into the free list for the given order
static void free_list_insert(m61_freenode* node, int order) {
    node->next = free_lists[order];
    node->prev = nullptr;
    if (free_lists[order]) { // If free list of this order is not empty
        free_lists[order]->prev = node;
    }
    free_lists[order] = node;
}

// Remove a free block from the free list for the given order
static void free_list_remove(m61_freenode* node, int order) {
    // Remove node from free list
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        free_lists[order] = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    }
    // Reset node pointers
    node->next = nullptr;
    node->prev = nullptr;
}

// Initialize header and footer for a block
static void write_boundary_tags(void* block, size_t block_size, size_t user_size,
                                 int status, const char* file, int line) {
    m61_header* h = (m61_header*)block;
    h->size = block_size;
    h->user_size = user_size;
    h->file = file;
    h->line = line;
    h->status = status;
    h->magic = MAGIC;
    h->self = h;

    m61_footer* f = (m61_footer*)((char*)block + block_size - FOOT_SZ);
    f->size = block_size;
}

// Fill padding region with guard
static void fill_padding(m61_header* h, size_t user_size) {
    size_t block_size = h->size;
    size_t padding_start = HEAD_SZ + user_size;
    size_t padding_end = block_size - FOOT_SZ;
    if (padding_end > padding_start) { // Ensure padding region exists
        memset((char*)h + padding_start, 0xBD, padding_end - padding_start);
    }
}

// Find which buffer contains a pointer
static m61_memory_buffer* find_buffer(void* ptr) {
    for (m61_memory_buffer* b = buffer_list; b; b = b->next) {
        if ((char*)ptr >= b->buffer && (char*)ptr < b->buffer + b->size) {
            return b;
        }
    }
    return nullptr;
}

// Record a failed allocation
static void record_fail(size_t sz) {
    ++memory_stats.nfail;
    memory_stats.fail_size += sz;
}

// Record a successful allocation
static void record_success(size_t sz) {
    ++memory_stats.ntotal;
    ++memory_stats.nactive;
    memory_stats.active_size += sz;
    memory_stats.total_size += sz;
}

// Report an invalid free
static void report_error(const char* file, int line, void* ptr, const char* msg) {
    fprintf(stderr, "MEMORY BUG: %s:%d: invalid free of pointer %p, %s\n",
            file, line, ptr, msg);
    abort();
}
// Report a wild write
static void report_wild_write(const char* file, int line, void* ptr) {
    fprintf(stderr, "MEMORY BUG: %s:%d: detected wild write during free of pointer %p\n",
            file, line, ptr);
    abort();
}

/// m61_malloc(sz, file, line)
///    Returns a pointer to `sz` bytes of freshly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc may
///    return either `nullptr` or a pointer to a unique allocation.
///    The allocation request was made at source code location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, int line) {
    // Check for overflow
    if (sz > SIZE_MAX - HEAD_SZ - FOOT_SZ) {
        record_fail(sz);
        return nullptr;
    }

    size_t total_size = sz + HEAD_SZ + FOOT_SZ;
    int order = order_for_size(total_size);

    // If the allocation is too large, use a dedicated buffer
    if (order > MAX_ORDER) {
        m61_memory_buffer* buf = new m61_memory_buffer(total_size);
        if (!buf->buffer) { // If allocation failed
            delete buf;
            record_fail(sz);
            return nullptr;
        }
        // Add buffer to list 
        buf->next = buffer_list;
        buffer_list = buf;
        buf->pos = total_size;

        // Initialize header and footer 
        m61_header* h = (m61_header*)buf->buffer;
        write_boundary_tags(h, total_size, sz, 1, file, line);
        fill_padding(h, sz);

        // Update heap bounds
        if (total_size > memory_stats.heap_max) {
            memory_stats.heap_max = total_size;
        }
        if (total_size < memory_stats.heap_min) {
            memory_stats.heap_min = total_size;
        }

        // Update statistics
        record_success(sz);
        return (char*)h + HEAD_SZ;
    }

    // If allocation is not too large, we use buddy system to allocate
    size_t block_size = size_for_order(order);

    // If buffer list is empty, intiailize first buffer
    if (!buffer_list) {
        m61_memory_buffer* buf = new m61_memory_buffer(DEFAULT_BUFFER_SIZE);
        if (!buf->buffer) { // If allocation failed
            delete buf;
            record_fail(sz);
            return nullptr;
        }
        // Add buffer to list
        buffer_list = buf;
        buf->pos = DEFAULT_BUFFER_SIZE;
        // Initialize header and footer
        m61_header* h = (m61_header*)buf->buffer;
        write_boundary_tags(h, DEFAULT_BUFFER_SIZE, 0, 0, "?", 0);
        // Update free list
        free_list_insert((m61_freenode*)((char*)h + HEAD_SZ), MAX_ORDER);
    }

    // Find a free block of sufficient order
    int k = order;
    while (k <= MAX_ORDER && !free_lists[k]) {
        ++k;
    }

    // If none found, allocate a new buffer
    if (k > MAX_ORDER) {
        m61_memory_buffer* buf = new m61_memory_buffer(DEFAULT_BUFFER_SIZE);
        if (!buf->buffer) { // If allocation failed
            delete buf;
            record_fail(sz);
            return nullptr;
        }
        // Add buffer to list
        buf->next = buffer_list;
        buffer_list = buf;
        buf->pos = DEFAULT_BUFFER_SIZE;

        // Initialize header and footer
        m61_header* h = (m61_header*)buf->buffer;
        write_boundary_tags(h, DEFAULT_BUFFER_SIZE, 0, 0, "?", 0);
        // Update free list
        free_list_insert((m61_freenode*)((char*)h + HEAD_SZ), MAX_ORDER);
        k = MAX_ORDER;
    }

    // Remove the chosen block from its free list
    m61_freenode* node = free_lists[k];
    free_list_remove(node, k);
    m61_header* block = (m61_header*)((char*)node - HEAD_SZ);

    // Split block until minimum sufficient order is reached
    while (k > order) {
        --k;
        size_t split_size = size_for_order(k);

        // Right "buddy" goes to free list
        m61_header* buddy = (m61_header*)((char*)block + split_size);
        write_boundary_tags(buddy, split_size, 0, 0, "?", 0);
        free_list_insert((m61_freenode*)((char*)buddy + HEAD_SZ), k);

        // Left "buddy" becomes the used block
        write_boundary_tags(block, split_size, 0, 0, "?", 0);
    }

    // Update header/footer/padding of chosen block
    write_boundary_tags(block, block_size, sz, 1, file, line);
    fill_padding(block, sz);

    // Update heap bounds
    uintptr_t addr = (uintptr_t)((char*)block + HEAD_SZ);
    if (memory_stats.heap_min == 0 || addr < memory_stats.heap_min) {
        memory_stats.heap_min = addr;
    }
    if (addr + sz > memory_stats.heap_max) {
        memory_stats.heap_max = addr + sz;
    }

    // Update statistics
    record_success(sz);
    return (char*)block + HEAD_SZ;
}


/// m61_free(ptr, file, line)
///    Frees the memory allocation pointed to by `ptr`. If `ptr == nullptr`,
///    does nothing. Otherwise, `ptr` must point to a currently active
///    allocation returned by `m61_malloc`. The free was called at location
///    `file`:`line`.

void m61_free(void* ptr, const char* file, int line) {
    if (!ptr) return; // Handle nullptr case

    // Find buffer containing pointer
    char* p = (char*)ptr;
    m61_memory_buffer* buf = find_buffer(ptr);

    // Check if pointer is in heap
    if (!buf || p < buf->buffer + HEAD_SZ) {
        report_error(file, line, ptr, "not in heap");
    }

    // Save a copy of header
    m61_header h_copy;
    memcpy(&h_copy, p - HEAD_SZ, sizeof(m61_header));

    // Check if header is valid
    if (h_copy.magic != MAGIC || h_copy.self != (m61_header*)(p - HEAD_SZ)) {
        fprintf(stderr, "MEMORY BUG: %s:%d: invalid free of pointer %p, not allocated\n",
                file, line, ptr);

        // Scan all buffers to find if pointer is inside another allocation
        for (m61_memory_buffer* b = buffer_list; b; b = b->next) {
            char* curr = b->buffer;
            char* end = b->buffer + b->pos;
            // Iterate through buffer
            while (curr < end) {
                m61_header* ch = (m61_header*)curr;
                if (ch->magic != MAGIC || ch->size == 0) break; // Invalid header 
                // If pointer is inside another allocation, report error
                if (p > curr && p < curr + ch->size && ch->status == 1) {
                    size_t offset = p - (curr + HEAD_SZ);
                    fprintf(stderr, "  %s:%d: %p is %zu bytes inside a %zu byte region allocated here\n",
                            ch->file, ch->line, ptr, offset, ch->user_size);
                    abort();
                }
                curr += ch->size;
            }
        }
        abort();
    }

    m61_header* h = (m61_header*)(p - HEAD_SZ);

    // Check if already freed (double free)
    if (h->status == 0) {
        report_error(file, line, ptr, "double free");
    }

    // Check if footer is intact
    m61_footer* f = (m61_footer*)((char*)h + h->size - FOOT_SZ);
    if (f->size != h->size) {
        report_wild_write(file, line, ptr);
    }

    // Check if padding is intact 
    size_t padding_start = HEAD_SZ + h->user_size;
    size_t padding_end = h->size - FOOT_SZ;
    if (padding_end > padding_start) {
        unsigned char* pad = (unsigned char*)h + padding_start;
        size_t len = padding_end - padding_start;
        for (size_t i = 0; i < len; ++i) {
            if (pad[i] != 0xBD) report_wild_write(file, line, ptr);
        }
    }

    // Update statistics
    h->status = 0;
    --memory_stats.nactive;
    memory_stats.active_size -= h->user_size;

    // For large allocations, return buffer to OS
    int order = order_for_size(h->size);
    if (order > MAX_ORDER && buf->buffer == (char*)h) {
        // Unlink from buffer list
        if (buffer_list == buf) {
            buffer_list = buf->next;
        } else {
            for (m61_memory_buffer* prev = buffer_list; prev; prev = prev->next) {
                if (prev->next == buf) {
                    prev->next = buf->next;
                    break;
                }
            }
        }
        // Return buffer to OS
        delete buf;
        return;
    }

    size_t size = h->size;
    char* base = buf->buffer;
    // Try to coalesce with buddies
    while (order < MAX_ORDER) {
        // Since offset is aligned, we find buddy offset by XORing offset with size
        // XOR with size flips the bit at position log2(size) to find the adjacent buddy
        size_t offset = (char*)h - base;
        size_t buddy_offset = offset ^ size;
        m61_header* buddy = (m61_header*)(base + buddy_offset);

        // If buddy is free, coalesce
        if (buddy->magic == MAGIC && buddy->status == 0 && buddy->size == size) { // Buddy must be valid/free
            free_list_remove((m61_freenode*)((char*)buddy + HEAD_SZ), order); // Remove buddy from free list 
            if (buddy < h) h = buddy; // Choose lower address as new header
            size <<= 1;
            ++order;
            write_boundary_tags(h, size, 0, 0, "?", 0); // Update header
        } else {
            break;
        }
    }

    // Add to free list
    write_boundary_tags(h, size, 0, 0, "?", 0);
    free_list_insert((m61_freenode*)((char*)h + HEAD_SZ), order);
}

/// m61_calloc(count, sz, file, line)
///    Returns a pointer a fresh dynamic memory allocation big enough to
///    hold an array of `count` elements of `sz` bytes each. Returned
///    memory is initialized to zero. The allocation request was at
///    location `file`:`line`. Returns `nullptr` if out of memory; may
///    also return `nullptr` if `count == 0` or `size == 0`.

void* m61_calloc(size_t count, size_t sz, const char* file, int line) {
    // Check for overflow
    if (count != 0 && sz > SIZE_MAX / count) {
        record_fail(count * sz);
        return nullptr;
    }
    // Allocate memory
    void* ptr = m61_malloc(count * sz, file, line);
    if (ptr) {
        memset(ptr, 0, count * sz);
    }
    return ptr;
}

/// m61_realloc(ptr, sz, file, line)
///    Changes the size of the dynamic allocation pointed to by `ptr`
///    to hold at least `sz` bytes. If the existing allocation cannot be
///    enlarged, this function makes a new allocation, copies as much data
///    as possible from the old allocation to the new, and returns a pointer
///    to the new allocation. If `ptr` is `nullptr`, behaves like
///    `m61_malloc(sz, file, line). `sz` must not be 0. If a required
///    allocation fails, returns `nullptr` without freeing the original
///    block.

void* m61_realloc(void* ptr, size_t sz, const char* file, int line) {
    if (!ptr) return m61_malloc(sz, file, line); // Handle nullptr case
    if (sz == 0) { // If new size is zero, free and return nullptr
        m61_free(ptr, file, line);
        return nullptr;
    }

    // If pointer is invalid (not in heap or before header)
    char* p = (char*)ptr;
    m61_memory_buffer* buf = find_buffer(ptr);
    if (!buf || p < buf->buffer + HEAD_SZ) {
        m61_free(ptr, file, line);  // Let m61_free handle error reporting
        return nullptr;
    }

    // If header is corrupted or block is already freed
    m61_header h_copy;
    memcpy(&h_copy, p - HEAD_SZ, sizeof(m61_header));
    if (h_copy.magic != MAGIC || h_copy.self != (m61_header*)(p - HEAD_SZ) ||
        h_copy.status != 1) {
        m61_free(ptr, file, line);  // Let m61_free handle error reporting
        return nullptr;
    }

    // Get old size
    m61_header* h = (m61_header*)(p - HEAD_SZ);
    size_t old_size = h->user_size;

    // If new size is smaller or equal to old size: shrink in place
    if (sz <= old_size) {
        h->user_size = sz;
        memory_stats.active_size -= (old_size - sz);
        fill_padding(h, sz);
        return ptr;
    }

    // If new size is larger: allocate new, copy old to new, free old
    void* new_ptr = m61_malloc(sz, file, line);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        m61_free(ptr, file, line);
    }
    return new_ptr;
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
    // Iterate through all buffers, checking for leaks in each
    for (m61_memory_buffer* b = buffer_list; b; b = b->next) {
        char* curr = b->buffer;
        char* end = b->buffer + b->pos;

        // Check for leaks
        while (curr < end) {
            m61_header* h = (m61_header*)curr;
            if (h->size == 0) break; // Avoid infinite loops/corrupted buffer errors
            if (h->status == 1) { //If block is active
                printf("LEAK CHECK: %s:%d: allocated object %p with size %zu\n",
                       h->file, h->line, curr + HEAD_SZ, h->user_size);
            }
            curr += h->size;
        }
    }
}
