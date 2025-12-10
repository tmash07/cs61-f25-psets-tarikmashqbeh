#include "m61.hh"
#include <cstdio>
#include <cassert>

// Check m61_realloc shrinking and freeing.

int main() {
    // Allocate initial block using malloc
    char* ptr = (char*) m61_malloc(100);
    assert(ptr); // Check that allocation was successful
    ptr[0] = 'a';
    ptr[99] = 'z';

    // Realloc to smaller size
    char* same_ptr = (char*) m61_realloc(ptr, 50);
    assert(same_ptr == ptr); // Check that realloc shrunk in place
    assert(ptr[0] == 'a'); // Check that realloc preserved data

    // Realloc to 0 (same as freeing the allocation)
    void* null_ptr = m61_realloc(ptr, 0);
    assert(null_ptr == nullptr); // Check that realloc returned nullptr

    m61_print_statistics();
}

//! alloc count: active          0   total          1   fail          0
//! alloc size:  active          0   total        100   fail          0
