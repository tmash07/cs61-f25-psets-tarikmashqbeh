#include "m61.hh"
#include <cstdio>
#include <cstring>
#include <cassert>

// Check m61_realloc

int main() {
    // Allocate initial block using malloc
    int* ptr = (int*) m61_malloc(sizeof(int) * 10);
    assert(ptr); // Check that allocation was successful
    for (int i = 0; i < 10; ++i) {
        ptr[i] = i;
    }

    // Realloc to larger size
    int* new_ptr = (int*) m61_realloc(ptr, sizeof(int) * 20);
    assert(new_ptr); // Check that realloc was successful

    // Ensure data was copied
    for (int i = 0; i < 10; ++i) {
        assert(new_ptr[i] == i);
    }

    // Ensure new space is valid for writing
    for (int i = 10; i < 20; ++i) {
        new_ptr[i] = i * 2;
    }

    m61_free(new_ptr);

    m61_print_statistics();
}

//! alloc count: active          0   total          2   fail          0
//! alloc size:  active          0   total        120   fail          0
