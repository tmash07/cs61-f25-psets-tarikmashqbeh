#include "m61.hh"
#include <cstdio>
#include <cstring>
#include <cassert>

// Test allocating a block larger than the default 8MiB buffer.

int main() {
    // Attempt to allocate 10 MiB block
    size_t sz = 10 << 20;
    void* ptr = m61_malloc(sz);
    
    m61_statistics stats = m61_get_statistics();
    
    // Checks that only one block was alloacted
    if (stats.ntotal != 1) {
         fprintf(stderr, "ntotal is %llu, expected 1\n", stats.ntotal);
         return 1;
    }
    // Checks that the alloacted block is correctly sized
    if (stats.active_size != sz) {
         fprintf(stderr, "active_size is %llu, expected %zu\n", stats.active_size, sz);
         return 1;
    }

    m61_free(ptr);
    
    stats = m61_get_statistics();
    // Checks that block is correctly freed
    if (stats.nactive != 0) {
         fprintf(stderr, "nactive is %llu, expected 0\n", stats.nactive);
         return 1;
    }
    
    m61_print_statistics();
}

//! alloc count: active          0   total          1   fail          0
//! alloc size:  active          0   total   10485760   fail          0
